#pragma once
// =============================================================
//  dynamic_handler.h
//
//  Standalone dynamic insertion module.
//  Reads an already-solved output.json + a new_employees.json,
//  inserts the new employees into the existing solution
//  respecting ALL original constraints, then writes an updated
//  output JSON in exactly the same format.
//
//  Compile / link independently of main.cpp:
//
//    g++ -std=c++17 -O2 \
//        dynamic_main.cpp dynamic_handler.cpp \
//        config.cpp geo.cpp time_utils.cpp \
//        -o dynamic_insert
//
//    ./dynamic_insert  output.json  new_employees.json  updated_output.json
// =============================================================

#include <string>
#include <vector>
#include "types.h"

// ── Per-employee outcome ──────────────────────────────────────
struct DynOutcome {
    std::string emp_id;
    bool        inserted    = false;
    bool        new_trip    = false;   // true  →  a brand-new trip was opened
    std::string vehicle_id;
    int         trip_number = -1;      // 1-based
    std::string reason;                // set when inserted == false
};

// ── Aggregate result ─────────────────────────────────────────
struct DynResult {
    bool                     all_inserted = false;
    std::vector<DynOutcome>  outcomes;
    std::vector<DynOutcome>  failed;
};

// ── Main entry-point ─────────────────────────────────────────
//
//  solved_output_path  – path to the JSON produced by your main solver
//  new_emps_path       – path to the new_employees.json you provide
//  updated_output_path – where to write the patched solution
//  debug               – verbose per-step logging to stdout
//
DynResult run_dynamic_insertion(
    const std::string& solved_output_path,
    const std::string& new_emps_path,
    const std::string& updated_output_path,
    bool               debug = false
);