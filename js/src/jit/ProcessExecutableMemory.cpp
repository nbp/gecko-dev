/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ProcessExecutableMemory.h"

#include "mozilla/Array.h"
#include "mozilla/Atomics.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"
#include "mozilla/TaggedAnonymousMemory.h"
#include "mozilla/XorShift128PlusRNG.h"

#include <errno.h>

#include "jsfriendapi.h"
#include "jsmath.h"

#include "gc/Memory.h"
#include "jit/FlushICache.h"  // js::jit::FlushICache
#include "jit/JitOptions.h"
#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "util/Memory.h"
#include "util/Poison.h"
#include "util/WindowsWrapper.h"
#include "vm/MutexIDs.h"

#ifdef XP_WIN
#  include "mozilla/StackWalk_windows.h"
#  include "mozilla/WindowsVersion.h"
#elif defined(__wasi__)
#  if defined(JS_CODEGEN_WASM32)
#    include <cstdlib>
#  else
// Nothing.
#  endif
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

#ifdef MOZ_VALGRIND
#  include <valgrind/valgrind.h>
#endif

#if defined(XP_IOS)
#  include <BrowserEngineCore/BEMemory.h>
#endif

using namespace js;
using namespace js::jit;

static int GetPkey(void* addr, size_t size);

#ifdef XP_WIN
#  if defined(HAVE_64BIT_BUILD)
#    define NEED_JIT_UNWIND_HANDLING
#  endif

static void* ComputeRandomAllocationAddress() {
  /*
   * Inspiration is V8's OS::Allocate in platform-win32.cc.
   *
   * VirtualAlloc takes 64K chunks out of the virtual address space, so we
   * keep 16b alignment.
   *
   * x86: V8 comments say that keeping addresses in the [64MiB, 1GiB) range
   * tries to avoid system default DLL mapping space. In the end, we get 13
   * bits of randomness in our selection.
   * x64: [2GiB, 4TiB), with 25 bits of randomness.
   */
#  ifdef HAVE_64BIT_BUILD
  static const uintptr_t base = 0x0000000080000000;
  static const uintptr_t mask = 0x000003ffffff0000;
#  elif defined(_M_IX86) || defined(__i386__)
  static const uintptr_t base = 0x04000000;
  static const uintptr_t mask = 0x3fff0000;
#  else
#    error "Unsupported architecture"
#  endif

  uint64_t rand = js::GenerateRandomSeed();
  return (void*)(base | (rand & mask));
}

#  ifdef NEED_JIT_UNWIND_HANDLING
static js::JitExceptionHandler sJitExceptionHandler;
static bool sHasInstalledFunctionTable = false;
#  endif

JS_PUBLIC_API void js::SetJitExceptionHandler(JitExceptionHandler handler) {
#  ifdef NEED_JIT_UNWIND_HANDLING
  MOZ_ASSERT(!sJitExceptionHandler);
  sJitExceptionHandler = handler;
#  else
  // Just do nothing if unwind handling is disabled.
#  endif
}

#  ifdef NEED_JIT_UNWIND_HANDLING
#    if defined(_M_ARM64)
// See the ".xdata records" section of
// https://docs.microsoft.com/en-us/cpp/build/arm64-exception-handling
// These records can have various fields present or absent depending on the
// bits set in the header. Our struct will use one 32-bit slot for unwind codes,
// and no slots for epilog scopes.
struct UnwindData {
  uint32_t functionLength : 18;
  uint32_t version : 2;
  uint32_t hasExceptionHandler : 1;
  uint32_t packedEpilog : 1;
  uint32_t epilogCount : 5;
  uint32_t codeWords : 5;
  uint8_t unwindCodes[4];
  uint32_t exceptionHandler;
};

static const unsigned ThunkLength = 20;
#    else
// From documentation for UNWIND_INFO on
// https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64
struct UnwindInfo {
  uint8_t version : 3;
  uint8_t flags : 5;
  uint8_t sizeOfPrologue;
  uint8_t countOfUnwindCodes;
  uint8_t frameRegister : 4;
  uint8_t frameOffset : 4;
};
static const unsigned ThunkLength = 12;
union UnwindCode {
  struct {
    uint8_t codeOffset;
    uint8_t unwindOp : 4;
    uint8_t opInfo : 4;
  };
  uint16_t frameOffset;
};

static constexpr int kNumberOfUnwindCodes = 2;
static constexpr int kPushRbpInstructionLength = 1;
static constexpr int kMovRbpRspInstructionLength = 3;
static constexpr int kRbpPrefixCodes = 2;
static constexpr int kRbpPrefixLength =
    kPushRbpInstructionLength + kMovRbpRspInstructionLength;

struct UnwindData {
  UnwindInfo unwindInfo;
  UnwindCode unwindCodes[kNumberOfUnwindCodes];
  uint32_t exceptionHandler;

  UnwindData() {
    static constexpr int kOpPushNonvol = 0;
    static constexpr int kOpSetFPReg = 3;

    unwindInfo.version = 1;
    unwindInfo.flags = UNW_FLAG_EHANDLER;
    unwindInfo.sizeOfPrologue = kRbpPrefixLength;
    unwindInfo.countOfUnwindCodes = kRbpPrefixCodes;
    unwindInfo.frameRegister = 5;
    unwindInfo.frameOffset = 0;

    // Offset here are specified to beginning of the -next- instruction.
    unwindCodes[0].codeOffset = kRbpPrefixLength;  // movq rbp, rsp
    unwindCodes[0].unwindOp = kOpSetFPReg;
    unwindCodes[0].opInfo = 0;

    unwindCodes[1].codeOffset = kPushRbpInstructionLength;  // push rbp
    unwindCodes[1].unwindOp = kOpPushNonvol;
    unwindCodes[1].opInfo = 5;
  }
};
#    endif

struct ExceptionHandlerRecord {
  void* dynamicTable;
  UnwindData unwindData;
  uint8_t thunk[ThunkLength];
  RUNTIME_FUNCTION runtimeFunction;
};

// This function must match the function pointer type PEXCEPTION_HANDLER
// mentioned in:
//   http://msdn.microsoft.com/en-us/library/ssa62fwe.aspx.
// This type is rather elusive in documentation; Wine is the best I've found:
//   http://source.winehq.org/source/include/winnt.h
static DWORD ExceptionHandler(PEXCEPTION_RECORD exceptionRecord,
                              _EXCEPTION_REGISTRATION_RECORD*, PCONTEXT context,
                              _EXCEPTION_REGISTRATION_RECORD**) {
  if (sJitExceptionHandler) {
    return sJitExceptionHandler(exceptionRecord, context);
  }

  return ExceptionContinueSearch;
}

// Required for enabling Stackwalking on windows using external tools.
extern "C" NTSYSAPI DWORD NTAPI RtlAddGrowableFunctionTable(
    PVOID* DynamicTable, PRUNTIME_FUNCTION FunctionTable, DWORD EntryCount,
    DWORD MaximumEntryCount, ULONG_PTR RangeBase, ULONG_PTR RangeEnd);

// For an explanation of the problem being solved here, see
// SetJitExceptionFilter in jsfriendapi.h.
static bool RegisterExecutableMemory(void* p, size_t bytes, size_t pageSize) {
  if (!VirtualAlloc(p, pageSize, MEM_COMMIT, PAGE_READWRITE)) {
    MOZ_CRASH();
  }

  // A page was reserved inside this structure for the record. This is because
  // all entries in the record are describes as an offset from the start of the
  // memory region. We construct the record there.
  ExceptionHandlerRecord* r = new (p) ExceptionHandlerRecord();
  void* handler = JS_FUNC_TO_DATA_PTR(void*, ExceptionHandler);

  // Because the .xdata format on ARM64 can only encode sizes up to 1M (much
  // too small for our JIT code regions), we register a function table callback
  // to provide RUNTIME_FUNCTIONs at runtime. Windows doesn't seem to care about
  // the size fields on RUNTIME_FUNCTIONs that are created in this way, so the
  // same RUNTIME_FUNCTION can work for any address in the region. We'll set up
  // a generic one now and the callback can just return a pointer to it.

  // All these fields are specified to be offsets from the base of the
  // executable code (which is 'p'), even if they have 'Address' in their
  // names. In particular, exceptionHandler is a ULONG offset which is a
  // 32-bit integer. Since 'p' can be farther than INT32_MAX away from
  // sJitExceptionHandler, we must generate a little thunk inside the
  // record. The record is put on its own page so that we can take away write
  // access to protect against accidental clobbering.

#    if defined(_M_ARM64)
  if (!sJitExceptionHandler) {
    return false;
  }

  r->runtimeFunction.BeginAddress = pageSize;
  r->runtimeFunction.UnwindData = offsetof(ExceptionHandlerRecord, unwindData);
  static_assert(offsetof(ExceptionHandlerRecord, unwindData) % 4 == 0,
                "The ARM64 .pdata format requires that exception information "
                "RVAs be 4-byte aligned.");

  memset(&r->unwindData, 0, sizeof(r->unwindData));
  r->unwindData.hasExceptionHandler = true;
  r->unwindData.exceptionHandler = offsetof(ExceptionHandlerRecord, thunk);

  // Use a fake unwind code to make the Windows unwinder do _something_. If the
  // PC and SP both stay unchanged, we'll fail the unwinder's sanity checks and
  // it won't call our exception handler.
  r->unwindData.codeWords = 1;  // one 32-bit word gives us up to 4 codes
  r->unwindData.unwindCodes[0] =
      0b00000001;  // alloc_s small stack of size 1*16
  r->unwindData.unwindCodes[1] = 0b11100100;  // end

  uint32_t* thunk = (uint32_t*)r->thunk;
  uint16_t* addr = (uint16_t*)&handler;

  // xip0/r16 should be safe to clobber: Windows just used it to call our thunk.
  const uint8_t reg = 16;

  // Say `handler` is 0x4444333322221111, then:
  thunk[0] = 0xd2800000 | addr[0] << 5 | reg;  // mov  xip0, 1111
  thunk[1] = 0xf2a00000 | addr[1] << 5 | reg;  // movk xip0, 2222 lsl #0x10
  thunk[2] = 0xf2c00000 | addr[2] << 5 | reg;  // movk xip0, 3333 lsl #0x20
  thunk[3] = 0xf2e00000 | addr[3] << 5 | reg;  // movk xip0, 4444 lsl #0x30
  thunk[4] = 0xd61f0000 | reg << 5;            // br xip0
#    else
  r->runtimeFunction.BeginAddress = pageSize;
  r->runtimeFunction.EndAddress = (DWORD)bytes;
  r->runtimeFunction.UnwindData = offsetof(ExceptionHandlerRecord, unwindData);
  r->unwindData.exceptionHandler = offsetof(ExceptionHandlerRecord, thunk);

  // mov imm64, rax
  r->thunk[0] = 0x48;
  r->thunk[1] = 0xb8;
  memcpy(&r->thunk[2], &handler, 8);

  // jmp rax
  r->thunk[10] = 0xff;
  r->thunk[11] = 0xe0;
#    endif

  // RtlAddGrowableFunctionTable will write into the region. We must therefore
  // only write-protect is after this has been called.

  // XXX NB: The profiler believes this function is only called from the main
  // thread. If that ever becomes untrue, the profiler must be updated
  // immediately.
  {
    AutoSuppressStackWalking suppress;
    DWORD result = RtlAddGrowableFunctionTable(
        &r->dynamicTable, &r->runtimeFunction, 1, 1, (ULONG_PTR)p,
        (ULONG_PTR)p + bytes - pageSize);
    if (result != S_OK) {
      return false;
    }
  }

  DWORD oldProtect;
  if (!VirtualProtect(p, pageSize, PAGE_EXECUTE_READ, &oldProtect)) {
    MOZ_CRASH();
  }

  return true;
}

static void UnregisterExecutableMemory(void* p, size_t bytes, size_t pageSize) {
  // There's no such thing as RtlUninstallFunctionTableCallback, so there's
  // nothing to do here.
}
#  endif

static void* ReserveProcessJitMemory(size_t xBytes, size_t roBytes) {
  size_t bytes = xBytes + roBytes;

#  ifdef NEED_JIT_UNWIND_HANDLING
  size_t pageSize = gc::SystemPageSize();
  // Always reserve space for the unwind information.
  bytes += pageSize;
#  endif

  void* p = nullptr;
  for (size_t i = 0; i < 10; i++) {
    void* randomAddr = ComputeRandomAllocationAddress();
    p = VirtualAlloc(randomAddr, bytes, MEM_RESERVE, PAGE_NOACCESS);
    if (p) {
      break;
    }
  }

  if (!p) {
    // Try again without randomization.
    p = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) {
      return nullptr;
    }
  }

#  ifdef NEED_JIT_UNWIND_HANDLING
  if (RegisterExecutableMemory(p, xBytes, pageSize)) {
    sHasInstalledFunctionTable = true;
  } else {
    if (sJitExceptionHandler) {
      // This should have succeeded if we have an exception handler. Bail.
      VirtualFree(p, 0, MEM_RELEASE);
      return nullptr;
    }
  }

  // Skip the first page where we might have allocated an exception handler
  // record.
  p = (uint8_t*)p + pageSize;

  RegisterJitCodeRegion((uint8_t*)p, xBytes);
#  endif
  return p;
}

static void DeallocateProcessJitMemory(void* addr, size_t xBytes,
                                       size_t roBytes) {
#  ifdef NEED_JIT_UNWIND_HANDLING
  UnregisterJitCodeRegion((uint8_t*)addr, xBytes);

  size_t pageSize = gc::SystemPageSize();
  addr = (uint8_t*)addr - pageSize;

  if (sHasInstalledFunctionTable) {
    UnregisterExecutableMemory(addr, xBytes, pageSize);
  }
#  endif

  VirtualFree(addr, 0, MEM_RELEASE);
}

static DWORD ProtectionSettingToFlags(ProtectionSetting protection, int pkey) {
  if (!JitOptions.writeProtectCode || pkey != -1) {
    return PAGE_EXECUTE_READWRITE;
  }
  switch (protection) {
    case ProtectionSetting::ReadOnly:
      return PAGE_READ;
    case ProtectionSetting::Writable:
      return PAGE_READWRITE;
    case ProtectionSetting::Executable:
      return PAGE_EXECUTE_READ;
  }
  MOZ_CRASH();
}

[[nodiscard]] static bool CommitPages(void* addr, size_t bytes,
                                      ProtectionSetting protection) {
  int pkey = GetPkey(addr, bytes);
  void* p = VirtualAlloc(addr, bytes, MEM_COMMIT,
                         ProtectionSettingToFlags(protection, pkey));
  if (!p) {
    return false;
  }
  MOZ_RELEASE_ASSERT(p == addr);
  return true;
}

static void DecommitPages(void* addr, size_t bytes) {
  if (!VirtualFree(addr, bytes, MEM_DECOMMIT)) {
    MOZ_CRASH("DecommitPages failed");
  }
}
#elif defined(__wasi__)
#  if defined(JS_CODEGEN_WASM32)
static void* ReserveProcessJitMemory(size_t xBytes, size_t roBytes) {
  return malloc(xBytes + roBytes);
}

static void DeallocateProcessJitMemory(void* addr, size_t xBytes,
                                       size_t roBytes) {
  free(addr);
}

[[nodiscard]] static bool CommitPages(void* addr, size_t bytes,
                                      ProtectionSetting protection) {
  return true;
}

static void DecommitPages(void* addr, size_t bytes) {}

#  else
static void* ReserveProcessJitMemory(size_t xBytes, size_t roBytes) {
  MOZ_CRASH("NYI for WASI.");
  return nullptr;
}
static void DeallocateProcessJitMemory(void* addr, size_t xBytes,
                                       size_t roBytes) {
  MOZ_CRASH("NYI for WASI.");
}
[[nodiscard]] static bool CommitPages(void* addr, size_t bytes,
                                      ProtectionSetting protection) {
  MOZ_CRASH("NYI for WASI.");
  return false;
}
static void DecommitPages(void* addr, size_t bytes) {
  MOZ_CRASH("NYI for WASI.");
}
#  endif
#else  // !XP_WIN && !__wasi__
#  ifndef MAP_NORESERVE
#    define MAP_NORESERVE 0
#  endif

static void* ComputeRandomAllocationAddress() {
#  ifdef __OpenBSD__
  // OpenBSD already has random mmap and the idea that all x64 cpus
  // have 48-bit address space is not correct. Returning nullptr
  // allows OpenBSD do to the right thing.
  return nullptr;
#  else
  uint64_t rand = js::GenerateRandomSeed();

#    ifdef HAVE_64BIT_BUILD
  // x64 CPUs have a 48-bit address space and on some platforms the OS will
  // give us access to 47 bits, so to be safe we right shift by 18 to leave
  // 46 bits.
  rand >>= 18;
#    else
  // On 32-bit, right shift by 34 to leave 30 bits, range [0, 1GiB). Then add
  // 512MiB to get range [512MiB, 1.5GiB), or [0x20000000, 0x60000000). This
  // is based on V8 comments in platform-posix.cc saying this range is
  // relatively unpopulated across a variety of kernels.
  rand >>= 34;
  rand += 512 * 1024 * 1024;
#    endif

  // Ensure page alignment.
  uintptr_t mask = ~uintptr_t(gc::SystemPageSize() - 1);
  return (void*)uintptr_t(rand & mask);
#  endif
}

static void DecommitPages(void* addr, size_t bytes);

static void* ReserveProcessJitMemory(size_t xBytes, size_t roBytes) {
  // On most Unix platforms our strategy is as follows:
  //
  // * Reserve:  mmap with PROT_NONE
  // * Commit:   mmap with MAP_FIXED, PROT_READ | ...
  // * Decommit: mmap with MAP_FIXED, PROT_NONE
  //
  // On Apple Silicon this only works if we use mprotect to implement W^X. To
  // use RWX pages with the faster pthread_jit_write_protect_np API for
  // thread-local writable/executable switching, the kernel enforces the
  // following rules:
  //
  // * The initial mmap must be called with MAP_JIT.
  // * MAP_FIXED can't be used with MAP_JIT.
  // * Since macOS 11.2, mprotect can't be used to change permissions of RWX JIT
  //   pages (even PROT_NONE fails).
  //   See https://developer.apple.com/forums/thread/672804.
  //
  // This means we have to use the following strategy on Apple Silicon:
  //
  // * Reserve:  1) mmap with PROT_READ | PROT_WRITE | PROT_EXEC and MAP_JIT
  //             2) decommit
  // * Commit:   madvise with MADV_FREE_REUSE
  // * Decommit: madvise with MADV_FREE_REUSABLE
  //
  // On Intel Macs we also need to use MAP_JIT, to be compatible with the
  // Hardened Runtime (with com.apple.security.cs.allow-jit = true). The
  // pthread_jit_write_protect_np API is not available on Intel and MAP_JIT
  // can't be used with MAP_FIXED, so we have to use a hybrid of the above two
  // strategies:
  //
  // * Reserve:  1) mmap with PROT_NONE and MAP_JIT
  //             2) decommit
  // * Commit:   1) madvise with MADV_FREE_REUSE
  //             2) mprotect with PROT_READ | ...
  // * Decommit: 1) mprotect with PROT_NONE
  //             2) madvise with MADV_FREE_REUSABLE
  //
  // This is inspired by V8's code in OS::SetPermissions.

  // Note that randomAddr is just a hint: if the address is not available
  // mmap will pick a different address.
  size_t bytes = xBytes + roBytes;
  void* randomAddr = ComputeRandomAllocationAddress();
  unsigned protection = PROT_NONE;
  unsigned flags = MAP_NORESERVE | MAP_PRIVATE | MAP_ANON;
#  if defined(XP_DARWIN)
  flags |= MAP_JIT;
#    if defined(JS_USE_APPLE_FAST_WX)
  protection = PROT_READ | PROT_WRITE | PROT_EXEC;
#    endif
#  endif
  void* p = MozTaggedAnonymousMmap(randomAddr, bytes, protection, flags, -1, 0,
                                   "js-jit-memory");
  if (p == MAP_FAILED) {
    return nullptr;
  }
#  if defined(XP_DARWIN)
  DecommitPages(p, bytes);
#  endif
  return p;
}

static void DeallocateProcessJitMemory(void* addr, size_t xBytes,
                                       size_t roBytes) {
  mozilla::DebugOnly<int> result = munmap(addr, xBytes + roBytes);
  MOZ_ASSERT(!result || errno == ENOMEM);
}

static unsigned ProtectionSettingToFlags(ProtectionSetting protection, int pkey) {
  if (!JitOptions.writeProtectCode || pkey != -1) {
    return PROT_READ | PROT_WRITE | PROT_EXEC;
  }
#  ifdef MOZ_VALGRIND
  // If we're configured for Valgrind and running on it, use a slacker
  // scheme that doesn't change execute permissions, since doing so causes
  // Valgrind a lot of extra overhead re-JITting code that loses and later
  // regains execute permission.  See bug 1338179.
  if (RUNNING_ON_VALGRIND) {
    switch (protection) {
      case ProtectionSetting::ReadOnly:
        return PROT_READ;
      case ProtectionSetting::Writable:
        return PROT_READ | PROT_WRITE | PROT_EXEC;
      case ProtectionSetting::Executable:
        return PROT_READ | PROT_EXEC;
    }
    MOZ_CRASH();
  }
  // If we get here, we're configured for Valgrind but not running on
  // it, so use the standard scheme.
#  endif
  switch (protection) {
    case ProtectionSetting::ReadOnly:
      return PROT_READ;
    case ProtectionSetting::Writable:
      return PROT_READ | PROT_WRITE;
    case ProtectionSetting::Executable:
      return PROT_READ | PROT_EXEC;
  }
  MOZ_CRASH();
}

[[nodiscard]] static bool CommitPages(void* addr, size_t bytes,
                                      ProtectionSetting protection) {
  // See the comment in ReserveProcessJitMemory.
  int pkey = GetPkey(addr, bytes);
#  if defined(XP_DARWIN)
  int ret;
  do {
    ret = madvise(addr, bytes, MADV_FREE_REUSE);
  } while (ret != 0 && errno == EAGAIN);
  if (ret != 0) {
    return false;
  }
#    if !defined(JS_USE_APPLE_FAST_WX)
  unsigned flags = ProtectionSettingToFlags(protection, pkey);
  if (pkey_mprotect(addr, bytes, flags, pkey)) {
    return false;
  }
#    endif
  return true;
#  else
  unsigned flags = ProtectionSettingToFlags(protection, pkey);
  void* p = MozTaggedAnonymousMmap(addr, bytes, flags,
                                   MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0,
                                   "js-executable-memory");
  if (p == MAP_FAILED) {
    return false;
  }
  MOZ_RELEASE_ASSERT(p == addr);
  // Assign the pkey to the committed pages.
  if (pkey != -1 && pkey_mprotect(addr, bytes, flags, pkey)) {
    return false;
  }
  return true;
#  endif
}

static void DecommitPages(void* addr, size_t bytes) {
  // See the comment in ReserveProcessJitMemory.
#  if defined(XP_DARWIN)
  int ret;
#    if !defined(JS_USE_APPLE_FAST_WX)
  int pkey = GetPkey(addr, bytes);
  ret = pkey_mprotect(addr, bytes, PROT_NONE, pkey);
  MOZ_RELEASE_ASSERT(ret == 0);
#    endif
  do {
    ret = madvise(addr, bytes, MADV_FREE_REUSABLE);
  } while (ret != 0 && errno == EAGAIN);
  MOZ_RELEASE_ASSERT(ret == 0);
#  else
  // Use mmap with MAP_FIXED and PROT_NONE. Inspired by jemalloc's
  // pages_decommit.
  void* p = MozTaggedAnonymousMmap(addr, bytes, PROT_NONE,
                                   MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0,
                                   "js-executable-memory");
  MOZ_RELEASE_ASSERT(addr == p);
#  endif
}
#endif

template <size_t NumBits>
class PageBitSet {
  using WordType = uint32_t;
  static const size_t BitsPerWord = sizeof(WordType) * 8;

  static_assert((NumBits % BitsPerWord) == 0,
                "NumBits must be a multiple of BitsPerWord");
  static const size_t NumWords = NumBits / BitsPerWord;

  mozilla::Array<WordType, NumWords> words_;

  uint32_t indexToWord(uint32_t index) const {
    MOZ_ASSERT(index < NumBits);
    return index / BitsPerWord;
  }
  WordType indexToBit(uint32_t index) const {
    MOZ_ASSERT(index < NumBits);
    return WordType(1) << (index % BitsPerWord);
  }

 public:
  void init() { mozilla::PodArrayZero(words_); }
  bool contains(size_t index) const {
    uint32_t word = indexToWord(index);
    return words_[word] & indexToBit(index);
  }
  void insert(size_t index) {
    MOZ_ASSERT(!contains(index));
    uint32_t word = indexToWord(index);
    words_[word] |= indexToBit(index);
  }
  void remove(size_t index) {
    MOZ_ASSERT(contains(index));
    uint32_t word = indexToWord(index);
    words_[word] &= ~indexToBit(index);
  }

#ifdef DEBUG
  bool empty() const {
    for (size_t i = 0; i < NumWords; i++) {
      if (words_[i] != 0) {
        return false;
      }
    }
    return true;
  }
#endif
};

// Per-process executable memory allocator. It reserves a block of memory of
// MaxCodeBytesPerProcess bytes for executable section and
// MaxDataBytesPerProcess bytes for the data section of JIT code, then
// allocates/deallocates pages from these reserved memory regions.
//
// This has a number of benefits compared to raw mmap/VirtualAlloc:
//
// * More resillient against certain attacks.
//
// * Behaves more consistently across platforms: it avoids the 64K granularity
//   issues on Windows, for instance.
//
// * On x64, near jumps can be used for jumps to other JIT pages.
//
// * On Win64, we have to register the exception handler only once (at process
//   startup). This saves some memory and avoids RtlAddFunctionTable profiler
//   deadlocks.
class ProcessJitMemory {
  static_assert(
      (MaxCodeBytesPerProcess % ExecutableCodePageSize) == 0,
      "MaxCodeBytesPerProcess must be a multiple of ExecutableCodePageSize");
  static const size_t MaxCodePages =
      MaxCodeBytesPerProcess / ExecutableCodePageSize;
  static_assert(
      (MaxDataBytesPerProcess % ReadOnlyDataPageSize) == 0,
      "MaxDataBytesPerProcess must be a multiple of ReadOnlyDataPageSize");
  static const size_t MaxDataPages =
      MaxDataBytesPerProcess / ReadOnlyDataPageSize;
  static const size_t MaxJitMemoryPerProcess =
      MaxCodeBytesPerProcess + MaxDataBytesPerProcess;

  // Start of the MaxCodeBytesPerProcess and MaxDataBytesPerProcess memory
  // blocks or nullptr if uninitialized. Note that this is NOT guaranteed to be
  // aligned to ExecutableCodePageSize.
  uint8_t* base_;

  // The fields below should only be accessed while we hold the lock.
  Mutex lock_ MOZ_UNANNOTATED;

  // pagesAllocated_ is an Atomic so that bytesAllocated does not have to
  // take the lock.
  mozilla::Atomic<size_t, mozilla::ReleaseAcquire> xPagesAllocated_;
  mozilla::Atomic<size_t, mozilla::ReleaseAcquire> roPagesAllocated_;

  // Page where we should try to allocate next executable pages.
  size_t xCursor_;
  // Page where we should try to allocate the next data pages.
  size_t roCursor_;

  // Random number generator to skip page allocations.
  mozilla::Maybe<mozilla::non_crypto::XorShift128PlusRNG> rng_;

  // Set of executable pages which are already allocated.
  PageBitSet<MaxCodePages> xPages_;

  // Set of data pages which are already allocated.
  PageBitSet<MaxDataPages> roPages_;

  // Memory protection key used to toggle on/off the read & write access to
  // executable pages on a single thread.
  int execKey_;

 public:
  ProcessJitMemory()
      : base_(nullptr),
        lock_(mutexid::ProcessExecutableRegion),
        xPagesAllocated_(0),
        roPagesAllocated_(0),
        xCursor_(0),
        roCursor_(0),
        xPages_(),
        roPages_(),
        execKey_(-1) {}

  [[nodiscard]] bool init() {
    xPages_.init();
    roPages_.init();

    MOZ_RELEASE_ASSERT(!initialized());
    MOZ_RELEASE_ASSERT(HasJitBackend());
    MOZ_RELEASE_ASSERT(gc::SystemPageSize() <= ExecutableCodePageSize);

    void* p = ReserveProcessJitMemory(MaxCodeBytesPerProcess,
                                      MaxDataBytesPerProcess);
    if (!p) {
      return false;
    }

    base_ = static_cast<uint8_t*>(p);

    // TODO: only made to work on Linux x86_64 at the moment.
    //
    // Return -1 in case of error, such as non-supported hardware or all 15 keys
    // have been allocated. Using -1 value is safe as argument of pkey_mprotect.
    //
    // CommitPages will set the pkey to the desired pages.
    execKey_ = pkey_alloc(0, PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE);

    mozilla::Array<uint64_t, 2> seed;
    GenerateXorShift128PlusSeed(seed);
    rng_.emplace(seed[0], seed[1]);
    return true;
  }

  uint8_t* base() const { return base_; }

  bool initialized() const { return base_ != nullptr; }

  size_t bytesAllocated() const {
    MOZ_ASSERT(xPagesAllocated_ <= MaxCodePages);
    MOZ_ASSERT(roPagesAllocated_ <= MaxDataPages);
    return xPagesAllocated_ * ExecutableCodePageSize +
        roPagesAllocated_ * ReadOnlyDataPageSize;
  }

  void release() {
    MOZ_ASSERT(initialized());
    MOZ_ASSERT(xPages_.empty());
    MOZ_ASSERT(roPages_.empty());
    MOZ_ASSERT(xPagesAllocated_ == 0);
    MOZ_ASSERT(roPagesAllocated_ == 0);
    DeallocateProcessJitMemory(base_, MaxCodeBytesPerProcess,
                               MaxDataBytesPerProcess);
    base_ = nullptr;
    rng_.reset();
    MOZ_ASSERT(!initialized());
  }

  const uint8_t* codeBase() const { return base_; }
  const uint8_t* codeEnd() const { return codeBase() + MaxCodeBytesPerProcess; }
  const uint8_t* dataBase() const { return codeEnd(); }
  const uint8_t* dataEnd() const { return dataBase() + MaxDataBytesPerProcess; }

  bool containsCodeAddressRange(const void* p, size_t bytes) const {
    return p >= codeBase() && uintptr_t(p) + bytes < uintptr_t(codeEnd());
  }

  bool containsDataAddressRange(const void* p, size_t bytes) const {
    return p >= dataBase() && uintptr_t(p) + bytes < uintptr_t(dataEnd());
  }

  void assertCodeValidAddress(void* p, size_t bytes) const {
    MOZ_RELEASE_ASSERT(containsCodeAddressRange(p, bytes));
  }
  void assertDataValidAddress(void* p, size_t bytes) const {
    MOZ_RELEASE_ASSERT(containsDataAddressRange(p, bytes));
  }
  void assertValidProtection(void* p, size_t bytes,
                             ProtectionSetting protection) const {
    if (containsCodeAddressRange(p, bytes)) {
      MOZ_RELEASE_ASSERT(protection == ProtectionSetting::Executable ||
                         protection == ProtectionSetting::Writable);
      return;
    }
    MOZ_RELEASE_ASSERT(containsDataAddressRange(p, bytes));
    MOZ_RELEASE_ASSERT(protection == ProtectionSetting::ReadOnly ||
                       protection == ProtectionSetting::Writable);
  }

  bool containsAddress(const void* p) const {
    return containsCodeAddressRange(p, 0) ||
        containsDataAddressRange(p, 0);
  }

  // Allocate enough pages to fit some executable bytes.
  void* xAllocate(size_t bytes, ProtectionSetting protection,
                 MemCheckKind checkKind);
  // "Free" some executable allocations.
  void xDeallocate(void* addr, size_t bytes, bool decommit);

  // Allocate enough pages to fit some data bytes.
  void* roAllocate(size_t bytes, ProtectionSetting protection,
                 MemCheckKind checkKind);
  // "Free" some data allocations.
  void roDeallocate(void* addr, size_t bytes, bool decommit);

  // Return the memory protection key associated with the pointer, or -1 if none
  // were allocated.
  int memoryProtectionKey(void* p, size_t bytes) const {
    if (containsCodeAddressRange(p, bytes)) {
      return execKey_;
    }
    MOZ_RELEASE_ASSERT(containsDataAddressRange(p, bytes));
    return -1;
  }
  static int ThreadProtection(ProtectionSetting protection) {
    switch (protection) {
      case ProtectionSetting::ReadOnly:
        return PKEY_DISABLE_WRITE;
      case ProtectionSetting::Writable:
        return 0;
      case ProtectionSetting::Executable:
        return PKEY_DISABLE_WRITE | PKEY_DISABLE_ACCESS;
    }
  }
};

static ProcessJitMemory jitMemory;

static int GetPkey(void* addr, size_t size) {
  return jitMemory.memoryProtectionKey(addr, size);
}

void* ProcessJitMemory::xAllocate(size_t bytes, ProtectionSetting protection,
                                  MemCheckKind checkKind) {
  MOZ_ASSERT(initialized());
  MOZ_ASSERT(HasJitBackend());
  MOZ_ASSERT(bytes > 0);
  MOZ_ASSERT((bytes % ExecutableCodePageSize) == 0);

  size_t numPages = bytes / ExecutableCodePageSize;

  // Take the lock and try to allocate.
  void* p = nullptr;
  {
    LockGuard<Mutex> guard(lock_);
    MOZ_ASSERT(xPagesAllocated_ <= MaxCodePages);

    // Check if we have enough pages available.
    if (xPagesAllocated_ + numPages >= MaxCodePages) {
      return nullptr;
    }

    MOZ_ASSERT(bytes <= MaxCodeBytesPerProcess);

    // Maybe skip a page to make allocations less predictable.
    size_t page = xCursor_ + (rng_.ref().next() % 2);

    for (size_t i = 0; i < MaxCodePages; i++) {
      // Make sure page + numPages - 1 is a valid index.
      if (page + numPages > MaxCodePages) {
        page = 0;
      }

      bool available = true;
      for (size_t j = 0; j < numPages; j++) {
        if (xPages_.contains(page + j)) {
          available = false;
          break;
        }
      }
      if (!available) {
        page++;
        continue;
      }

      // Mark the pages as unavailable.
      for (size_t j = 0; j < numPages; j++) {
        xPages_.insert(page + j);
      }

      xPagesAllocated_ += numPages;
      MOZ_ASSERT(xPagesAllocated_ <= MaxCodePages);

      // If we allocated a small number of pages, move cursor_ to the
      // next page. We don't do this for larger allocations to avoid
      // skipping a large number of small holes.
      if (numPages <= 2) {
        xCursor_ = page + numPages;
      }

      p = base_ + page * ExecutableCodePageSize;
      break;
    }
    if (!p) {
      return nullptr;
    }
  }

  // Commit the pages after releasing the lock.
  if (!CommitPages(p, bytes, protection)) {
    xDeallocate(p, bytes, /* decommit = */ false);
    return nullptr;
  }

  SetMemCheckKind(p, bytes, checkKind);

  return p;
}

void ProcessJitMemory::xDeallocate(void* addr, size_t bytes, bool decommit) {
  MOZ_ASSERT(initialized());
  MOZ_ASSERT(addr);
  MOZ_ASSERT((uintptr_t(addr) % gc::SystemPageSize()) == 0);
  MOZ_ASSERT(bytes > 0);
  MOZ_ASSERT((bytes % ExecutableCodePageSize) == 0);

  assertCodeValidAddress(addr, bytes);

  size_t firstPage =
      (static_cast<uint8_t*>(addr) - base_) / ExecutableCodePageSize;
  size_t numPages = bytes / ExecutableCodePageSize;

  // Decommit before taking the lock.
  MOZ_MAKE_MEM_NOACCESS(addr, bytes);
  if (decommit) {
    DecommitPages(addr, bytes);
  }

  LockGuard<Mutex> guard(lock_);
  MOZ_ASSERT(numPages <= xPagesAllocated_);
  xPagesAllocated_ -= numPages;

  for (size_t i = 0; i < numPages; i++) {
    xPages_.remove(firstPage + i);
  }

  // Move the cursor back so we can reuse pages instead of fragmenting the
  // whole region.
  if (firstPage < xCursor_) {
    xCursor_ = firstPage;
  }
}

void* ProcessJitMemory::roAllocate(size_t bytes, ProtectionSetting protection,
                                   MemCheckKind checkKind) {
  MOZ_ASSERT(initialized());
  MOZ_ASSERT(HasJitBackend());
  MOZ_ASSERT(bytes > 0);
  MOZ_ASSERT((bytes % ReadOnlyDataPageSize) == 0);

  size_t numPages = bytes / ReadOnlyDataPageSize;

  // Take the lock and try to allocate.
  void* p = nullptr;
  {
    LockGuard<Mutex> guard(lock_);
    MOZ_ASSERT(roPagesAllocated_ <= MaxDataPages);

    // Check if we have enough pages available.
    if (roPagesAllocated_ + numPages >= MaxDataPages) {
      return nullptr;
    }

    MOZ_ASSERT(bytes <= MaxDataBytesPerProcess);

    size_t page = roCursor_;

    for (size_t i = 0; i < MaxDataPages; i++) {
      // Make sure page + numPages - 1 is a valid index.
      if (page + numPages > MaxDataPages) {
        page = 0;
      }

      bool available = true;
      for (size_t j = 0; j < numPages; j++) {
        if (roPages_.contains(page + j)) {
          available = false;
          break;
        }
      }
      if (!available) {
        page++;
        continue;
      }

      // Mark the pages as unavailable.
      for (size_t j = 0; j < numPages; j++) {
        roPages_.insert(page + j);
      }

      roPagesAllocated_ += numPages;
      MOZ_ASSERT(roPagesAllocated_ <= MaxDataPages);

      // If we allocated a small number of pages, move cursor_ to the
      // next page. We don't do this for larger allocations to avoid
      // skipping a large number of small holes.
      if (numPages <= 2) {
        roCursor_ = page + numPages;
      }

      p = base_ + page * ReadOnlyDataPageSize + MaxCodeBytesPerProcess;
      break;
    }
    if (!p) {
      return nullptr;
    }
  }

  // Commit the pages after releasing the lock.
  if (!CommitPages(p, bytes, protection)) {
    roDeallocate(p, bytes, /* decommit = */ false);
    return nullptr;
  }

  SetMemCheckKind(p, bytes, checkKind);

  return p;
}

void ProcessJitMemory::roDeallocate(void* addr, size_t bytes, bool decommit) {
  MOZ_ASSERT(initialized());
  MOZ_ASSERT(addr);
  MOZ_ASSERT((uintptr_t(addr) % gc::SystemPageSize()) == 0);
  MOZ_ASSERT(bytes > 0);
  MOZ_ASSERT((bytes % ExecutableCodePageSize) == 0);

  assertDataValidAddress(addr, bytes);

  uintptr_t roOffset =
      static_cast<uint8_t*>(addr) - base_ - MaxCodeBytesPerProcess;
  size_t firstPage = roOffset / ReadOnlyDataPageSize;
  size_t numPages = bytes / ReadOnlyDataPageSize;

  // Decommit before taking the lock.
  MOZ_MAKE_MEM_NOACCESS(addr, bytes);
  if (decommit) {
    DecommitPages(addr, bytes);
  }

  LockGuard<Mutex> guard(lock_);
  MOZ_ASSERT(numPages <= roPagesAllocated_);
  roPagesAllocated_ -= numPages;

  for (size_t i = 0; i < numPages; i++) {
    roPages_.remove(firstPage + i);
  }

  // Move the cursor back so we can reuse pages instead of fragmenting the
  // whole region.
  if (firstPage < roCursor_) {
    roCursor_ = firstPage;
  }
}

void* js::jit::AllocateExecutableMemory(size_t bytes,
                                        ProtectionSetting protection,
                                        MemCheckKind checkKind) {
  return jitMemory.xAllocate(bytes, protection, checkKind);
}

void js::jit::DeallocateExecutableMemory(void* addr, size_t bytes) {
  jitMemory.xDeallocate(addr, bytes, /* decommit = */ true);
}

void* js::jit::AllocateReadOnlyMemory(size_t bytes,
                                      ProtectionSetting protection,
                                      MemCheckKind checkKind) {
  return jitMemory.roAllocate(bytes, protection, checkKind);
}

void js::jit::DeallocateReadOnlyMemory(void* addr, size_t bytes) {
  jitMemory.roDeallocate(addr, bytes, /* decommit = */ true);
}

bool js::jit::InitProcessJitMemory() { return jitMemory.init(); }

void js::jit::ReleaseProcessJitMemory() { jitMemory.release(); }

size_t js::jit::LikelyAvailableExecutableMemory() {
  // Round down available memory to the closest MB.
  return MaxCodeBytesPerProcess -
         AlignBytes(jitMemory.bytesAllocated(), 0x100000U);
}

bool js::jit::CanLikelyAllocateMoreExecutableMemory() {
  // Use a 8 MB buffer.
  static const size_t BufferSize = 8 * 1024 * 1024;

  MOZ_ASSERT(jitMemory.bytesAllocated() <= MaxCodeBytesPerProcess);

  return jitMemory.bytesAllocated() + BufferSize <= MaxCodeBytesPerProcess;
}

bool js::jit::AddressIsInExecutableMemory(const void* p) {
  return jitMemory.containsAddress(p);
}

bool js::jit::ReprotectRegion(void* start, size_t size,
                              ProtectionSetting protection,
                              MustFlushICache flushICache) {
#if defined(JS_CODEGEN_WASM32)
  return true;
#endif

  // Flush ICache when making code executable, before we modify |size|.
  if (flushICache == MustFlushICache::Yes) {
    MOZ_ASSERT(protection == ProtectionSetting::Executable);
    jit::FlushICache(start, size);
  }

  // Calculate the start of the page containing this region,
  // and account for this extra memory within size.
  size_t pageSize = gc::SystemPageSize();
  intptr_t startPtr = reinterpret_cast<intptr_t>(start);
  intptr_t pageStartPtr = startPtr & ~(pageSize - 1);
  void* pageStart = reinterpret_cast<void*>(pageStartPtr);
  size += (startPtr - pageStartPtr);

  // Round size up
  size += (pageSize - 1);
  size &= ~(pageSize - 1);

  MOZ_ASSERT((uintptr_t(pageStart) % pageSize) == 0);

  jitMemory.assertValidProtection(pageStart, size, protection);

  // On weak memory systems, make sure new code is visible on all cores before
  // addresses of the code are made public.  Now is the latest moment in time
  // when we can do that, and we're assuming that every other thread that has
  // written into the memory that is being reprotected here has synchronized
  // with this thread in such a way that the memory writes have become visible
  // and we therefore only need to execute the fence once here.  See bug 1529933
  // for a longer discussion of why this is both necessary and sufficient.
  //
  // We use the C++ fence here -- and not AtomicOperations::fenceSeqCst() --
  // primarily because ReprotectRegion will be called while we construct our own
  // jitted atomics.  But the C++ fence is sufficient and correct, too.
#ifdef __wasi__
  MOZ_CRASH("NYI FOR WASI.");
#else
  std::atomic_thread_fence(std::memory_order_seq_cst);

  volatile int pkey = pkey = jitMemory.memoryProtectionKey(pageStart, size);
  if (pkey == -1 && !JitOptions.writeProtectCode) {
    return true;
  }

#  ifdef JS_USE_APPLE_FAST_WX
  MOZ_CRASH("writeProtectCode should always be false on Apple Silicon");
#  endif

  // The flags variable is defined as volatile in order to tell the compiler to
  // not optimize away the multiple calls to ProtectionSettingToFlags. If an
  // attacker wants to use this mprotect / VirtualAlloc call, the content of the
  // flags register might be forged and it would have to match the protection
  // settings.
  //
  // Of course this fails if the content of the flags register is cloned...
  // which is unfortunately likely.
  //
  // TODO: Verify the assembly ...
#  ifdef XP_WIN
  volatile DWORD flags = ProtectionSettingToFlags(protection, pkey);
  // This is a essentially a VirtualProtect, but with lighter impact on
  // antivirus analysis. See bug 1823634.
  if (!VirtualAlloc(pageStart, size, MEM_COMMIT, flags)) {
    return false;
  }
#  else
  volatile unsigned flags = ProtectionSettingToFlags(protection, pkey);
  volatile int pkr = ProcessJitMemory::ThreadProtection(protection);
  if (pkey == -1) {
    if (pkey_mprotect(pageStart, size, flags, pkey)) {
      return false;
    }
  } else {
    // Modify user-accessible register (PKRU), without syscalls.
    // This access-right modification is thread-local.
    pkey_set(pkey, pkr);
  }

  // This code is repeated in case an attacker would jump above the previous
  // pkey_mprotect call or above the pkey_set instruction in order to inject
  // corrupted values.
  pkey = jitMemory.memoryProtectionKey(pageStart, size);
  if (pkey != -1) {
    pkr = ProcessJitMemory::ThreadProtection(protection);
    MOZ_RELEASE_ASSERT(pkey_get(pkey) == pkr);
  }
#  endif
#endif  // __wasi__

  MOZ_RELEASE_ASSERT(flags == ProtectionSettingToFlags(protection, pkey));
  jitMemory.assertValidProtection(pageStart, size, protection);
  return true;
}

#ifdef JS_USE_APPLE_FAST_WX
void js::jit::AutoMarkJitCodeWritableForThread::markExecutable(
    bool executable) {
#  if defined(XP_IOS)
  if (executable) {
    be_memory_inline_jit_restrict_rwx_to_rx_with_witness();
  } else {
    be_memory_inline_jit_restrict_rwx_to_rw_with_witness();
  }
#  else
  if (__builtin_available(macOS 11.0, *)) {
    pthread_jit_write_protect_np(executable);
  } else {
    MOZ_CRASH("pthread_jit_write_protect_np must be available");
  }
#  endif
}
#endif

#ifdef DEBUG
static MOZ_THREAD_LOCAL(bool) sMarkingWritable;

void js::jit::AutoMarkJitCodeWritableForThread::checkConstructor() {
  if (!sMarkingWritable.initialized()) {
    sMarkingWritable.infallibleInit();
  }
  MOZ_ASSERT(!sMarkingWritable.get(),
             "AutoMarkJitCodeWritableForThread shouldn't be nested");
  sMarkingWritable.set(true);
}

void js::jit::AutoMarkJitCodeWritableForThread::checkDestructor() {
  MOZ_ASSERT(sMarkingWritable.get());
  sMarkingWritable.set(false);
}
#endif
