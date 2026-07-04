# Algorithms & Logic

This is the deep-dive on every algorithm in the solver. It covers the distance/
time model, the objective, and each of the three pipeline stages
(Solomon I1 → infeasibility resolver → ALNS), plus the local-search operators.

---

## 0. Geometry & time model (`geo.cpp`, `time_utils.cpp`)

### Distance — `get_dist`

Two sources, in priority order:

1. **Override table (real map distances).** If the test case sets
   `allow_external_maps = true`, `io.cpp` loads each employee's `distances` map
   (values in **metres**) into a global override table. `get_dist(Stop a, Stop b)`
   first looks up the pair `(a.emp_id, b.emp_id)` (either direction; the `END`/
   office node is normalised to the key `"drop"`) and returns
   `metres / 1000` km if found.
2. **Haversine great-circle fallback.** Otherwise the distance is computed from
   lat/lng:
   ```
   a = sin²(Δlat/2) + cos(lat1)·cos(lat2)·sin²(Δlng/2)
   d = 2R·asin(√a),   R = 6371 km
   ```

### Travel time — `travel_minutes`
```cpp
travel_minutes(dist_km, speed_kmh) = round( (dist_km / speed_kmh) * 60 )
```
Speed is per-vehicle (`avg_speed_kmph`). A non-positive speed returns ∞.

### Clock — `time_utils`
Times are integers = **minutes since midnight**. `parse_time("08:15") = 495`;
`format_time(495) = "08:15"`. The loader also accepts **decimal day-fractions**
(e.g. `0.3333… = 08:00`) which it converts via `round(fraction·24·60)`.

---

## 1. Stage 1 — Solomon I1 constructive heuristic (`heuristic.cpp`)

Builds an initial feasible solution using Solomon's classic **I1 sequential
insertion** criteria, adapted with a **global regret** term and a
tightness-based processing order.

### Step 0 — initialise one empty trip per vehicle
Each vehicle gets a `Route` = `[START@current_loc, END@office]`, with the
vehicle's `available_time` (defaulting to 08:00) as the start time.

### Step 1 — processing order (`get_sorted_indices_by_tightness`)
Employees are inserted in order of:
1. **Tightest time window first** (`due_time − ready_time` ascending),
2. then **higher priority** (lower number) first,
3. then **earliest ready_time**.

Rationale: the hardest-to-place riders compete for the good positions before the
easy ones fill them.

### Step 2 — per-employee insertion (three phases)

For each employee, the heuristic finds the best (vehicle, route, position):

**Phase 1 — feasible positions & best `c1` per route.**
For every current trip, try every insertion position. Each candidate is validated
by `simulate_insertion_and_check` (full timeline resim + due-time check). Among
feasible positions, keep the one with the lowest **c1 insertion cost**:

```
c11 = d(i,u) + d(u,j) − μ·d(i,j)          // extra distance of inserting u between i and j
c12 = b_u − prev_departure                 // extra delay introduced
c1  = α1·c11 + α2·c12
```
with defaults `α1 = 1.0`, `α2 = 0.0`, `μ = 1.0` (`config.cpp`).

**Phase 2 — global regret.** Across *all* routes' best-c1 values, find the global
best and second-best insertion cost. `regret = second_best − best` (0 if only one
option). Regret measures how much you'd lose if the single best route became
unavailable — high-regret employees deserve their best slot now.

**Phase 3 — pick the route by `c2`.**
```
c2 = λ·d(depot, u) − best_c1 + 0.5·regret     (λ = 1.0)
```
The route with the **maximum c2** wins. The employee is inserted there, the
route's `max_capacity` is shrunk if the employee is `SINGLE`, times/cost are
recomputed, and the vehicle's `available_time`/`current_loc` advance to the
office.

### Step 3 — new-trip fallback
If no existing trip can feasibly take the employee, the heuristic opens a **new
trip** (Trip #2+) on the first compatible vehicle. Subsequent trips **start from
the office** (the hub), not the vehicle's original depot. If even that fails, the
employee is left unrouted with a recorded reason in `g_unrouted_reason`.

### Compatibility gate — `check_compatibility`
Before scoring, a (vehicle, route, employee) triple must pass:
- Category: `veh_pref == PREMIUM` ⇒ vehicle must be premium.
- Capacity: `current_capacity + 1 ≤ min(route.max_capacity, emp_limit)` where
  `emp_limit = 1` for `SINGLE`, else full vehicle capacity (Hybrid). *(This is
  where the model variants diverge — see MODELS_DOCUMENTATION.md.)*

---

## 2. Stage 2 — Binary-search infeasibility resolver (`infeasible_handler.cpp`)

After Stage 1, some employees may still be unrouted (their deadline could not be
met under strict rules). This stage guarantees that **every employee any vehicle
can physically carry gets routed**, by relaxing the deadline the *minimum* amount
and recording that relaxation.

### Per-priority delay budget
Each priority level has a "maximum tolerable delay" in minutes, parsed from
metadata keys `priority_<k>_max_delay_min` (`build_priority_budget`). Defaults if
absent: `{p1:9, p2:6, p3:5, p4:3, p5:2}`. (Note: the metadata in the sample test
cases specifies larger, increasing budgets, e.g. p1=5…p5=30.)

### Minimum-slack search (`binary_search_slack`)
For a given employee and route, "slack" `s` means "allow office arrival up to
`due_time + s`". The routine:
1. Checks feasibility at `max_slack` (if impossible even then, returns −1).
2. **Binary-searches** the smallest `s ∈ [0, max_slack]` for which some insertion
   position is feasible (respecting all *other* passengers' true deadlines).

`simulate_office_arrival` computes the office arrival for a trial insertion
without imposing the due-time check, so the search can probe how much slack is
needed.

### Three relaxation tiers (applied in order until routed)
Employees are processed sorted by slack-needed (then priority). For each:

| Tier | Vehicle rule | Slack allowed |
|------|--------------|---------------|
| **1** | strict category (`vehicle_ok_strict`) | the employee's effective budget (`max(needed, priority_budget)`) |
| **2** | strict category | **unlimited** (1440 min) — deadline violated but category respected |
| **3** | **relaxed** category (any vehicle) | unlimited — last resort, only when the preferred category has no vehicles at all |

Within a tier, `try_route_employee` first tries **Path A** (insert into an
existing route at the minimum feasible slack, cheapest delta wins), then
**Path B** (open a brand-new appended trip, choosing the vehicle that reaches the
pickup earliest → least passenger wait). When routed, the employee's `due_time`
is advanced by the applied slack and the method/slack is recorded.

The resolver prints an analysis table and a resolution report
(`print_infeasible_report`) documenting exactly which employees needed a budget
override and by how much — so every constraint relaxation is auditable.

---

## 3. Stage 3 — ALNS (`alns.cpp`)

**Adaptive Large Neighbourhood Search** with simulated-annealing acceptance. It
repeatedly *destroys* part of the solution and *repairs* it, keeping good moves,
occasionally accepting worse ones to escape local optima, and adaptively favouring
the operators that have been working.

### The single feasibility authority — `simulate_route`
Every candidate route is re-simulated from scratch: it validates employees, sets
pickup locations, accumulates the tightest share cap, computes all arrival/
service/departure times, enforces every passenger's due-time, checks capacity,
and recomputes distance and cost. No stale cached checks are trusted. An employee
lookup map is built **once per iteration** and threaded through all operators to
avoid rebuilding it per call.

### Scoring — the objective (`score`)
```
score = unrouted_count · BIG_M + Σ vehicle.total_cost          (BIG_M = 1e7)
```
If any vehicle has **temporally overlapping trips**, the solution is treated as
fully infeasible: `score = (unrouted_count + 1) · BIG_M`. Thus the optimiser
always prefers (a) routing everyone, then (b) no overlaps, then (c) low cost.

### Destroy operators (remove `q` employees)
`q` is drawn uniformly from `[min_remove, max_remove]`, clamped to ~⅓ of the
fleet-wide employee count.

| Operator | Idea |
|----------|------|
| **Random** | remove `q` random routed employees |
| **Shaw** (relatedness) | pick a seed, remove employees most *related* to it by `0.5·geo + 0.3·(time/60) + 0.2·priorityΔ`; biased-random pick via `pow(U,6)` |
| **Worst** | remove the employees whose removal saves the most cost (greedy per-route gain), biased-random via `pow(U,3)` |
| **Zone** (cluster) | remove the `q` employees geographically nearest a random seed |

### Repair operators (re-insert removed employees)
| Operator | Idea |
|----------|------|
| **Priority-greedy** | insert in priority order, each at its globally cheapest feasible spot (`insert_anywhere`) |
| **Regret-2** | insert the employee with the largest gap between its best and 2nd-best insertion first |
| **Regret-3** | same, using best vs 3rd-best (looks further ahead) |

`insert_anywhere` first tries the cheapest delta into any existing route (never
introducing a trip overlap), and if none fits, opens a new trip on the cheapest
compatible vehicle. Regret repair adds a priority bonus `(5 − priority)·10` so
important riders are placed first, and uses a sentinel regret for employees no
existing route can take (so the new-trip fallback still fires).

### Acceptance — simulated annealing
```
delta   = trial_score − current_score
accept  if delta ≤ 0  OR  U(0,1) < exp(−delta / T)
```
- **T0 auto-calibration:** `T0 = 0.05·init_cost / −ln(0.8)` (accept a 5%-worse
  move with P≈0.8 initially), floored at 500 if tiny.
- **Cooling:** `T ← T · cooling` each iteration; the configs pick `cooling` so
  the final temperature is ~2% of T0 over the iteration budget.

### Adaptive operator weights
Operators are chosen by roulette-wheel over weights. Each is rewarded by tier:
`W_NEW_BEST = 9`, `W_BETTER_CURR = 3`, `W_ACCEPTED = 1`, `W_REJECTED = 0`. Every
`SEGMENT_LEN = 100` iterations the weights update via exponential smoothing
`w ← (1−ρ)·w + ρ·(reward/uses)` with `ρ = 0.18`, floored at 0.1.

### Early stopping & multi-restart
A run stops after `no_improve_stop` consecutive non-improving iterations. After
the loop, a **deep local search** polishes the best solution. `run_alns` performs
`n_runs` independent restarts (each from the same post-Stage-2 solution, fresh
seed) and keeps the best. All seeds are logged for reproducibility.

### Two effort profiles (`main.cpp`)
| Mode | `alns_depth` | iterations | no_improve_stop | remove range | cooling |
|------|-------------|-----------|-----------------|--------------|---------|
| **INSTANT** | 2 (default) | 350 | 120 | 3–10 | 0.988885 (~2% of T0) |
| **QUALITY** | 1 | 6000 | 800 | 4–20 | 0.999239 (~2% of T0) |

---

## 4. Local search operators (`alns.cpp`)

Run after repair (optionally) and as a deep pass on the best solution. All moves
are validated by `simulate_route` and rejected if they introduce a trip overlap.

| Operator | Scope | What it does |
|----------|-------|--------------|
| **2-opt** | intra-route | reverses a stop sub-segment to untangle a route; keeps it if cheaper |
| **Relocate** | inter-route | moves one employee from its route to a cheaper route (net gain = source saving − destination delta) |
| **Swap** | inter-route | exchanges employee A (route X) with employee B (route Y) when the *pair* trade helps even if neither single move would |
| **Or-opt (1/2/3)** | inter-route | moves a contiguous chain of 1, 2, or 3 stops from one route to another |

The deep pass runs 2-opt on every route, then repeated relocate passes, then swap
passes, then or-opt for chain lengths 1→3, refreshing vehicle totals throughout.

---

## 5. Numerical constants reference

| Constant | Value | Where | Meaning |
|----------|-------|-------|---------|
| `BIG_M` | 1e7 | alns.cpp | penalty per unrouted employee / infeasible overlap |
| `SEGMENT_LEN` | 100 | alns.cpp | adaptive-weight update interval |
| `RHO` | 0.18 | alns.cpp | adaptive-weight smoothing factor |
| `W_NEW_BEST/BETTER/ACCEPTED/REJECTED` | 9/3/1/0 | alns.cpp | operator reward tiers |
| `SHAW_DIST/TIME/PRI_W` | 0.5/0.3/0.2 | alns.cpp | Shaw relatedness weights |
| `ALPHA1, ALPHA2` | 1.0, 0.0 | config.cpp | Solomon c1 weights (c11 vs c12) |
| `LAMBDA, MU` | 1.0, 1.0 | config.cpp | Solomon c2 depot weight / c11 detour weight |
| `W_COST, W_TIME` | 0.6, 0.4 (default) | config.cpp | objective weights (overridden by metadata) |
| `SERVICE_MIN` | 0 | config.cpp | dwell per pickup (All-constraints uses 2 in its constructor) |
| `R` | 6371 km | geo.cpp | Earth radius for haversine |
