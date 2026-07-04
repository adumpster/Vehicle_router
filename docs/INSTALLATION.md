# Installation & Build

Each of the three `optimization-*` directories is an **independent CMake
project**. Building one is the same as building any other — pick the model you
want (see [MODELS_DOCUMENTATION.md](MODELS_DOCUMENTATION.md)) and build inside its
folder. Make sure the toolchain from [ENVIRONMENT_SETUP.md](ENVIRONMENT_SETUP.md)
is installed first.

## 1. Get the code

```bash
git clone <repo-url> Vehicle_router
cd Vehicle_router
```

Directory layout after cloning:
```
Vehicle_router/
├── optimization-Hybrid_final/     ← recommended
├── optimization-All_constarint/
├── optimization-No_Constraint/
├── docs/
└── README.md
```

## 2. Build a model

Pick a folder and configure + build. The commands differ only by the CMake
**generator**.

### With g++ / Clang (default Make generator)
```bash
cd optimization-Hybrid_final
cmake -S . -B build
cmake --build build -j
```

### With MinGW on Windows
```powershell
cd optimization-Hybrid_final
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
```

### With Ninja (any platform)
```bash
cd optimization-Hybrid_final
cmake -S . -B build -G Ninja
cmake --build build
```

### With MSVC / Visual Studio
```powershell
cd optimization-Hybrid_final
cmake -S . -B build
cmake --build build --config Release -j
```

`-j` builds in parallel. The configure step (`cmake -S . -B build`) only needs to
be re-run when `CMakeLists.txt` changes; after that, `cmake --build build`
recompiles incrementally.

## 3. Build artefacts

Two executables are produced in `build/` (or `build/Release/` under MSVC):

| Executable | What it is |
|------------|------------|
| `velora` (`velora.exe`) | The batch solver. |
| `dynamic_insert` (`dynamic_insert.exe`) | Real-time new-employee insertion (see [DYNAMIC_INSERTION.md](DYNAMIC_INSERTION.md)). |

`build/` is git-ignored (`.gitignore` = `build/`), so it is safe to delete and
regenerate at any time.

## 4. Verify the build

From inside the model folder:
```bash
./build/velora ./testcases/TC02.json ./results/TC02_output.json
```
Expect the VELORA banner, stage logs, a routing summary, and a written output
file. For the full run walk-through see [QUICK_START.md](QUICK_START.md).

## 5. Build all three models

```bash
for d in optimization-Hybrid_final optimization-All_constarint optimization-No_Constraint; do
  ( cd "$d" && cmake -S . -B build && cmake --build build -j )
done
```
(On Windows PowerShell, loop with `foreach ($d in ...) { ... }` or just build each
folder individually.)

## 6. Clean rebuild

```bash
rm -rf build          # Windows PowerShell: Remove-Item -Recurse -Force build
cmake -S . -B build
cmake --build build -j
```

## 7. Troubleshooting

| Symptom | Fix |
|---------|-----|
| `cmake: command not found` | Install CMake and add it to `PATH` (see ENVIRONMENT_SETUP.md). |
| `No CMAKE_CXX_COMPILER could be found` | Install a C++ compiler / open a Developer shell; ensure `g++`/`cl` is on `PATH`. |
| MinGW: "generator … does not match" | Delete `build/` and reconfigure with the correct `-G` generator. |
| MSVC: `velora.exe` not in `build/` | It's under `build/Release/` (multi-config generator). |
| Wrong output path | Both CLI args are paths; the `results/` folder must exist (it does in each model). |
