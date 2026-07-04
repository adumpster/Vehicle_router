# Constraints Model

This document catalogues every constraint the solver enforces, how it is checked,
and how it can be relaxed. Constraints are validated by full timeline
re-simulation in three places that stay behaviourally consistent:
`heuristic.cpp` (`simulate_insertion_and_check`), `alns.cpp` (`simulate_route`),
and `dynamic_handler.cpp` (`resimulate`).

---

## 1. Route structure (hard, always)

- Every route must begin with a `START` stop and end with an `END` stop located
  at the **office** (`OFFICE`).
- Trip #1 of a vehicle starts at the vehicle's `current_loc` (its dataset start).
- Trip #2+ starts at the **office hub** (where the previous trip ended), at the
  vehicle's updated `available_time`.
- All passengers on a trip share a **single office drop event**; their
  `drop_time` is the trip's office arrival time.

## 2. Time-window constraints (hard by default, relaxable in Stage 2)

### Earliest pickup (`ready_time`)
The vehicle may arrive early but **cannot pick up before** `ready_time`:
```
begin_service = max(arrival_time, employee.ready_time)   // waits if early
```
This is never violated — the vehicle simply waits.

### Latest drop (`due_time`)
The trip's **office arrival must be ≤ `due_time`** for *every* passenger on the
trip (they all arrive together). This is the binding deadline constraint.

- In Stages 1 & 3 it is **hard**: any insertion violating it is rejected.
- In Stage 2 (infeasibility resolver) it can be **relaxed** by a minimum amount
  of "slack", found via binary search, in three tiers (see §7 and
  [ALGORITHMS.md](ALGORITHMS.md#2-stage-2--binary-search-infeasibility-resolver)).
  Any slack applied is recorded and reported.

## 3. Capacity constraint (hard, always)

A route's passenger count must not exceed the **effective capacity**:
```
effective_cap = min( vehicle.capacity, route.max_capacity, share_cap )
```
where `share_cap` is the tightest `get_global_share_limit()` among all passengers
currently on the route. `route.max_capacity` is recomputed after any removal
(`recompute_max_capacity`) so a departed `SINGLE` rider does not permanently pin a
route to capacity 1.

## 4. Vehicle-category constraint (hard)

Driven by the employee's `vehicle_preference` vs the vehicle's `category`:

| `veh_pref` | Allowed vehicles |
|------------|------------------|
| `premium` | **premium only** |
| `normal` | non-premium only *(Hybrid & All-constraints; enforced in `vehicle_ok`)* |
| `any` | any category |

The **No-constraint** variant drops the category checks from its hot paths.

## 5. Sharing-preference constraint (model-dependent)

This is the constraint that distinguishes the three variants. It is expressed
entirely through `get_global_share_limit(SharingPref)`:

| `sharing_preference` | Hybrid | All-constraints | No-constraint |
|----------------------|:------:|:---------------:|:-------------:|
| `single` | 1 (alone) | 1 | ignored (999) |
| `double` | 999 (share freely) | 2 | ignored (999) |
| `triple` | 999 (share freely) | 3 | ignored (999) |
| `any` | 999 | 999 | ignored (999) |

The cap is the **maximum passengers allowed on a route that carries such an
employee** — the route's `share_cap` is the minimum over all its passengers. So a
single `single` rider forces the whole trip to just that person. See
[MODELS_DOCUMENTATION.md](MODELS_DOCUMENTATION.md).

## 6. Trip-overlap constraint (hard, ALNS)

A single vehicle cannot run two trips at once. Two trips overlap if their
`[departure_time, arrival_time]` intervals intersect (`routes_time_overlap`);
back-to-back trips sharing exactly one boundary are allowed. ALNS treats any
overlapping schedule as fully infeasible (`score` penalty tier equal to having an
unrouted employee), and every insert/relocate/swap/or-opt move is rejected if it
would introduce an overlap (`would_overlap`).

## 7. Priority & delay budgets (Stage 2)

- **Priority** (1 = highest) breaks ties in insertion order and biases repair
  (regret repair adds `(5 − priority)·10`).
- **Delay budgets:** each priority level has a `priority_<k>_max_delay_min`
  budget (metadata). The infeasibility resolver first tries to route within
  `max(slack_needed, budget)` (Tier 1). If that fails it escalates to unlimited
  slack (Tier 2), then to relaxed category (Tier 3). Whenever the applied slack
  exceeds the budget, the result is flagged **OVERRIDE** / *budget overridden* in
  the report, so no silent constraint violation ever occurs.

## 8. Constraint hardness summary

| Constraint | Stage 1 (Solomon) | Stage 2 (Infeasibility) | Stage 3 (ALNS) |
|------------|:-----------------:|:-----------------------:|:--------------:|
| Route structure | hard | hard | hard |
| Earliest pickup | hard (wait) | hard (wait) | hard (wait) |
| Latest drop | hard | **relaxable (min slack, tiered)** | hard |
| Capacity | hard | hard | hard |
| Category | hard | hard (Tier 1–2) / **relaxed (Tier 3)** | hard |
| Sharing (per model) | hard | hard | hard |
| Trip overlap | n/a (single trip build) | avoided | **hard** |

## 9. Guarantee

With a non-empty fleet, **every employee that any vehicle can physically carry
will be routed** — Stage 2's tiered relaxation ensures this, documenting exactly
what (if any) deadline or category rule had to bend to achieve it. An employee is
only left truly unrouted if the fleet cannot serve them at all.
