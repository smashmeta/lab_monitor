# lab_monitor

Cross-platform (Windows + Linux) C++23 fleet monitoring. A central **server**
discovers **client** machines over DDS, reconciles them against an expected-host
list, and distributes a **template** of compliance rules; clients report resource
usage and rule compliance back.

Design spec: `docs/superpowers/specs/2026-08-11-lab-monitor-design.md`
Implementation plan: `docs/superpowers/plans/2026-08-11-lab-monitor-foundation.md`

## Building — read this first

**The CMake on PATH is 3.24.2 and is too old.** It predates Visual Studio 2026,
so it has no `Visual Studio 18 2026` generator and cannot satisfy this project's
`cmake_minimum_required(VERSION 3.28)`. Always use the CMake that ships with VS:

```
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --preset windows
"C:\Program Files\...\CMake\bin\cmake.exe" --build --preset windows-debug
"C:\Program Files\...\CMake\bin\ctest.exe"  --preset windows-debug
```

`.vscode/settings.json` points the CMake Tools extension at that binary. If a
configure fails with *"Could not create named generator Visual Studio 18 2026"*,
the wrong CMake is being used.

Adding a source file or subdirectory requires re-running configure, not just build.

### Presets

| Preset | Purpose |
|---|---|
| `windows` / `windows-headless` | Local development. Generator `Visual Studio 18 2026`. |
| `windows-ci` / `windows-headless-ci` | CI only. Generator `Visual Studio 17 2022`, because GitHub's `windows-latest` has no VS 2026. Do not use locally. |
| `linux-debug` / `linux-headless` | Ninja. Never compiled — see Known gaps. |

`*-headless` sets `LM_BUILD_GUI=OFF`, dropping Qt, `lm_ui` and both apps.

## Architecture

| Target | Responsibility | Tests |
|---|---|---|
| `lm_core` | Pure domain logic: rules, templates, JSON, `evaluate()`, `reconcile()`, `ClientRegistry` | 84 |
| `lm_platform` | OS probes behind interfaces, plus public fakes in `fakes.hpp` | 36 |
| `lm_transport` | `IClientTransport`/`IServerTransport`, FastCDR codecs, in-memory bus, Fast DDS backend | 23 |
| `lm_ui` | Shared Qt5 widgets, theme, `FleetModel`, `SampleCoalescer`, `RuleDetail` | 34 + 3 |
| `lab_monitor_client` | Hidden tray app; worker thread samples and publishes | 8 |
| `lab_monitor_server` | Fleet console; discovery, reconciliation, template publishing | 11 |

**199 unit tests**, plus 4 Fast DDS loopback integration tests gated behind
`LM_BUILD_INTEGRATION_TESTS` (default OFF — they need loopback multicast).

`lm_ui`'s second figure is `lm_ui_render_tests`, a separate binary because it
paints real widgets and so needs a `QApplication` and a platform plugin, where
every other `lm_ui` test runs under a `QCoreApplication`. It renders a
`QTableView` through the real stylesheet and inspects the pixels — the only way
to catch a QSS rule silently overriding what the model or a delegate asked for.

**`lm_core` depends on `nlohmann-json` and nothing else.** No Qt, no DDS, no
syscalls, no Boost. This is load-bearing: it is what makes `evaluate()` and
`reconcile()` testable without mocks. Adding any other dependency to `libs/core`
is a design violation.

Conventions: C++23, types `PascalCase`, functions `snake_case`, private members
`trailing_`, include prefix `lm/<lib>/…`, warnings-as-errors (`/W4`).

## Deviations from the original design — do not "tidy" these away

Each of these exists because something did not work as expected. The rationale
matters more than the code.

### Fast DDS instead of OpenDDS
The spec asked for OpenDDS. It is in **neither vcpkg nor Conan** (verified against
`conan-center-index`: no `opendds`, `ace` or `tao` recipes) and needs a from-source
ACE/TAO build. Fast DDS 3.4.1 is a vcpkg one-liner on the same RTPS wire protocol.
Everything sits behind `ITransport`, so OpenDDS remains addable as a second backend.

### Qt 5.15.18 instead of Qt 6
Chosen because `qt5-base 5.15.18` was already in the local vcpkg binary cache from
the sibling `discnet` project, restoring in **9 seconds** versus a 1–3 hour cold Qt 6
build. Qt 5.15 is upstream EOL (vcpkg carries KDE's patched branch). `lm_ui` avoids
version-specific APIs so the Qt 6 move stays cheap. **An upgrade is planned, not
merely possible.**

### vcpkg overlay port forcing gtest to static linkage
`vcpkg-overlays/ports/gtest/` — see its README.

At the pinned baseline, vcpkg builds gtest shared. googletest's CMake compiles
`gtest_main` with `GTEST_CREATE_SHARED_LIBRARY=1` while it also inherits
`GTEST_LINKED_AS_SHARED_LIBRARY=1`, and `gtest.h` checks CREATE first — so
`gtest_main.dll` re-exports its own copy of the `UnitTest` singleton. Tests
registered into `gtest.dll` were invisible to it: **every test binary reported
success while running zero tests.**

Static linkage removes the DLL boundary. `lm_add_test()` also sets
`FAIL_REGULAR_EXPRESSION "Running 0 tests from 0 test suites"` as a second
backstop. Deleting either brings the silent failure back.

### Fast DDS `max_serialized_type_size` seed
`libs/transport/src/fastdds/topic_data_type.hpp`. Fast DDS 3.4.1 crashes with an
access violation when given `max_serialized_type_size == 0` — which its own header
documents as the convention for unbounded types — because `TopicPayloadPool::get()`
returns a null pool that is then dereferenced. Worked around by seeding a genuine
lower bound and setting `DYNAMIC_RESERVE_MEMORY_MODE`, which sizes allocations at
message arrival rather than treating the seed as a cap. Re-check on any Fast DDS
upgrade.

### Hand-written FastCDR codecs
`fastddsgen` is a Java tool, is not a vcpkg port, and no JDK is installed. The four
topics are small and flat, so `libs/transport/src/codec.cpp` encodes them by hand.
`decode()` is hardened against hostile input: element counts read off the wire drive
a **capped** `reserve` rather than a `resize`, and both `fastcdr::exception::Exception`
(which does **not** derive from `std::exception`) and `std::exception` are caught.

### `Rule` has no `kind` field
The spec gave `Rule` both a `kind` and a payload — two sources of truth that can
disagree. The kind is derived from the payload variant via `kind_of()`.

### Template assignment implies expected host
`ServerController::effective_expected_hosts()` unions the explicit expected-host
list with every host named in a template assignment. Without this, assigning a
template to a machine left it sitting in **Unexpected** forever, since only the
explicit list reached `reconcile()`. Explicit entries win so a typed address is
never dropped.

### Client recovers rule descriptions locally
`core::CheckResult` travels the wire carrying only a rule id, status, observed
value and message — never the description. Rather than widen the wire format,
`MonitorWorker` recovers display fields from the `TemplateBundle` it already holds
and emits them alongside the report (`apps/client/rule_detail.hpp`).

### nlohmann-json, not boost-json
Team choice. Boost is consequently confined to `program_options` in the two apps.

## Gotchas that have already cost time

- **Qt AUTOMOC misses `Q_OBJECT` headers** when headers live in `include/` and
  sources in `src/`. List such headers explicitly as `add_library` sources.
- **moc records a signal's parameter type as the unqualified literal it sees.**
  `qRegisterMetaType<T>()` registers the fully-qualified name, so the two can
  silently disagree and `QSignalSpy` then captures **empty** payloads — tests pass
  while verifying nothing. Register with the explicit name string when the signal
  is declared inside a namespace.
- **`connect()`'s 4-argument overload runs the functor on the *context object's*
  thread.** Passing a worker as context made `thread->wait()` a self-wait, which Qt
  detects, warns about, and skips — so the join silently never happened.
- **A stylesheet does not reach everything.** Message boxes and palette-drawn
  widgets ignore QSS; `Theme::apply()` installs Fusion and a dark `QPalette` too.
  An unstyled widget class falls back to the platform *light* style.
- **QSS beats an item delegate, and does it last.** With a stylesheet active,
  `QStyleSheetStyle` draws `CE_ItemViewItem` and feeds the `::item` rule's own
  `color` to `QRenderRule::configurePalette()` as `QPalette::HighlightedText` —
  *after* `initStyleOption()` has run. So `color:` on `QTableView::item:selected`
  silently undid `KeepForegroundDelegate` and turned every selected fleet row
  white. Selection styling in `theme.qss` is background-only, deliberately.
  `lm_ui_render_tests` pins this: it is not reproducible in a non-painting test.
- **DDS `TRANSIENT_LOCAL` durability lives on the DataWriter, not on disk.** A
  restarted server has written nothing, which is why `start()` re-announces the
  published bundle at its existing revision (never bumping it).
- **Include hygiene**: the Linux leg compiles with libstdc++, far stricter than
  MSVC's STL about transitive includes. Include what you use, directly.

## Known gaps

- **The Linux code paths have never been compiled.** Everything has been built with
  MSVC. CI is authored but has never run; expect the first Linux job to fail on GCC
  warnings under `-Wall -Wextra -Wpedantic -Wshadow` with warnings-as-errors.
- **Service checks are stubbed** on both platforms — `IServiceProbe` has no
  implementation, so service rules report `NotApplicable`. Process and registry
  probes are implemented on **Windows only**; all three Linux probes beyond
  resources are stubbed.
- The compliance list groups by status rather than Applications/Services/Registry.
- `FastDdsLoopback.ResourceSamplesReachTheServer` was de-flaked by publishing in a
  loop; the topic is BEST_EFFORT so a single publish races discovery.

## Running

```
build\windows\apps\server\Debug\lab_monitor_server.exe
build\windows\apps\client\Debug\lab_monitor_client.exe
```

Both accept `--domain-id`, `--config`, `--offline` (in-process transport, no DDS)
and `--log-level`. The client **starts hidden** — look for the tray icon.
`.vscode/launch.json` has configurations for both apps and every test binary,
including a compound that starts server and client together.
