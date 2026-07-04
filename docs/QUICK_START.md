# Quick Start

The fastest path from a fresh clone to a solved test case. Assumes a C++17
compiler and CMake ≥ 3.16 are installed (see
[ENVIRONMENT_SETUP.md](ENVIRONMENT_SETUP.md) if not).

## 1. Build the recommended model (60 seconds)

```bash
cd optimization-Hybrid_final
cmake -S . -B build          # add -G "MinGW Makefiles" on Windows/MinGW
cmake --build build -j
```

## 2. Solve a test case

```bash
./build/velora ./testcases/TC02.json ./results/TC02_output.json
```

Windows:
```powershell
.\build\velora.exe .\testcases\TC02.json .\results\TC02_output.json
```

### What you'll see

```
=======================================================
    VELORA — Vehicle Routing Optimiser
    Pipeline: Solomon I1 → ALNS → Infeasibility fix
=======================================================
Loading data from: ./testcases/TC02.json
  Loaded 12 employees
  Loaded 5 vehicles
--- Solomon I1 Insertion Heuristic ...
[Binary-search infeasibility handler] ... resolution report ...
[ALNS] INSTANT mode  (~3 s)
[ALNS] 4 independent run(s) × 350 iterations ...
=======================================================
          VELORA ROUTING SUMMARY
... per-vehicle trips, savings, priority-delay analysis ...
Wrote output to: ./results/TC02_output.json
```

Open `results/TC02_output.json` to see the solved plan (schema in
[DATA_FORMATS.md](DATA_FORMATS.md)).

## 3. Command-line reference

```
velora [input.json] [output.json] [--debug]
```
- `input.json` — test case (default `TC02.json` if omitted).
- `output.json` — where to write the result (default `output.json`).
- `--debug` — verbose per-employee / per-iteration tracing.

## 4. Choose solver effort

Add a `config` entry to the **input JSON** to switch ALNS depth:

```json
{ "config": [ { "key": "alns_depth", "value": 1 } ], "employees": { ... } }
```
- `2` (default) → **INSTANT** mode (~3 s, 350 iterations).
- `1` → **QUALITY** mode (~5 min, 6000 iterations) — lower cost.

## 5. Try the other models

Same commands, different folder:
```bash
cd ../optimization-All_constarint   && cmake -S . -B build && cmake --build build -j
./build/velora ./testcases/TC02.json ./results/TC02_output.json

cd ../optimization-No_Constraint    && cmake -S . -B build && cmake --build build -j
./build/velora ./testcases/TC02.json ./results/TC02_output.json
```
Compare `total_optimized_cost` in the three outputs to see the cost of the
sharing constraints. See [MODELS_DOCUMENTATION.md](MODELS_DOCUMENTATION.md).

## 6. Add employees to a solved plan (dynamic insertion)

After a `velora` run produced `results/TC02_output.json`, create a
`new_employees.json` (schema in
[DATA_FORMATS.md](DATA_FORMATS.md#3-new-employees-json-for-dynamic_insert)) and:

```bash
./build/dynamic_insert  results/TC02_output.json \
                        new_employees.json         \
                        results/TC02_updated.json  \
                        --debug
```
See [DYNAMIC_INSERTION.md](DYNAMIC_INSERTION.md).

## 7. Next steps

- Understand the problem → [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md)
- Understand the code → [APP_STRUCTURE.md](APP_STRUCTURE.md) and
  [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)
- Understand the maths → [ALGORITHMS.md](ALGORITHMS.md)
- Understand the rules → [CONSTRAINTS.md](CONSTRAINTS.md)
