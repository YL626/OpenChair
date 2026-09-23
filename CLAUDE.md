# wheelchair-diagnostic-reader: team rules

## Project purpose

Build a handheld Raspberry Pi wheelchair diagnostic reader for CS370.

The device:
- runs on a Raspberry Pi Zero 2 W with Linux
- uses at least two cooperating physical sensors
- reads a wheelchair controller's flashing diagnostic LED
- validates sensor placement using a proximity/distance sensor
- decodes the captured flash pattern using software written by us
- looks up the resulting controller-specific diagnostic code
- displays a short, human-readable explanation
- supports manual diagnostic-code entry as a fallback
- operates without cloud services or runtime AI

The final product must satisfy all CS370 term-project guardrails,
including two below-application-layer mechanisms, fault tolerance,
quantitative evaluation, and a 48-hour unattended soak test.


## Hardware

Main computer:
- Raspberry Pi Zero 2 W
- Raspberry Pi OS / Linux
- 40-pin GPIO header

Sensor A:
- ALS-PT19 optical sensor
- purpose: capture wheelchair diagnostic LED brightness over time
- analog output passes through an external ADC such as MCP3008

Sensor B:
- VL6180X time-of-flight proximity/distance sensor
- purpose: verify sensor placement and detect movement during capture

Sensor cooperation:
- optical readings are not accepted independently
- distance/stability measurements participate in capture validation
- unstable or invalid positioning causes the optical capture to be
  rejected or retried
- the final diagnostic decision therefore depends on both sensors

Interface:
- 2–3 inch color display
- UP button
- DOWN button
- ENTER button
- physical power control
- manual code-entry fallback

Power:
- internal rechargeable battery for handheld use
- regulated 5 V supply appropriate for Zero 2 W
- device must also support continuous external power for soak testing


## Architecture

Systems core is written in C23.

Likely processes:

sensor-light
    captures optical/ADC samples

sensor-distance
    captures proximity/distance measurements

analysis
    fuses sensor state
    segments flashes
    measures pulse timing
    validates repeated patterns
    decodes controller-specific fault pattern

storage
    persists events / measurements / diagnostic results

ui
    handles screen and three-button interface

supervisor
    monitors processes
    detects failures
    restarts recoverable children
    reports degraded operation

Exact process boundaries may change in DESIGN.md, but graded mechanisms
must remain in the C systems core.


## Graded OS mechanisms

At least two mechanisms from CS370 Section 3.2 must be implemented by us,
explicitly justified, and quantitatively measured.

Current candidates:

### Mechanism E — multi-process architecture

Separate sensor/capture components into processes and aggregate data
using explicit IPC.

Requirements:
- defined IPC mechanism
- supervisor process
- sensor process may be killed or fail without taking down whole system
- child failures must be detected
- degraded state must be reported
- restart/recovery behavior must be logged
- no orphaned file descriptors across restart

### Second mechanism — NOT YET FROZEN

Primary candidates:

D — custom storage layer
- append-only log and/or ring buffer
- explicit batching policy
- explicit fsync policy
- recovery after interrupted write/power loss

or

B — interrupt-driven input
- implement interrupt/event-driven path
- implement polling comparison
- measure CPU utilization and event latency for both

Do not silently choose or replace a graded mechanism.
Changes require updating DESIGN.md and team agreement.


## Hard constraints

- Claude Code is required during development.
- Claude, ChatGPT, LLM APIs, cloud inference, and pretrained diagnostic
  models are forbidden in the running product.
- All detection, filtering, signal processing, sensor fusion, state
  machines, decoding, and diagnosis logic must be written by us.
- Product operation must not depend on Internet access.
- The device must continue to function with networking unavailable.
- No graded OS mechanism may be hidden inside Python or another
  high-level interface layer.
- Systems core must be C17 or C23. Use C23 unless course tooling
  requires otherwise.
- Compile with:
    -Wall -Wextra -Werror
- Treat compiler warnings as build failures.
- All allocations must be checked.
- All relevant syscall/error paths must be handled.
- Hardware failures must produce an honest degraded state rather than
  fabricated data or a guessed diagnosis.
- An unrecognized or unreliable flash pattern must return
  UNKNOWN / RETRY / MANUAL ENTRY, never an invented diagnosis.
- Do not add runtime AI or network-dependent diagnosis as a shortcut.


## Diagnostic intelligence

A single brightness threshold is not sufficient.

The optical pipeline should include, as appropriate:
- baseline estimation
- filtering / noise handling
- adaptive thresholding
- hysteresis
- edge detection
- timestamps
- pulse-duration measurement
- inter-flash timing
- flash-group segmentation
- complete-sequence detection
- repeated-cycle agreement
- distance/stability validation
- confidence / rejection rules
- controller-specific decoding

Wheelchair diagnostic entries must come from verified documentation.
Do not invent codes, meanings, or supported models.

Manual entry is a fallback interface, not a substitute for the required
sensor pipeline.


## Fault behavior

Every hardware/software component needs a defined failure mode.

Examples:

ALS / ADC unavailable:
- mark optical sensor failed
- suspend automatic diagnosis
- log error
- UI reports sensor failure
- allow appropriate manual fallback

VL6180X unavailable:
- mark positioning validation unavailable
- do not silently claim a fully validated automatic reading
- log failure
- supervisor attempts recovery where appropriate

Sensor process crash:
- supervisor notices child death
- log PID/status/timestamp
- enter degraded state
- restart according to restart policy

Invalid optical capture:
- never guess
- log rejection reason
- ask user to reposition/retry/manual-enter

Storage failure:
- report/log as far as safely possible
- do not crash unrelated sensor components without reason

The final build must survive deliberate fault injection.


## Logging and soak requirements

The final system must survive at least 48 continuous hours unattended.

Logs must include:
- timestamped startup
- heartbeat at least once per hour
- process liveness
- RSS for each relevant process
- cumulative event counts
- sensor failures
- process deaths/restarts
- diagnostic attempts/results
- injected fault
- detection of injected fault
- degraded behavior
- recovery
- orderly final state

Logs used for grading must remain raw and unedited.

Begin shorter soak tests well before the final 48-hour run.


## Storage rules

Storage format and crash-consistency semantics must be explicit.

Never assume an SD-card write is durable merely because write() returned.

If Mechanism D is selected:
- document record format
- write batching
- fsync policy
- corruption detection
- recovery behavior
- retention/rotation policy

Do not allow soak logs to grow without bound.


## Build and test commands

TODO: replace these with real commands once repository layout exists.

Build:
    make

Tests:
    make test

Warnings:
    build must be clean under -Wall -Wextra -Werror

Sanitizers:
    make asan

Valgrind:
    make memcheck

Deploy:
    make deploy PI=<pi-host>

Short soak:
    make soakcheck

A change is DONE only when the applicable build/tests pass and the
relevant evidence is shown.


## Evidence discipline

Hardware claims require hardware evidence.

When debugging, provide actual evidence such as:
- exact program output
- dmesg
- journal/log excerpts
- /proc information
- timing measurements
- GPIO/interrupt counters
- sensor traces
- ADC samples
- CPU utilization
- RSS
- logic-analyzer output when available

Do not ask Claude to infer a hardware failure from a vague verbal
description when measured evidence is available.

Prefer:
"Here are 500 timestamped optical samples and the measured distance
stream. Identify why sequence segmentation rejected cycle 4."

Over:
"The sensor isn't working."


## Evaluation requirements

Claims must be quantitative.

Measure at minimum:
- relevant detection/event latency distributions
- behavior under idle CPU
- behavior under loaded CPU
- CPU utilization
- RSS
- resource behavior throughout soak
- one domain metric such as diagnostic-pattern detection accuracy
- fault-injection behavior
- the required comparison experiment for each graded OS mechanism

Report distributions where required, not only averages.


## Ownership

TODO once partner roles are finalized.

Partner A:
- first-author ownership:
  - ...

Partner B:
- first-author ownership:
  - ...

Shared:
- integration
- supervisor/interface where appropriate
- architecture review

Ownership means first authorship and demo-day answerability,
not exclusive permission to understand or modify the subsystem.

Both partners must understand the other side well enough to answer a
cross-boundary question.


## Code style

Systems core:
- C23
- -Wall -Wextra -Werror
- small functions with explicit responsibilities
- check allocations
- check syscall returns
- explicit resource ownership
- no silent error swallowing
- cleanup paths must release acquired resources
- avoid unnecessary global mutable state
- document concurrency invariants
- document IPC message semantics
- document ownership/lifetime of buffers and file descriptors

Python or another language may be used only for allowed tooling/UI
layers if useful.
No graded mechanism may live solely in that layer.


## Claude Code workflow

For significant changes:
1. explore existing code first
2. identify relevant specification/design constraints
3. plan before editing
4. make the smallest coherent change
5. run tests/build
6. inspect real output
7. perform adversarial review
8. receive human partner review
9. commit only from a known-good state

Do not rewrite unrelated code.

Do not weaken, delete, or skip a test merely to make a change pass.

For algorithmic or multi-file work, present a plan before implementation.

For hardware bugs, reason from actual measurements.


## Git / collaboration

- At least 40 meaningful commits across the project.
- Both partners must have substantial authorship.
- Work from separate clones / personal Claude sessions.
- Partner reviews are required.
- Avoid giant end-of-project commits.
- Commit messages should identify the milestone/change clearly.

Example:
    M2: add MCP3008 optical capture path


## Claude transcript requirements

Each partner preserves their own raw Claude Code .jsonl transcripts.

Copy them into the repository/submission artifacts at every milestone
from M1 through M5 rather than relying on local retention.

Do not:
- summarize instead of submitting them
- retype them
- edit them
- combine both partners into one Claude login/session history


## Files and documentation

CLAUDE.md
    persistent team/development rules

PROBLEM.md
    user, problem, why physical device, sensors, mechanisms, risk

DESIGN.md
    architecture, rates, mechanism mapping, failure table, storage,
    evaluation plan, ownership, constraints/substitutions, AI-use plan

EVALUATION.md
    final quantitative experiments and limitations

PROMPTLOG.md
    selected annotated Claude episodes

REFLECTION.md
    individual reflection

README.md
    clean-Pi setup, build, deploy, and run instructions

Do not turn CLAUDE.md into DESIGN.md.
Detailed design rationale belongs in DESIGN.md.


## Current major project risk

Primary technical risk:
reliably extracting a wheelchair fault sequence from a real diagnostic
LED across different brightness, positioning, ambient-light, and
controller conditions.

Prototype this early.

The software must be able to reject an uncertain reading instead of
forcing a diagnosis.


## Current unresolved decisions

- exact second graded OS mechanism: D or B
- exact MCP3008/optical circuit
- final LCD model/interface
- final rechargeable power board / 5 V regulation
- final IPC mechanism
- exact supported wheelchair/controller families for first release
- capture sampling rate
- confidence/rejection criteria
- final process ownership between partners

Do not treat unresolved decisions as settled facts.