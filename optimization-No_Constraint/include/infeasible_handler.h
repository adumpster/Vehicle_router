#pragma once
#include <vector>
#include <map>
#include <string>
#include "types.h"

struct InfeasibleResult {
    std::string emp_id;
    bool        resolved;
    std::string method;            // "existing_route" | "new_trip" | "infeasible"
    int         slack_applied;     // exact minutes added to due_time (0 if none needed)
    int         slack_needed;      // geometric minimum slack required
    bool        budget_overridden; // true if slack_needed exceeded priority budget
                                   // but we routed anyway (employee IS in output)
    std::string vehicle_id;
    std::string original_reason;
};

// Binary-search based infeasible employee handler.
// Routing policy: if an employee is GEOMETRICALLY POSSIBLE (slack_needed is finite),
// they are ALWAYS routed using the exact minimum slack — the budget is advisory only.
// budget_overridden=true is set in the result to flag the deviation for reporting.
std::vector<InfeasibleResult> resolve_infeasible(
    std::vector<Employee>&       employees,
    std::vector<Vehicle>&        vehicles,
    const std::map<int, int>&    priority_budget
);

// Read priority_X_max_delay_min from JSON metadata key-value pairs.
std::map<int,int> build_priority_budget(
    const std::vector<std::pair<std::string,int>>& metadata_kv
);

void print_infeasible_report(const std::vector<InfeasibleResult>& results);