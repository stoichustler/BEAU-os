# AI Scheduler Training Flow

The BEAU shell emits `AI_SCHED` records through `schedai snapshot`. Capture
those records from the platform under evaluation, then create a base dataset:

```sh
python3 sdk/ai-sched/collect.py --input serial.log --output snapshots.csv
```

Reviewed training data extends each row with `action_entry_count`,
`action_total_budget_us`, `action_min_budget_us`, `action_max_budget_us`,
`action_span_us`, and a scalar `reward`. The reviewed action is a bounded list
of `{vmid, budget_us}` entries, not a free-form model output. A future approved
experiment may train and export a model:

```sh
python3 sdk/ai-sched/model/train.py --input reviewed.csv \
  --output sdk/ai-sched/model/ai_sched_model.h
python3 sdk/ai-sched/model/verify.py \
  --manifest sdk/ai-sched/model/ai_sched_model.json
```

The host advisor is deliberately untrained by default:

```sh
make -C sdk/ai-sched advisor
sdk/ai-sched/out/ai_sched_advisor
```

It prints `model-unavailable observe-only`. No command in this directory can
change a BEAU CBS reservation. `HC_AI_SCHED` validates a proposal against the
platform DTS envelope and records an observe-only result.

## Training Completion TODO

The files above provide an offline starting point, not a completed training and
deployment pipeline. Complete the following gates in order before enabling a
model in the VM0 advisor:

1. Extend `HC_AI_SCHED(SNAPSHOT)` data collection with reviewed runtime
   signals: VM/vCPU runtime, wait-latency distribution, runqueue depth,
   context-switch deltas, and CBS budget/deadline consumption. Preserve the
   snapshot timestamp and pCPU identity for every sample.
2. Produce reviewed candidate actions as bounded `{vmid, budget_us}` lists and
   an explicit reward. Automate trace-to-dataset conversion where possible, but
   retain human review of workload labels, invalid samples, and safety bounds.
3. Add offline evaluation scripts and acceptance thresholds. They must compare
   a candidate model with the static CBS baseline, reject envelope violations,
   cover overload and stale-snapshot cases, and retain reproducible reports.
4. Export the approved bounded model through emlearn and verify its manifest,
   feature schema, model depth, and dataset hash. A model must be rejected when
   any generated ABI or schema differs from the VM0 advisor contract.
5. Deploy a reviewed generated header into Zephyr `beau_ai_model.h` through an
   explicit release step. The deployment flow must retain the previous model,
   record the model version, and support rollback without changing BEAU EL2
   code or DTS scheduler limits.
6. Keep the deployed model in observe-only mode until a separate runtime
   validation proves the advisor's timing budget, proposal rejection behavior,
   reset re-registration behavior, and measured benefit. An APPLY capability
   requires a separately designed and approved state gate.
