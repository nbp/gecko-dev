#include "gdb-tests.h"

#include "jit/ExecutableAllocator.h"
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
  Executable exec(execAlloc.alloc(cx, ExecutableDesc{16 * 1024, CodeKind::Baseline}));

  breakpoint();

  use(exec);
  use(execAlloc);
}

FRAGMENT(ExecutableAllocator, twopools) {
  using namespace js::jit;
  const size_t INIT_ALLOC_SIZE = 16 * 1024;
  const size_t ALLOC_SIZE = 32 * 1024;
  const ExecutableDesc baselineAlloc{INIT_ALLOC_SIZE, CodeKind::Baseline};
  const ExecutableDesc ionAlloc{ALLOC_SIZE, CodeKind::Ion};

  ExecutableAllocator execAlloc;
  size_t allocated = 0;

  Executable xInit(execAlloc.alloc(cx, baselineAlloc));
  Executable xAlloc(execAlloc.alloc(cx, ionAlloc));
  allocated += ALLOC_SIZE;

  while (true) {  // Keep allocating until we get a second pool.
    if (xInit.pool != xAlloc.pool)
      break;
    // This should not appear in our code base... And there is no reason to add
    // an operator= for replacing the content only for the test case.
    new (&xAlloc) Executable(execAlloc.alloc(cx, ionAlloc));
    allocated += ALLOC_SIZE;
  };

  breakpoint();

  xInit.pool->release(INIT_ALLOC_SIZE, CodeKind::Baseline);
  xInit.pool->release(allocated - INIT_ALLOC_SIZE, CodeKind::Ion);
  xAlloc.pool->release(ALLOC_SIZE, CodeKind::Ion);
}
