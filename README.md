# Vehicle Router Optimization

This repository contains the C++ based optimization engine developed for the **Kriti 26** optimization software competition (Vehicle Router problem).

The project is structured into three distinct approaches/models, each located in its own directory:

## Structure

- **`optimization-All_constarint/`**: 
  Contains the solver model that enforces all constraints strictly.

- **`optimization-Hybrid_final/`**: 
  Contains the final, refined hybrid optimization approach combining static routing and dynamic insertion capabilities.

- **`optimization-No_Constraint/`**: 
  Contains a baseline or relaxed model evaluating routing without strict constraints.

## General Build & Run Instructions

Each of these directories functions as a standalone CMake project containing its own `CMakeLists.txt` file and source code. In general, to build any of the models (e.g., inside the hybrid folder):

### Requirements
- A C++17 compatible compiler (g++, MinGW, MSVC)
- CMake (version 3.16+)

### Using g++
```bash
cd <optimization_folder>
cmake -S . -B build
cmake --build build -j
```

### Using MinGW
```bash
cd <optimization_folder>
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
```

### Running the Solver
After building, the executables will be available in the `build/` directory of the respective project.

Example:
```bash
.\build\velora.exe .\testcases\TC04.json .\results\TC04_output.json
```

### Documentation

Detailed documentation covering the project overview, models, constraints, data formats, architecture, and algorithms is available in the `docs/` directory.

---

*For more specific details, please refer to the documentation within each respective optimization directory.*
