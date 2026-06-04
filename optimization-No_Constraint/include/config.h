#pragma once
#include <map>
#include <string>
#include "types.h"

extern double ALPHA1;
extern double ALPHA2;
extern double LAMBDA;
extern double MU;

// Objective function weights (loaded from JSON metadata)
// total_cost = W_COST * distance_cost + W_TIME * time_cost
extern double W_COST;
extern double W_TIME;

extern const double INF;
extern const double PI;

// Common corporate office drop location (set from dataset)
extern Location OFFICE;

// Filled during solve to explain WHY someone could not be routed.
extern std::map<std::string, std::string> g_unrouted_reason;
// --- ADDED GLOBAL CONFIGURATIONS ---
extern const int SERVICE_MIN;

// Unified objective cost formula for all modules
inline double calc_route_cost(double dist_km, double cost_per_km, int time_min)
{
    return W_COST * (dist_km * cost_per_km) + W_TIME * time_min;
}

// Unified sharing preference limits (No Constraint: Everything is relaxed)
inline int get_global_share_limit(SharingPref /*p*/)
{
    return 999; // Explicitly relaxed for the unconstrained variant
}
