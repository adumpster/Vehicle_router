// =============================================================
//  dynamic_main.cpp
//
//  Standalone entry-point for dynamic employee insertion.
//
//  Usage:
//    ./dynamic_insert  <solved_output.json>  <new_employees.json>  \
//                      <updated_output.json>  [--debug]
//
//  Example:
//    ./dynamic_insert  results/TC02_output.json  \
//                      new_employees.json         \
//                      results/TC02_updated.json  \
//                      --debug
// =============================================================

#include <iostream>
#include <string>
#include "dynamic_handler.h"

int main(int argc, char** argv) {
    std::string solved_out   = "output.json";
    std::string new_emp_file = "new_employees.json";
    std::string updated_out  = "updated_output.json";
    bool        debug        = false;

    if (argc > 1) solved_out   = argv[1];
    if (argc > 2) new_emp_file = argv[2];
    if (argc > 3) updated_out  = argv[3];
    if (argc > 4 && std::string(argv[4]) == "--debug") debug = true;

    std::cout << "\n=======================================================\n";
    std::cout << "  VELORA  –  Dynamic Insertion Handler\n";
    std::cout << "=======================================================\n";
    std::cout << "  Solved output  : " << solved_out   << "\n";
    std::cout << "  New employees  : " << new_emp_file << "\n";
    std::cout << "  Updated output : " << updated_out  << "\n\n";

    try {
        DynResult res = run_dynamic_insertion(
            solved_out, new_emp_file, updated_out, debug);

        std::cout << "\n--- Results ---\n";
        for (const auto& o : res.outcomes) {
            if (o.inserted) {
                std::cout << "  [OK]   " << o.emp_id
                          << "  →  " << o.vehicle_id
                          << "  Trip#" << o.trip_number
                          << (o.new_trip ? "  (new trip opened)" : "")
                          << "\n";
            } else {
                std::cout << "  [FAIL] " << o.emp_id
                          << "  :  " << o.reason << "\n";
            }
        }

        if (res.failed.empty())
            std::cout << "\n  All new employees successfully inserted.\n";
        else
            std::cout << "\n  WARNING: " << res.failed.size()
                      << " employee(s) could not be routed.\n";

        std::cout << "\n  Updated solution written to: " << updated_out << "\n";
        std::cout << "=======================================================\n\n";

    } catch (const std::exception& ex) {
        std::cerr << "\nERROR: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}