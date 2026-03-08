#pragma once
#include <vector>
#include <map>
#include <string>
#include "types.h"

std::vector<int> get_sorted_indices_by_tightness(const std::vector<Employee>& emps);

void solve_solomon_insertion(std::vector<Employee>& employees,
                            std::vector<Vehicle>& vehicles,
                            bool debug=false);
