#include "io.h"
#include "mini_json.h"
#include "time_utils.h"
#include "config.h"
#include "geo.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include <unordered_map>
#include <cctype>

using namespace std;
using Json = mini_json::Value;

VehicleCat parse_vehicle_category(string cat)
{
    if (cat == "premium")
        return PREMIUM;
    if (cat == "normal")
        return NORMAL;
    return ANY_CAT;
}

SharingPref parse_sharing_pref(string pref)
{
    if (pref == "single")
        return SINGLE;
    if (pref == "double")
        return DOUBLE;
    if (pref == "triple")
        return TRIPLE;
    return ANY_SHARE;
}

bool load_from_json(const string &filename, vector<Employee> &emps, vector<Vehicle> &vehs)
{
    try
    {
        ifstream file(filename);
        if (!file.is_open())
        {
            cerr << "ERROR: Cannot open file: " << filename << endl;
            return false;
        }

        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        Json data = mini_json::parse(text);

        cout << "Loading data from: " << filename << endl;

        // Load baseline costs
        auto has_key = [](const Json &v, const std::string &k) -> bool
        {
            return v.is_object() && v.obj.find(k) != v.obj.end();
        };

        // ------------------------------------------------------------------
        // Distance source selection:
        // If testcase metadata allows external maps, we load per-employee API
        // distances from employees[emp_id].distances (meters) and enable them
        // in geo.cpp via geo_set_distance_overrides_m().
        // Otherwise we clear overrides to force haversine.
        // ------------------------------------------------------------------
        bool allow_external_maps = false;
        if (has_key(data, "metadata"))
        {
            const Json &md = data["metadata"];
            if (md.is_array())
            {
                for (const auto &item : md.arr)
                {
                    if (!item.is_object())
                        continue;
                    std::string k = item["key"].as_string("");
                    if (k == "allow_external_maps")
                    {
                        // value may be a native JSON boolean, a number, or a string
                        const auto &val = item["value"];
                        if (val.is_bool())
                            allow_external_maps = val.as_bool(false);
                        else if (val.is_number())
                            allow_external_maps = (val.as_number() != 0.0);
                        else
                        {
                            std::string vv = val.as_string("");
                            for (auto &ch : vv)
                                ch = (char)std::tolower((unsigned char)ch);
                            allow_external_maps = (vv == "true" || vv == "1" || vv == "yes");
                        }
                    }
                }
            }
        }

        std::unordered_map<std::string, std::unordered_map<std::string, double>> overrides_m;

        map<string, double> baseline_map;
        if (has_key(data, "baseline"))
        {
            const Json &base = data["baseline"];
            if (base.is_array())
            {
                for (const auto &b : base.arr)
                {
                    if (!b.is_object())
                        continue;
                    std::string eid = b["employee_id"].as_string();
                    double bc = b["baseline_cost"].as_number(0.0);
                    if (!eid.empty())
                        baseline_map[eid] = bc;
                }
            }
        }

        // Load employees
        if (has_key(data, "employees"))
        {
            const Json &em = data["employees"];
            if (!em.is_object())
                throw std::runtime_error("employees must be an object");
            for (const auto &kv : em.obj)
            {
                string emp_id = kv.first;
                const Json &emp_data = kv.second;

                Employee e;
                e.id = emp_id;
                e.priority = emp_data["priority"].as_int(999);
                e.pickup = {emp_data["pickup"]["lat"].as_number(), emp_data["pickup"]["lng"].as_number()};
                e.drop = {emp_data["drop"]["lat"].as_number(), emp_data["drop"]["lng"].as_number()};

                // Set global OFFICE from the first employee drop location (assumed common for all)
                if (OFFICE.lat == 0.0 && OFFICE.lng == 0.0)
                {
                    OFFICE = e.drop;
                }

                // FIX: Handle decimal day fractions for earliest_pickup
                const Json &earliest_pickup_json = emp_data["earliest_pickup"];
                if (earliest_pickup_json.is_number())
                {
                    // Convert day fraction to minutes: fraction * 24 * 60
                    double day_fraction = earliest_pickup_json.as_number();
                    e.ready_time = (int)llround(day_fraction * 24.0 * 60.0);
                }
                else
                {
                    // Fallback to string parsing
                    e.ready_time = parse_time(emp_data["earliest_pickup"].as_string("08:00"));
                }

                // FIX: Handle decimal day fractions for latest_drop
                const Json &latest_drop_json = emp_data["latest_drop"];
                if (latest_drop_json.is_number())
                {
                    // Convert day fraction to minutes: fraction * 24 * 60
                    double day_fraction = latest_drop_json.as_number();
                    e.due_time = (int)llround(day_fraction * 24.0 * 60.0);
                }
                else
                {
                    // Fallback to string parsing
                    e.due_time = parse_time(emp_data["latest_drop"].as_string("23:59"));
                }

                e.veh_pref = parse_vehicle_category(emp_data["vehicle_preference"].as_string("any"));
                e.share_pref = parse_sharing_pref(emp_data["sharing_preference"].as_string("any"));
                e.is_routed = false;
                e.baseline_cost = baseline_map.count(emp_id) ? baseline_map[emp_id] : 0;

                // Load API/map distances from testcase JSON (meters), if enabled.
                if (allow_external_maps && has_key(emp_data, "distances"))
                {
                    const Json &dj = emp_data["distances"];
                    if (dj.is_object())
                    {
                        for (const auto &dk : dj.obj)
                        {
                            // dk.first is "E02" / "drop" etc, dk.second is meters
                            if (dk.second.is_number())
                            {
                                overrides_m[emp_id][dk.first] = dk.second.as_number();
                            }
                        }
                    }
                }

                emps.push_back(e);
            }
        }

        cout << "  Loaded " << emps.size() << " employees" << endl;

        // Enable/disable external-map distances globally for this run.
        if (allow_external_maps)
        {
            geo_set_distance_overrides_m(overrides_m); // meters
            cout << "  Distance mode: API/JSON overrides (external maps enabled)" << endl;
        }
        else
        {
            geo_set_distance_overrides_m({});
            cout << "  Distance mode: haversine (external maps disabled)" << endl;
        }

        // Load vehicles
        if (has_key(data, "vehicles"))
        {
            const Json &vv = data["vehicles"];
            if (!vv.is_array())
                throw std::runtime_error("vehicles must be an array");
            for (const auto &veh_data : vv.arr)
            {
                if (!veh_data.is_object())
                    continue;
                Vehicle v;
                v.id = veh_data["vehicle_id"].as_string();
                v.capacity = veh_data["capacity"].as_number(0.0);
                v.cost_per_km = veh_data["cost_per_km"].as_number(0.0);
                v.speed_kmh = veh_data["avg_speed_kmph"].as_number(30.0);
                v.depot_loc = {veh_data["current_lat"].as_number(), veh_data["current_lng"].as_number()};

                // Trip #1 starts from vehicle start (dataset)
                v.current_loc = v.depot_loc;

                // FIX: Also handle vehicle available_from as decimal day fraction
                const Json &available_json = veh_data["available_from"];
                if (available_json.is_number())
                {
                    double day_fraction = available_json.as_number();
                    v.available_time = (int)llround(day_fraction * 24.0 * 60.0);
                }
                else
                {
                    v.available_time = parse_time(veh_data["available_from"].as_string("08:00"));
                }

                v.category = parse_vehicle_category(veh_data["category"].as_string("any"));
                v.total_cost = 0;

                vehs.push_back(v);
            }
        }

        cout << "  Loaded " << vehs.size() << " vehicles\n"
             << endl;
        return true;
    }
    catch (const exception &e)
    {
        cerr << "ERROR parsing JSON: " << e.what() << endl;
        return false;
    }
}

bool load_from_json_keep_root(
    const std::string &filename,
    std::vector<Employee> &emps,
    std::vector<Vehicle> &vehs,
    mini_json::Value &out_root)
{
    std::ifstream in(filename);
    if (!in.is_open())
        return false;

    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();

    out_root = mini_json::parse(text);

    emps.clear();
    vehs.clear();

    return load_from_json(filename, emps, vehs);
}

// Feasibility check by full resimulation after inserting (pickup, drop) after stop index `after_idx`.
// Returns true and fills `out_stops` with the recomputed schedule if feasible.
// Full resimulation feasibility check after inserting a SINGLE pickup stop.
// Route structure is enforced as: START -> (pickup stops...) -> END(office).
// Drop time for every passenger is the arrival time at END.
//
// Feasibility rules:
//  - Each pickup begins no earlier than employee.ready_time (wait if early).
//  - Office arrival (END arrival) must be <= employee.due_time for *all* picked employees.
//
