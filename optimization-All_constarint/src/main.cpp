#include <iostream>
#include <vector>
#include <string>
#include <map>

#include "io.h"
#include "heuristic.h"
#include "report.h"
#include "output_json.h"
#include "file_utils.h"
#include "alns.h"
#include "infeasible_handler.h"
#include "mini_json.h"
#include "config.h"

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// ALNS configuration
// All parameters can be tuned here without touching alns.cpp/alns.h.
// ─────────────────────────────────────────────────────────────────────────────


static ALNSConfig make_config_instant()
{
    ALNSConfig cfg;
    cfg.n_runs                   = 4;
    cfg.iterations               = 350;
    cfg.no_improve_stop          = 120;
    cfg.min_remove               = 3;
    cfg.max_remove               = 10;
    cfg.T0                       = 0.0;      // auto-calibrate from initial cost
    cfg.cooling                  = 0.988885; // 0.988885^350 = 2% of T0
    cfg.apply_two_opt_after_repair = false;
    cfg.seed                     = 0;
    return cfg;
}

static ALNSConfig make_config_quality()
{
    ALNSConfig cfg;
    cfg.n_runs                   = 4;
    cfg.iterations               = 6000;
    cfg.no_improve_stop          = 800;
    cfg.min_remove               = 4;
    cfg.max_remove               = 20;
    cfg.T0                       = 0.0;      // auto-calibrate
    cfg.cooling                  = 0.999239; // 0.999239^6000 = 2% of T0
    cfg.apply_two_opt_after_repair = false;
    cfg.seed                     = 0;
    return cfg;
}
// ─────────────────────────────────────────────────────────────────────────────
// Extract priority_X_max_delay_min values from JSON metadata
// ─────────────────────────────────────────────────────────────────────────────
static map<int,int> extract_budget(const mini_json::Value& root) {
    vector<pair<string,int>> kv;
    const auto& meta = root["metadata"];
    if (meta.is_array()) {
        for (const auto& item : meta.arr) {
            string key = item["key"].as_string();
            int    val = (int)item["value"].as_number(0.0);
            if (!key.empty()) kv.push_back({key, val});
        }
    }
    return build_priority_budget(kv);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// Usage: ./velora [input.json] [output.json] [--debug]
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    cout << "\n=======================================================\n"
         << "    VELORA — Vehicle Routing Optimiser                 \n"
         << "    Pipeline: Solomon I1 → ALNS → Infeasibility fix    \n"
         << "=======================================================\n";

    string input_file = "TC02.json";
    string out_file   = "output.json";
    bool   debug      = false;

    if (argc > 1) input_file = argv[1];
    if (argc > 2) out_file   = argv[2];
    if (argc > 3 && string(argv[3]) == "--debug") debug = true;

    // ── Load input ────────────────────────────────────────────────────────────
    vector<Employee> employees;
    vector<Vehicle>  vehicles;
    mini_json::Value json_root;

    if (!load_from_json_keep_root(input_file, employees, vehicles, json_root)) {
        if (!load_from_json(input_file, employees, vehicles)) {
            cerr << "ERROR: Failed to load input file: " << input_file << "\n";
            return 1;
        }
    }

    auto budget = extract_budget(json_root);

    

    // ── Read alns_depth from JSON config ─────────────────────────────────────
    int alns_depth = 2; // default: instant
    const auto& cfg_arr = json_root["config"];
    if (cfg_arr.is_array()) {
        for (const auto& item : cfg_arr.arr) {
            if (item["key"].as_string() == "alns_depth")
                alns_depth = item["value"].as_int(2);
        }
    }

    // ── Extract objective weights from metadata ───────────────────────────────
    const auto& meta = json_root["metadata"];
    if (meta.is_array()) {
        for (const auto& item : meta.arr) {
            string key = item["key"].as_string();
            if (key == "objective_cost_weight") W_COST = item["value"].as_number(1.0);
            if (key == "objective_time_weight") W_TIME = item["value"].as_number(0.0);
        }
    }
    

    // ── Stage 1: Solomon I1 constructive heuristic ────────────────────────────
    solve_solomon_insertion(employees, vehicles, debug);

    
    
    // ── Stage 2: Infeasibility resolution (binary-search slack) ──────────────
    if(true)
    {
        auto results = resolve_infeasible(employees, vehicles, budget);
        print_infeasible_report(results);
    }
    // ── Stage 3: ALNS metaheuristic ───────────────────────────────────────────
    {
        ALNSConfig alns_cfg;
        if (alns_depth == 1) {
            alns_cfg = make_config_quality();
            cout << "\n[ALNS] QUALITY mode  (~5 min)\n";
        } else {
            alns_cfg = make_config_instant();
            cout << "\n[ALNS] INSTANT mode  (~3 s)\n";
        }
        run_alns(employees, vehicles, alns_cfg, debug);
    }
    // ── Output ────────────────────────────────────────────────────────────────
    display_report(vehicles, employees);

    string input_raw = read_file_to_string(input_file);
    if (!write_output_json(out_file, input_raw, vehicles, employees)) {
        cerr << "ERROR: Failed to write output: " << out_file << "\n";
        return 1;
    }
    cout << "Wrote output to: " << out_file << "\n";
    return 0;
}
