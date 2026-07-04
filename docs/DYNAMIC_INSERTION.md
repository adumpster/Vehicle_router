# Dynamic Insertion Tool (`dynamic_insert`)

`dynamic_insert` is a **standalone second executable** (built by the same
`CMakeLists.txt` as `velora`). It handles the *real-time* scenario: a plan has
already been solved and dispatched, and now **new employees** arrive who must be
slotted into the existing routes **without re-optimising everything from
scratch**.

Source: `src/dynamic_main.cpp` (entry point) + `src/dynamic_handler.cpp` (logic),
header `include/dynamic_handler.h`.

## 1. Usage

```bash
./build/dynamic_insert  <solved_output.json>  <new_employees.json>  <updated_output.json>  [--debug]
```

Example:
```bash
./build/dynamic_insert  results/TC02_output.json \
                        new_employees.json         \
                        results/TC02_updated.json  \
                        --debug
```

- **arg1** `solved_output.json` — a plan previously produced by `velora`
  (contains both the echoed `input` and the solved `vehicles`).
- **arg2** `new_employees.json` — the employees to add (see schema in
  [DATA_FORMATS.md](DATA_FORMATS.md#3-new-employees-json-for-dynamic_insert)).
- **arg3** `updated_output.json` — where the updated plan is written (same schema
  as a normal `velora` output).
- **arg4** optional `--debug` — verbose per-candidate tracing.

## 2. How it works — three phases

### Phase A — parse & reconstruct the solved solution
`run_dynamic_insertion` reads the solved output and rebuilds the full in-memory
state:
1. `parse_employees_from_input` and `parse_vehicles_from_input` recreate the
   `Employee`/`Vehicle` objects from the echoed `input` block.
2. `reconstruct_routes` walks `vehicles[].trips[]` and rebuilds each `Route` /
   `Stop`, re-deriving arrival/service/departure times from the stored
   `route` node list, `passengers` pickup times, and `start_time`. Employees
   present in a route are marked `is_routed = true`, and each vehicle's
   `available_time` / `current_loc` are chained to its last trip's end.
3. The original `input` block is re-serialised verbatim so the updated output can
   echo it unchanged.

### Phase B — insert the new employees
New employees are read from `new_employees` (skipping any id already in the
plan), sorted by **tightest `due_time` first** (then earliest `ready_time`) — the
same tightness ordering as the batch heuristic — and merged into the master
employee list so lookups resolve. Each is then inserted by a **two-pass** greedy:

- **Pass 1 — `insert_existing`.** Try every position in every existing route,
  validated by a full `resimulate` (ready-time waits, due-time deadlines for all
  passengers). Score positions with Solomon **c1** (cheapest insertion) and pick
  the route with the best **c2**. Respects category and the effective capacity
  cap (`eff_cap` = `min(vehicle.capacity, route.max_capacity,
  get_global_share_limit(share_pref))`).
- **Pass 2 — `insert_new_trip`.** If no existing route fits, open a brand-new
  trip (starting from the office) on the **cheapest feasible vehicle**.

If both passes fail, the employee is recorded as failed with a reason and added to
`g_unrouted_reason`.

### Phase C — write the updated output
`write_updated_output` emits the same JSON structure as `output_json.cpp`
(summary, unrouted list, per-vehicle trips), preserving the echoed input, so any
downstream consumer (dashboard / evaluator) needs no changes.

## 3. Console result

For each new employee the tool prints either:
```
[OK]   E13  →  V02  Trip#1
[OK]   E14  →  V03  Trip#2  (new trip opened)
```
or, on failure:
```
[FAIL] E15  :  No feasible position in any route, and no vehicle can open a new trip within constraints.
```
and a final tally of how many were inserted.

## 4. Differences vs the batch solver

| Aspect | `velora` (batch) | `dynamic_insert` |
|--------|------------------|------------------|
| Scope | solves the whole instance | adds a few employees to a solved plan |
| Optimisation | Solomon + Stage 2 + full ALNS | greedy two-pass insertion only (no ALNS) |
| Existing routes | built from scratch | reconstructed and left largely intact |
| Speed | seconds–minutes | instant (single greedy pass) |
| Time-window relaxation | Stage 2 tiers | none — new employees must fit the hard windows |
| Shared cost/geometry/time code | yes | yes (same `config`/`geo`/`time_utils`) |

The dynamic tool intentionally **does not** re-run ALNS or relax deadlines: its
job is fast, safe incremental insertion that keeps already-dispatched routes
stable. If you need a globally re-optimised plan after many additions, re-run
`velora` on an updated input instead.
