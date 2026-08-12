# lab_monitor

A client/server PC-monitoring system for lab workstations: a server publishes a
ruleset (per-host checks against processes, services, and registry state) and
clients evaluate it locally, reporting status over DDS.

This repository is a greenfield C++23 project built with CMake and vcpkg in
manifest mode.

## Prerequisites

- **CMake ≥ 3.28.** The `cmake` that ships on `PATH` on this machine may be
  older than that (Visual Studio 2026 support requires a newer CMake). Use
  the CMake bundled with Visual Studio 2026 instead:

  ```
  C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
  ```

  Either add that directory to the front of `PATH` for this shell, or invoke
  `cmake.exe` by its full path as shown in the examples below.

- **Visual Studio Community 2026 (v18)** with the MSVC C++ toolset, for the
  `Visual Studio 18 2026` generator used by the Windows presets.

- **vcpkg**, with the environment variable `VCPKG_ROOT` set to its root
  directory (e.g. `C:\path\to\vcpkg`). Dependencies are declared in
  [`vcpkg.json`](vcpkg.json) and are restored automatically during CMake
  configure — no manual `vcpkg install` step is required.

- **Ninja**, on Linux only (the Linux presets use the Ninja generator).

## Presets

Four configure presets are defined in [`CMakePresets.json`](CMakePresets.json):

| Preset             | Platform | Generator            | GUI (Qt) | Notes                          |
|---------------------|----------|-----------------------|----------|---------------------------------|
| `windows`            | Windows  | Visual Studio 18 2026 | On       | Multi-config; pass `--config`   |
| `windows-headless`   | Windows  | Visual Studio 18 2026 | Off      | No Qt in the dependency graph   |
| `linux-debug`        | Linux    | Ninja                 | On       | Single-config, `Debug`          |
| `linux-headless`     | Linux    | Ninja                 | Off      | No Qt in the dependency graph   |

The Windows presets use the multi-config Visual Studio generator (there is no
`ninja` on `PATH` on this machine), so the corresponding build and test
presets must specify `--config`/`configuration`.

## Configuring, building, and testing

Windows (Debug):

```bash
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --preset windows
cmake --build --preset windows-debug
ctest --preset windows-debug
```

(Substitute the full `cmake.exe` path above wherever `cmake`/`ctest` are
invoked, unless that directory has been added to `PATH`.)

Windows, headless (no GUI dependencies):

```bash
cmake --preset windows-headless
cmake --build --preset windows-headless
```

Linux (Debug):

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

## Build options

- `LM_BUILD_GUI` (default `ON`) — build the Qt GUI applications. The
  `*-headless` presets set this `OFF` and also disable vcpkg's default
  features (`VCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON`), so Qt is never
  restored or built for those presets.
- `LM_BUILD_INTEGRATION_TESTS` (default `OFF`) — build tests that open a
  real DDS domain (needs loopback multicast).

## Repository layout

- `cmake/` — shared CMake modules: `LabMonitorWarnings.cmake`
  (`lm_warnings` target), `LabMonitorRuntime.cmake`
  (`copy_runtime_dependencies()`), `LabMonitorTesting.cmake`
  (`lm_add_test()`).
- `libs/core/` — `lm_core`, the platform-independent core library (ids,
  enums, `lm::core::Capabilities`).

Later tasks add `libs/platform`, `libs/transport`, `libs/ui`, and the
`apps/client` / `apps/server` executables.
