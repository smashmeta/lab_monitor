# Remote Script Execution — Design

**Date:** 2026-09-02
**Status:** Draft, for review

## 1. Overview

A **Scripts** tab in the server lets an operator pick a PowerShell script from a shared
folder tree, choose which hosts to run it on, and watch the results arrive per host in
real time. Each dispatch is a **run** — one transaction, with one outcome per targeted
host — and runs are kept so that "what did we push on Tuesday, and where did it fail"
is answerable later.

This is the first thing lab_monitor does *to* a machine rather than *about* one. That
distinction drives most of the design below.

## 2. The security posture, stated plainly

Remote script execution turns a monitoring agent into a management agent. Three facts
belong at the top rather than in a footnote:

**The DDS domain has no authentication.** Anything able to publish on it can issue
commands. This is unchanged by this feature but becomes materially more serious with it,
because the consequence moves from "publishes a misleading sample" to "runs code".
Bounding it is the opt-in below and, ultimately, network reach.

**Scripts run elevated.** See §7. That is the point — installs and uninstalls need it —
and it means a command from the domain runs as a local administrator.

**Execution is off unless a machine opts in.** A client runs nothing unless started with
`--allow-scripts`. Without it the client advertises no script capability and ignores
every command it sees. An agent upgrade therefore never silently converts a monitoring
box into one that executes remote code; enrolling a machine is a deliberate, per-machine
act.

## 3. Scope

### In scope

1. `Capability::Scripts` and `Capability::Elevated`, both riding the existing announce.
2. Two DDS topics: `ScriptCommand` (server → client) and `ScriptResult` (client → server).
3. `IScriptRunner` in `lm_platform`, with a Windows PowerShell implementation and a
   Linux stub.
4. Server Scripts tab: script picker over a shared folder tree, host selection, run
   dispatch, live per-host results, output viewing.
5. Ad-hoc script entry as a second mode of the same tab.
6. Persisted run history with operator-driven cleanup.
7. Elevation detection, a warning when absent, and a Scheduled Task to obtain it.

### Out of scope

- **Streamed output.** A result arrives once, when the script finishes or times out.
  Chunked streaming means ordering, reassembly and partial results from a client that
  dies mid-run; it can be added later without changing the run model.
- **Scheduling.** Runs happen when the operator presses Run. No cron, no maintenance
  windows.
- **Script editing in the server.** The share is edited with whatever edits files.
- **Signing or hash verification of scripts.** Noted as the obvious next hardening step
  in §12.

## 4. The run model

A **run** is the unit the operator thinks in:

```
Run
  run_id          unique, generated server-side
  script_name     "Maintenance/Clear-TempFiles.ps1", or "(ad-hoc)"
  script_body     what was actually sent, verbatim
  issued_at, issued_by
  timeout_seconds
  targets[]       one entry per host
```

Each target moves through a small state machine:

```
Pending ──dispatched──> Dispatched ──result──> Completed
   │                        │                  Failed
   │                        └──deadline──────> No response
   └──host not Online, or no Scripts capability──> Refused
```

The four terminal outcomes are deliberately distinct, because they need different
actions from a person:

| Outcome | Means | What to do |
|---|---|---|
| `Completed` | ran, and reported success | nothing |
| `Failed` | ran, and reported failure | read the output |
| `Refused` | the host will not run it | enrol the machine, or check elevation |
| `No response` | nothing came back in time | check whether the host is alive |

Collapsing `Refused` and `No response` into one "error" would hide exactly the
distinction an operator needs: one is a configuration problem on a machine that is
working, the other is a machine that may not be.

**Hosts that are not `Online` at dispatch are `Refused` immediately**, without a command
being sent. This follows from the volatile durability in §5: there is no queue, so
promising delivery to an absent host would be a lie.

## 5. Wire format

Two topics, both keyed by `host_id`, both **`VOLATILE`**.

```
ScriptCommand   (server → client)
  host_id           the single target; one sample per targeted host
  run_id            correlates the result back to its run
  script_name       for the client's log and the audit trail
  script_body       the full text — see below
  timeout_seconds

ScriptResult    (client → server)
  host_id, run_id
  status            Completed | Failed | Refused | Error
  refusal_reason    set when Refused: not enrolled, not elevated, already running
  exit_code
  reported_ok       tri-state: the script's own verdict, if it gave one
  reported_message  from the structured line, if present
  stdout, stderr    captured, capped
  started_at, duration_ms
```

Three decisions here are load-bearing.

**Durability is `VOLATILE`, never `TRANSIENT_LOCAL`.** Every other topic in this system
carries state, where a late joiner *should* receive the last value. A command is an
event. With transient-local durability a client that restarts would receive and execute
whatever command it missed — so rebooting a machine could silently re-run last week's
uninstall. This is the single most dangerous mistake available in this feature.

**The client remembers `run_id`s it has already executed** and ignores repeats, because
volatile durability prevents replay after a restart but not redelivery within a session.
Belt and braces: the durability setting is the design, the dedupe is the guard.

**The server sends the body, never a name.** The client never resolves a script name,
never reads the share, and needs no access to it. That removes an entire failure mode —
client and server disagreeing about what `Clear-TempFiles.ps1` contains — and means the
share needs one ACL, for the operators, rather than one for every lab PC.

## 6. Client side

`IScriptRunner` in `lm_platform`, with `run(body, timeout) -> ScriptOutcome`.

**Execution must not block the monitoring worker.** The worker thread carries the 10 s
announce; a 60 s script blocking it would push the host past its liveliness lease, so
the fleet would show it going `Offline` mid-run and then reappearing. The runner
therefore executes on its own thread, and the worker only receives the finished outcome.

**One script at a time per host.** A second command arriving while one is running is
`Refused` with `already running` rather than queued. A queue is a promise about ordering
and completion that a machine which may be rebooted cannot keep.

**Invocation:** `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File
<temp>.ps1`, the body written to a temporary file under the agent's own directory and
deleted afterwards. `-NonInteractive` matters: a script that prompts would otherwise hang
until the timeout with no indication why.

**Output is capped at 64 KB per stream**, truncated with an explicit marker rather than
silently. A script that prints a megabyte is a script whose author will be glad the
fleet did not try to carry it.

**Timeout kills the process tree**, not just the shell — a PowerShell script that
launched an installer leaves the installer running otherwise.

### How a script reports its own verdict

The exit code is the baseline: `0` is success, anything else is failure. Every existing
script works unchanged under that rule.

A script that wants to say more writes a final line:

```
LM-RESULT: {"ok": false, "message": "3 of 5 packages failed"}
```

The client parses the last such line if present. `reported_ok` is a tri-state — the
script said yes, said no, or said nothing — so "exited 0 and said nothing" is
distinguishable from "exited 0 and asserted success". Nothing is required; a script with
no marker is judged on its exit code alone.

## 7. Elevation

Scripts run with whatever privileges the agent has. Installs and uninstalls need
administrator, so the agent needs to be elevated — without a UAC prompt, and without
becoming a service.

**Mechanism: a Scheduled Task, triggered at logon, with "Run with highest privileges".**
That launches the existing executable elevated and silently. `--install-autostart` and
`--uninstall-autostart` create and remove it via `schtasks`, and require elevation
themselves — a one-time setup step per machine.

**The manifest is deliberately *not* changed to `requireAdministrator`.** That would
prompt on every launch, including the logon launch, which is precisely what this avoids.

**Precondition, and it is absolute:** "highest privileges" elevates the logged-in user's
*own* token. If that user is not a local administrator there is no administrator token to
elevate to, and the task runs unelevated. This design assumes lab users are local
administrators, which was confirmed. On a machine where they are not, scripts needing
administrator will fail — legibly, per below, but they will fail. No configuration
changes that; it is the security model.

**The agent detects its own state** with `GetTokenInformation`/`TokenElevation` and:

- logs it in the startup banner, elevated or not
- shows a warning in the detail window when not elevated, saying scripts may fail
- advertises `Capability::Elevated` on the announce, so the **server** knows before
  dispatching

The last is what makes the failure legible rather than mysterious: the Scripts tab can
mark un-elevated hosts in the target list, and warn before running rather than producing
a column of access-denied afterwards.

## 8. Server side

### The script picker

A configured root path — a UNC share — presented as a tree. **Folders are the
categories**; no separate taxonomy, no metadata file. Only `*.ps1` is listed. The tree is
read on demand and refreshed on a button, not watched: a share can be slow or briefly
unavailable, and that must never stall the console.

Selecting a script shows its content read-only, so the operator sees what they are about
to run on a hundred machines.

If the share is unreachable the tab says so and stays usable for ad-hoc, rather than
presenting an empty tree that looks like an empty share.

### Ad-hoc mode

A text area, as a second mode of the same tab. Dispatch is identical — the share path is
simply where a body came from. Runs record `(ad-hoc)` as the script name and keep the
body verbatim, so history is complete either way.

### Host selection

A checkable list of the fleet with **Select all** and **Clear**, defaulting to whatever
the Fleet tab has selected. Hosts that cannot comply are shown as such — not enrolled,
not elevated, not `Online` — and are unchecked by default, though the operator may
override.

The number of selected hosts is displayed next to Run. The blast radius should be
readable without counting checkboxes.

### Live run view

A run appears as soon as Run is pressed, with every target `Pending`, and updates in
place as results arrive. Selecting a target shows its output, exit code and reported
message.

Aggregate counts — *12 completed, 2 failed, 1 no response* — sit at the top, because on a
large run the counts are what is read and the rows are what is drilled into.

### History and cleanup

Runs are persisted in the server's config directory, one file per run in a `runs/`
subdirectory. One file per run rather than a single growing document: a run is written
once and never mutated afterwards, deletion is a file delete, and a corrupt file costs
one run rather than all of them.

Cleanup is explicit and operator-driven, per the requirement: **Delete** on a selected
run, and **Delete runs older than…** with a date. No automatic pruning — silently
discarding an audit trail is worse than a large directory.

## 9. Logging

Every dispatch and every outcome is logged on both ends, at `info`. This is the audit
trail for remote execution and is exempt from the "nothing periodic" rule only because
none of it is periodic:

- **Server:** run created (script name, host count), each dispatch, each result, run
  completed with its tally.
- **Client:** command received (run_id, script name), refusal with reason, execution
  started, finished with exit code and duration.

A refusal logs its reason on the client even though the server also records it, because
the machine's own log is where somebody looks when a specific PC is not doing what it
was told.

## 10. Testing

The wire format, the run state machine and the result parsing are all pure and testable
without DDS:

- `ScriptCommand`/`ScriptResult` codec round-trips, including a truncated buffer
- the run state machine: every target reaching every terminal state, including the
  deadline path
- `LM-RESULT:` parsing — present, absent, malformed, several lines, and a line that
  merely looks like one
- dedupe: the same `run_id` delivered twice runs once
- a fake `IScriptRunner` drives the client path with no PowerShell involved

Under the integration gate, one end-to-end case: a real command over a real DDS domain
to a client running a trivial real script, asserting the result comes back.

Deliberately untested: `schtasks` registration and the elevation check, which are
environment, not logic. Both are kept to a few lines behind an interface.

## 11. Phasing

The tab is the deliverable, but it lands in two pieces that each work:

1. **Dispatch and results** — capabilities, both topics, the runner, ad-hoc mode, the
   host list, the live run view. Ad-hoc first because it needs no share, so the whole
   path is exercised end to end before any file browsing exists.
2. **The share and history** — the folder tree, script preview, persistence and cleanup.

Elevation (§7) rides with the first piece, since without it the feature does not do what
it is for.

## 12. Risks and future hardening

**An unauthenticated domain is the weak point.** The opt-in bounds which machines listen;
it does nothing about who may speak. If this feature is deployed beyond a trusted
network, DDS Security or a signed command envelope becomes necessary rather than
optional.

**Scripts are not verified.** The server sends whatever the share contains. Somebody with
write access to the share has code execution on the fleet, at administrator. Hashing
scripts at publish and recording the hash in the run history would at least make a change
visible after the fact; signing would prevent it. Neither is in this phase, and the share
ACL is doing all the work in the meantime.

**A run is not transactional across hosts.** Half the fleet can succeed and half fail;
there is no rollback. That is inherent — this is not a distributed transaction — but the
word "transaction" was used in the original request, so it is worth being explicit that
the grouping is for *observation*, not atomicity.

**Timeouts are per host, not per run.** A run has no overall deadline; a slow host
finishes late without failing the others. The run's completion is simply when every
target has reached a terminal state.
