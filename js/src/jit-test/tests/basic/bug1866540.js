// |jit-test| skip-if: helperThreadCount() === 0

// On x86, MaxCodePages is 2240. Because we sometimes leave a one-page
// gap, this will guarantee there are no free two-page chunks.
//
// While the above is valid, when we allocate data pages, we might waste a lot
// of unused page content for each new context.
for (var i = 0; i < 220; i++) {
  evalcx("function s(){}", evalcx('lazy'));
}

// Allocating trampolines for the JitRuntime requires two pages.
evalInWorker("");
