# Lab Monitor — Design

**Date:** 2026-08-11
**Status:** Approved for planning

## 1. Overview

Lab Monitor is a cross-platform (Windows + Linux) fleet monitoring system for a lab
network. A central **server** discovers and tracks **client** machines, and verifies
that each one matches an expected configuration.

Two classes of monitoring:

- **Resource monitoring** — CPU load, memory, per-volume disk usage. Always on, needs
  no configuration.
- **Compliance monitoring** — applications and services that must be present or absent,
  and registry values that must hold. Driven entirely by a template distributed from the
  server. **A client with no template monitors resources only.**

The system is built as three C++23 deliverables — a client, a server, and a set of
shared libraries — so that the critical logic is shared, and independently unit tested.

## 2. Scope

### In scope for the first implementation

1. Complete build system: CMake, vcpkg manifest, presets, test harness, CI.
2. All four shared libraries with their public interfaces defined.
3. One **working vertical slice**: a client collects hostname + CPU + memory + disk,
   publishes over DDS, and the server discovers it and displays it live in the fleet view.
4. Client tray application: hidden by default, tray icon, detail window.
5. Server application: fleet view with status ribbon and host detail.
6. Unit tests for `lm_core` (rule evaluation, reconciliation, serialisation) and
   `lm_platform` (via fakes).

### Deliberately stubbed for the first implementation

Interfaces are defined and compile, implementations return empty/`NotApplicable`:

- Registry, service and process probes (`lm_platform` concrete implementations). The
  interfaces exist and are called; they return empty results, and the capability set
  reports them as unavailable, so every compliance rule evaluates to `NotApplicable`.
- The template editor UI and the publish path.
- The client's compliance timer loop is wired up and calls `evaluate`, but with empty
  probe results it always produces an empty report.

### Out of scope entirely

- Historical time-series storage and trend charts.
- Remote remediation (installing/removing software, starting services).
- Authentication, authorisation, and transport encryption. **This system assumes a
  trusted lab network.** See §14.
- Deployment installers and service registration.

## 3. Decisions and rationale

| # | Decision | Rationale |
|---|---|---|
| 1 | **Fast DDS behind an `ITransport` abstraction** | OpenDDS exists in neither vcpkg nor Conan (verified against `conan-center-index`: no `opendds`, `ace` or `tao` recipes), and requires a from-source ACE/TAO build. Fast DDS 3.4.1 is a vcpkg one-liner on the same RTPS wire protocol. The abstraction keeps OpenDDS available as a second backend without touching application code, and enables a fake transport for tests. |
| 2 | **Skeleton + one working vertical slice** | Proves the riskiest integration paths (DDS discovery, Qt tray, cross-platform metrics) before breadth is built on top of them. |
| 3 | **Capability model; unsupported checks report `NotApplicable`** | The registry does not exist on Linux. A Linux host must not show a false failure for a Windows-only rule, nor a misleading pass. |
| 4 | **Named, reusable templates assigned to hosts** | Fleets are role-shaped: most machines share a configuration. Per-host rulesets would duplicate the same rules across every machine. |
| 5 | **Single tray process, user-session scoped** | Simplest to build, debug and deploy. These are lab workstations with users at them; a client shows as offline when nobody is logged in. Splitting into service + tray later requires no change to the shared libraries. |
| 6 | **Four focused libraries** | Keeps the two critical pure functions free of Qt, DDS and syscalls, so their tests are fast and mock-free. |
| 7 | **Qt 5.15.18 rather than Qt 6.10** | `qt5-base 5.15.18` is already built for `x64-windows` in the local vcpkg binary cache (1468 archives / 9.6 GB) from this exact port tree, and matches the sibling `discnet` project. Qt 6.10 is a 1–3 hour cold build. Qt Widgets code is ~99 % source-compatible; `lm_ui` avoids version-specific APIs so migration stays cheap. |
| 8 | **Hand-written `TopicDataType` over FastCDR** | `fastddsgen` is a Java tool and is not a vcpkg port; no JDK is installed. Four topics at roughly 40 lines each is cheaper than adding a Java toolchain dependency to every developer machine and CI job. |

## 4. Repository layout

```
lab_monitor/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── .gitignore  .clang-format  .editorconfig  README.md
├── cmake/
│   ├── LabMonitorWarnings.cmake      # lm_warnings INTERFACE target
│   ├── LabMonitorTesting.cmake       # lm_add_test() helper
│   └── LabMonitorRuntime.cmake       # copy_runtime_dependencies()
├── libs/
│   ├── core/       include/lm/core/       src/  tests/
│   ├── platform/   include/lm/platform/   src/{common,windows,linux}/  tests/
│   ├── transport/  include/lm/transport/  src/  tests/
│   └── ui/         include/lm/ui/         src/  resources/
├── apps/
│   ├── client/
│   └── server/
├── docs/superpowers/specs/
└── .github/workflows/ci.yml
```

**Conventions:** C++23. Include prefix `lm/<lib>/…`. Namespaces `lm::core`,
`lm::platform`, `lm::transport`, `lm::ui`. Types `PascalCase`, functions and variables
`snake_case`, private members `trailing_`. Every target links `lm_warnings`
(`/W4` on MSVC, `-Wall -Wextra -Wpedantic` elsewhere) with
`CMAKE_COMPILE_WARNING_AS_ERROR ON`. These conventions follow the existing `discnet`
project.

## 5. Build system

- **CMake ≥ 3.24**, `CMakePresets.json` schema v3 for maximum compatibility.
- Presets: `windows-debug`, `windows-release`, `linux-debug`, `linux-release`,
  all single-config Ninja, each setting the vcpkg toolchain file.
- **vcpkg manifest mode**, pinned:
  `"builtin-baseline": "4f326c4072038c8624c36a8ba5ed23f616adda53"`.
  This is the commit both local vcpkg checkouts sit on, and the one that produced the
  existing binary cache — so every dependency we share with `discnet` (boost, spdlog,
  gtest, qt5-base) restores from cache rather than rebuilding. Only `fastdds` and
  `fastcdr` are cold, and those build in roughly ten minutes.
- Dependencies: `boost-program-options`, `boost-json`, `fastdds`, `fastcdr`, `spdlog`,
  `gtest`; and under the `gui` feature `qt5-base`, `qt5-svg`.
- **`LM_BUILD_GUI`** (default `ON`). When `OFF`, Qt is dropped from both the CMake build
  and vcpkg feature resolution; `lm_ui` and both applications are excluded, leaving
  `lm_core`, `lm_platform`, `lm_transport` and all of their tests. CI uses this for a
  fast headless job.
- **`LM_BUILD_INTEGRATION_TESTS`** (default `OFF`). Gates tests that open a real DDS
  domain, since those need loopback multicast.
- `copy_runtime_dependencies(target)` (ported from `discnet`) stages Qt and Fast DDS
  DLLs next to the executables on Windows.

## 6. Module design

### 6.1 `lm_core` — domain logic, zero I/O

Depends on Boost and nothing else. No Qt, no DDS, no syscalls. This is where the
critical, heavily-tested logic lives.

Owns: domain types (§7), template model, JSON serialisation via `boost::json`, and the
two pure functions:

```cpp
ComplianceReport evaluate(const TemplateBundle& bundle,
                          const HostFacts&     facts,
                          Capabilities         caps);

FleetView reconcile(const ExpectedHosts&      expected,
                    const DiscoveredClients&  discovered,
                    TimePoint                 now);
```

`evaluate` selects the rules applying to `facts.host_id` (baseline plus assigned
templates, composed by union), evaluates each against the snapshot, and marks any rule
whose required capability is absent from `caps` as `NotApplicable`.

`reconcile` classifies every machine into exactly one state:

| State | Meaning |
|---|---|
| `Online` | In the expected list and currently reporting |
| `Offline` | In the expected list, seen before, silent beyond the liveliness lease |
| `Missing` | In the expected list, never seen |
| `Unexpected` | Reporting, but not in the expected list |

### 6.2 `lm_platform` — OS probes behind interfaces

```cpp
class IResourceProbe { public: virtual ResourceSample sample() = 0; ... };
class IProcessProbe  { public: virtual std::vector<ProcessInfo> enumerate() = 0; ... };
class IServiceProbe  { public: virtual std::vector<ServiceInfo> enumerate() = 0; ... };
class IRegistryProbe { public: virtual RegistryReadResult read(const RegistryPath&) = 0; ... };
```

A `HostProbes` aggregate assembles a `HostFacts` snapshot and reports the `Capabilities`
bitset for the running platform. Windows uses PDH/`GlobalMemoryStatusEx`, the SCM, and
the Win32 registry API. Linux uses `/proc/stat`, `/proc/meminfo` and `statvfs` for
resources, `/proc/<pid>` for processes, and shells out to
`systemctl show <unit> --property=ActiveState,SubState` for services — chosen over
`sd_bus` specifically to avoid a `libsystemd` build dependency.

`IRegistryProbe` has no Linux implementation, and the Linux capability set omits
`Capability::Registry`.

### 6.3 `lm_transport` — messaging

`ITransport` exposes typed publish/subscribe for the four topics (§8) using `lm_core`
DTOs; DDS types never leak past this boundary. Two implementations:
`FastDdsTransport` (production) and `InMemoryTransport` (tests, and a `--offline`
development mode). Connection state is surfaced through a callback.

### 6.4 `lm_ui` — shared Qt widgets

The QSS theme, SVG icon loading and runtime recolouring, status pill widget,
custom-painted sparkline widget, animated bar widget, and the `QAbstractTableModel`
subclasses shared by both applications. Depends on Qt and `lm_core`.

## 7. Domain model

```cpp
enum class RuleKind    { Process, Service, Registry };
enum class Presence    { MustBePresent, MustBeAbsent };
enum class CheckStatus { Pass, Fail, NotApplicable, Error };
enum class Capability  { Resources, Processes, Services, Registry };

struct Rule {
    RuleId                        id;          // stable, generated at creation
    std::string                   description;
    RuleKind                      kind;
    Presence                      expectation;
    RulePayload                   payload;     // variant per kind
    std::optional<VersionConstraint> version;  // Process rules only
};
```

- **Process payload:** executable name; optional version constraint
  (`>=`, `>`, `==`, `<`, `<=` against a dotted version, read from the file version
  resource on Windows and from the package manager on Linux).
- **Service payload:** service name; optional expected state (`Running` / `Stopped`).
- **Registry payload:** hive, key path, value name; optional expected value with a
  comparison mode (`Equals`, `Contains`, `Exists`).

```cpp
struct CheckResult {
    RuleId       rule_id;
    CheckStatus  status;
    std::string  observed;   // human-readable observed value
    std::string  message;    // error detail when status == Error
};
```

`TemplateBundle` contains the baseline template, the named templates, the host→template
assignments, a monotonic `revision` and a content `hash`.

## 8. DDS topics

| Topic | Direction | Reliability | Durability | Key | Notes |
|---|---|---|---|---|---|
| `ClientAnnounce` | client → server | Reliable | TransientLocal | hostname | Identity, capabilities, agent version. Liveliness lease **10 s** |
| `ResourceSample` | client → server | BestEffort | Volatile, KeepLast(1) | hostname | 2 s cadence; stale samples have no value |
| `TemplateBundle` | server → client | Reliable | TransientLocal, KeepLast(1) | — | Late joiners receive the current bundle automatically |
| `ComplianceReport` | client → server | Reliable | TransientLocal | hostname | Last known compliance survives the client going offline |

The client identifier is the **hostname**, as specified.

Offline detection uses the DDS Liveliness QoS lease on `ClientAnnounce` combined with
`on_liveliness_changed`, rather than a hand-rolled heartbeat.

## 9. Template lifecycle

1. The server owns templates, assignments and the expected-host list, persisted as
   `boost::json` under the platform config directory.
2. Edits in the UI mutate a **draft**. The draft is compared against the published
   bundle; Publish is enabled only when they differ, and is labelled with the revision
   it will create.
3. Publishing bumps `revision`, recomputes `hash`, persists, and writes a single
   `TemplateBundle` sample.
4. TransientLocal durability means there is **no push-to-all-clients code path**. DDS
   delivers the sample to every current and future subscriber.
5. Each client filters the bundle locally for the baseline plus templates assigned to
   its hostname, re-evaluates immediately, and publishes a `ComplianceReport` carrying
   `applied_revision`.
6. The server marks any client whose `applied_revision` is behind the current revision
   as **stale**.

An empty bundle is the default state, giving resource-only monitoring.

## 10. Client application

Single Qt Widgets process, hidden at startup, autostarted at login.

- **Tray icon encodes status**: a ring shifting green → amber → red → grey
  (disconnected), tooltip showing hostname and live CPU/disk.
- Left-click opens the detail window; right-click gives Open / Pause reporting /
  Copy diagnostics / Quit. Closing the window **hides** it rather than quitting.
- **Detail window**, three bands: header (hostname, connection pill, applied template
  and revision); live resource strip (CPU sparkline, memory bar, per-volume disk bars);
  compliance grouped into Applications / Services / Registry, with `NotApplicable` rows
  dimmed rather than hidden.
- CLI via `boost::program_options`: `--domain-id`, `--config`, `--offline`, `--log-level`.

## 11. Server application

Single Qt Widgets window.

- **Status ribbon** across the top counting Online / Offline / Missing / Unexpected /
  Non-compliant. **Clicking a counter filters the fleet** — one click answers
  "who is missing?".
- **Host sidebar**, filterable, sorted by severity then name.
- **Centre pane**: live detail for the selected host.
- **Templates tab**: template list, rule editor, host→template assignment view, and the
  Publish control described in §9.
- The expected-host list is editable in the UI (hostname or IP) and persisted alongside
  the templates.

## 12. UI design language

Dark slate background, a single cyan accent, generous spacing, one type scale.

**Status is never conveyed by hue alone.** Each state pairs a colour with a distinct
glyph shape (`✓` ring / `!` triangle / `✕` / `◌` dashed for N/A), so it remains readable
for colour-blind users and in greyscale screenshots. Icons are SVG, recoloured at
runtime, so they stay crisp at any DPI.

**Responsiveness techniques** — these matter more than styling:

- `QAbstractTableModel` + `QSortFilterProxyModel`, emitting `dataChanged` for individual
  cells only. No `QTableWidget`, no model resets. This is the dominant factor at a few
  hundred hosts.
- **Update coalescing**: incoming samples land in a buffer that a 100 ms timer flushes
  into the model, so a burst of DDS traffic cannot thrash the UI.
- `QVariantAnimation` easing on bars and sparklines so values glide rather than snap.
- Sparklines custom-painted in a small `paintEvent` widget rather than pulling in
  QtCharts — faster, and one less dependency.
- Window geometry and splitter state persisted via `QSettings`.

## 13. Threading model

All probing runs on a worker `QThread`. The GUI thread only ever receives completed
snapshots via queued signals, and never touches the registry, the SCM or `/proc`.

- Resource sampling: every **2 s**.
- Compliance evaluation: every **30 s**, or immediately on a template change.

Both intervals are configurable.

## 14. Error handling

- **Probe failures are per-check, never fatal.** A failed registry read yields
  `CheckStatus::Error` carrying the OS error string; every other rule still evaluates.
- **Transport loss** shows as a disconnected badge while sampling continues locally.
  Reconnection is handled by DDS discovery; no manual retry loop.
- **Corrupt configuration** on the server refuses to publish, surfaces the parse error
  in the UI, and keeps the last good bundle in memory.
- Logging via `spdlog` to a rotating file plus console, at a level set by CLI flag.

**Security posture:** this design assumes a trusted lab network. DDS traffic is
unauthenticated and unencrypted, so any host on the domain can publish a
`TemplateBundle` or impersonate a client. This is an accepted limitation of the current
scope; Fast DDS supports a security plugin (authentication, access control, encryption)
that can be enabled later without changing the `ITransport` boundary.

## 15. Testing strategy

`lm_core` carries the real coverage, and needs no mocks — `evaluate` and `reconcile` are
pure functions over plain structs:

- Rule evaluation matrix: `{Process, Service, Registry}` × `{MustBePresent, MustBeAbsent}`
  × `{Pass, Fail, NotApplicable, Error}`.
- Version constraint parsing and comparison, including malformed input.
- `TemplateBundle` JSON round-trip; revision and hash stability across serialisation.
- `reconcile` state machine: every transition between Online / Offline / Missing /
  Unexpected, including liveliness-lease boundary conditions.

`lm_platform` uses probe fakes for logic, plus guarded sanity tests against the real OS
(CPU within 0–100, at least one volume reported, capability set matches the platform).

`lm_transport` round-trips every topic through `InMemoryTransport`. A DDS loopback test
publishing and subscribing within one process is gated behind
`LM_BUILD_INTEGRATION_TESTS`.

## 16. CI

GitHub Actions, `windows-latest` and `ubuntu-latest`:

1. **Headless job** — `LM_BUILD_GUI=OFF`, builds the three non-UI libraries and runs all
   their tests. Fast feedback on every push, and no Qt in the dependency graph.
2. **Full job** — `LM_BUILD_GUI=ON` with vcpkg binary caching via the GitHub Actions
   cache backend.

## 17. Risks

| Risk | Mitigation |
|---|---|
| DDS multicast discovery blocked by network policy or firewall | Fast DDS supports an explicit initial-peers list; expose it as a CLI/config option if discovery fails |
| Qt 5.15 is upstream EOL (KDE-patched in vcpkg) | `lm_ui` avoids version-specific APIs, keeping a Qt 6 migration cheap; revisit once a Qt 6 build has been warmed |
| C++23 support varies across MSVC and GCC | CMake maps `CXX_STANDARD 23` to `/std:c++latest` on MSVC; keep to widely-implemented features and verify both CI legs early |
| Hand-written `TopicDataType` code is error-prone | Round-trip tests for every topic in `lm_transport`; the four types are deliberately small and flat |
| Service enumeration on Linux may need D-Bus | Stubbed behind `IServiceProbe` in the first pass; capability reported honestly until implemented |

## 18. Future work

Historical trends and charting; remote remediation; DDS security plugin; installers and
service registration; splitting the client into a service plus a tray UI; host groups
for template assignment; an OpenDDS backend behind the existing `ITransport`.
