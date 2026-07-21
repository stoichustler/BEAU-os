# AI Scheduling SDK

This directory contains host-side support for the AI-assisted scheduling
experiment. It is not linked into the EL2 hypervisor image.

`emlearn/` is retained in full so the training-to-C generation path remains
available for learning and controlled local changes. A future advisor will use
only an audited generated C model and the bounded `HC_AI_SCHED` control contract.
It must not execute model inference in an EL2 scheduler path or bypass the
hypervisor admission, range, sequence, and rate-limit checks.

No trained model is included yet. Training data and the feature/action schema
must be reviewed before a generated model is added.

`collect.py`, `model/train.py`, and `model/verify.py` define the offline
training and C-export flow. `advisor/` builds a C99 host program that remains
observe-only until an audited generated model is deliberately selected.

## Current Limits

The current SDK provides collection, reviewed CSV input, decision-tree export,
and manifest verification only. It does not yet provide runtime trace coverage,
automatic reward labeling, offline comparison reports, automatic deployment to
the Zephyr VM0 advisor, model version rollback, or an APPLY path. The required
completion gates and deployment boundary are documented in `TRAINING.md`.
