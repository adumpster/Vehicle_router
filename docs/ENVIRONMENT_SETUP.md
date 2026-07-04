# Environment Setup

The project is plain **C++17 with CMake** and **no external libraries** (JSON is
handled by the bundled header-only `mini_json.h`). Setting up the environment
means installing a C++17 compiler and CMake. Below are per-platform instructions.

## 1. Requirements

| Tool | Minimum version | Notes |
|------|-----------------|-------|
| C++ compiler | C++17-capable | GCC 7+, Clang 5+, or MSVC 2017+ |
| CMake | 3.16 | declared in every `CMakeLists.txt` |
| A build backend | — | Make, Ninja, MinGW Makefiles, or MSBuild/Visual Studio |

There is **nothing to `pip install` / `npm install`** — no Python, Node, or
third-party C++ packages are used.

## 2. Windows

You have two common toolchain choices.

### Option A — MinGW-w64 (g++) + CMake
1. Install **MSYS2** from <https://www.msys2.org/>, then in the MSYS2 shell:
   ```bash
   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-make
   ```
2. Add the MinGW `bin` directory (e.g. `C:\msys64\ucrt64\bin`) to your `PATH`.
3. Verify:
   ```powershell
   g++ --version
   cmake --version
   ```
4. Build with the MinGW generator (see [INSTALLATION.md](INSTALLATION.md)):
   ```powershell
   cmake -S . -B build -G "MinGW Makefiles"
   cmake --build build -j
   ```

### Option B — Visual Studio (MSVC) + CMake
1. Install **Visual Studio 2019/2022** with the "Desktop development with C++"
   workload (includes MSVC and CMake).
2. From a *Developer PowerShell for VS*:
   ```powershell
   cmake -S . -B build
   cmake --build build --config Release -j
   ```
   (MSVC is multi-config; the executable lands under `build/Release/`.)

> This repo's `how_to_run.txt` and README examples assume a single-config
> generator (g++/MinGW/Ninja), where the binary is at `build/velora.exe`. With
> MSVC it is at `build/Release/velora.exe`.

## 3. Linux

```bash
# Debian/Ubuntu
sudo apt update && sudo apt install -y build-essential cmake
# Fedora
sudo dnf install -y gcc-c++ cmake make
```
Verify and build:
```bash
g++ --version && cmake --version
cmake -S . -B build
cmake --build build -j
./build/velora ./testcases/TC02.json ./results/TC02_output.json
```

## 4. macOS

```bash
xcode-select --install          # Apple Clang + Make
brew install cmake              # via Homebrew
cmake -S . -B build
cmake --build build -j
./build/velora ./testcases/TC02.json ./results/TC02_output.json
```

## 5. Optional: Ninja for faster builds

```bash
cmake -S . -B build -G Ninja
cmake --build build
```
Install Ninja via your package manager (`apt install ninja-build`,
`brew install ninja`, `pacman -S ninja`).

## 6. Editor / IDE

Any editor works. For IntelliSense, point your IDE at the generated
`build/compile_commands.json` (add `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` to the
configure step to produce it). VS Code with the CMake Tools + C/C++ extensions
opens any `optimization-*` folder directly as a CMake project.

## 7. Sanity check

After building, from inside an `optimization-*` folder:
```bash
./build/velora ./testcases/TC00.json ./results/TC00_check.json
```
You should see the VELORA banner, the Solomon/infeasibility/ALNS logs, a routing
summary, and `Wrote output to: ./results/TC00_check.json`.
