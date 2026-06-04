#include "infeasible_handler.h"
#include "config.h"
#include "geo.h"
#include "time_utils.h"
#include <iostream>
#include <iomanip>
#include <limits>
#include <algorithm>
#include <map>

// static const int SERVICE_MIN = 0;

// ─────────────────────────────────────────────────────────────
// CONSTRAINT HELPERS
// ─────────────────────────────────────────────────────────────

// static int sharing_cap(const Employee &emp)
// {
//     return 999;

// }

// Strict vehicle check — respects employee's preference exactly.
static bool vehicle_ok_strict(const Vehicle &v, const Employee &emp)
{

    return true;
}

// Relaxed vehicle check — ignores vehicle_preference entirely.
// Used only as a last-resort fallback when no strict vehicle works.
static bool vehicle_ok_relaxed(const Vehicle & /*v*/, const Employee & /*emp*/)
{
    return true; // any vehicle is acceptable
}

static bool capacity_ok(const Route &route, const Vehicle &v, const Employee &emp)
{
    int cap = std::min({(int)v.capacity, route.max_capacity, get_global_share_limit(emp.share_pref)});
    return (route.current_capacity + 1) <= cap;
}

// ─────────────────────────────────────────────────────────────
// Build employee lookup map
// ─────────────────────────────────────────────────────────────
static std::map<std::string, const Employee *>
make_lookup(const std::vector<Employee> &employees)
{
    std::map<std::string, const Employee *> m;
    for (const auto &e : employees)
        m[e.id] = &e;
    return m;
}

// ─────────────────────────────────────────────────────────────
// SIMULATE: insert emp at pos, return office arrival time.
// Does NOT check due_time — caller decides what slack to allow.
// Returns INT_MAX on structural failure.
// ─────────────────────────────────────────────────────────────
static int simulate_office_arrival(
    const Route &route,
    const Vehicle &v,
    const Employee &emp,
    int pos,
    const std::map<std::string, const Employee *> &lookup)
{
    int n = (int)route.stops.size();
    std::vector<Stop> cand;
    cand.reserve(n + 1);
    for (int i = 0; i < pos; ++i)
        cand.push_back(route.stops[i]);

    Stop pu;
    pu.emp_id = emp.id;
    pu.loc = emp.pickup;
    pu.is_pickup = true;
    pu.arrival_time = pu.begin_service = pu.departure_time = 0;
    cand.push_back(pu);

    for (int i = pos; i < n; ++i)
        cand.push_back(route.stops[i]);
    if (cand.back().emp_id == "END")
        cand.back().loc = OFFICE;

    for (int i = 1; i < (int)cand.size(); ++i)
    {
        int arrive = cand[i - 1].departure_time + travel_minutes(get_dist(cand[i - 1], cand[i]), v.speed_kmh);
        cand[i].arrival_time = arrive;
        if (cand[i].emp_id == "END")
        {
            cand[i].begin_service = cand[i].departure_time = arrive;
        }
        else
        {
            auto it = lookup.find(cand[i].emp_id);
            if (it == lookup.end())
                return std::numeric_limits<int>::max();
            int begin = std::max(arrive, it->second->ready_time);
            cand[i].begin_service = begin;
            cand[i].departure_time = begin + SERVICE_MIN;
        }
    }
    return cand.back().arrival_time;
}

// ─────────────────────────────────────────────────────────────
// BINARY SEARCH: find minimum slack s such that emp can be
// inserted into the route with office_arrival <= due_time + s.
// Pass max_slack=1440 for unlimited (full day).
// Returns -1 if impossible even at max_slack.
// ─────────────────────────────────────────────────────────────
static int binary_search_slack(
    const Route &route,
    const Vehicle &v,
    const Employee &emp,
    const std::map<std::string, const Employee *> &lookup,
    int max_slack)
{
    int n = (int)route.stops.size();

    auto feasible = [&](int s) -> bool
    {
        for (int pos = 1; pos <= n - 1; ++pos)
        {
            int office_arr = simulate_office_arrival(route, v, emp, pos, lookup);
            if (office_arr == std::numeric_limits<int>::max())
                continue;
            if (office_arr > emp.due_time + s)
                continue;

            bool ok = true;
            for (int i = 1; i + 1 < (int)route.stops.size(); ++i)
            {
                auto it = lookup.find(route.stops[i].emp_id);
                if (it == lookup.end())
                    continue;
                if (office_arr > it->second->due_time)
                {
                    ok = false;
                    break;
                }
            }
            if (ok)
                return true;
        }
        return false;
    };

    if (!feasible(max_slack))
        return -1;

    int lo = 0, hi = max_slack;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (feasible(mid))
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

// ─────────────────────────────────────────────────────────────
// COMMIT: insert emp at cheapest feasible position with `slack`.
// ─────────────────────────────────────────────────────────────
static bool commit_insertion(
    Route &route,
    const Vehicle &v,
    const Employee &emp,
    const std::map<std::string, const Employee *> &lookup,
    int slack)
{
    int n = (int)route.stops.size();
    double best_cost = std::numeric_limits<double>::infinity();
    std::vector<Stop> best_stops;

    for (int pos = 1; pos <= n - 1; ++pos)
    {
        std::vector<Stop> cand;
        cand.reserve(n + 1);
        for (int i = 0; i < pos; ++i)
            cand.push_back(route.stops[i]);

        Stop pu;
        pu.emp_id = emp.id;
        pu.loc = emp.pickup;
        pu.is_pickup = true;
        pu.arrival_time = pu.begin_service = pu.departure_time = 0;
        cand.push_back(pu);
        for (int i = pos; i < n; ++i)
            cand.push_back(route.stops[i]);
        if (cand.back().emp_id == "END")
            cand.back().loc = OFFICE;

        bool ok = true;
        for (int i = 1; i < (int)cand.size(); ++i)
        {
            int arrive = cand[i - 1].departure_time + travel_minutes(get_dist(cand[i - 1], cand[i]), v.speed_kmh);
            cand[i].arrival_time = arrive;
            if (cand[i].emp_id == "END")
            {
                cand[i].begin_service = cand[i].departure_time = arrive;
            }
            else
            {
                auto it = lookup.find(cand[i].emp_id);
                if (it == lookup.end())
                {
                    ok = false;
                    break;
                }
                int begin = std::max(arrive, it->second->ready_time);
                cand[i].begin_service = begin;
                cand[i].departure_time = begin + SERVICE_MIN;
            }
        }
        if (!ok)
            continue;

        int office_arr = cand.back().arrival_time;
        if (office_arr > emp.due_time + slack)
            continue;

        for (int i = 1; i + 1 < (int)cand.size() && ok; ++i)
        {
            if (cand[i].emp_id == emp.id)
                continue;
            auto it = lookup.find(cand[i].emp_id);
            if (it == lookup.end())
                continue;
            if (office_arr > it->second->due_time)
                ok = false;
        }
        if (!ok)
            continue;

        double dist = 0.0;
        for (int i = 1; i < (int)cand.size(); ++i)
            dist += get_dist(cand[i - 1], cand[i]);
        int t = cand.back().arrival_time - cand.front().departure_time;
        double cost = calc_route_cost(dist, v.cost_per_km, t);
        if (cost < best_cost)
        {
            best_cost = cost;
            best_stops = cand;
        }
    }

    if (best_stops.empty())
        return false;

    route.stops = std::move(best_stops);
    route.current_capacity++;
    route.total_distance = 0.0;
    for (int i = 1; i < (int)route.stops.size(); ++i)
        route.total_distance += get_dist(route.stops[i - 1], route.stops[i]);
    {
        int t = route.stops.back().arrival_time - route.stops.front().departure_time;
        route.total_cost = calc_route_cost(route.total_distance, v.cost_per_km, t);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// Compute minimum slack needed. Uses current_loc (not depot_loc).
// veh_ok_fn controls whether we use strict or relaxed category.
// ─────────────────────────────────────────────────────────────
static int compute_min_slack_needed(
    const Employee &emp,
    const std::vector<Vehicle> &vehicles,
    bool (*veh_ok_fn)(const Vehicle &, const Employee &))
{
    int best_office_arr = std::numeric_limits<int>::max();
    for (const auto &v : vehicles)
    {
        if (!veh_ok_fn(v, emp))
            continue;
        double d1 = get_dist(v.current_loc, emp.pickup);
        double d2 = get_dist(emp.pickup, OFFICE);
        int arr = v.available_time + travel_minutes(d1, v.speed_kmh);
        int dep = std::max(arr, emp.ready_time) + SERVICE_MIN;
        int off = dep + travel_minutes(d2, v.speed_kmh);
        best_office_arr = std::min(best_office_arr, off);
    }
    if (best_office_arr == std::numeric_limits<int>::max())
        return best_office_arr;
    return std::max(0, best_office_arr - emp.due_time);
}

// ─────────────────────────────────────────────────────────────
// Build a fresh route from current_loc at vehicle's available_time.
// ─────────────────────────────────────────────────────────────
static Route make_depot_route(const Vehicle &v, const Employee &emp)
{
    Route r;
    r.max_capacity = std::min((int)v.capacity, get_global_share_limit(emp.share_pref));
    r.current_capacity = 0;
    r.total_distance = r.total_cost = 0.0;

    Stop st;
    st.emp_id = "START";
    st.loc = v.current_loc;
    st.is_pickup = false;
    st.arrival_time = st.begin_service = st.departure_time = v.available_time;
    r.stops.push_back(st);

    Stop en;
    en.emp_id = "END";
    en.loc = OFFICE;
    en.is_pickup = false;
    r.stops.push_back(en);

    return r;
}

// ─────────────────────────────────────────────────────────────
// Core logic: try to route emp on all vehicles passing veh_ok_fn.
// PATH A: insert into existing route within budget slack.
// PATH B: open new trip with unlimited slack, prepend as Trip 1.
// Returns true if routed.
// ─────────────────────────────────────────────────────────────
static bool try_route_employee(
    Employee &emp,
    std::vector<Vehicle> &vehicles,
    const std::map<std::string, const Employee *> &lookup,
    int effective_budget,
    bool (*veh_ok_fn)(const Vehicle &, const Employee &),
    InfeasibleResult &res)
{
    // ── PATH A: existing route ────────────────────────────────
    {
        struct RouteCand
        {
            int vi, ri, min_slack;
            double cost_delta;
        };
        std::vector<RouteCand> route_cands;

        for (int vi = 0; vi < (int)vehicles.size(); ++vi)
        {
            auto &v = vehicles[vi];
            if (!veh_ok_fn(v, emp))
                continue;

            for (int ri = 0; ri < (int)v.routes.size(); ++ri)
            {
                auto &r = v.routes[ri];
                if (!capacity_ok(r, v, emp))
                    continue;

                int s = binary_search_slack(r, v, emp, lookup, effective_budget);
                if (s < 0)
                    continue;

                Route tmp = r;
                if (commit_insertion(tmp, v, emp, lookup, s))
                {
                    double delta = tmp.total_cost - r.total_cost;
                    route_cands.push_back({vi, ri, s, delta});
                }
            }
        }

        if (!route_cands.empty())
        {
            std::sort(route_cands.begin(), route_cands.end(),
                      [](const RouteCand &a, const RouteCand &b)
                      {
                          if (a.min_slack != b.min_slack)
                              return a.min_slack < b.min_slack;
                          return a.cost_delta < b.cost_delta;
                      });
            auto &best = route_cands[0];
            if (commit_insertion(vehicles[best.vi].routes[best.ri],
                                 vehicles[best.vi], emp, lookup, best.min_slack))
            {
                vehicles[best.vi].total_cost = 0.0;
                for (const auto &rr : vehicles[best.vi].routes)
                    vehicles[best.vi].total_cost += rr.total_cost;
                emp.is_routed = true;
                emp.due_time += best.min_slack;
                res.resolved = true;
                res.method = "existing_route";
                res.slack_applied = best.min_slack;
                res.vehicle_id = vehicles[best.vi].id;
                return true;
            }
        }
    }

    // ── PATH B: new trip, unlimited slack, prepended as Trip 1 ──
    {
        struct TripCand
        {
            int vi, min_slack, arrive_pickup;
            double trip_cost;
        };
        std::vector<TripCand> trip_cands;

        for (int vi = 0; vi < (int)vehicles.size(); ++vi)
        {
            const Vehicle &v = vehicles[vi];
            if (!veh_ok_fn(v, emp))
                continue;

            Route r = make_depot_route(v, emp);

            int s = binary_search_slack(r, v, emp, lookup, 1440);
            if (s < 0)
                continue;

            Route tmp = r;
            if (commit_insertion(tmp, v, emp, lookup, s))
            {
                double d1 = get_dist(v.current_loc, emp.pickup);
                int arr_pu = v.available_time + travel_minutes(d1, v.speed_kmh);
                trip_cands.push_back({vi, s, arr_pu, tmp.total_cost});
            }
        }

        if (!trip_cands.empty())
        {
            // Pick vehicle that reaches pickup earliest → least wait for employee
            std::sort(trip_cands.begin(), trip_cands.end(),
                      [](const TripCand &a, const TripCand &b)
                      {
                          if (a.arrive_pickup != b.arrive_pickup)
                              return a.arrive_pickup < b.arrive_pickup;
                          if (a.min_slack != b.min_slack)
                              return a.min_slack < b.min_slack;
                          return a.trip_cost < b.trip_cost;
                      });

            auto &best = trip_cands[0];
            Vehicle &v = vehicles[best.vi];

            Route r = make_depot_route(v, emp);
            if (commit_insertion(r, v, emp, lookup, best.min_slack))
            {
                // APPEND: this trip happens after the vehicle's current trips
                v.routes.push_back(std::move(r));

                v.total_cost = 0.0;
                for (const auto &rr : v.routes)
                    v.total_cost += rr.total_cost;

                // Update v.available_time and v.current_loc to chain trips
                v.available_time = v.routes.back().stops.back().arrival_time;
                v.current_loc = v.routes.back().stops.back().loc;

                emp.is_routed = true;
                emp.due_time += best.min_slack;
                res.resolved = true;
                res.method = "new_trip (appended)";
                res.slack_applied = best.min_slack;
                res.vehicle_id = v.id;
                return true;
            }
        }
    }

    return false;
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: resolve_infeasible
//
// Constraint relaxation tiers (applied in order until routed):
//
//   Tier 1 — Strict vehicle preference + budget slack
//             (normal handler behaviour, respects all constraints)
//
//   Tier 2 — Strict vehicle preference + UNLIMITED slack
//             (time window violated, but category/sharing respected)
//
//   Tier 3 — Relaxed vehicle preference + UNLIMITED slack
//             (last resort: any vehicle, any time window)
//             Only fires if employee has no compatible vehicle at all.
//
// Every employee with at least one vehicle in the fleet WILL be routed.
// The method field in the result records which tier was used.
// ─────────────────────────────────────────────────────────────
std::vector<InfeasibleResult> resolve_infeasible(
    std::vector<Employee> &employees,
    std::vector<Vehicle> &vehicles,
    const std::map<int, int> &priority_budget)
{
    auto lookup = make_lookup(employees);

    struct Candidate
    {
        Employee *emp;
        int slack_needed; // with strict vehicle pref
        int budget;
        int effective_budget;
    };
    std::vector<Candidate> queue;

    for (auto &emp : employees)
    {
        if (emp.is_routed)
            continue;

        // Compute slack needed with strict pref first; fall back to relaxed
        // for the table display so we always show a number.
        int needed = compute_min_slack_needed(emp, vehicles, vehicle_ok_strict);
        if (needed == std::numeric_limits<int>::max())
            needed = compute_min_slack_needed(emp, vehicles, vehicle_ok_relaxed);

        int budget = 90;
        auto it = priority_budget.find(emp.priority);
        if (it != priority_budget.end())
            budget = it->second;

        int effective = (needed == std::numeric_limits<int>::max())
                            ? needed
                            : std::max(needed, budget);
        queue.push_back({&emp, needed, budget, effective});
    }

    std::sort(queue.begin(), queue.end(), [](const Candidate &a, const Candidate &b)
              {
        if (a.slack_needed != b.slack_needed) return a.slack_needed < b.slack_needed;
        return a.emp->priority < b.emp->priority; });

    // ── Print analysis table ──────────────────────────────────
    std::cout << "\n[Binary-search infeasibility handler]\n";
    std::cout << std::left
              << std::setw(6) << "Emp"
              << std::setw(5) << "Pri"
              << std::setw(8) << "VehPref"
              << std::setw(10) << "Share"
              << std::setw(14) << "SlackNeeded"
              << std::setw(12) << "Budget"
              << std::setw(16) << "Status"
              << "\n"
              << std::string(71, '-') << "\n";

    for (const auto &c : queue)
    {
        std::string vpref = (c.emp->veh_pref == PREMIUM)  ? "premium"
                            : (c.emp->veh_pref == NORMAL) ? "normal"
                                                          : "any";
        std::string spref = (c.emp->share_pref == SINGLE)   ? "single"
                            : (c.emp->share_pref == DOUBLE) ? "double"
                            : (c.emp->share_pref == TRIPLE) ? "triple"
                                                            : "any";
        bool impossible = (c.slack_needed == std::numeric_limits<int>::max());
        bool over = !impossible && (c.slack_needed > c.budget);
        std::string slack_str = impossible ? "impossible"
                                           : ("+" + std::to_string(c.slack_needed) + " min");
        std::string stat_str = impossible ? "NO VEH"
                               : over     ? "OVERRIDE"
                                          : "ok";

        std::cout << std::setw(6) << c.emp->id
                  << std::setw(5) << c.emp->priority
                  << std::setw(8) << vpref
                  << std::setw(10) << spref
                  << std::setw(14) << slack_str
                  << std::setw(12) << ("+" + std::to_string(c.budget) + " min")
                  << std::setw(16) << stat_str
                  << "\n";
    }
    std::cout << std::string(71, '-') << "\n\n";

    std::vector<InfeasibleResult> results;

    for (auto &cand : queue)
    {
        Employee &emp = *cand.emp;

        auto it_r = g_unrouted_reason.find(emp.id);
        std::string orig = (it_r != g_unrouted_reason.end()) ? it_r->second : "unknown";

        InfeasibleResult res;
        res.emp_id = emp.id;
        res.resolved = false;
        res.slack_applied = 0;
        res.slack_needed = cand.slack_needed;
        res.budget_overridden = (cand.slack_needed != std::numeric_limits<int>::max()) && (cand.slack_needed > cand.budget);
        res.original_reason = orig;
        res.method = "infeasible";

        bool solved = false;

        // ── TIER 1: strict vehicle pref + budget slack ────────────
        solved = try_route_employee(emp, vehicles, lookup,
                                    cand.effective_budget,
                                    vehicle_ok_strict, res);
        if (solved)
        {
            if (res.method.find("new_trip") != std::string::npos)
                res.method = "Tier1: new_trip (appended)";
            else
                res.method = "Tier1: existing_route";
        }

        // ── TIER 2: strict vehicle pref + unlimited slack ─────────
        // Fires when budget wasn't enough but category is still respected.
        if (!solved)
        {
            solved = try_route_employee(emp, vehicles, lookup,
                                        1440,
                                        vehicle_ok_strict, res);
            if (solved)
            {
                res.budget_overridden = true; // we went past budget
                if (res.method.find("new_trip") != std::string::npos)
                    res.method = "Tier2: new_trip (unlimited slack, appended)";
                else
                    res.method = "Tier2: existing_route (unlimited slack)";
            }
        }

        // ── TIER 3: relaxed vehicle pref + unlimited slack ────────
        // Last resort — any vehicle in the fleet, no category check.
        // Only reaches here if employee's preferred category has no
        // vehicles at all (e.g. wants premium but fleet has none).
        if (!solved)
        {
            solved = try_route_employee(emp, vehicles, lookup,
                                        1440,
                                        vehicle_ok_relaxed, res);
            if (solved)
            {
                res.budget_overridden = true;
                if (res.method.find("new_trip") != std::string::npos)
                    res.method = "Tier3: new_trip (relaxed pref + unlimited slack, appended)";
                else
                    res.method = "Tier3: existing_route (relaxed pref + unlimited slack)";
            }
        }

        if (solved)
        {
            g_unrouted_reason.erase(emp.id);
            lookup = make_lookup(employees); // refresh after mutation
        }
        else
        {
            // Should never happen with a non-empty fleet, but handle gracefully.
            g_unrouted_reason[emp.id] = "Fleet is empty — cannot route any employee";
        }

        results.push_back(res);
    }

    // Recompute all vehicle totals cleanly
    for (auto &v : vehicles)
    {
        v.total_cost = 0.0;
        for (const auto &r : v.routes)
            v.total_cost += r.total_cost;
    }
    return results;
}

// ─────────────────────────────────────────────────────────────
// Build priority budget from JSON metadata.
// No *10 multiplier — metadata value is already in minutes.
// ─────────────────────────────────────────────────────────────
std::map<int, int> build_priority_budget(
    const std::vector<std::pair<std::string, int>> &metadata_kv)
{
    std::map<int, int> budget;
    for (const auto &kv : metadata_kv)
    {
        const std::string &key = kv.first;
        if (key.rfind("priority_", 0) != 0)
            continue;
        if (key.find("_max_delay_min") == std::string::npos)
            continue;
        size_t a = key.find('_') + 1;
        size_t b = key.find('_', a);
        if (b == std::string::npos)
            continue;
        try
        {
            int pri = std::stoi(key.substr(a, b - a));
            budget[pri] = kv.second; // raw minutes, no *10
        }
        catch (...)
        {
        }
    }
    int defaults[] = {9, 6, 5, 3, 2};
    for (int p = 1; p <= 5; ++p)
        if (!budget.count(p))
            budget[p] = defaults[p - 1];
    return budget;
}

// ─────────────────────────────────────────────────────────────
// Report printer
// ─────────────────────────────────────────────────────────────
void print_infeasible_report(const std::vector<InfeasibleResult> &results)
{
    if (results.empty())
    {
        std::cout << "\n[infeasible handler] nothing to resolve.\n";
        return;
    }

    std::cout << "\n=======================================================\n";
    std::cout << "         INFEASIBILITY RESOLUTION REPORT               \n";
    std::cout << "=======================================================\n";

    int resolved = 0, overridden = 0, unresolved = 0;
    for (const auto &r : results)
    {
        std::cout << "\n  Employee        : " << r.emp_id << "\n";
        std::cout << "  Original reason : " << r.original_reason << "\n";
        if (r.slack_needed == std::numeric_limits<int>::max())
            std::cout << "  Slack needed    : impossible (no compatible vehicle)\n";
        else
            std::cout << "  Slack needed    : +" << r.slack_needed << " min\n";

        if (r.resolved)
        {
            ++resolved;
            if (r.budget_overridden)
            {
                ++overridden;
                std::cout << "  *** Budget overridden: needed +" << r.slack_needed
                          << " min, routed anyway (constraint violation documented) ***\n";
            }
            std::cout << "  Status          : RESOLVED\n";
            std::cout << "  Method          : " << r.method << "\n";
            std::cout << "  Vehicle         : " << r.vehicle_id << "\n";
            std::cout << "  Slack applied   : +" << r.slack_applied
                      << " min (exact minimum via binary search)\n";
        }
        else
        {
            ++unresolved;
            std::cout << "  Status          : *** TRULY INFEASIBLE ***\n";
            auto it = g_unrouted_reason.find(r.emp_id);
            if (it != g_unrouted_reason.end())
                std::cout << "  Detail          : " << it->second << "\n";
        }
    }

    std::cout << "\n-------------------------------------------------------\n";
    std::cout << "  Resolved        : " << resolved
              << " (of which " << overridden << " exceeded priority budget)\n";
    std::cout << "  Truly infeasible: " << unresolved << "\n";
    std::cout << "=======================================================\n";
}