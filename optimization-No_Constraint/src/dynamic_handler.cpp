// =============================================================
//  dynamic_handler.cpp
//
//  Two-phase standalone dynamic insertion.
//
//  PHASE A – Parse & reconstruct solution
//    Reads solved_output.json:
//      • "input" sub-object → employee coords, time-windows,
//        preferences, vehicle specs (same schema as TC0x.json)
//      • "vehicles[].trips[]" → reconstruct Route / Stop objects
//        with correct arrival / departure timestamps
//
//  PHASE B – Insert new employees
//    Reads new_employees.json:
//      Each entry carries full employee fields + a "distances"
//      map (used only as a hint; actual geometry is computed
//      from lat/lng via haversine, keeping parity with the
//      original solver).
//    Insertion order: tightest due_time first (same as heuristic).
//    For each candidate:
//      Pass 1 – try every position in every existing route
//               (Solomon c1/c2 scoring, full timeline resim).
//      Pass 2 – open a brand-new trip on the cheapest
//               feasible vehicle.
//
//  PHASE C – Write updated output JSON
//    Same format as the original output_json.cpp output so the
//    downstream consumer (dashboard / evaluator) needs no change.
// =============================================================

#include "dynamic_handler.h"

#include "config.h"
#include "geo.h"
#include "time_utils.h"
#include "mini_json.h"
#include "io.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using std::map;
using std::string;
using std::vector;

// ──────────────────────────────────────────────────────────────
//  Internal constants
// ──────────────────────────────────────────────────────────────
// static const int SERVICE_MIN = 1;   // service time at each pickup stop (min)

// ──────────────────────────────────────────────────────────────
//  Utility helpers
// ──────────────────────────────────────────────────────────────
static string json_esc(const string &s)
{
    string o;
    o.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
        case '"':
            o += "\\\"";
            break;
        case '\\':
            o += "\\\\";
            break;
        case '\n':
            o += "\\n";
            break;
        case '\r':
            o += "\\r";
            break;
        case '\t':
            o += "\\t";
            break;
        default:
            o += c;
            break;
        }
    }
    return o;
}

static string read_file(const string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        throw std::runtime_error("Cannot open: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static map<string, Employee *> make_emp_map(vector<Employee> &emps)
{
    map<string, Employee *> m;
    for (auto &e : emps)
        m[e.id] = &e;
    return m;
}

// static int emp_share_limit(SharingPref sp) {
//    return 999;
// }

static double total_veh_cost(const vector<Vehicle> &vehs)
{
    double c = 0;
    for (const auto &v : vehs)
        c += v.total_cost;
    return c;
}

static int count_routed(const vector<Employee> &emps)
{
    int c = 0;
    for (const auto &e : emps)
        if (e.is_routed)
            c++;
    return c;
}

// ──────────────────────────────────────────────────────────────
//  PHASE A  –  Parse solved output JSON
// ──────────────────────────────────────────────────────────────

// Reconstruct employees from the "input.employees" sub-object
// (same schema as TC0x.json – keyed object).
static void parse_employees_from_input(
    const mini_json::Value &inp,
    vector<Employee> &emps)
{
    const auto &je = inp["employees"];
    if (!je.is_object())
        throw std::runtime_error("input.employees not an object");

    for (const auto &kv : je.obj)
    {
        const string &id = kv.first;
        const auto &jed = kv.second;

        Employee e;
        e.id = id;
        e.priority = jed["priority"].as_int(999);
        e.pickup = {jed["pickup"]["lat"].as_number(),
                    jed["pickup"]["lng"].as_number()};
        e.drop = {jed["drop"]["lat"].as_number(),
                  jed["drop"]["lng"].as_number()};

        // earliest_pickup / latest_drop can be "HH:MM" strings or
        // decimal-day fractions (TC02-TC04 style).
        auto parse_tw = [](const mini_json::Value &v) -> int
        {
            if (v.is_number())
            {
                return (int)llround(v.as_number() * 24.0 * 60.0);
            }
            return parse_time(v.as_string("08:00"));
        };
        e.ready_time = parse_tw(jed["earliest_pickup"]);
        e.due_time = parse_tw(jed["latest_drop"]);

        e.veh_pref = parse_vehicle_category(
            jed["vehicle_preference"].as_string("any"));
        e.share_pref = parse_sharing_pref(
            jed["sharing_preference"].as_string("any"));
        e.is_routed = false; // will be corrected below
        e.baseline_cost = 0.0;

        // Check baseline array if present
        if (inp["baseline"].is_array())
        {
            for (const auto &b : inp["baseline"].arr)
            {
                if (b["employee_id"].as_string() == id)
                {
                    e.baseline_cost = b["baseline_cost"].as_number(0.0);
                    break;
                }
            }
        }

        // Set global OFFICE from first employee's drop location.
        if (OFFICE.lat == 0.0 && OFFICE.lng == 0.0)
            OFFICE = e.drop;

        emps.push_back(e);
    }
}

// Reconstruct vehicles (fleet metadata) from input.vehicles array.
static void parse_vehicles_from_input(
    const mini_json::Value &inp,
    vector<Vehicle> &vehs)
{
    const auto &jv = inp["vehicles"];
    if (!jv.is_array())
        throw std::runtime_error("input.vehicles not an array");

    for (const auto &jvd : jv.arr)
    {
        Vehicle v;
        v.id = jvd["vehicle_id"].as_string();
        v.capacity = jvd["capacity"].as_number(4.0);
        v.cost_per_km = jvd["cost_per_km"].as_number(10.0);
        v.speed_kmh = jvd["avg_speed_kmph"].as_number(30.0);
        v.depot_loc = {jvd["current_lat"].as_number(),
                       jvd["current_lng"].as_number()};
        v.current_loc = v.depot_loc;
        v.category = parse_vehicle_category(
            jvd["category"].as_string("any"));

        auto parse_tw = [](const mini_json::Value &val) -> int
        {
            if (val.is_number())
                return (int)llround(val.as_number() * 24.0 * 60.0);
            return parse_time(val.as_string("08:00"));
        };
        v.available_time = parse_tw(jvd["available_from"]);
        v.total_cost = 0.0;
        v.routes.clear();
        vehs.push_back(v);
    }
}

// Rebuild Route / Stop objects from the solved vehicles[] trips[].
// We re-derive exact arrival / departure times from the stop
// locations + the employee time-window data already loaded.
static void reconstruct_routes(
    const mini_json::Value &jout, // full output JSON root
    vector<Vehicle> &vehs,
    vector<Employee> &emps)
{
    map<string, Employee *> emp_map = make_emp_map(emps);

    const auto &jvehs = jout["vehicles"];
    if (!jvehs.is_array())
        return;

    for (const auto &jv : jvehs.arr)
    {
        string vid = jv["vehicle_id"].as_string();

        // Find matching vehicle in our vector.
        Vehicle *veh = nullptr;
        for (auto &v : vehs)
            if (v.id == vid)
            {
                veh = &v;
                break;
            }
        if (!veh)
            continue;

        const auto &jtrips = jv["trips"];
        if (!jtrips.is_array())
            continue;

        for (const auto &jt : jtrips.arr)
        {
            Route route;
            route.current_capacity = jt["load"].as_int(0);
            route.max_capacity = jt["capacity_limit"].as_int((int)veh->capacity);
            route.total_distance = jt["trip_distance_km"].as_number(0.0);
            route.total_cost = jt["trip_cost"].as_number(0.0);

            // Rebuild stop list from "route" array (["START","E01","END",...])
            const auto &jroute = jt["route"];
            if (!jroute.is_array())
                continue;

            // Build passenger pickup-time map from passengers array.
            map<string, int> pu_time_map; // emp_id → departure_time at pickup
            int drop_time = 0;            // office arrival time (shared)
            const auto &jpax = jt["passengers"];
            if (jpax.is_array())
            {
                for (const auto &jp : jpax.arr)
                {
                    string eid = jp["employee_id"].as_string();
                    int pt = parse_time(jp["pickup_time"].as_string("00:00"));
                    drop_time = parse_time(jp["drop_time"].as_string("00:00"));
                    pu_time_map[eid] = pt;
                }
            }

            int start_dep = parse_time(jt["start_time"].as_string("08:00"));

            for (size_t si = 0; si < jroute.arr.size(); si++)
            {
                string emp_id = jroute.arr[si].as_string();
                Stop s;
                s.emp_id = emp_id;

                if (emp_id == "START")
                {
                    s.is_pickup = false;
                    s.loc = veh->depot_loc;
                    s.arrival_time = start_dep;
                    s.begin_service = start_dep;
                    s.departure_time = start_dep;
                }
                else if (emp_id == "END")
                {
                    s.is_pickup = false;
                    s.loc = OFFICE;
                    s.arrival_time = drop_time;
                    s.begin_service = drop_time;
                    s.departure_time = drop_time;
                }
                else
                {
                    auto it = emp_map.find(emp_id);
                    if (it == emp_map.end())
                        throw std::runtime_error("Unknown employee in route: " + emp_id);

                    const Employee &e = *it->second;
                    e.is_routed; // just to suppress unused warning
                    s.is_pickup = true;
                    s.loc = e.pickup;

                    int pt = pu_time_map.count(emp_id) ? pu_time_map[emp_id] : 0;
                    // departure_time at pickup = begin_service + SERVICE_MIN
                    // begin_service             = pt - SERVICE_MIN (stored as departure)
                    s.departure_time = pt;
                    s.begin_service = std::max(pt - SERVICE_MIN, e.ready_time);
                    s.arrival_time = s.begin_service;

                    // Mark employee as routed.
                    it->second->is_routed = true;
                }

                route.stops.push_back(s);
            }

            // Update vehicle's available_time to this trip's end time.
            if (!route.stops.empty())
            {
                veh->available_time = route.stops.back().departure_time;
                veh->current_loc = route.stops.back().loc;
            }

            veh->routes.push_back(std::move(route));
        }

        // Recompute vehicle total cost from reconstructed routes.
        veh->total_cost = 0.0;
        for (const auto &r : veh->routes)
            veh->total_cost += r.total_cost;
    }
}

// ──────────────────────────────────────────────────────────────
//  PHASE B  –  Heuristic insertion logic
// ──────────────────────────────────────────────────────────────

// Full timeline resimulation on a candidate stop list.
// Returns false and sets fail_reason if any feasibility rule fails.
static bool resimulate(
    vector<Stop> &stops,
    double speed_kmh,
    const map<string, Employee *> &emp_map,
    string &fail_reason)
{
    if (stops.size() < 2)
    {
        fail_reason = "route too short";
        return false;
    }

    // START times are fixed – never move them.
    for (int i = 1; i < (int)stops.size(); i++)
    {
        Stop &prev = stops[i - 1];
        Stop &cur = stops[i];

        double dist = get_dist(prev, cur);
        int tmin = travel_minutes(dist, speed_kmh);
        int arr = prev.departure_time + tmin;
        cur.arrival_time = arr;

        if (cur.emp_id == "END")
        {
            cur.begin_service = arr;
            cur.departure_time = arr;
        }
        else
        {
            auto it = emp_map.find(cur.emp_id);
            if (it == emp_map.end())
            {
                fail_reason = "unknown employee: " + cur.emp_id;
                return false;
            }
            const Employee &e = *it->second;
            int begin = std::max(arr, e.ready_time);
            cur.begin_service = begin;
            cur.departure_time = begin + SERVICE_MIN;
        }
    }

    // Check due_time for all passengers.
    int office_arr = stops.back().arrival_time;
    for (int i = 1; i + 1 < (int)stops.size(); i++)
    {
        auto it = emp_map.find(stops[i].emp_id);
        if (it == emp_map.end())
            continue;
        const Employee &e = *it->second;
        if (office_arr > e.due_time)
        {
            fail_reason = "due_time violated for " + e.id + " (office=" + format_time(office_arr) + " due=" + format_time(e.due_time) + ")";
            return false;
        }
    }

    return true;
}

// Effective capacity cap for inserting emp into this route on veh.
static int eff_cap(const Vehicle &veh, const Route &route,
                   const Employee &emp)
{
    int cap = (int)veh.capacity;
    if (route.max_capacity > 0)
        cap = std::min(cap, route.max_capacity);
    cap = std::min(cap, get_global_share_limit(emp.share_pref));
    return cap;
}

// Hard vehicle/route compatibility check.
static bool is_compat(const Vehicle &veh, const Route &route,
                      const Employee &emp)
{
    if (route.current_capacity + 1 > eff_cap(veh, route, emp))
        return false;
    return true;
}

// Solomon c1 criterion.
static double c1_cost(const Route &route, const Employee &emp,
                      int pos, double speed_kmh)
{
    const Stop &prev_stop = route.stops[pos - 1];
    const Stop &next_stop = route.stops[pos];
    Stop emp_stop{emp.id, emp.pickup, 0, 0, 0, true};

    double d_iu = get_dist(prev_stop, emp_stop);
    double d_uj = get_dist(emp_stop, next_stop);
    double d_ij = get_dist(prev_stop, next_stop);

    double c11 = d_iu + d_uj - MU * d_ij;

    int pt = prev_stop.departure_time;
    double t_iu = (d_iu / speed_kmh) * 60.0;
    int b_u = std::max((int)(pt + t_iu), emp.ready_time);
    double c12 = (double)(b_u - pt);

    return ALPHA1 * c11 + ALPHA2 * c12;
}

// Solomon c2 criterion.
static double c2_score(const Route &route, const Employee &emp,
                       double c1)
{
    Stop emp_stop{emp.id, emp.pickup, 0, 0, 0, true};
    double d_0u = get_dist(route.stops.front(), emp_stop);
    return LAMBDA * d_0u - c1;
}

// Update route totals after a modification.
static void finalize_route(Route &r, const Vehicle &v)
{
    r.total_distance = recompute_distance_km(r.stops);
    int t = r.stops.back().arrival_time - r.stops.front().departure_time;
    r.total_cost = calc_route_cost(r.total_distance, v.cost_per_km, t);
}
static void finalize_vehicle(Vehicle &v)
{
    v.total_cost = 0.0;
    for (const auto &r : v.routes)
        if (r.stops.size() > 1)
            v.total_cost += r.total_cost;
}

// Pass 1 – insert into an existing route.
static bool insert_existing(
    const Employee &emp,
    vector<Employee> &employees,
    vector<Vehicle> &vehicles,
    DynOutcome &out,
    bool debug)
{
    map<string, Employee *> emp_map = make_emp_map(employees);

    double best_c2 = -std::numeric_limits<double>::infinity();
    int best_vi = -1, best_ri = -1, best_pos = -1;
    vector<Stop> best_stops;

    for (int vi = 0; vi < (int)vehicles.size(); vi++)
    {
        Vehicle &veh = vehicles[vi];
        for (int ri = 0; ri < (int)veh.routes.size(); ri++)
        {
            Route &route = veh.routes[ri];

            if (route.stops.size() < 2)
                continue;
            if (route.stops.back().emp_id != "END")
                continue;
            if (!is_compat(veh, route, emp))
            {
                if (debug)
                    std::cout << "    [" << veh.id << " T" << ri + 1
                              << "] skip (category/cap)\n";
                continue;
            }

            double best_c1h = std::numeric_limits<double>::infinity();
            int best_posh = -1;
            vector<Stop> best_sh;

            for (int pos = 1; pos <= (int)route.stops.size() - 1; pos++)
            {
                // Build candidate stop list with new pickup at `pos`.
                vector<Stop> cand = route.stops;
                Stop pu;
                pu.emp_id = emp.id;
                pu.loc = emp.pickup;
                pu.is_pickup = true;
                cand.insert(cand.begin() + pos, pu);

                string why;
                if (!resimulate(cand, veh.speed_kmh, emp_map, why))
                {
                    if (debug)
                        std::cout << "    [" << veh.id << " T" << ri + 1
                                  << " p" << pos << "] infeasible: " << why << "\n";
                    continue;
                }

                double c1 = c1_cost(route, emp, pos, veh.speed_kmh);
                if (c1 < best_c1h)
                {
                    best_c1h = c1;
                    best_posh = pos;
                    best_sh = cand;
                }
            }

            if (best_posh == -1)
                continue;

            double c2 = c2_score(route, emp, best_c1h);
            if (debug)
                std::cout << "    [" << veh.id << " T" << ri + 1
                          << "] c1=" << best_c1h << " c2=" << c2
                          << " pos=" << best_posh << "\n";

            if (c2 > best_c2)
            {
                best_c2 = c2;
                best_vi = vi;
                best_ri = ri;
                best_pos = best_posh;
                best_stops = best_sh;
            }
        }
    }

    if (best_vi == -1)
        return false;

    Vehicle &veh = vehicles[best_vi];
    Route &route = veh.routes[best_ri];

    route.max_capacity = (int)veh.capacity;
    route.stops = std::move(best_stops);
    route.current_capacity++;
    finalize_route(route, veh);
    finalize_vehicle(veh);

    for (auto &e : employees)
        if (e.id == emp.id)
        {
            e.is_routed = true;
            break;
        }

    out.inserted = true;
    out.new_trip = false;
    out.vehicle_id = veh.id;
    out.trip_number = best_ri + 1;

    if (debug)
        std::cout << "  >>> INSERTED " << emp.id
                  << " → " << veh.id << " Trip#" << (best_ri + 1)
                  << " pos=" << best_pos << "\n";
    return true;
}

// Pass 2 – open a new trip.
static bool insert_new_trip(
    const Employee &emp,
    vector<Employee> &employees,
    vector<Vehicle> &vehicles,
    DynOutcome &out,
    bool debug)
{
    map<string, Employee *> emp_map = make_emp_map(employees);

    double best_cost = std::numeric_limits<double>::infinity();
    int best_vi = -1;
    vector<Stop> best_stops;

    for (int vi = 0; vi < (int)vehicles.size(); vi++)
    {
        Vehicle &veh = vehicles[vi];

        if ((int)veh.capacity == 0)
            continue;

        // New trip starts from OFFICE.
        vector<Stop> cand;
        Stop st;
        st.emp_id = "START";
        st.loc = OFFICE;
        st.is_pickup = false;
        st.arrival_time = veh.available_time;
        st.begin_service = veh.available_time;
        st.departure_time = veh.available_time;
        cand.push_back(st);

        Stop pu;
        pu.emp_id = emp.id;
        pu.loc = emp.pickup;
        pu.is_pickup = true;
        cand.push_back(pu);

        Stop en;
        en.emp_id = "END";
        en.loc = OFFICE;
        en.is_pickup = false;
        cand.push_back(en);

        string why;
        if (!resimulate(cand, veh.speed_kmh, emp_map, why))
        {
            if (debug)
                std::cout << "    [" << veh.id << " NEW] infeasible: " << why << "\n";
            continue;
        }

        double dist = recompute_distance_km(cand);
        double cost = dist * veh.cost_per_km;
        if (debug)
            std::cout << "    [" << veh.id << " NEW] feasible cost=" << cost << "\n";

        if (cost < best_cost)
        {
            best_cost = cost;
            best_vi = vi;
            best_stops = cand;
        }
    }

    if (best_vi == -1)
        return false;

    Vehicle &veh = vehicles[best_vi];
    Route nr;
    nr.current_capacity = 1;
    nr.max_capacity = (int)veh.capacity;
    nr.stops = std::move(best_stops);
    finalize_route(nr, veh);

    int idx = (int)veh.routes.size();
    veh.routes.push_back(std::move(nr));
    veh.available_time = veh.routes.back().stops.back().departure_time;
    veh.current_loc = veh.routes.back().stops.back().loc;
    finalize_vehicle(veh);

    for (auto &e : employees)
        if (e.id == emp.id)
        {
            e.is_routed = true;
            break;
        }

    out.inserted = true;
    out.new_trip = true;
    out.vehicle_id = veh.id;
    out.trip_number = idx + 1;

    if (debug)
        std::cout << "  >>> NEW TRIP " << veh.id
                  << " Trip#" << (idx + 1) << " for " << emp.id << "\n";
    return true;
}

// ──────────────────────────────────────────────────────────────
//  PHASE C  –  Write updated output JSON
//  (same format as output_json.cpp so consumers need no changes)
// ──────────────────────────────────────────────────────────────

static vector<string> passengers_in_route(const Route &r)
{
    vector<string> ids;
    for (const auto &s : r.stops)
        if (s.is_pickup)
            ids.push_back(s.emp_id);
    return ids;
}

static void write_updated_output(
    const string &path,
    const string &original_input_raw, // the "input" blob preserved verbatim
    const vector<Vehicle> &vehs,
    const vector<Employee> &emps)
{
    std::ofstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot write: " + path);

    double baseline_total = 0.0;
    for (const auto &e : emps)
        baseline_total += e.baseline_cost;
    double opt_total = total_veh_cost(vehs);
    double savings = baseline_total - opt_total;
    double sav_pct = (baseline_total > 1e-9) ? (savings / baseline_total) * 100.0 : 0.0;
    int routed = count_routed(emps);
    int unrouted = (int)emps.size() - routed;

    f << std::fixed << std::setprecision(6);
    f << "{\n";
    f << "  \"input\": " << original_input_raw << ",\n";

    f << "  \"summary\": {\n";
    f << "    \"total_employees\": " << emps.size() << ",\n";
    f << "    \"employees_routed\": " << routed << ",\n";
    f << "    \"employees_unrouted\": " << unrouted << ",\n";
    f << "    \"total_baseline_cost\": " << baseline_total << ",\n";
    f << "    \"total_optimized_cost\": " << opt_total << ",\n";
    f << "    \"net_savings\": " << savings << ",\n";
    f << "    \"savings_percentage\": " << sav_pct << "\n";
    f << "  },\n";

    // unrouted list
    f << "  \"unrouted_employees\": [\n";
    bool first = true;
    for (const auto &e : emps)
    {
        if (e.is_routed)
            continue;
        auto it = g_unrouted_reason.find(e.id);
        string reason = (it != g_unrouted_reason.end()) ? it->second : "unrouted";
        if (!first)
            f << ",\n";
        first = false;
        f << "    {\"employee_id\": \"" << json_esc(e.id)
          << "\", \"reason\": \"" << json_esc(reason) << "\"}";
    }
    f << "\n  ],\n";

    // vehicles
    f << "  \"vehicles\": [\n";
    for (size_t vi = 0; vi < vehs.size(); vi++)
    {
        const auto &v = vehs[vi];
        if (vi)
            f << ",\n";
        f << "    {\n";
        f << "      \"vehicle_id\": \"" << json_esc(v.id) << "\",\n";
        f << "      \"category\": \""
          << (v.category == PREMIUM ? "premium" : "normal") << "\",\n";
        f << "      \"capacity\": " << v.capacity << ",\n";
        f << "      \"speed_kmh\": " << v.speed_kmh << ",\n";
        f << "      \"cost_per_km\": " << v.cost_per_km << ",\n";
        f << "      \"total_cost\": " << v.total_cost << ",\n";
        f << "      \"trips\": [\n";

        for (size_t ti = 0; ti < v.routes.size(); ti++)
        {
            const auto &r = v.routes[ti];
            if (ti)
                f << ",\n";

            int t_start = r.stops.empty() ? 0 : r.stops.front().departure_time;
            int t_end = r.stops.empty() ? 0 : r.stops.back().arrival_time;

            f << "        {\n";
            f << "          \"trip_number\": " << (ti + 1) << ",\n";
            f << "          \"load\": " << r.current_capacity << ",\n";
            f << "          \"capacity_limit\": " << r.max_capacity << ",\n";
            f << "          \"start_time\": \"" << format_time(t_start) << "\",\n";
            f << "          \"end_time\": \"" << format_time(t_end) << "\",\n";
            f << "          \"trip_distance_km\": " << r.total_distance << ",\n";
            f << "          \"trip_cost\": " << r.total_cost << ",\n";

            // route node names
            f << "          \"route\": [";
            for (size_t si = 0; si < r.stops.size(); si++)
            {
                if (si)
                    f << ", ";
                f << "\"" << json_esc(r.stops[si].emp_id) << "\"";
            }
            f << "],\n";

            // passengers
            auto pax = passengers_in_route(r);
            f << "          \"passengers\": [";
            int drop_t = r.stops.empty() ? 0 : r.stops.back().arrival_time;
            for (size_t pi = 0; pi < pax.size(); pi++)
            {
                const string &eid = pax[pi];
                // Find pickup stop
                int pu_t = 0;
                for (const auto &s : r.stops)
                    if (s.is_pickup && s.emp_id == eid)
                    {
                        pu_t = s.departure_time;
                        break;
                    }
                if (pi)
                    f << ",\n";
                f << "            {\"employee_id\": \"" << json_esc(eid)
                  << "\", \"pickup_time\": \"" << format_time(pu_t)
                  << "\", \"drop_time\": \"" << format_time(drop_t) << "\"}";
            }
            f << "]\n";
            f << "        }";
        }

        f << "\n      ]\n";
        f << "    }";
    }
    f << "\n  ]\n}\n";
}

// ──────────────────────────────────────────────────────────────
//  PUBLIC API
// ──────────────────────────────────────────────────────────────
DynResult run_dynamic_insertion(
    const string &solved_output_path,
    const string &new_emps_path,
    const string &updated_output_path,
    bool debug)
{
    DynResult result;
    result.all_inserted = true;

    // ── A1: Read and parse the solved output JSON ─────────────
    if (debug)
        std::cout << "[DynamicHandler] Reading solved output: "
                  << solved_output_path << "\n";
    string out_raw = read_file(solved_output_path);
    mini_json::Value jout = mini_json::parse(out_raw);

    // The "input" sub-object holds the original TC JSON.
    const mini_json::Value &jinput = jout["input"];
    if (!jinput.is_object())
        throw std::runtime_error("solved output missing 'input' key");

    // We need to re-emit the input blob verbatim in the updated output.
    // Re-serialize the parsed input (mini_json round-trip).
    // Simpler: extract the raw substring between the first "input": and
    // the matching brace. Since mini_json stores the parsed tree, we
    // re-encode it below using write_json from json_serialize.
    // (Alternatively just carry the raw string – but we'd need to parse
    //  it out of out_raw, which risks fragility with nested braces.)
    //
    // Clean approach: stringify the parsed jinput.
    std::ostringstream inp_ss;
    // Use a simple recursive JSON serializer (inline below).
    std::function<void(std::ostream &, const mini_json::Value &)> emit_json;
    emit_json = [&](std::ostream &os, const mini_json::Value &v)
    {
        using T = mini_json::Value::Type;
        switch (v.type)
        {
        case T::Null:
            os << "null";
            break;
        case T::Bool:
            os << (v.b ? "true" : "false");
            break;
        case T::Number:
            os << std::setprecision(15) << v.num;
            break;
        case T::String:
            os << "\"" << json_esc(v.str) << "\"";
            break;
        case T::Array:
        {
            os << "[";
            for (size_t i = 0; i < v.arr.size(); i++)
            {
                if (i)
                    os << ",";
                emit_json(os, v.arr[i]);
            }
            os << "]";
            break;
        }
        case T::Object:
        {
            os << "{";
            size_t i = 0;
            for (const auto &kv : v.obj)
            {
                if (i++)
                    os << ",";
                os << "\"" << json_esc(kv.first) << "\":";
                emit_json(os, kv.second);
            }
            os << "}";
            break;
        }
        }
    };
    emit_json(inp_ss, jinput);
    string input_raw_preserved = inp_ss.str();

    // ── A2: Reconstruct employees and vehicles ────────────────
    vector<Employee> employees;
    vector<Vehicle> vehicles;

    parse_employees_from_input(jinput, employees);
    parse_vehicles_from_input(jinput, vehicles);
    reconstruct_routes(jout, vehicles, employees);

    if (debug)
    {
        int routed = 0;
        for (const auto &e : employees)
            if (e.is_routed)
                routed++;
        std::cout << "[DynamicHandler] Reconstructed " << employees.size()
                  << " employees (" << routed << " routed), "
                  << vehicles.size() << " vehicles.\n";
    }

    // ── B1: Parse new_employees.json ──────────────────────────
    if (debug)
        std::cout << "[DynamicHandler] Reading new employees: "
                  << new_emps_path << "\n";
    string new_raw = read_file(new_emps_path);
    mini_json::Value jnew = mini_json::parse(new_raw);

    const auto &jne = jnew["new_employees"];
    if (!jne.is_object())
        throw std::runtime_error("new_employees.json missing 'new_employees' object");

    vector<Employee> new_emps;
    for (const auto &kv : jne.obj)
    {
        const string &id = kv.first;
        const mini_json::Value &jed = kv.second;

        // Skip if already in the existing employee list.
        bool exists = false;
        for (const auto &e : employees)
            if (e.id == id)
            {
                exists = true;
                break;
            }
        if (exists)
        {
            if (debug)
                std::cout << "  [skip] " << id << " already in solution\n";
            continue;
        }

        Employee e;
        e.id = id;
        e.priority = jed["priority"].as_int(999);
        e.pickup = {jed["pickup"]["lat"].as_number(),
                    jed["pickup"]["lng"].as_number()};
        e.drop = {jed["drop"]["lat"].as_number(),
                  jed["drop"]["lng"].as_number()};

        // Time windows accept "HH:MM" strings.
        e.ready_time = parse_time(jed["earliest_pickup"].as_string("08:00"));
        e.due_time = parse_time(jed["latest_drop"].as_string("23:59"));

        e.veh_pref = parse_vehicle_category(
            jed["vehicle_preference"].as_string("any"));
        e.share_pref = parse_sharing_pref(
            jed["sharing_preference"].as_string("any"));
        e.is_routed = false;
        e.baseline_cost = jed["baseline_cost"].as_number(0.0);

        new_emps.push_back(e);
    }

    if (new_emps.empty())
    {
        if (debug)
            std::cout << "[DynamicHandler] No new employees to insert.\n";
        write_updated_output(updated_output_path, input_raw_preserved,
                             vehicles, employees);
        result.all_inserted = true;
        return result;
    }

    // ── B2: Sort by tightness, then insert ───────────────────
    std::sort(new_emps.begin(), new_emps.end(),
              [](const Employee &a, const Employee &b)
              {
                  if (a.due_time != b.due_time)
                      return a.due_time < b.due_time;
                  return a.ready_time < b.ready_time;
              });

    if (debug)
    {
        std::cout << "[DynamicHandler] Inserting order: ";
        for (const auto &e : new_emps)
            std::cout << e.id << " ";
        std::cout << "\n";
    }

    // Merge new employees into the master list BEFORE inserting
    // so that resimulate() can look them up by id.
    for (const auto &ne : new_emps)
        employees.push_back(ne);

    for (const auto &emp : new_emps)
    {
        DynOutcome outcome;
        outcome.emp_id = emp.id;

        if (debug)
            std::cout << "\n[" << emp.id << "] ready=" << format_time(emp.ready_time)
                      << " due=" << format_time(emp.due_time)
                      << " pref="
                      << (emp.veh_pref == PREMIUM  ? "premium"
                          : emp.veh_pref == NORMAL ? "normal"
                                                   : "any")
                      << " share="
                      << (emp.share_pref == SINGLE   ? "single"
                          : emp.share_pref == DOUBLE ? "double"
                          : emp.share_pref == TRIPLE ? "triple"
                                                     : "any")
                      << "\n";

        if (debug)
            std::cout << "  [Pass 1] checking existing routes...\n";
        if (insert_existing(emp, employees, vehicles, outcome, debug))
        {
            result.outcomes.push_back(outcome);
            continue;
        }

        if (debug)
            std::cout << "  [Pass 2] opening new trip...\n";
        if (insert_new_trip(emp, employees, vehicles, outcome, debug))
        {
            result.outcomes.push_back(outcome);
            continue;
        }

        outcome.inserted = false;
        outcome.reason = "No feasible position in any route, and no vehicle "
                         "can open a new trip within constraints.";
        result.outcomes.push_back(outcome);
        result.failed.push_back(outcome);
        result.all_inserted = false;
        g_unrouted_reason[emp.id] = outcome.reason;

        if (debug)
            std::cout << "  !!! FAILED: " << outcome.reason << "\n";
    }

    // ── C: Write updated output ───────────────────────────────
    write_updated_output(updated_output_path, input_raw_preserved,
                         vehicles, employees);

    if (debug)
    {
        int ok = 0;
        for (const auto &o : result.outcomes)
            if (o.inserted)
                ok++;
        std::cout << "\n[DynamicHandler] " << ok << "/"
                  << new_emps.size() << " inserted → "
                  << updated_output_path << "\n";
    }

    return result;
}