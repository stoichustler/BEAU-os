"""Stable AI scheduler dataset schema shared by collection and training."""

SCHEMA_VERSION = "AI_SCHED"
FEATURE_COLUMNS = (
    "period_us", "admission_ppm", "runqueue", "active_mask",
    "pool_budget_us", "cs_delta", "resched_delta",
    "action_entry_count", "action_total_budget_us", "action_min_budget_us",
    "action_max_budget_us", "action_span_us",
)
REQUIRED_COLUMNS = ("ticks", "pcpu", "reward") + FEATURE_COLUMNS


def validate_columns(columns):
    missing = [column for column in REQUIRED_COLUMNS if column not in columns]
    if missing:
        raise ValueError("missing dataset columns: " + ", ".join(missing))
