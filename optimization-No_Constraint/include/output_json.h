#pragma once
#include <string>
#include <vector>
#include "types.h"
#include "mini_json.h"

bool write_output_json(
    const std::string& filename,
    const std::string& input_json_raw,
    const std::vector<Vehicle>& vehs,
    const std::vector<Employee>& emps
    
);
