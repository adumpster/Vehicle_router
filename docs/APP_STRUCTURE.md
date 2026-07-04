# Application Structure

This document maps the repository layout and describes every source module, its
responsibility, and how the modules connect.

## 1. Top-level layout

```
Vehicle_router/
├── README.md                      ← repo-level readme (build & run summary)
├── docs/                          ← this documentation folder
├── optimization-Hybrid_final/     ← final refined solver (recommended)
├── optimization-All_constarint/   ← strict-sharing solver variant
└── optimization-No_Constraint/    ← relaxed baseline solver variant
```

Each `optimization-*` directory is a **self-contained CMake project** with an
identical internal structure (only the constraint logic differs — see
[MODELS_DOCUMENTATION.md](MODELS_DOCUMENTATION.md)):

```
optimization-Hybrid_final/
├── CMakeLists.txt                 ← builds `velora` and `dynamic_insert`
├── how_to_run.txt                 ← quick build/run cheatsheet
├── .gitignore                     ← ignores build/
├── include/                       ← public headers (one per module)
├── src/                           ← implementation (.cpp)
├── testcases/                     ← input JSON test cases (TC00..TC10)
└── results/                       ← solved output JSON per test case
```

## 2. Build targets (`CMakeLists.txt`)

Two executables are produced:

| Target | Entry point | Purpose |
|--------|-------------|---------|
| `velora` | `src/main.cpp` | The full batch solver (Solomon → infeasibility → ALNS → output) |
| `dynamic_insert` | `src/dynamic_main.cpp` | Standalone real-time new-employee insertion (see [DYNAMIC_INSERTION.md](DYNAMIC_INSERTION.md)) |

`velora` links: `main, config, io, geo, time_utils, heuristic, report, stubs,
output_json, file_utils, json_serialize, alns, infeasible_handler`.

`dynamic_insert` links a smaller subset: `dynamic_main, dynamic_handler, config,
geo, time_utils, io, json_serialize`.

## 3. Module-by-module reference

### Core data model

| File | Role |
|------|------|
| `include/types.h` | The whole domain model: `Location`, `Employee`, `Stop`, `Route`, `Vehicle`, and the `VehicleCat` / `SharingPref` enums. No logic. |
| `include/config.h` / `src/config.cpp` | Global tunables and the two **unified formulas**: `calc_route_cost()` (objective) and `get_global_share_limit()` (sharing cap). Also declares globals `W_COST`, `W_TIME`, `OFFICE`, `SERVICE_MIN`, `INF`, `PI`, and `g_unrouted_reason`. |

### Input / output

| File | Role |
|------|------|
| `src/io.cpp` (`include/io.h`) | Loads a test-case JSON into `vector<Employee>` + `vector<Vehicle>`. Parses time windows (both `"HH:MM"` strings and decimal day-fractions), preferences, baselines, and optional external map distances. Sets the global `OFFICE` from the first employee's drop. |
| `include/mini_json.h` | A **bundled header-only JSON parser** (`mini_json::Value`, `mini_json::parse`). No external dependency. Provides `is_object/is_array/as_string/as_number/as_int/as_bool`. |
| `src/json_serialize.cpp` (`include/json_serialize.h`) | Helpers to serialise the parsed tree back to JSON text (used to echo the input verbatim into output). |
| `src/file_utils.cpp` (`include/file_utils.h`) | `read_file_to_string()` — slurps a file so the raw input can be embedded in the output. |
| `src/output_json.cpp` (`include/output_json.h`) | `write_output_json()` — emits the solved plan: a `summary`, `unrouted_employees`, and per-vehicle `trips` with routes and passenger pickup/drop times. Echoes the original input under an `"input"` key. |

### Geometry & time

| File | Role |
|------|------|
| `src/geo.cpp` (`include/geo.h`) | Distance model. `get_dist()` uses the **haversine** great-circle distance by default, or a **per-pair override table** (real map distances in metres) when `allow_external_maps` is set. `travel_minutes()` converts km → minutes via vehicle speed. `recompute_distance_km()` sums a stop list. |
| `src/time_utils.cpp` (`include/time_utils.h`) | `parse_time("HH:MM") → minutes-since-midnight` and `format_time(minutes) → "HH:MM"`. |

### The three solver stages

| File | Role |
|------|------|
| `src/heuristic.cpp` (`include/heuristic.h`) | **Stage 1** — the Solomon I1 constructive insertion heuristic. Builds an initial feasible solution (see [ALGORITHMS.md](ALGORITHMS.md#1-stage-1--solomon-i1)). |
| `src/infeasible_handler.cpp` (`include/infeasible_handler.h`) | **Stage 2** — binary-search infeasibility resolver. Routes any leftover employees by finding the minimum deadline slack needed, in 3 relaxation tiers. Also parses per-priority delay budgets. |
| `src/alns.cpp` (`include/alns.h`) | **Stage 3** — the ALNS metaheuristic: destroy/repair operators, simulated-annealing acceptance, adaptive operator weights, local search, multi-restart. The bulk of the optimisation logic. |

### Reporting & dynamic tool

| File | Role |
|------|------|
| `src/report.cpp` (`include/report.h`) | `display_report()` — the human-readable console summary (per-vehicle trips, savings, and a priority-delay analysis). |
| `src/main.cpp` | The `velora` orchestrator: load → configure → Solomon → infeasibility → ALNS → report → write output. |
| `src/dynamic_main.cpp` + `src/dynamic_handler.cpp` (`include/dynamic_handler.h`) | The `dynamic_insert` tool: reconstructs a solved plan from its output JSON and inserts new employees. |
| `src/stubs.cpp` (`include/stubs.h`) | Small placeholder/utility definitions kept separate so the main modules stay focused. |

## 4. How the modules connect (dependency sketch)

```
              ┌──────────────┐
              │   types.h    │  (shared domain model, no deps)
              └──────┬───────┘
                     │ used by everything
   ┌─────────────────┼──────────────────────────────┐
   ▼                 ▼                                ▼
config.h/.cpp    geo.{h,cpp}                    time_utils.{h,cpp}
 (formulas)      (distance/time)                 (HH:MM parsing)
   ▲  ▲             ▲   ▲                             ▲
   │  │             │   │                             │
   │  └────────┬────┘   └──────────┬──────────────────┘
   │           │                   │
main.cpp ──► io.cpp ──► heuristic.cpp ──► infeasible_handler.cpp ──► alns.cpp
   │           │(mini_json)                                            │
   │           └────────────────────────────────────────────┐         │
   ▼                                                         ▼         ▼
report.cpp                                        output_json.cpp / file_utils.cpp
```

- **`main.cpp`** is the conductor. It calls, in order:
  `load_from_json_keep_root` → `solve_solomon_insertion` →
  `resolve_infeasible` → `run_alns` → `display_report` → `write_output_json`.
- **`types.h`** is the lingua franca: `Employee`/`Vehicle`/`Route`/`Stop` objects
  are mutated in place and passed between stages.
- **`config.h`** functions (`calc_route_cost`, `get_global_share_limit`) are the
  *single source of truth* every stage consults, so behaviour stays consistent.
- **`geo.cpp`** and **`time_utils.cpp`** are pure leaf utilities with no upward
  dependencies.

## 5. Test data & results

- `testcases/TCxx.json` — inputs. `TC00`–`TC08` exist in all variants; the
  Hybrid and No-constraint variants also include `TC10`.
- `results/TCxx_output.json` — committed sample outputs (the solved plans),
  useful as regression references and as inputs to `dynamic_insert`.

The exact JSON schemas for both are documented in
[DATA_FORMATS.md](DATA_FORMATS.md).
