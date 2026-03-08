cmake_minimum_required(VERSION 3.16)
project(velora_split)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

# ── Original solver (unchanged) ──────────────────────────────
add_executable(velora
  src/main.cpp
  src/config.cpp
  src/io.cpp
  src/geo.cpp
  src/time_utils.cpp
  src/heuristic.cpp
  src/report.cpp
  src/stubs.cpp
  src/output_json.cpp
  src/file_utils.cpp
  src/json_serialize.cpp
  src/alns.cpp
  src/infeasible_handler.cpp
)

# ── Standalone dynamic insertion tool ────────────────────────
# Run AFTER velora has produced an output JSON.
#
#   ./dynamic_insert  results/TC02_output.json  \
#                     new_employees.json          \
#                     results/TC02_updated.json   \
#                     --debug
#
add_executable(dynamic_insert
  src/dynamic_main.cpp        # entry-point  (new)
  src/dynamic_handler.cpp     # core logic   (new)
  src/config.cpp
  src/geo.cpp
  src/time_utils.cpp
  src/io.cpp                  # for parse_vehicle_category / parse_sharing_pref
  src/json_serialize.cpp
)
