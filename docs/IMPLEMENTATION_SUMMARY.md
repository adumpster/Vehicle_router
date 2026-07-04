# Implementation Summary

This document describes the core data structures, the end-to-end control flow of
the `velora` solver, and how a "solution" is represented and mutated between the
three optimisation stages. For the mathematics inside each stage, see
[ALGORITHMS.md](ALGORITHMS.md).

## 1. Core data structures (`include/types.h`)

### `Location`
```cpp
struct Location { double lat, lng; };
```
A WGS-84 coordinate. `{0,0}` doubles as a sentinel meaning "office / unset".

### `Employee`
```cpp
struct Employee {
    string id;                 // "E01"
    int priority;              // 1 = highest priority
    Location pickup, drop;     // drop is the common office for all employees
    int ready_time;            // earliest_pickup, minutes since midnight
    int due_time;              // latest_drop, minutes since midnight (office arrival deadline)
    VehicleCat veh_pref;       // PREMIUM | NORMAL | ANY_CAT
    SharingPref share_pref;    // SINGLE | DOUBLE | TRIPLE | ANY_SHARE
    bool is_routed;            // solver state: has this employee been placed?
    double baseline_cost;      // naive per-employee cost, for savings reporting
};
```

### `Stop`
```cpp
struct Stop {
    string emp_id;             // "E01", or the sentinels "START" / "END"
    Location loc;
    int arrival_time;          // when the vehicle reaches this stop
    int begin_service;         // max(arrival, employee.ready_time)  (waits if early)
    int departure_time;        // begin_service + SERVICE_MIN
    bool is_pickup;            // true for real employee pickups; false for START/END
};
```
A route is a list of stops. `START` = where the trip begins; `END` = the office.

### `Route` (one trip)
```cpp
struct Route {
    vector<Stop> stops;        // START -> pickup... -> END
    int current_capacity;      // passengers currently on board (== #pickup stops)
    int max_capacity;          // effective cap for this route (shrunk by SINGLE riders)
    double total_distance;     // km
    double total_cost;         // via calc_route_cost()
};
```

### `Vehicle`
```cpp
struct Vehicle {
    string id;
    double capacity;           // physical seats
    double cost_per_km;
    double speed_kmh;
    Location depot_loc;        // where the vehicle starts trip #1
    VehicleCat category;       // PREMIUM | NORMAL
    int available_time;        // minutes since midnight; advances as trips complete
    Location current_loc;      // physical location; advances as trips complete
    vector<Route> routes;      // MULTIPLE trips, chained in time & space
    double total_cost;         // sum of route costs
};
```

> **Multi-trip chaining.** `available_time` and `current_loc` are the crux of the
> multi-trip model. After a trip ends at the office, the vehicle's next trip
> begins *there* and *then* — the code explicitly comments that without this the
> vehicle would "teleport" back to its depot between trips.

### Enums
```cpp
enum VehicleCat  { PREMIUM, NORMAL, ANY_CAT };
enum SharingPref { SINGLE, DOUBLE, TRIPLE, ANY_SHARE };
```

## 2. What a "solution" is

A complete solution is just the `vector<Vehicle>` (each carrying its `routes`)
plus the `vector<Employee>` (each carrying its `is_routed` flag). Every stage
receives these two vectors **by reference** and mutates them in place. There is
no separate "Solution" object — the domain vectors *are* the solution.

Global side-channel state lives in `config.cpp`:
- `OFFICE` — the common drop `Location` (set from the first employee's drop).
- `W_COST`, `W_TIME` — objective weights (overwritten from JSON metadata).
- `g_unrouted_reason` — `map<emp_id, string>` explaining *why* someone is
  unrouted; surfaced in the console report and the output JSON.

## 3. End-to-end control flow (`src/main.cpp`)

```
main(argv):
  1. Parse args: input.json, output.json, optional --debug
  2. load_from_json_keep_root(input) → employees, vehicles, json_root
       - falls back to load_from_json() if the keep-root variant fails
  3. budget = extract_budget(json_root)          // per-priority delay budgets
  4. alns_depth = json_root.config["alns_depth"] // default 2 (INSTANT)
  5. W_COST / W_TIME  = json_root.metadata weights (default 0.6 / 0.4)
  6. STAGE 1: solve_solomon_insertion(employees, vehicles)
  7. STAGE 2: resolve_infeasible(employees, vehicles, budget)
              print_infeasible_report(...)
  8. STAGE 3: pick INSTANT (depth 2) or QUALITY (depth 1) config
              run_alns(employees, vehicles, cfg)
  9. display_report(vehicles, employees)         // console summary
 10. write_output_json(output, raw_input, vehicles, employees)
```

### Stage boundaries — what each stage guarantees to the next

| Stage | Input state | Output guarantee |
|-------|-------------|------------------|
| **1 · Solomon I1** | all `is_routed = false`, one empty trip per vehicle | a *feasible* assignment for as many employees as greedy insertion allows; the rest are marked unrouted with a reason |
| **2 · Infeasibility** | some employees may be unrouted | every employee that *any* vehicle can carry is now routed, by relaxing deadlines the minimum amount (recorded per employee) |
| **3 · ALNS** | a complete feasible solution | the same set of routed employees, but with total cost driven down; never increases the unrouted count (guarded by `BIG_M` penalty) |

## 4. Objective & feasibility — the two unified functions

Both are `inline` in `config.h`, so every module computes cost and capacity the
same way (this consistency is called out repeatedly in the code comments):

```cpp
double calc_route_cost(double dist_km, double cost_per_km, int time_min) {
    return W_COST * (dist_km * cost_per_km) + W_TIME * time_min;
}

int get_global_share_limit(SharingPref p);   // maps preference → max passengers
```

`get_global_share_limit` is the one function whose body **differs per model**
(see [MODELS_DOCUMENTATION.md](MODELS_DOCUMENTATION.md)).

## 5. Feasibility rules enforced everywhere

Any candidate route is validated by a full **timeline re-simulation**
(`simulate_route` in `alns.cpp`, `simulate_insertion_and_check` in
`heuristic.cpp`, `resimulate` in `dynamic_handler.cpp`). A route is feasible iff:

1. **Structure:** first stop is `START`, last stop is `END` at the office.
2. **Ready time:** each pickup's `begin_service = max(arrival, ready_time)` — the
   vehicle *waits* if it arrives early, never picks up before `ready_time`.
3. **Due time:** the office arrival time ≤ `due_time` for **every** passenger on
   the trip (because they all share the same office drop event).
4. **Capacity:** passenger count ≤ `min(vehicle.capacity, route.max_capacity,
   share_cap)` where `share_cap` is the tightest `get_global_share_limit` among
   the passengers.
5. **Category:** premium-requiring employees only on premium vehicles (and, in
   Hybrid/All variants, normal-requiring employees never on premium vehicles).
6. **No trip overlap:** a single vehicle's trips may not overlap in time
   (checked in ALNS via `routes_time_overlap` / `would_overlap`).

See [CONSTRAINTS.md](CONSTRAINTS.md) for the full constraint catalogue.

## 6. Timeline computation (how times are derived)

For each consecutive pair of stops:

```
dist_km   = get_dist(prev, cur)                       // haversine or override table
tmin      = travel_minutes(dist_km, vehicle.speed)    // round((dist/speed)*60)
arrival   = prev.departure_time + tmin
begin     = max(arrival, employee.ready_time)         // wait if early
departure = begin + SERVICE_MIN                        // dwell (0 in Hybrid/No, 2 in All's constructor)
```

The office (`END`) has no service time: `begin_service = departure_time =
arrival`. All passengers' `drop_time` is the office arrival time of the trip.

## 7. Reproducibility

ALNS is stochastic. `run_alns` performs `n_runs` independent restarts, each
seeded either from hardware entropy (`cfg.seed == 0`, the default) or from a
fixed `cfg.seed` for deterministic debugging. **Every seed is printed**, so any
run can be reproduced exactly by pinning that seed.
