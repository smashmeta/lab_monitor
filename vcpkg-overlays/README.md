# vcpkg-overlays

## ports/gtest -- forces static linkage

**Do not remove this overlay or "simplify" it away to the upstream vcpkg
gtest port.** It is a workaround for a real, silent-failure defect, not
leftover scaffolding.

### The defect

Google's vcpkg gtest port at this project's pinned baseline builds
`gtest_main` as a DLL that re-exports its own, separate copy of the
googletest `UnitTest` registry. `TEST(...)` cases in a test binary link
against `gtest.dll` and correctly register themselves into *its* registry,
but `gtest_main.dll`'s bundled `main()` reads its *own* private registry
across that DLL boundary and finds nothing there. The result: every
`lm_add_test()` binary printed `Running 0 tests from 0 test suites`, ran
zero assertions, and exited `0` -- which `ctest` happily reported as 100%
passing. Every test in this repository was a silent no-op until this was
found and fixed.

### The fix

This overlay port (`portfile.cmake`, same upstream source, version, and
patches as the pinned vcpkg baseline) forces `VCPKG_LIBRARY_LINKAGE
static`, so `gtest` and `gtest_main` are both compiled into the test
binary directly. There is then a single `UnitTest` registry and no DLL
boundary for `gtest_main`'s `main()` to fail to see across.
`VCPKG_OVERLAY_PORTS` in `CMakePresets.json` points every configure preset
at `vcpkg-overlays/ports`, so this shadows the upstream `gtest` port
automatically -- no changes are needed anywhere a target calls
`lm_add_test()`.

### How to tell if it regresses

Two independent guards exist, in case this overlay is ever accidentally
dropped (e.g. during a vcpkg baseline bump that changes the port
directory layout) or a future gtest upgrade reintroduces the same DLL
registry split:

1. `cmake/LabMonitorTesting.cmake`'s `lm_add_test()` sets
   `FAIL_REGULAR_EXPRESSION "Running 0 tests from 0 test suites"` on every
   test target it defines, so ctest fails loudly instead of reporting
   false success if a binary ever regresses to registering zero tests.
2. More directly: run any test binary manually (e.g.
   `build/windows/libs/core/Debug/lm_core_tests.exe`) and confirm the
   `[==========] Running N tests from M test suites.` banner reports a
   nonzero N. The baseline counts as of the last full review were
   `lm_core_tests` 82, `lm_platform_tests` 17, `lm_transport_tests` 23,
   `lm_ui_tests` 14, `lab_monitor_client_tests` 7 -- if a rebuild after
   touching vcpkg configuration ever reports drastically fewer tests
   discovered (especially 0), suspect this overlay first.
