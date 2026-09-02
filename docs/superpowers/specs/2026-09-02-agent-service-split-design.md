# Agent / Tray Split — Design

**Date:** 2026-09-02
**Status:** **Superseded, not implemented** — see
`2026-09-02-remote-scripts-design.md`

> ## Why this was dropped
>
> Two facts settled after this was written, and between them they removed the
> whole justification:
>
> 1. **The interactive users on the lab machines are local administrators.** A
>    Scheduled Task with "Run with highest privileges" at logon therefore starts
>    the existing tray application elevated, with no UAC prompt. Elevation does
>    not need a service.
> 2. **Reporting only while somebody is logged in is acceptable.** The other
>    motivation below — that a locked or logged-out machine leaves the fleet
>    entirely — is a real limitation, and a deliberately accepted one.
>
> With both gone, the service bought nothing that a Scheduled Task and twenty
> lines of `GetTokenInformation` do not, at the cost of an installer, session-0
> isolation, `CreateProcessAsUser`, a status file, supervision, and two
> processes to keep version-matched.
>
> **Kept rather than deleted for one section.** "The arrangement deliberately
> not chosen" below explains why a tray application must never relay execution
> to a SYSTEM helper over a local endpoint. That reasoning outlives this design:
> the shortcut stays attractive, and the answer to it is not obvious. If a
> service is ever revisited — most likely because monitoring while logged out
> starts to matter — start here.
>
> Section 8 (pause) is also still live: pause reporting was going to be removed
> because the split broke it. Without the split it is not broken, so **pause
> stays as it is**.

## 1. Why

`lab_monitor_client` is a tray application. Everything it does — probing, evaluating,
publishing — happens inside a process that only exists while a user is logged in, at
that user's privilege level. Two consequences follow, and the second is why this
document exists.

**A locked or logged-out machine reports nothing.** Not "reports stale data" — it
disappears from the fleet, goes `Offline` after the liveliness lease, and stays there
until somebody signs in. For a lab of machines that spend nights logged out, that is a
monitoring tool that stops monitoring exactly when nobody is watching.

**Nothing it runs can be elevated.** The next feature is remote script execution, and
the scripts that matter are installs and uninstalls, which need administrative rights.
A process running as an interactive standard user cannot provide them.

The fix for both is the same: the agent becomes a Windows service, and the tray
application becomes a view of it.

### The arrangement deliberately *not* chosen

The obvious smaller change is to leave the tray application as the agent and give it a
SYSTEM helper service to run scripts through. That was rejected on security grounds and
the reasoning belongs here, because it will look like an attractive shortcut again later.

For the tray application — running as a possibly-unprivileged interactive user — to hand
work to a SYSTEM service, that service must expose a local endpoint accepting "run this
PowerShell as SYSTEM". The endpoint cannot be ACL'd to administrators, because the
legitimate caller is not one. Any local user can therefore connect to it and obtain
SYSTEM. That is a local privilege-escalation vector on every machine the agent is
installed on, and no care in the protocol removes it, because the caller is
unprivileged *by design*.

The rule that avoids it: **the component that executes must be the component that
receives the command.** The UI never brokers execution. Everything below follows from
that.

## 2. Scope

### In scope

1. A new `lab_monitor_service` target: a Windows service that owns the DDS client, the
   probes, `evaluate()`, and template bundle receipt.
2. `lab_monitor_client` reduced to a view: tray icon, detail window, log view. It
   publishes nothing to DDS.
3. Service-to-tray status transfer.
4. The service launching and supervising the tray application in the active session.
5. Close Program stopping the service, via elevation.
6. Install and uninstall of the service.
7. A `--console` mode so the service logic runs in the foreground for development and
   test.

### Explicitly out of scope for this phase

- **Script execution.** No `IScriptRunner`, no new DDS topics, no Scripts tab. This
  phase creates the elevated, always-running host that the next one needs, and nothing
  more. The two were separated so that this restructure lands and is proven before
  anything is built on it.
- **Linux.** The Linux build keeps today's single-process arrangement. A systemd unit
  is a later change, and per the known gaps the Linux paths have never been compiled.
- **Elevation *use*.** The service runs as LocalSystem, which is what makes elevation
  possible, but nothing in this phase exercises it.

## 3. Process architecture

| | `lab_monitor_service` | `lab_monitor_client` |
|---|---|---|
| Runs as | LocalSystem, no session | Interactive user, session 1+ |
| Qt | Core only | Widgets |
| DDS | Owns the client transport | **None** |
| Probes | All of them | None |
| `evaluate()` | Yes | No |
| Visible | No | Tray icon, detail window |
| Lifetime | Boot to shutdown | Logon to logoff |

The service is what the server sees as the host: it announces, publishes resource
samples and publishes compliance reports. `host_id` is determined by the service.

### What moves

`MonitorWorker`, `lm::platform::HostProbes`, the transport construction and the DDS
probe move from `apps/client` into `apps/service`.

`MonitorWorker` sheds `#include "lm/ui/rule_detail.hpp"` on the way. It currently emits
`report_ready(ComplianceReport, QVector<RuleDetail>)` — recovering display fields for
the detail window. A headless service must not link `lm_ui`, so it publishes the raw
report and the tray runs `lm::ui::describe()` itself, which is where that call belonged.
This is a simplification the split forces rather than a cost it imposes.

### Naming

`lab_monitor_client` keeps its name despite no longer being the client in the DDS sense.
Renaming it would churn the launch configurations, the log filenames, the CLAUDE.md
references and the muscle memory of anyone using it, to gain a distinction that only
matters inside this document.

## 4. Service hosting

A hand-rolled service entry point: `StartServiceCtrlDispatcher`, a `ServiceMain`, a
handler responding to `SERVICE_CONTROL_STOP` and `SERVICE_CONTROL_SHUTDOWN`, and
`SetServiceStatus` transitions. Roughly 150 lines. Qt 6 has no service module and
pulling in a dependency for this would cost more than it saves.

**All logic lives in a `ServiceAgent` class.** `ServiceMain` constructs one, starts it,
and waits; `--console` constructs one, starts it, and waits on Ctrl-C. The two entry
points share every line that matters. This is not a convenience:

- Without `--console` there is no debugging story. Attaching to a service is possible
  and unpleasant, and the `.vscode/launch.json` compounds — *Server + Client*, *Server +
  Client + Shopping Cart* — stop working entirely.
- Without `--console` the agent cannot be driven by a test at all.

`ServiceMain` and the installer are the two pieces that cannot be tested; they are kept
thin enough to be read instead.

### Install

`--install` and `--uninstall`, using `CreateService` / `DeleteService`, requiring
elevation and saying so plainly when run without it. Start type `SERVICE_AUTO_START`,
account `LocalSystem`.

`--uninstall` stops the service first if it is running, then deregisters it, then
removes `%ProgramData%\lab_monitor\status.json`. It leaves the log files and the
config directory alone: those are the record of what the agent did, and an uninstall
is the moment somebody most wants them.

## 5. Supervising the tray

The service launches the tray application into the active console session:
`WTSGetActiveConsoleSessionId`, `WTSQueryUserToken`, `CreateEnvironmentBlock`,
`CreateProcessAsUser`. It subscribes to `SERVICE_CONTROL_SESSIONCHANGE` and relaunches
on `WTS_SESSION_LOGON`, so a user signing in gets the tray icon without a logon script.

This was chosen over per-user autostart (a `Run` key), which is six lines and lets
Windows handle logon and fast user switching. The service-launch route was preferred
because it also **restarts the tray if a user kills it**, which autostart cannot do —
and on a monitored machine, a view that can be silently dismissed and never return is
worth avoiding.

The cost is honest: session-0 isolation, token lifetimes and fast user switching make
this the fiddliest code in the phase, and it is Windows-only by construction.

**The service does not require the tray to be running.** Monitoring is unaffected by
its absence; supervision is a convenience, not a dependency.

## 6. Service → tray

The service writes `%ProgramData%\lab_monitor\status.json`, replaced atomically
(write to a temporary file in the same directory, then `MoveFileEx` with
`MOVEFILE_REPLACE_EXISTING`, so a reader never sees a half-written file). It carries:

- `host_id`, capabilities, connection state, agent version
- the latest `ResourceSample`
- the latest `ComplianceReport`, raw
- the applied template revision
- the service's own start time

The tray watches it with `QFileSystemWatcher` and re-reads on change. Direction is
one-way. The service creates the directory with an ACL granting Users read and
Administrators full control, so an unprivileged process can display the status but
cannot forge it.

The alternative — the tray holding its own read-only DDS subscription — was rejected
because it would duplicate every probe and put a second participant on the domain for a
host that already has one.

**Staleness is shown, not hidden.** If the file stops changing, or the service is not
running, the detail window says so rather than displaying its last reading as though it
were current. This is the same rule the fleet table already applies to a silent host.

## 7. Close Program

With the service supervising, a tray that merely exits is relaunched. Close Program must
therefore stop the service first, then exit.

Stopping a service requires `SERVICE_STOP`, which a standard user does not have.
Rather than widening the service's security descriptor to grant it — which would let any
logged-in user quietly opt their machine out of monitoring, defeating much of the reason
for the service — Close Program **elevates**: `ShellExecute` with the `runas` verb on the
service binary with a `--stop` argument, producing a UAC prompt.

If the user cancels, or is not an administrator, the window says it could not stop
monitoring and offers to minimise instead. It does not pretend to have worked, and it
does not leave a window that cannot be dismissed.

The confirmation text changes accordingly: it currently warns that this machine stops
reporting, which remains true, and now also says that administrator approval is
required.

## 8. Pause reporting is removed

The tray's "Pause reporting" cannot work after the split: the tray no longer publishes,
so pausing it pauses nothing.

**Removed:** the tray menu item, `TrayController::reporting_paused_changed`, and
`MonitorWorker::set_reporting_paused`.

**Retained, dormant:** `HostState::Paused`, `ClientAnnounce::paused`, the fleet column's
`Paused` handling, its colour, its ribbon counter and its `reconcile()` resolution.

The retention is deliberate. Pause is most naturally a *server-side* action once the
agent is a service — you pause a machine from the console where you noticed it needed
pausing — and the scripts phase builds the server-to-client control path that would
carry it. Tearing out 22 files of working, tested behaviour to re-add it two phases
later is churn for its own sake. The dormancy is documented here and in CLAUDE.md so it
does not read as an accident.

Reviewer's note: if you would rather it came out to the wire, say so — it is a larger
deletion, not a smaller one, and better decided now than half-done.

## 9. Logging

Both processes log beside their own executable, using the existing `configure_logging()`
with its AppData fallback. The service's log is where everything interesting happens;
the tray's is nearly empty by design.

The tray's **Log tab tails the service's log file** rather than showing its own. A
`LogView` fed by a file tail rather than a live sink is a small addition to the widget
that already exists.

## 10. Configuration

`--install` accepts the same options the client takes today — `--domain-id`, `--config`,
`--offline`, `--log-level` — and writes them into the service's registered image path,
so `sc qc lab_monitor_service` shows exactly what the agent will run with. Changing them
means re-running `--install`, which updates the existing registration rather than
failing.

**The tray's own command line shrinks.** It no longer joins a domain or selects a
transport, so `--domain-id` and `--offline` leave it; `--log-level` stays. Passing a
removed option fails with a message naming the service instead, rather than being
silently ignored — someone will type the old command line out of habit.

**The tray runs without a service.** It is not an error state to be crashed on: the
detail window says the service is not running and offers nothing else, which is also
exactly what an operator sees if the service has stopped. One path, not two.

## 11. Testing

`ServiceAgent` is constructed and driven directly, in-process, exactly as `--console`
does. The 17 existing client tests split:

- worker and probe tests move to `lab_monitor_service_tests`
- `DetailWindow` tests stay with `lab_monitor_client_tests`

New coverage:

- the status file round-trips every field, and a partially written file is never read
  as valid
- the tray shows a stale-service state when the file stops changing
- Close Program reports failure honestly when elevation is refused

`ServiceMain`, the installer and `CreateProcessAsUser` are the untestable edges. They
are exercised by hand during the phase and kept as thin as possible.

## 12. Risks

**Session-0 isolation.** The most likely source of defects. Mitigated by the service not
depending on the tray, so a supervision bug degrades the view rather than the monitoring.

**Two processes where there was one.** Install, upgrade and uninstall all get harder,
and a version mismatch between service and tray becomes possible. The status file
carries the agent version so the tray can say when it is talking to a service it does
not match.

**The debugging loop.** `--console` is the mitigation and is treated as a first-class
mode, not a fallback.

**No authentication on the DDS domain.** Unchanged by this phase, but worth restating:
once scripts arrive, anything that can publish on the domain can run code on enrolled
clients. The per-machine opt-in bounds it. This is a known limitation, not an oversight,
and it belongs in the scripts spec as a first-class concern.

## 13. What this enables

The scripts phase then adds: two DDS topics, `IScriptRunner` in the service, a
`Capability::Scripts` gated behind per-machine opt-in, and the server's Scripts tab with
share browsing, host selection, live runs and persisted history. None of it touches this
phase's decisions, which is the point of doing them in this order.
