# Project Overview

## 1. What problem does this solve?

This project is a solver for a **corporate employee transport routing problem**,
built for the **Kriti 26** optimisation software competition. It is a variant of
the classic **Vehicle Routing Problem with Time Windows (VRPTW)**, specialised
for the "morning commute to a single office" scenario.

Concretely: every morning a company must pick up a set of employees from their
individual home locations and drop them all at a **single common office**. It
owns a **heterogeneous fleet** of vehicles (different capacities, speeds,
per-km costs and categories). The solver must decide:

1. **Which vehicle** serves each employee.
2. **In what order** each vehicle visits its assigned pickups.
3. **How many trips** each vehicle runs (a vehicle can do multiple back-to-back
   trips — pick up a group, drop them at the office, then go out again).

…all while **minimising total operating cost** and **respecting every
constraint** (see [CONSTRAINTS.md](CONSTRAINTS.md)).

### Key characteristics that make it a *specialised* VRPTW

- **Single common destination (the office).** Every route ends at the office
  (the `END` sentinel). There is no per-employee drop routing — all passengers
  on a trip share the same drop event at the office.
- **Directional time windows.** Each employee has an `earliest_pickup`
  (the vehicle may wait but not pick up before it) and a `latest_drop`
  (the office arrival must be no later than this).
- **Multi-trip vehicles.** A vehicle chains trips; its time *and* physical
  location carry over between trips (it starts trip 2 from wherever it ended
  trip 1 — usually the office — not by teleporting back to its depot).
- **Rich preferences.** Employees can require a *premium* vehicle and can demand
  to ride *alone* (single), among other sharing preferences.
- **Priorities & delay budgets.** Employees carry a priority (1 = highest).
  When the plan is over-constrained, higher-priority employees are protected and
  each priority level has a "maximum tolerable delay" budget.

## 2. Objective — what "best" means

Every route is scored by a **unified weighted cost function** (see
`config.h::calc_route_cost`):

```
route_cost = W_COST · (distance_km · cost_per_km) + W_TIME · time_minutes
```

- `distance_km · cost_per_km` is the monetary operating cost of the route.
- `time_minutes` is the route's total elapsed duration (office arrival − start).
- `W_COST` and `W_TIME` are objective weights read from the test-case metadata
  (`objective_cost_weight` / `objective_time_weight`); they default to
  `0.6 / 0.4` if absent.

The **global objective** is the sum of all route costs across all vehicles, with
a very large penalty (`BIG_M = 1e7`) per **unrouted employee** so that the
optimiser always prefers routing everyone before shaving cost. A schedule that
puts two overlapping trips on one vehicle is treated as fully infeasible (same
penalty tier as unrouted employees). See [ALGORITHMS.md](ALGORITHMS.md#scoring).

## 3. The solution pipeline at a glance

```
 input.json
     │
     ▼
┌─────────────────────────────┐
│ Stage 1: Solomon I1          │  Build a feasible initial solution fast.
│ constructive heuristic       │  (greedy cheapest-insertion, time-window aware)
└─────────────────────────────┘
     │
     ▼
┌─────────────────────────────┐
│ Stage 2: Infeasibility       │  Anyone left unrouted? Relax constraints in
│ resolver (binary search)     │  tiers and find the *minimum* deadline slack
│                              │  that makes them routable. Nobody is dropped
│                              │  if any vehicle exists.
└─────────────────────────────┘
     │
     ▼
┌─────────────────────────────┐
│ Stage 3: ALNS metaheuristic  │  Iteratively destroy & repair the solution
│ (Adaptive Large Neighbourhood│  under simulated-annealing acceptance to
│  Search) + local search      │  drive the cost down. Multi-restart.
└─────────────────────────────┘
     │
     ▼
 output.json  (+ human-readable console report)
```

The complete data structures and control flow are in
[IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md); the maths of each stage
is in [ALGORITHMS.md](ALGORITHMS.md).

## 4. The three models

The repository ships **three independent, standalone CMake projects**. They
share almost all code; they differ only in **how strictly ride-sharing
preferences are enforced** (and a couple of tuning knobs). This lets you compare
the cost impact of the constraints:

| Model | Directory | Sharing enforcement |
|-------|-----------|---------------------|
| **Hybrid (final)** | `optimization-Hybrid_final/` | Only `single` is strict; `double`/`triple` allow any co-riders |
| **All-constraints** | `optimization-All_constarint/` | `single`=1, `double`≤2, `triple`≤3 seats — strict |
| **No-constraint** | `optimization-No_Constraint/` | Sharing ignored entirely (capacity is the only limit) |

Full details, including the exact code differences, are in
[MODELS_DOCUMENTATION.md](MODELS_DOCUMENTATION.md).

## 5. Two executables

- **`velora`** — the batch solver. `velora input.json output.json [--debug]`.
- **`dynamic_insert`** — a real-time tool that takes an *already solved* plan
  plus a file of *new* employees and slots them into the existing routes
  (or opens new trips) without re-optimising from scratch. See
  [DYNAMIC_INSERTION.md](DYNAMIC_INSERTION.md).

## 6. Example result

Running the Hybrid model on `TC02` (12 employees, 5 vehicles) routes **all 12
employees** and reports an **optimised cost of ≈ 674** against a **baseline of
4945**, i.e. an **~86% saving** versus the naive per-employee baseline. (The
baseline is the cost of sending each employee individually, supplied in the
test-case `baseline` array.)
