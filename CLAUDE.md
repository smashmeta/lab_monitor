# lab_monitor

Cross-platform (Windows + Linux) C++23 fleet monitoring. A central **server**
discovers **client** machines over DDS, reconciles them against an expected-host
list, and distributes a **template** of compliance rules; clients report resource
usage and rule compliance back.

Design spec: `docs/superpowers/specs/2026-08-11-lab-monitor-design.md`
Implementation plan: `docs/superpowers/plans/2026-08-11-lab-monitor-foundation.md`
Architecture overview: `docs/architecture.html` — the module layering, the four
DDS topics and every field on them, drawn. Open it in a browser from the working
tree. Its Mermaid comes from a CDN, so the diagrams need a network connection;
without one each falls back to its own source text.

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
| `lm_core` | Pure domain logic: rules, templates, JSON, `evaluate()`, `reconcile()`, `ClientRegistry` | 135 |
| `lm_platform` | OS probes behind interfaces, plus public fakes in `fakes.hpp` | 51 |
| `lm_transport` | `IClientTransport`/`IServerTransport`, FastCDR codecs, in-memory bus, Fast DDS backend | 28 |
| `lm_ui` | Shared Qt5 widgets, theme, `FleetModel`, `SampleCoalescer`, `RuleDetail`, `TokenEdit`, `AdapterList` | 48 + 32 |
| `lab_monitor_client` | Hidden tray app; worker thread samples and publishes | 17 |
| `lab_monitor_server` | Fleet console; discovery, reconciliation, template publishing | 34 |

**345 unit tests**, plus 4 Fast DDS loopback integration tests gated behind
`LM_BUILD_INTEGRATION_TESTS` (default OFF — they need loopback multicast).

`lm_ui`'s second figure is `lm_ui_widget_tests`, a separate binary because it
constructs and paints real widgets and so needs a `QApplication` and a platform
plugin, where every other `lm_ui` test runs under a `QCoreApplication`. Some of
its cases render through the real stylesheet and inspect the pixels
(`tests/pixel_probe.hpp`) — the only way to catch a QSS rule silently overriding
what the model, a delegate or a dynamic property asked for. It cannot use
`QT_QPA_PLATFORM=offscreen` on Windows: vcpkg's applocal deployment copies only
`qwindows`, so the binary would abort before `main()`.

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

### Network adapters ride the resource sample
`core::NetworkAdapter` (name, description, id, `AdapterType`, `LinkState`) is a field
of `ResourceSample`, so adapters travel on the topic that already ticks — link
state is a live reading, and no rule references them, so unlike processes and
registry there is nothing to probe lazily against. Deliberately no IP addresses:
this answers "what is this plugged into, is it up", and addresses would churn the
sample under DHCP, defeating the equality check that suppresses no-op updates.

`Capability::Network` is what separates "no adapters" from "a client that cannot
report them" — the fleet column shows `-` for the second and `1 / 4` for the
first. Never infer it from an empty list.

**Link is a `LinkState`, not a bool.** An Ethernet port with nothing plugged into
it (`NoMedia` — the cross Windows draws on it), a card the operator switched off
(`Disabled`), and a dial-up entry sitting idle (`Disconnected`) are three
different problems, and a boolean calls all three "not connected". `NETCON_STATUS`
already separates them, so the states mirror it. Only `Connected` counts as up,
via `core::is_up()`, so the fleet column and the adapter list cannot disagree
about the total. The fallback path's `IF_OPER_STATUS` is coarser — it cannot tell
an unplugged cable from a disabled adapter, both being simply Down — so it reports
the weaker `Disconnected` rather than claiming a distinction it does not have.

**`INetConnectionManager` decides which adapters exist.** The list is meant to
match the Network Connections folder, and that is the API the folder is built on,
so it matches by construction. `GetAdaptersAddresses` alone reports plenty the
folder hides — the software loopback pseudo-interface, Wi-Fi Direct virtual
adapters — and **the registry offers no reliable signal for which**: on one test
machine `MediaSubType` is set on 2 of 20 connection keys, including neither of
two plainly visible adapters. Do not reach for a heuristic there; ask the folder.

The interface list is still needed for **types**: `NCM_LAN` lumps Ethernet and
Wi-Fi together, so each connection is matched to its interface by GUID and takes
the `IfType` from it. And do not pass `GAA_FLAG_INCLUDE_ALL_INTERFACES` — it adds
one entry per *filter driver bound to each card* (QoS Packet Scheduler, WFP
LightWeight Filter, every Npcap binding), 6 real adapters becoming 40.

`name` is the folder's renameable name ("smash-wifi"), `description` the hardware,
`id` the GUID — unique where the name is not, so any future matching keys on it.

When the folder cannot be enumerated (no COM, no session) the probe **falls back**
to `GetAdaptersAddresses` plus `RasEnumEntries`: noisier, but a monitoring agent
reporting nothing is worse. That fallback is where the RAS phonebook still
matters — an entry that is not dialled has **no interface at all**, so nothing
else can see it. `RasEnumEntries` returns names and little else, so each entry's
type comes from its own `RasGetEntryProperties`; without that a VPN reads as a
modem.

`tests/test_ras_entries_windows.cpp` builds a **temporary phonebook** with
`RasSetEntryProperties` rather than touching the real one under `%APPDATA%`,
since creating entries there would be editing the machine's network config. That
is what makes the disconnected-entry path genuinely tested on a machine with no
dial-up configured.

### Every failure says what was expected
`CheckResult::message` carries the cause for an `Error` **and the expectation
for a `Fail`** — "expected at least 1 connected", "expected version >= 2.0",
"expected the value to be \"2.0\"". Without it a failure only ever states the
observation ("2 of 4 connected"), leaving the reader to work out which rule that
violates. That is a guessing game on a wall display, where the rule's own
free-text description is the only other text on the row.

`resolve()` takes the expectation and attaches it **only on failure**: repeating
it beside a tick is noise, and both compliance views append `message` to
`observed` unconditionally. `expected_text()` picks the phrasing from the rule's
own `Presence`, so a `MustBeAbsent` failure does not read backwards.

`FailureMessages.EveryFailureCarriesOne` is the blanket guarantee — it fails one
rule of every kind at once and asserts each result explains itself. A new rule
kind that forgets this trips it.

### The Compliance tab is written for a wall display
It is read from across a room, on a screen nobody walks over to. That drives
every choice in it, and none of them should be "tidied" toward the conventions
the other tabs follow:

- **Nothing hides behind interaction.** No tooltips carrying the real content,
  no collapsed groups, no selection. Groups are expanded on creation and the
  tree is `NoSelection` with `NoFocus`.
- **Passing rules are counted, never listed.** A wall of green pushes the two red
  lines that matter off the screen. The host row already says `3 / 5 rules
  passed`; only failures, errors and not-applicable rules get their own line.
- **A group that would be empty says "All N checked rules passing"** instead —
  blank space reads as "no data" at a distance. Only when it would otherwise be
  empty: a host with not-applicable rows does not need telling twice.
- **Rows are ordered failing → error → not applicable**, and hosts worst-first.
- The font is deliberately larger than the rest of the window, where the fleet
  table is instead sized to fit ~35 rows for someone at the keyboard.

### The Compliance tab lists the whole fleet, not the report cache
`rebuild_compliance_table()` walks `ServerController::fleet()` and looks each
host's report up, rather than iterating `report_cache()`. Driving it off the
cache had two consequences on a display nobody walks over to. A host that had
**never reported** — `Missing` — was simply absent, and absence on a wall reads
as "nothing wrong with it". A host that reported once and then **died** kept its
last score on the glass forever, because the cache only ever grows: a dead
machine sat there at a green `5 / 5`.

So `Missing` and `Offline` hosts get their own group, above the failing ones
(the Fleet tab already ranks them that way, and two views disagreeing about
which host is most urgent is worse than either order). Their row says
`Missing — never reported` or `Offline — last seen 14:22:07`, and their single
child line is **"Not reporting — no rules are being checked"**, never
`0 / 0 rules passed`, which reads as a clean bill of health. A cached report from
before the machine went quiet is shown only as `last known 3 / 5`. Silence also
counts in the headline (`· 2 not reporting`) and paints it red — an unreachable
machine is a red line, not a quiet one. The colour comes from
`FleetModel::colour_for()`, so the two tabs cannot paint the same host
differently.

An `Online` host with no report yet is listed too, saying so, rather than
appearing only once it has evaluated something.

This is why `on_announce()` and `on_report()` now call `reconcile_now()`: the tab
reads liveness and the report together, so a host whose first report has just
arrived would otherwise be drawn as silent for up to a second. `reconcile_now()`
emits the new `fleet_changed()` — compared on the host → state mapping alone,
because `last_seen` advances on every sample and comparing whole entries would
fire it every tick and rebuild the tree once a second.

### The Compliance tab scores against what was *checked*
`core::summarise()` reduces a report to counts, and `ComplianceSummary::checked()`
is `passed + failing + errors` — **NotApplicable is excluded from the
denominator**. A rule the client cannot evaluate can never pass, so counting it
would park a Linux machine at "5 of 10" forever for having no registry, with no
action able to improve it. The excluded rules keep their own visible column
rather than disappearing.

Errors *are* in the denominator even though `is_compliant()` ignores them:
"could not check it" is not "passed" and must not be scored as one.

The score is a ratio (`passed_ratio()`, 0–1) rather than a percentage, because
that is how it is displayed — "3 / 5", not "60 %" — and one representation is
enough. The row colour reuses `Theme::color_for_load()` inverted, since a fully
compliant host is the good end where full load is the bad one. Rows sort
worst-first: the tab exists to find what needs attention, and alphabetical order
would bury a host with three failures under one with none.

### Network rules are two payloads, not one with a mode flag
`AdapterCountRule` ("at least 2 connected") and `AdapterStateRule` ("smash-wifi
must be Up") are separate alternatives of `RulePayload`, both mapping to
`RuleKind::Network` and so to `Capability::Network`. One struct with a "which
form is this" flag would be the same two-sources-of-truth mistake `Rule` avoids
by not storing its own `kind`.

`Presence` applies to the state rule — `MustBeAbsent` means "must **not** be in
this state", which is how every other kind reads it — but **not** to the count
rule, whose `Comparison` already carries the direction. The Add Rule flow skips
the question there rather than asking and ignoring the answer.

A named adapter that is missing entirely reports **Fail**, not Error: a removed
or renamed NIC is exactly what a fleet check should catch, and Error means "could
not tell". Names are matched case-insensitively against
`NetworkAdapter::name` — the Network Connections name, not the GUID — because
rules are typed by people.

JSON uses its own wire names (`"AtLeast"`, `"NoMedia"`), deliberately not
`core::to_string()`'s display strings ("at least", "No link"): the latter are
free to be reworded, while anything in a saved bundle has to keep parsing.

### The client quits deliberately, never incidentally
`DetailWindow` drops `Qt::WindowCloseButtonHint` and carries its own **Minimize**
and **Close Program** buttons; the second asks first, defaulting to No, and names
the consequence (this machine stops reporting) rather than asking "are you sure?".

Windows only **greys** the title bar's X — disabling it is as far as the platform
goes, there is no way to remove it while keeping a native title bar. So
`closeEvent()` still has to handle Alt+F4 and the system menu, and still hides
rather than quits.

`set_tray_available()` (was `set_hide_on_close()`) drives both behaviours from
the one fact that decides them: with a tray, Minimize hides and a stray close
hides; without one this window is the app's entire UI, so Minimize must leave a
taskbar button and a close must really close. Both quit routes — the tray's Quit
and the window's confirmed button — land on the same `QApplication::quit`.

Covered in `apps/client/tests/test_detail_window.cpp`, which is why
`lab_monitor_client_tests` now builds widgets and runs under a `QApplication`.
The modal is tested by scheduling the answer with a zero-timer *before* the click
that opens it — `QMessageBox::question` blocks until something clicks it.

### Percentages are coloured by their reading, state by `RowHealth`
`Theme::color_for_load()` maps 0–100% onto the palette's green, amber and red,
and everything percentage-based uses it: the fleet list's CPU, Memory and Disk
cells, both detail panes' CPU sparkline, and every `MeterBar`.

`FleetModel::data()` still paints whole rows by `RowHealth` — but the three
percentage columns override that with their own reading, because a machine can
be perfectly compliant and out of disk, which is exactly what a health colour
hides. The override applies **only while the host is `Online`**: the last sample
from a host that has gone quiet is stale, and painting it green would read as a
live, idle machine. `load_percent()` is the single source for both the number
and the colour, so they cannot disagree.

Consequences worth knowing. The status hues now do double duty — a red CPU cell
and a red `Missing` host are the same colour meaning different things — which is
why every percentage cell also shows its number. And a state change must emit
`dataChanged` across the **whole row**, not just the columns whose text moved:
health colours every cell, and the percentage cells additionally swap between
their load colour and the health one as a host starts and stops reporting.

### Rule ids are generated, never typed
The Add Rule flow used to open with a "Unique rule id:" prompt. `RuleId` is the
join key between a rule and the `CheckResult` reported for it, and `rules_for()`
merges the baseline with every assigned template keeping only the **first** rule
per id — so a reused id silently dropped a rule from evaluation and made every
lookup by id ambiguous. Nothing enforced uniqueness; the operator was expected to
remember which strings were taken, across every template.

`make_rule_id()` now derives one from the rule's kind and target
(`process-chrome-exe`, `registry-displayversion`), suffixed `-2`, `-3`… against
ids already in the bundle. Uniqueness is bundle-wide, not per-template, because
templates are combined per host. `deduplicate_rule_ids()` repairs bundles
authored under the old flow; `load_config()` runs it over the **draft** only and
logs each rename, leaving `published_` alone — it records what clients actually
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
a second row of that name in the list — and since rows were matched by their
*label*, `selected_template()` resolved both to the bundle's baseline, so the
stray one could be neither selected nor deleted. Assignments created exactly
that: typing "Baseline" as a chip was an unknown name, so it made a template.

`core::is_baseline_name()` (case-insensitive) now reserves it: Add Template
refuses it, and an assignment chip is dropped rather than created. List rows
carry `kTemplateNameRole` and `kIsBaselineRole` instead of being matched by
label, which is what makes a legacy stray removable through the UI — the only
route to clearing one out of an existing bundle. The baseline's own row reads
"Baseline — always applied", in italics, and disables Remove Template while
selected rather than silently ignoring the click.

`apps/server/tests/test_fleet_window.cpp` covers these against a real
`FleetWindow`, which is why `lab_monitor_server_tests` now builds the widgets
and needs a `QApplication`.

### Naming a template in an assignment creates it
The Templates tab's assignment column is a `lm::ui::TokenEdit` — an Outlook-style
recipient field: committed names become chips with their own remove button, a
completer offers the existing templates, and text matching none of them is
yellow while it is typed. Committing such a name **creates the template**, empty.

The widget itself has no opinion about that: it completes, marks and reports.
Creation is `FleetWindow::on_assignment_tokens_changed()`'s policy, which is what
keeps `TokenEdit` reusable rather than a template picker with a general name.

Two traps live in that slot. It must **not** call `rebuild_assignment_table()` —
it runs from inside the `TokenEdit` that emitted, and rebuilding deletes that
widget mid-signal; `refresh_assignment_completions()` updates them in place
instead. And renaming a host in column 0 has to `extract()`/re-key the
`assignments` map rather than assign through `operator[]`, which would leave the
old entry behind, with the rebuild deferred through a zero-timer because the item
whose `setData()` is still on the stack would otherwise be deleted underneath it.

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
- **`QWidget::render()` cannot see a stale screen.** It repaints from scratch, so
  a wrong *backing store* never reaches the image — every pixel test in
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
  `color` to `QRenderRule::configurePalette()` as `QPalette::HighlightedText` —
  *after* `initStyleOption()` has run. So `color:` on `QTableView::item:selected`
  silently undid `KeepForegroundDelegate` and turned every selected fleet row
  white. Selection styling in `theme.qss` is background-only, deliberately.
  `lm_ui_render_tests` pins this: it is not reproducible in a non-painting test.
- **DDS `TRANSIENT_LOCAL` durability lives on the DataWriter, not on disk.** A
  restarted server has written nothing, which is why `start()` re-announces the
  published bundle at its existing revision (never bumping it).
- **The client re-announces on a timer, and must.** `ClientAnnounce` is the only
  carrier of a client's `Capabilities`, and the server can lose them:
  `ClientRegistry::mark_lost()` erases the entry outright on a liveliness drop,
  and the resource samples that keep arriving recreate it through `touch()`,
  which knows nothing about capabilities. Announcing once at startup made that
  permanent — the server showed the machine as unable to report adapters (`-` in
  the column, "Not reported" in the pane) for the rest of its run, and only
  restarting the *client* fixed it. `TRANSIENT_LOCAL` does not save you here: it
  covers a server that has never seen the client, not one that saw it and then
  forgot. Repeating every 10 s turns a permanent state into a blip, and lets an
  upgraded agent's new capabilities reach a running server.
- **Include hygiene**: the Linux leg compiles with libstdc++, far stricter than
  MSVC's STL about transitive includes. Include what you use, directly.

## Known gaps

- **The Linux code paths have never been compiled.** Everything has been built with
  MSVC. CI is authored but has never run; expect the first Linux job to fail on GCC
  warnings under `-Wall -Wextra -Wpedantic -Wshadow` with warnings-as-errors.
- **Service checks are stubbed** on both platforms — `IServiceProbe` has no
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
and `--log-level`. The client **starts hidden** — look for the tray icon.
`.vscode/launch.json` has configurations for both apps and every test binary,
including a compound that starts server and client together.
