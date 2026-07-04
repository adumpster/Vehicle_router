# VELORA — Vehicle Router Optimisation · Documentation

> Complete documentation for the **Kriti 26** Vehicle Routing optimisation engine.

This `docs/` folder explains everything about the project: the problem being
solved, the code structure, every algorithm and constraint, how to build and run
each model, the input/output data formats, and the design differences between the
three solver variants shipped in this repository.

The engine is codenamed **VELORA**. It solves a corporate employee **pickup →
office** routing problem: given a fleet of heterogeneous vehicles and a set of
employees (each with a pickup point, a time window, a priority, a vehicle
preference, and a sharing preference), it assigns employees to vehicles and
orders each vehicle's stops so that total operating cost is minimised while
respecting time windows, capacity, vehicle-category and ride-sharing rules.

---

## Documentation map

Read them roughly in this order:

| # | Document | What it covers |
|---|----------|----------------|
| 1 | [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md) | The routing problem, goals, and the solution at a glance |
| 2 | [MODELS_DOCUMENTATION.md](MODELS_DOCUMENTATION.md) | The three solver variants and exactly how they differ |
| 3 | [APP_STRUCTURE.md](APP_STRUCTURE.md) | Directory layout, every module, and how they connect |
| 4 | [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) | The 3-stage pipeline, core data structures, control flow |
| 5 | [ALGORITHMS.md](ALGORITHMS.md) | Solomon I1, ALNS, binary-search infeasibility, geo & cost math |
| 6 | [CONSTRAINTS.md](CONSTRAINTS.md) | Time windows, capacity, category, sharing, trip-overlap rules |
| 7 | [DATA_FORMATS.md](DATA_FORMATS.md) | Input JSON schema and output JSON schema, field by field |
| 8 | [DYNAMIC_INSERTION.md](DYNAMIC_INSERTION.md) | The standalone `dynamic_insert` real-time add-employee tool |
| 9 | [ENVIRONMENT_SETUP.md](ENVIRONMENT_SETUP.md) | Toolchain requirements (compiler, CMake) per platform |
| 10 | [INSTALLATION.md](INSTALLATION.md) | Step-by-step build instructions |
| 11 | [QUICK_START.md](QUICK_START.md) | The fastest path from clone to a solved test case |

---

## 30-second summary

- **Language / build:** C++17, CMake ≥ 3.16, no external dependencies (a bundled
  header-only `mini_json` parser is used).
- **Executables:** `velora` (the solver) and `dynamic_insert` (real-time
  insertion of new employees into an already-solved plan).
- **Pipeline:** `Solomon I1 constructive heuristic → binary-search infeasibility
  resolver → ALNS metaheuristic → JSON output`.
- **Three variants:** `optimization-Hybrid_final` (the recommended final model),
  `optimization-All_constarint` (strict sharing caps), and
  `optimization-No_Constraint` (relaxed baseline). See
  [MODELS_DOCUMENTATION.md](MODELS_DOCUMENTATION.md).

## Run it now

```bash
cd optimization-Hybrid_final
cmake -S . -B build
cmake --build build -j
./build/velora ./testcases/TC02.json ./results/TC02_output.json
```

See [QUICK_START.md](QUICK_START.md) for platform-specific details.
