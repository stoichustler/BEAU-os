# Benchmark

## CoreMark

The upstream CoreMark snapshot lives in `coremark/` and must remain unchanged.
It is pinned to upstream commit `1f483d5b8316753a742cbf5590caf5bd0a4e4777`.
`LICENSE.md` in that directory retains the upstream Apache-2.0 and CoreMark
acceptable-use terms.

BEAU retains the upstream sources unchanged and executes their algorithms with
one persistent, fixed context per pCPU. `coremark [iterations]` starts one
context per available pCPU and prints an asynchronous box-drawing report. A
worker that does not start or complete before its configured deadline is shown
as `unavailable` or `late`; its private context is retained until completion and
is never reused by a later generation. The reported duration includes BEAU
scheduler and guest contention, so it is an EL2 diagnostic result and not an
isolated official CPU score.

## Dhrystone

The Dhrystone 2.2a port is also a serialized EL2 diagnostic because its
upstream data is global. `dhrystone [initial-runs]` runs one pinned worker at a
time. A queued worker that misses its start deadline is cancelled before it can
enter the benchmark; a worker that has started remains owned by the command
until it completes. Idle, completed, error, and cancelled workers block outside
the scheduler runqueue, so Dhrystone has no CPU runtime impact while the command
is not active.


## RT Tests

TODO.


---

Hustle Embedded OS.
