#include "gdb-tests.h"

#include "jit/ExecutableAllocator.h"
#include "js/Vector.h"
#include "vm/JSContext.h"

FRAGMENT(ExecutableAllocator, empty) {
  using namespace js::jit;
  ExecutableAllocator execAlloc;

  breakpoint();

  use(execAlloc);
}

FRAGMENT(ExecutableAllocator, onepool) {
  using namespace js::jit;
  ExecutableAllocator execAlloc;
  Executable exec(execAlloc.alloc(cx, ExecutableDesc{16 * 1024, 0, CodeKind::Baseline}));

  breakpoint();

  exec.discard(nullptr);
  use(execAlloc);
}

FRAGMENT(ExecutableAllocator, twopools) {
  using namespace js::jit;
  const size_t INIT_ALLOC_SIZE = 16 * 1024;
  const size_t ALLOC_SIZE = 32 * 1024;
  const ExecutableDesc baselineAlloc{INIT_ALLOC_SIZE, 0, CodeKind::Baseline};
  const ExecutableDesc ionAlloc{ALLOC_SIZE, 0, CodeKind::Ion};

  ExecutableAllocator execAlloc;
  js::Vector<Executable> allocated(cx);

  Executable xInit(execAlloc.alloc(cx, baselineAlloc));
  if (!allocated.append(std::move(xInit))) {
    return;
  }

  // Keep allocating until we get a second pool.
  while (allocated[0].pool == allocated.back().pool) {
    Executable xAlloc(execAlloc.alloc(cx, ionAlloc));
    if (!allocated.append(std::move(xInit))) {
      return;
    }
  };

  breakpoint();

  for (Executable& exec : allocated) {
    exec.discard(nullptr);
  }
  allocated.clear();
}
