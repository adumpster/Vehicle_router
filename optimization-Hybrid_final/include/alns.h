#pragma once
#include <vector>
#include "types.h"

// ============================================================
// ALNS Configuration
// Tuned defaults target instances with 20–100 employees.
// For very small instances (< 20) the heuristic alone is
// usually sufficient; ALNS overhead is not worth it.
// ============================================================
struct ALNSConfig {
    // ---- Search budget ----
    int iterations      = 1000;  // iterations per run (tuned for ~100 employees realtime)
    int no_improve_stop = 300;   // early-stop after N non-improving iterations

    // ---- Multi-run restarts ----
    // run_alns executes n_runs independent restarts and returns the best overall.
    // Each run gets a fresh random seed. All seeds are printed for reproducibility.
    // Set n_runs=1 for single-run behaviour.
    int n_runs = 3;

    // ---- Seed control ----
    // 0 = auto-generate from hardware entropy (non-deterministic, seed is logged).
    // Any other value = use that exact seed (fully deterministic, for debugging).
    unsigned seed = 0;

    // ---- Destruction size ----
    // Rule of thumb: remove 10–30 % of employees each iteration.
    // For 20 employees: min=2, max=6  |  For 100: min=5, max=25
    int min_remove = 3;
    int max_remove = 10;

    // ---- Simulated Annealing ----
    // T0 should be set so that a "typical bad move" (delta ≈ expected_bad_delta)
    // is accepted with ~50 % probability at the start:
    //   T0 ≈ expected_bad_delta / ln(2)
    // For medium instances (route costs in hundreds) T0 = 80–200 is reasonable.
    double T0      = 0.0;    // 0 = auto-calibrate from initial solution cost
    // cooling is tuned so T_final ≈ 5% of T0 over `iterations` steps.
    // Formula: cooling = 0.05 ^ (1 / iterations)
    // 300 iters → 0.9901  |  500 iters → 0.9940  |  1000 iters → 0.9970
    double cooling = 0.9901;

    // ---- Repair style ----
    bool use_regret2 = true;   // regret-2 > greedy for medium instances
    bool apply_two_opt_after_repair = false;  // skip mid-loop 2-opt for speed

    // ---- Adaptive weight update ----
    // Weights are updated once per segment of `segment_len` iterations.
    // rho controls how fast new evidence overwrites old weights.
    double weight_rho = 0.25;

    // ---- Shaw removal randomisation ----
    // Power applied to uniform sample to bias toward most-similar candidates.
    // Higher = less random (picks near-optimal cluster more often).
    double shaw_rnd_power = 3.0;

    // ---- Reward values for adaptive weights ----
    double reward_accept = 0.5;   // accepted improving-or-neutral move
    double reward_best   = 3.0;   // new global best found
};

// Runs ALNS in-place on the given solution.
// employees[].is_routed and vehicles[].routes are updated.
void run_alns(std::vector<Employee>& employees,
              std::vector<Vehicle>& vehicles,
              const ALNSConfig& cfg,
              bool debug = false);
