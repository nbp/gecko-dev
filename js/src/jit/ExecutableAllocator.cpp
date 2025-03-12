/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright (C) 2008 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "jit/ExecutableAllocator.h"

#include "gc/GCContext.h"
#include "js/MemoryMetrics.h"
#include "util/Poison.h"

using namespace js::jit;

#ifdef DEBUG
void Executable::assertInvariants() {
  MOZ_ASSERT_IF(xStart, pool);
  MOZ_ASSERT_IF(pool, pool->m_allocation.pages <= (char*) xStart);
  MOZ_ASSERT_IF(pool, (char*) xStart + desc.xSize <= pool->m_freePtr);
  MOZ_ASSERT_IF(roStart, roPool);
  MOZ_ASSERT_IF(roPool, roPool->m_allocation.pages <= (char*) roStart);
  MOZ_ASSERT_IF(roPool, (char*) roStart + desc.roSize <= roPool->m_freePtr);
}
#endif

void Executable::discard(JS::GCContext* gcx) {
  MOZ_ASSERT(xStart);
  assertInvariants();

  // With W^X JIT code, reprotecting memory for each JitCode instance is
  // slow, so we record the ranges and poison them later all at once. It's
  // safe to ignore OOM here, it just means we won't poison the code.
  if (gcx && gcx->appendJitPoisonRange(JitPoisonRange(pool, xStart, desc.xSize))) {
    pool->addRef();
  }

#ifdef JS_ION_PERF
  // Code buffers are stored inside ExecutablePools. Pools are refcounted.
  // Releasing the pool may free it. Horrible hack: if we are using perf
  // integration, we don't want to reuse code addresses, so we just leak the
  // memory instead.
  if (!PerfEnabled()) {
    pool->release(desc);
  }
#else
  pool->release(desc);
#endif

  xStart = nullptr;
  pool = nullptr;

  if (!roPool) {
    return;
  }
  roPool->release(desc);
  roStart = nullptr;
  roPool = nullptr;
}

ExecutablePool::~ExecutablePool() {
#ifdef DEBUG
  for (size_t bytes : m_codeBytes) {
    MOZ_ASSERT(bytes == 0);
  }
#endif

  MOZ_ASSERT(!isMarked());

  m_allocator->releasePoolPages(this);
}

void ExecutablePool::release(bool willDestroy) {
  MOZ_ASSERT(m_refCount != 0);
  MOZ_ASSERT_IF(willDestroy, m_refCount == 1);
  if (--m_refCount == 0) {
    js_delete(this);
  }
}

void ExecutablePool::release(const ExecutableDesc& desc) {
  m_codeBytes[desc.kind] -= desc.xSize;
  MOZ_ASSERT(m_codeBytes[desc.kind] < m_allocation.size);  // Shouldn't underflow.

  release();
}

void ExecutablePool::addRef() {
  // It should be impossible for us to roll over, because only small
  // pools have multiple holders, and they have one holder per chunk
  // of generated code, and they only hold 16KB or so of code.
  MOZ_ASSERT(m_refCount);
  ++m_refCount;
  MOZ_ASSERT(m_refCount, "refcount overflow");
}

Executable ExecutablePool::alloc(const ExecutableDesc& desc) {
  MOZ_ASSERT(desc.xSize <= available());
  void* result = m_freePtr;
  m_freePtr += desc.xSize;
  m_codeBytes[desc.kind] += desc.xSize;
  MOZ_MAKE_MEM_UNDEFINED(result, desc.xSize);
  return Executable(result, nullptr, this, nullptr, desc);
}

size_t ExecutablePool::available() const {
  MOZ_ASSERT(m_end >= m_freePtr);
  return m_end - m_freePtr;
}

ReadOnlyPool::~ReadOnlyPool() {
#ifdef DEBUG
  for (size_t bytes : m_dataBytes) {
    MOZ_ASSERT(bytes == 0);
  }
#endif

  MOZ_ASSERT(!isMarked());

  m_allocator->releasePoolPages(this);
}

void ReadOnlyPool::release(bool willDestroy) {
  MOZ_ASSERT(m_refCount != 0);
  MOZ_ASSERT_IF(willDestroy, m_refCount == 1);
  if (--m_refCount == 0) {
    js_delete(this);
  }
}

void ReadOnlyPool::release(const ExecutableDesc& desc) {
  m_dataBytes[desc.kind] -= desc.roSize;
  MOZ_ASSERT(m_dataBytes[desc.kind] < m_allocation.size);  // Shouldn't underflow.

  release();
}

void ReadOnlyPool::addRef() {
  // It should be impossible for us to roll over, because only small
  // pools have multiple holders, and they have one holder per chunk
  // of generated data, and they only hold 16KB or so of data.
  MOZ_ASSERT(m_refCount);
  ++m_refCount;
  MOZ_ASSERT(m_refCount, "refcount overflow");
}

Executable ReadOnlyPool::alloc(const ExecutableDesc& desc) {
  MOZ_ASSERT(desc.roSize <= available());
  void* result = m_freePtr;
  m_freePtr += desc.roSize;
  m_dataBytes[desc.kind] += desc.roSize;
  MOZ_MAKE_MEM_UNDEFINED(result, desc.roSize);
  return Executable(nullptr, result, nullptr, this, desc);
}

size_t ReadOnlyPool::available() const {
  MOZ_ASSERT(m_end >= m_freePtr);
  return m_end - m_freePtr;
}

ExecutablePoolAllocator::~ExecutablePoolAllocator() {
  for (size_t i = 0; i < smallExecPools.length(); i++) {
    smallExecPools[i]->release(/* willDestroy = */ true);
  }

  for (size_t i = 0; i < smallDataPools.length(); i++) {
    smallDataPools[i]->release(/* willDestroy = */ true);
  }

  // Explicitly clear the previous vectors to avoid danling pointers.
  smallExecPools.clear();
  smallDataPools.clear();

  // If this asserts we have a pool leak.
  MOZ_ASSERT(xPools.empty());
  MOZ_ASSERT(roPools.empty());
}

ExecutablePool* ExecutablePoolAllocator::execPoolForSize(
    const ExecutableDesc& least) {
  // Try to fit in an existing small allocator.  Use the pool with the
  // least available space that is big enough (best-fit).  This is the
  // best strategy because (a) it maximizes the chance of the next
  // allocation fitting in a small pool, and (b) it minimizes the
  // potential waste when a small pool is next abandoned.
  ExecutablePool* minPool = nullptr;
  for (size_t i = 0; i < smallExecPools.length(); i++) {
    ExecutablePool* pool = smallExecPools[i];
    if (least.xSize <= pool->available() &&
        (!minPool || pool->available() < minPool->available())) {
      minPool = pool;
    }
  }
  if (minPool) {
    // Pre-increments for the upcoming allocation.
    minPool->addRef();
    return minPool;
  }

  // If the request is large, we just provide a unshared allocator
  if (least.xSize > ExecutableCodePageSize) {
    return createExecPool(least);
  }

  // Create a new allocator (with a pre-incremented ref-count)
  ExecutablePool* pool = createExecPool({ExecutableCodePageSize, 0, CodeKind::Other});
  if (!pool) {
    return nullptr;
  }
  // At this point, local |pool| is the owner.

  if (smallExecPools.length() < maxSmallPools) {
    // We haven't hit the maximum number of live pools; add the new pool.
    // If append() OOMs, we just return an unshared allocator.
    //
    // Pools referenced by the smallExecPools are removed by
    // ExecutablePoolAllocator::purge.
    if (smallExecPools.append(pool)) {
      pool->addRef();
    }
  } else {
    // Find the pool with the least space.
    int iMin = 0;
    for (size_t i = 1; i < smallExecPools.length(); i++) {
      if (smallExecPools[i]->available() < smallExecPools[iMin]->available()) {
        iMin = i;
      }
    }

    // If the new allocator will result in more free space than the small
    // pool with the least space, then we will use it instead
    ExecutablePool* minPool = smallExecPools[iMin];
    if ((pool->available() - least.xSize) > minPool->available()) {
      minPool->release();
      smallExecPools[iMin] = pool;
      pool->addRef();
    }
  }

  // Pass ownership to the caller.
  return pool;
}

ReadOnlyPool* ExecutablePoolAllocator::dataPoolForSize(
    const ExecutableDesc& least) {
  // Try to fit in an existing small allocator.  Use the pool with the
  // least available space that is big enough (best-fit).  This is the
  // best strategy because (a) it maximizes the chance of the next
  // allocation fitting in a small pool, and (b) it minimizes the
  // potential waste when a small pool is next abandoned.
  ReadOnlyPool* minPool = nullptr;
  for (size_t i = 0; i < smallDataPools.length(); i++) {
    ReadOnlyPool* pool = smallDataPools[i];
    if (least.roSize <= pool->available() &&
        (!minPool || pool->available() < minPool->available())) {
      minPool = pool;
    }
  }
  if (minPool) {
    // Pre-increments for the upcoming allocation.
    minPool->addRef();
    return minPool;
  }

  // If the request is large, we just provide a unshared allocator
  if (least.roSize > ReadOnlyDataPageSize) {
    return createDataPool(least);
  }

  // Create a new allocator (with a pre-incremented ref-count)
  ReadOnlyPool* pool = createDataPool({0, ReadOnlyDataPageSize, CodeKind::Other});
  if (!pool) {
    return nullptr;
  }
  // At this point, local |pool| is the owner.

  if (smallDataPools.length() < maxSmallPools) {
    // We haven't hit the maximum number of live pools; add the new pool.
    // If append() OOMs, we just return an unshared allocator.
    //
    // Pools referenced by the smallExecPools are removed by
    // ExecutablePoolAllocator::purge.
    if (smallDataPools.append(pool)) {
      pool->addRef();
    }
  } else {
    // Find the pool with the least space.
    int iMin = 0;
    for (size_t i = 1; i < smallDataPools.length(); i++) {
      if (smallDataPools[i]->available() < smallDataPools[iMin]->available()) {
        iMin = i;
      }
    }

    // If the new allocator will result in more free space than the small
    // pool with the least space, then we will use it instead
    ReadOnlyPool* minPool = smallDataPools[iMin];
    if ((pool->available() - least.roSize) > minPool->available()) {
      minPool->release();
      smallDataPools[iMin] = pool;
      pool->addRef();
    }
  }

  // Pass ownership to the caller.
  return pool;
}

static const size_t OVERSIZE_ALLOCATION = size_t(-1);
static size_t roundUpAllocationSize(size_t request, size_t granularity) {
  if ((std::numeric_limits<size_t>::max() - granularity) <= request) {
    return OVERSIZE_ALLOCATION;
  }

  // Round up to next page boundary
  size_t size = request + (granularity - 1);
  size = size & ~(granularity - 1);
  MOZ_ASSERT(size >= request);
  return size;
}

ExecutablePool* ExecutablePoolAllocator::createExecPool(
    const ExecutableDesc& least) {
  size_t allocSize = roundUpAllocationSize(least.xSize, ExecutableCodePageSize);
  if (allocSize == OVERSIZE_ALLOCATION) {
    return nullptr;
  }

  ExecutablePool::Allocation a = systemExecAlloc(allocSize);
  if (!a.pages) {
    return nullptr;
  }

  ExecutablePool* pool = js_new<ExecutablePool>(this, a);
  if (!pool) {
    systemExecRelease(a);
    return nullptr;
  }

  if (!xPools.put(pool)) {
    // Note: this will call |systemExecRelease(a)|.
    js_delete(pool);
    return nullptr;
  }

  return pool;
}

ReadOnlyPool* ExecutablePoolAllocator::createDataPool(
    const ExecutableDesc& least) {
  size_t allocSize = roundUpAllocationSize(least.roSize, ReadOnlyDataPageSize);
  if (allocSize == OVERSIZE_ALLOCATION) {
    return nullptr;
  }

  ReadOnlyPool::Allocation a = systemDataAlloc(allocSize);
  if (!a.pages) {
    return nullptr;
  }

  ReadOnlyPool* pool = js_new<ReadOnlyPool>(this, a);
  if (!pool) {
    systemDataRelease(a);
    return nullptr;
  }

  if (!roPools.put(pool)) {
    // Note: this will call |systemDataRelease(a)|.
    js_delete(pool);
    return nullptr;
  }

  return pool;
}

Executable ExecutableAllocator::alloc(JSContext* cx, const ExecutableDesc& desc) {
  // Caller must ensure 'n' is word-size aligned. If all allocations are
  // of word sized quantities, then all subsequent allocations will be
  // aligned.
  MOZ_ASSERT(roundUpAllocationSize(desc.xSize, sizeof(void*)) == desc.xSize);
  MOZ_ASSERT(desc.kind < CodeKind::Count);

  if (desc.xSize == uint32_t(OVERSIZE_ALLOCATION)) {
    return Executable(nullptr);
  }

  // Find or allocate an ExecutablePool which can host the requested allocation.
  ExecutablePool* execPool = poolAlloc.execPoolForSize(desc);
  if (!execPool) {
    return Executable(nullptr);
  }

  if (!desc.roSize) {
    // This alloc is infallible because poolForSize() just obtained
    // (found, or created if necessary) a pool that had enough space.
    Executable result(execPool->alloc(desc));
    MOZ_RELEASE_ASSERT(result);

    return Executable(std::move(result));
  }

  // Find or allocate an ExecutablePool which can host the requested allocation.
  ReadOnlyPool* dataPool = poolAlloc.dataPoolForSize(desc);
  if (!dataPool) {
    // Failure to allocate data pages should remove the reference we implicitly
    // hold on the ExecutablePool.
    execPool->release();
    return Executable(nullptr);
  }

  // This alloc is infallible because poolForSize() just obtained
  // (found, or created if necessary) a pool that had enough space.
  Executable result(execPool->alloc(desc), dataPool->alloc(desc));
  MOZ_RELEASE_ASSERT(result);

  return Executable(std::move(result));
}

void ExecutablePoolAllocator::releasePoolPages(ExecutablePool* pool) {
  MOZ_ASSERT(pool->m_allocation.pages);
  systemExecRelease(pool->m_allocation);

  // Pool may not be present in m_pools if we hit OOM during creation.
  if (auto ptr = xPools.lookup(pool)) {
    xPools.remove(ptr);
  }
}

void ExecutablePoolAllocator::releasePoolPages(ReadOnlyPool* pool) {
  MOZ_ASSERT(pool->m_allocation.pages);
  systemDataRelease(pool->m_allocation);

  // Pool may not be present in m_pools if we hit OOM during creation.
  if (auto ptr = roPools.lookup(pool)) {
    roPools.remove(ptr);
  }
}

void ExecutablePoolAllocator::purge() {
  for (size_t i = 0; i < smallExecPools.length();) {
    ExecutablePool* pool = smallExecPools[i];
    if (pool->m_refCount > 1) {
      // Releasing this pool is not going to deallocate it, so we might as
      // well hold on to it and reuse it for future allocations.
      i++;
      continue;
    }

    MOZ_ASSERT(pool->m_refCount == 1);
    pool->release();
    smallExecPools.erase(&smallExecPools[i]);
  }

  for (size_t i = 0; i < smallDataPools.length();) {
    ReadOnlyPool* pool = smallDataPools[i];
    if (pool->m_refCount > 1) {
      // Releasing this pool is not going to deallocate it, so we might as
      // well hold on to it and reuse it for future allocations.
      i++;
      continue;
    }

    MOZ_ASSERT(pool->m_refCount == 1);
    pool->release();
    smallDataPools.erase(&smallDataPools[i]);
  }
}

void ExecutablePoolAllocator::addSizeOfCode(JS::CodeSizes* sizes) const {
  for (ExecPoolHashSet::Range r = xPools.all(); !r.empty(); r.popFront()) {
    ExecutablePool* pool = r.front();
    sizes->ion += pool->m_codeBytes[CodeKind::Ion];
    sizes->baseline += pool->m_codeBytes[CodeKind::Baseline];
    sizes->regexp += pool->m_codeBytes[CodeKind::RegExp];
    sizes->other += pool->m_codeBytes[CodeKind::Other];
    sizes->unused += pool->m_allocation.size - pool->usedCodeBytes();
  }

  for (DataPoolHashSet::Range r = roPools.all(); !r.empty(); r.popFront()) {
    ReadOnlyPool* pool = r.front();
    sizes->ion += pool->m_dataBytes[CodeKind::Ion];
    sizes->baseline += pool->m_dataBytes[CodeKind::Baseline];
    sizes->regexp += pool->m_dataBytes[CodeKind::RegExp];
    sizes->other += pool->m_dataBytes[CodeKind::Other];
    sizes->unused += pool->m_allocation.size - pool->usedDataBytes();
  }
}

/* static */
void ExecutableAllocator::reprotectPool(JSRuntime* rt, ExecutablePool* pool,
                                        ProtectionSetting protection,
                                        MustFlushICache flushICache) {
  char* start = pool->m_allocation.pages;
  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!ReprotectRegion(start, pool->m_freePtr - start, protection,
                       flushICache)) {
    oomUnsafe.crash("ExecutableAllocator::reprotectPool");
  }
}

/* static */
void ExecutableAllocator::poisonCode(JSRuntime* rt,
                                     JitPoisonRangeVector& ranges) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));

#ifdef DEBUG
  // Make sure no pools have the mark bit set.
  for (size_t i = 0; i < ranges.length(); i++) {
    MOZ_ASSERT(!ranges[i].pool->isMarked());
  }
#endif

  {
    AutoMarkJitCodeWritableForThread writable;

    for (size_t i = 0; i < ranges.length(); i++) {
      ExecutablePool* pool = ranges[i].pool;
      if (pool->m_refCount == 1) {
        // This is the last reference so the release() call below will
        // unmap the memory. Don't bother poisoning it.
        continue;
      }

      MOZ_ASSERT(pool->m_refCount > 1);

      // Use the pool's mark bit to indicate we made the pool writable.
      // This avoids reprotecting a pool multiple times.
      if (!pool->isMarked()) {
        reprotectPool(rt, pool, ProtectionSetting::Writable,
                      MustFlushICache::No);
        pool->mark();
      }

      // Note: we use memset instead of js::Poison because we want to poison
      // JIT code in release builds too. Furthermore, we don't want the
      // invalid-ObjectValue poisoning js::Poison does in debug builds.
      memset(ranges[i].start, JS_SWEPT_CODE_PATTERN, ranges[i].size);
      MOZ_MAKE_MEM_NOACCESS(ranges[i].start, ranges[i].size);
    }
  }

  // Make the pools executable again and drop references. We don't flush the
  // ICache here to not add extra overhead.
  for (size_t i = 0; i < ranges.length(); i++) {
    ExecutablePool* pool = ranges[i].pool;
    if (pool->isMarked()) {
      reprotectPool(rt, pool, ProtectionSetting::Executable,
                    MustFlushICache::No);
      pool->unmark();
    }
    pool->release();
  }
}

ExecutablePool::Allocation ExecutablePoolAllocator::systemExecAlloc(size_t n) {
  void* allocation = AllocateExecutableMemory(n, ProtectionSetting::Executable,
                                              MemCheckKind::MakeNoAccess);
  ExecutablePool::Allocation alloc = {reinterpret_cast<char*>(allocation), n};
  return alloc;
}

void ExecutablePoolAllocator::systemExecRelease(
    const ExecutablePool::Allocation& alloc) {
  DeallocateExecutableMemory(alloc.pages, alloc.size);
}

ReadOnlyPool::Allocation ExecutablePoolAllocator::systemDataAlloc(size_t n) {
  void* allocation = AllocateReadOnlyMemory(n, ProtectionSetting::Writable,
                                            MemCheckKind::MakeNoAccess);
  ReadOnlyPool::Allocation alloc = {reinterpret_cast<char*>(allocation), n};
  return alloc;
}

void ExecutablePoolAllocator::systemDataRelease(
    const ReadOnlyPool::Allocation& alloc) {
  DeallocateReadOnlyMemory(alloc.pages, alloc.size);
}
