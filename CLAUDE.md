# lab_monitor

Cross-platform (Windows + Linux) C++23 fleet monitoring. A central **server**
discovers **client** machines over DDS, reconciles them against an expected-host
list, and distributes a **template** of compliance rules; clients report resource
usage and rule compliance back.

Design spec: `docs/superpowers/specs/2026-08-11-lab-monitor-design.md`
Implementation plan: `docs/superpowers/plans/2026-08-11-lab-monitor-foundation.md`

## Building â€” read this first

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
| `linux-debug` / `linux-headless` | Ninja. Never compiled â€” see Known gaps. |

`*-headless` sets `LM_BUILD_GUI=OFF`, dropping Qt, `lm_ui` and both apps.

## Architecture

| Target | Responsibility | Tests |
|---|---|---|
| `lm_core` | Pure domain logic: rules, templates, JSON, `evaluate()`, `reconcile()`, `ClientRegistry` | 97 |
| `lm_platform` | OS probes behind interfaces, plus public fakes in `fakes.hpp` | 51 |
| `lm_transport` | `IClientTransport`/`IServerTransport`, FastCDR codecs, in-memory bus, Fast DDS backend | 27 |
| `lm_ui` | Shared Qt5 widgets, theme, `FleetModel`, `SampleCoalescer`, `RuleDetail`, `TokenEdit`, `AdapterList` | 47 + 32 |
| `lab_monitor_client` | Hidden tray app; worker thread samples and publishes | 16 |
| `lab_monitor_server` | Fleet console; discovery, reconciliation, template publishing | 22 |

**292 unit tests**, plus 4 Fast DDS loopback integration tests gated behind
`LM_BUILD_INTEGRATION_TESTS` (default OFF â€” they need loopback multicast).

`lm_ui`'s second figure is `lm_ui_widget_tests`, a separate binary because it
constructs and paints real widgets and so needs a `QApplication` and a platform
plugin, where every other `lm_ui` test runs under a `QCoreApplication`. Some of
its cases render through the real stylesheet and inspect the pixels
(`tests/pixel_probe.hpp`) â€” the only way to catch a QSS rule silently overriding
what the model, a delegate or a dynamic property asked for. It cannot use
`QT_QPA_PLATFORM=offscreen` on Windows: vcpkg's applocal deployment copies only
`qwindows`, so the binary would abort before `main()`.

**`lm_core` depends on `nlohmann-json` and nothing else.** No Qt, no DDS, no
syscalls, no Boost. This is load-bearing: it is what makes `evaluate()` and
`reconcile()` testable without mocks. Adding any other dependency to `libs/core`
is a design violation.

Conventions: C++23, types `PascalCase`, functions `snake_case`, private members
`trailing_`, include prefix `lm/<lib>/â€¦`, warnings-as-errors (`/W4`).

## Deviations from the original design â€” do not "tidy" these away

Each of these exists because something did not work as expected. The rationale
matters more than the code.

### Fast DDS instead of OpenDDS
The spec asked for OpenDDS. It is in **neither vcpkg nor Conan** (verified against
`conan-center-index`: no `opendds`, `ace` or `tao` recipes) and needs a from-source
ACE/TAO build. Fast DDS 3.4.1 is a vcpkg one-liner on the same RTPS wire protocol.
Everything sits behind `ITransport`, so OpenDDS remains addable as a second backend.

### Qt 5.15.18 instead of Qt 6
Chosen because `qt5-base 5.15.18` was already in the local vcpkg binary cache from
the sibling `discnet` project, restoring in **9 seconds** versus a 1â€“3 hour cold Qt 6
build. Qt 5.15 is upstream EOL (vcpkg carries KDE's patched branch). `lm_ui` avoids
version-specific APIs so the Qt 6 move stays cheap. **An upgrade is planned, not
merely possible.**

### vcpkg overlay port forcing gtest to static linkage
`vcpkg-overlays/ports/gtest/` â€” see its README.

At the pinned baseline, vcpkg builds gtest shared. googletest's CMake compiles
`gtest_main` with `GTEST_CREATE_SHARED_LIBRARY=1` while it also inherits
`GTEST_LINKED_AS_SHARED_LIBRARY=1`, and `gtest.h` checks CREATE first â€” so
`gtest_main.dll` re-exports its own copy of the `UnitTest` singleton. Tests
registered into `gtest.dll` were invisible to it: **every test binary reported
success while running zero tests.**

Static linkage removes the DLL boundary. `lm_add_test()` also sets
`FAIL_REGULAR_EXPRESSION "Running 0 tests from 0 test suites"` as a second
backstop. Deleting either brings the silent failure back.

### Fast DDS `max_serialized_type_size` seed
`libs/transport/src/fastdds/topic_data_type.hpp`. Fast DDS 3.4.1 crashes with an
access violation when given `max_serialized_type_size == 0` â€” which its own header
documents as the convention for unbounded types â€” because `TopicPayloadPool::get()`
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
The spec gave `Rule` both a `kind` and a payload â€” two sources of truth that can
disagree. The kind is derived from the payload variant via `kind_of()`.

### Template assignment implies expected host
`ServerController::effective_expected_hosts()` unions the explicit expected-host
list with every host named in a template assignment. Without this, assigning a
template to a machine left it sitting in **Unexpected** forever, since only the
explicit list reached `reconcile()`. Explicit entries win so a typed address is
never dropped.

### Network adapters ride the resource sample
`core::NetworkAdapter` (name, description, `AdapterType`, `connected`) is a field
of `ResourceSample`, so adapters travel on the topic that already ticks â€” link
state is a live reading, and no rule references them, so unlike processes and
registry there is nothing to probe lazily against. Deliberately no IP addresses:
this answers "what is this plugged into, is it up", and addresses would churn the
sample under DHCP, defeating the equality check that suppresses no-op updates.

`Capability::Network` is what separates "no adapters" from "a client that cannot
report them" â€” the fleet column shows `-` for the second and `2 / 6` for the
first. Never infer it from an empty list.

**`INetConnectionManager` decides which adapters exist.** The list is meant to
match the Network Connections folder, and that is the API the folder is built on,
so it matches by construction. `GetAdaptersAddresses` alone reports plenty the
folder hides â€” the software loopback pseudo-interface, Wi-Fi Direct virtual
adapters â€” and **the registry offers no reliable signal for which**: on one test
machine `MediaSubType` is set on 2 of 20 connection keys, including neither of
two plainly visible adapters. Do not reach for a heuristic there; ask the folder.

The interface list is still needed for **types**: `NCM_LAN` lumps Ethernet and
Wi-Fi together, so each connection is matched to its interface by GUID and takes
the `IfType` from it. And do not pass `GAA_FLAG_INCLUDE_ALL_INTERFACES` â€” it adds
one entry per *filter driver bound to each card* (QoS Packet Scheduler, WFP
LightWeight Filter, every Npcap binding), 6 real adapters becoming 40.

`name` is the folder's renameable name ("smash-wifi"), `description` the hardware,
`id` the GUID â€” unique where the name is not, so any future matching keys on it.

When the folder cannot be enumerated (no COM, no session) the probe **falls back**
to `GetAdaptersAddresses` plus `RasEnumEntries`: noisier, but a monitoring agent
reporting nothing is worse. That fallback is where the RAS phonebook still
matters â€” an entry that is not dialled has **no interface at all**, so nothing
else can see it. `RasEnumEntries` returns names and little else, so each entry's
type comes from its own `RasGetEntryProperties`; without that a VPN reads as a
modem.

`tests/test_ras_entries_windows.cpp` builds a **temporary phonebook** with
`RasSetEntryProperties` rather than touching the real one under `%APPDATA%`,
since creating entries there would be editing the machine's network config. That
is what makes the disconnected-entry path genuinely tested on a machine with no
dial-up configured.

### The client quits deliberately, never incidentally
`DetailWindow` drops `Qt::WindowCloseButtonHint` and carries its own **Minimize**
and **Close Program** buttons; the second asks first, defaulting to No, and names
the consequence (this machine stops reporting) rather than asking "are you sure?".

Windows only **greys** the title bar's X â€” disabling it is as far as the platform
goes, there is no way to remove it while keeping a native title bar. So
`closeEvent()` still has to handle Alt+F4 and the system menu, and still hides
rather than quits.

`set_tray_available()` (was `set_hide_on_close()`) drives both behaviours from
the one fact that decides them: with a tray, Minimize hides and a stray close
hides; without one this window is the app's entire UI, so Minimize must leave a
taskbar button and a close must really close. Both quit routes â€” the tray's Quit
and the window's confirmed button â€” land on the same `QApplication::quit`.

Covered in `apps/client/tests/test_detail_window.cpp`, which is why
`lab_monitor_client_tests` now builds widgets and runs under a `QApplication`.
The modal is tested by scheduling the answer with a zero-timer *before* the click
that opens it â€” `QMessageBox::question` blocks until something clicks it.

### Percentages are coloured by their reading, state by `RowHealth`
`Theme::color_for_load()` maps 0â€“100% onto the palette's green, amber and red,
and everything percentage-based uses it: the fleet list's CPU, Memory and Disk
cells, both detail panes' CPU sparkline, and every `MeterBar`.

`FleetModel::data()` still paints whole rows by `RowHealth` â€” but the three
percentage columns override that with their own reading, because a machine can
be perfectly compliant and out of disk, which is exactly what a health colour
hides. The override applies **only while the host is `Online`**: the last sample
from a host that has gone quiet is stale, and painting it green would read as a
live, idle machine. `load_percent()` is the single source for both the number
and the colour, so they cannot disagree.

Consequences worth knowing. The status hues now do double duty â€” a red CPU cell
and a red `Missing` host are the same colour meaning different things â€” which is
why every percentage cell also shows its number. And a state change must emit
`dataChanged` across the **whole row**, not just the columns whose text moved:
health colours every cell, and the percentage cells additionally swap between
their load colour and the health one as a host starts and stops reporting.

### Rule ids are generated, never typed
The Add Rule flow used to open with a "Unique rule id:" prompt. `RuleId` is the
join key between a rule and the `CheckResult` reported for it, and `rules_for()`
merges the baseline with every assigned template keeping only the **first** rule
per id â€” so a reused id silently dropped a rule from evaluation and made every
lookup by id ambiguous. Nothing enforced uniqueness; the operator was expected to
remember which strings were taken, across every template.

`make_rule_id()` now derives one from the rule's kind and target
(`process-chrome-exe`, `registry-displayversion`), suffixed `-2`, `-3`â€¦ against
ids already in the bundle. Uniqueness is bundle-wide, not per-template, because
templates are combined per host. `deduplicate_rule_ids()` repairs bundles
authored under the old flow; `load_config()` runs it over the **draft** only and
logs each rename, leaving `published_` alone â€” it records what clients actually
hold, and rewriting it would misreport the fleet. The operator publishes the
repair when they choose to.

Anything looking a rule up by id must go through `rules_for(bundle, host_id)`,
not a walk over every template: the latter both admits rules the host was never
assigned and resolves collisions last-wins, the opposite of `rules_for()`. That
is what made the server label a row with a different rule's description than the
client showed for the same result.

### "Baseline" is a reserved template name
`TemplateBundle::baseline` is a **field**, not an entry in `templates`, but the
Templates tab lists the two together. A template genuinely named "Baseline" put
a second row of that name in the list â€” and since rows were matched by their
*label*, `selected_template()` resolved both to the bundle's baseline, so the
stray one could be neither selected nor deleted. Assignments created exactly
that: typing "Baseline" as a chip was an unknown name, so it made a template.

`core::is_baseline_name()` (case-insensitive) now reserves it: Add Template
refuses it, and an assignment chip is dropped rather than created. List rows
carry `kTemplateNameRole` and `kIsBaselineRole` instead of being matched by
label, which is what makes a legacy stray removable through the UI â€” the only
route to clearing one out of an existing bundle. The baseline's own row reads
"Baseline â€” always applied", in italics, and disables Remove Template while
selected rather than silently ignoring the click.

`apps/server/tests/test_fleet_window.cpp` covers these against a real
`FleetWindow`, which is why `lab_monitor_server_tests` now builds the widgets
and needs a `QApplication`.

### Naming a template in an assignment creates it
The Templates tab's assignment column is a `lm::ui::TokenEdit` â€” an Outlook-style
recipient field: committed names become chips with their own remove button, a
completer offers the existing templates, and text matching none of them is
yellow while it is typed. Committing such a name **creates the template**, empty.

The widget itself has no opinion about that: it completes, marks and reports.
Creation is `FleetWindow::on_assignment_tokens_changed()`'s policy, which is what
keeps `TokenEdit` reusable rather than a template picker with a general name.

Two traps live in that slot. It must **not** call `rebuild_assignment_table()` â€”
it runs from inside the `TokenEdit` that emitted, and rebuilding deletes that
widget mid-signal; `refresh_assignment_completions()` updates them in place
instead. And renaming a host in column 0 has to `extract()`/re-key the
`assignments` map rather than assign through `operator[]`, which would leave the
old entry behind, with the rebuild deferred through a zero-timer because the item
whose `setData()` is still on the stack would otherwise be deleted underneath it.

### Client recovers rule descriptions locally
`core::CheckResult` travels the wire carrying only a rule id, status, observed
value and message â€” never the description. Rather than widen the wire format,
`MonitorWorker` recovers display fields from the `TemplateBundle` it already holds
and emits them alongside the report (`apps/client/rule_detail.hpp`).

### nlohmann-json, not boost-json
Team choice. Boost is consequently confined to `program_options` in the two apps.

## Gotchas that have already cost time

- **Qt AUTOMOC misses `Q_OBJECT` headers** when headers live in `include/` and
  sources in `src/`. List such headers explicitly as `add_library` sources.
- **moc records a signal's parameter type as the unqualified literal it sees.**
  `qRegisterMetaType<T>()` registers the fully-qualified name, so the two can
  silently disagree and `QSignalSpy` then captures **empty** payloads â€” tests pass
  while verifying nothing. Register with the explicit name string when the signal
  is declared inside a namespace.
- **`connect()`'s 4-argument overload runs the functor on the *context object's*
  thread.** Passing a worker as context made `thread->wait()` a self-wait, which Qt
  detects, warns about, and skips â€” so the join silently never happened.
- **`QWidget::render()` cannot see a stale screen.** It repaints from scratch, so
  a wrong *backing store* never reaches the image â€” every pixel test in
  `lm_ui_widget_tests` is blind to that whole class of bug, as is every logical
  assertion. To look at what is actually on screen, grab it:
  `QApplication::primaryScreen()->grabWindow(window->winId())` inside a shown
  window, save the PNG, and open it. That is the only tool here that shows a
  repaint artefact, and it is worth reaching for early rather than theorising.
- **A stylesheet does not reach everything.** Message boxes and palette-drawn
  widgets ignore QSS; `Theme::apply()` installs Fusion and a dark `QPalette` too.
  An unstyled widget class falls back to the platform *light* style.
- **QSS beats an item delegate, and does it last.** With a stylesheet active,
  `QStyleSheetStyle` draws `CE_ItemViewItem` and feeds the `::item` rule's own
  `color` to `QRenderRule::configurePalette()` as `QPalette::HighlightedText` â€”
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
- **Service checks are stubbed** on both platforms â€” `IServiceProbe` has no
  implementation, so service rules report `NotApplicable`. Process, registry and
  network probes are implemented on **Windows only**; every Linux probe beyond
  resources is stubbed, so a Linux client advertises no `Capability::Network` and
  its adapter column reads `-`.
- The compliance list groups by status rather than Applications/Services/Registry.
- `FastDdsLoopback.ResourceSamplesReachTheServer` was de-flaked by publishing in a
  loop; the topic is BEST_EFFORT so a single publish races discovery.

## Running

```
build\windows\apps\server\Debug\lab_monitor_server.exe
build\windows\apps\client\Debug\lab_monitor_client.exe
```

Both accept `--domain-id`, `--config`, `--offline` (in-process transport, no DDS)
and `--log-level`. The client **starts hidden** â€” look for the tray icon.
`.vscode/launch.json` has configurations for both apps and every test binary,
including a compound that starts server and client together.
