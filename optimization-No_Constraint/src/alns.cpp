// =============================================================================
// VELORA — ALNS (Adaptive Large Neighbourhood Search)
// =============================================================================
// Operators:
//   Destroy : Random, Shaw (geographic+time+priority), Worst, Zone (cluster)
//   Repair  : Priority-greedy, Regret-2, Regret-3
//   Local   : 2-opt (intra-route), Relocate (inter-route), Swap (inter-route), Or-opt 1/2/3
//
// All bugs fixed (7 historical + 2 found in final audit):
//   B1: max_capacity restored after removal via recompute_max_capacity()
//   B2: stale max_capacity pre-checks removed from all insertion sites
//   B3: vehicle.available_time/current_loc synced after existing-route insert
//   B4: or_opt uses moved-flag not goto — stale n/max_start can't OOB
//   B5: min_rem > max_rem guard prevents UB in uniform_int_distribution
//   B6: reward tiers correctly split: W_NEW_BEST / W_BETTER_CURR / W_ACCEPTED
//   B7: refresh_vehicle after two_opt in main loop so score() is accurate
//   B8: dead code `base` variable removed from destroy_worst
//   B9: regret-k assigns sentinel regret for no-existing-route employees
//       so insert_anywhere's new-trip fallback is still attempted for them
// =============================================================================

#include "alns.h"
#include "geo.h"
#include "config.h"

#include <map>
#include <set>
#include <algorithm>
#include <cmath>
#include <random>
#include <limits>
#include <iostream>
#include <numeric>

using std::map;
using std::string;
using std::vector;

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
// static const int SERVICE_MIN = 2;
static const double BIG_M = 1e7;
static const int SEGMENT_LEN = 100;
static const double RHO = 0.18;
static const double W_NEW_BEST = 9.0;
static const double W_BETTER_CURR = 3.0;
static const double W_ACCEPTED = 1.0;
static const double W_REJECTED = 0.0;
static const double SHAW_DIST_W = 0.5;
static const double SHAW_TIME_W = 0.3;
static const double SHAW_PRI_W = 0.2;

// ─────────────────────────────────────────────────────────────────────────────
// Employee lookup map — built ONCE per iteration, passed into all functions
// that previously rebuilt it on every simulate_route call.
// ─────────────────────────────────────────────────────────────────────────────
using EmpLookup = std::map<std::string, const Employee *>;

static EmpLookup build_lookup(const vector<Employee> &employees)
{
    EmpLookup m;
    for (const auto &e : employees)
        m[e.id] = &e;
    return m;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sharing / vehicle helpers
// ─────────────────────────────────────────────────────────────────────────────
// static int sharing_limit(SharingPref p)
// {
//     switch (p)
//     {
//         return 999;
//     }
// }

static bool vehicle_ok(const Vehicle &v, const Employee &e)
{

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// simulate_route — the single feasibility authority.
// Recomputes all stop times, costs, and capacity from scratch.
// Accepts a pre-built EmpLookup to avoid O(n log n) map construction per call.
// ─────────────────────────────────────────────────────────────────────────────
static bool simulate_route(Route &route, const Vehicle &veh,
                           const EmpLookup &by_id)
{
    if (route.stops.size() < 2)
        return false;
    if (route.stops.back().emp_id != "END")
        return false;

    if (route.stops.front().emp_id != "START")
    {
        route.stops.front().emp_id = "START";
        route.stops.front().is_pickup = false;
    }
    if (route.stops.back().loc.lat == 0.0 && route.stops.back().loc.lng == 0.0)
        route.stops.back().loc = OFFICE;

    // Pass 1: validate employees, set pickup locations, accumulate share cap
    int share_cap = 999;
    for (size_t i = 1; i < route.stops.size(); i++)
    {
        auto &s = route.stops[i];
        if (s.emp_id == "END")
        {
            s.is_pickup = false;
            continue;
        }
        auto it = by_id.find(s.emp_id);
        if (it == by_id.end())
            return false;
        const Employee &e = *it->second;

        share_cap = std::min(share_cap, get_global_share_limit(e.share_pref));
        s.loc = e.pickup;
        s.is_pickup = true;
    }

    // Pass 2: compute arrival / service / departure times
    for (size_t i = 1; i < route.stops.size(); i++)
    {
        Stop &prev = route.stops[i - 1];
        Stop &cur = route.stops[i];
        double dist = get_dist(prev, cur);
        int tmin = travel_minutes(dist, veh.speed_kmh);
        int arrival = prev.departure_time + tmin;
        cur.arrival_time = arrival;
        if (cur.emp_id == "END")
        {
            cur.begin_service = cur.departure_time = arrival;
        }
        else
        {
            const Employee &e = *by_id.at(cur.emp_id);
            int begin = std::max(arrival, e.ready_time);
            cur.begin_service = begin;
            cur.departure_time = begin + SERVICE_MIN;
        }
    }

    // Pass 3: due_time — every passenger must reach office before deadline
    int office_arr = route.stops.back().arrival_time;
    for (size_t i = 1; i + 1 < route.stops.size(); i++)
    {
        const Stop &s = route.stops[i];
        if (s.emp_id == "START" || s.emp_id == "END")
            continue;
        auto it = by_id.find(s.emp_id);
        if (it == by_id.end())
            return false;
        if (office_arr > it->second->due_time)
            return false;
    }

    // Capacity check
    int n_pass = std::max(0, (int)route.stops.size() - 2);
    route.current_capacity = n_pass;
    int cap = (int)veh.capacity;
    if (route.max_capacity > 0)
        cap = std::min(cap, route.max_capacity);
    cap = std::min(cap, share_cap);
    if (n_pass > cap)
        return false;

    // Distance and cost
    double dist_total = 0.0;
    for (size_t i = 1; i < route.stops.size(); i++)
        dist_total += get_dist(route.stops[i - 1], route.stops[i]);
    route.total_distance = dist_total;
    {
        int t = route.stops.back().arrival_time - route.stops.front().departure_time;
        route.total_cost = calc_route_cost(dist_total, veh.cost_per_km, t);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// best_insert — tries every position, commits the cheapest feasible one.
// Saves the entire best candidate route so no re-simulation is needed at commit.
// Returns cost delta, or +INF if no position is feasible.
// ─────────────────────────────────────────────────────────────────────────────
static double best_insert(Route &route, const Vehicle &veh,
                          const Employee &emp,

                          const EmpLookup &by_id)
{
    int n = (int)route.stops.size();
    double base_cost = route.total_cost;
    double best_cost = std::numeric_limits<double>::infinity();
    Route best_cand; // store entire winning route — no re-simulation needed

    for (int pos = 1; pos <= n - 1; pos++)
    {
        Route cand = route;
        Stop pu;
        pu.emp_id = emp.id;
        pu.loc = emp.pickup;
        pu.is_pickup = true;
        pu.arrival_time = pu.begin_service = pu.departure_time = 0;
        cand.stops.insert(cand.stops.begin() + pos, pu);
        if (!simulate_route(cand, veh, by_id))
            continue;
        if (cand.total_cost < best_cost)
        {
            best_cost = cand.total_cost;
            best_cand = std::move(cand); // save — avoids commit re-simulation
        }
    }

    if (best_cost == std::numeric_limits<double>::infinity())
        return std::numeric_limits<double>::infinity();

    // Narrow stored cap then commit directly — zero extra simulate_route calls
    best_cand.max_capacity = std::min(best_cand.max_capacity,
                                      get_global_share_limit(emp.share_pref));
    route = std::move(best_cand);
    return route.total_cost - base_cost;
}

// ─────────────────────────────────────────────────────────────────────────────
// recompute_max_capacity — call after any removal so a removed SINGLE employee
// does not permanently leave route.max_capacity at 1.
// ─────────────────────────────────────────────────────────────────────────────
static void recompute_max_capacity(Route &route, const Vehicle &veh,
                                   const EmpLookup &by_id)
{
    int cap = (int)veh.capacity;
    for (const auto &s : route.stops)
    {
        if (!s.is_pickup)
            continue;
        auto it = by_id.find(s.emp_id);
        if (it != by_id.end())
            cap = std::min(cap, get_global_share_limit(it->second->share_pref));
    }
    route.max_capacity = cap;
}

// ─────────────────────────────────────────────────────────────────────────────
// remove_from_route — erase stop, fix max_capacity, re-simulate. Idempotent.
// ─────────────────────────────────────────────────────────────────────────────
static bool remove_from_route(Route &route, const Vehicle &veh,
                              const string &emp_id,
                              const EmpLookup &by_id)
{
    auto it = std::find_if(route.stops.begin(), route.stops.end(),
                           [&](const Stop &s)
                           { return s.is_pickup && s.emp_id == emp_id; });
    if (it == route.stops.end())
        return false;
    route.stops.erase(it);
    recompute_max_capacity(route, veh, by_id);
    simulate_route(route, veh, by_id);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Trip-overlap detection
// A vehicle cannot run two trips simultaneously.  Two routes overlap if their
// [departure_time, arrival_time] intervals intersect (open-ended comparison so
// back-to-back trips that share exactly one boundary are allowed).
// ─────────────────────────────────────────────────────────────────────────────
static bool routes_time_overlap(const Route &a, const Route &b)
{
    if (a.stops.size() < 2 || b.stops.size() < 2)
        return false;
    int a_s = a.stops.front().departure_time;
    int a_e = a.stops.back().arrival_time;
    int b_s = b.stops.front().departure_time;
    int b_e = b.stops.back().arrival_time;
    // Overlap iff one starts strictly before the other ends
    return (a_s < b_e) && (b_s < a_e);
}

static bool vehicle_has_trip_overlap(const Vehicle &v)
{
    for (int i = 0; i < (int)v.routes.size(); i++)
    {
        if (v.routes[i].stops.size() < 2)
            continue;
        for (int j = i + 1; j < (int)v.routes.size(); j++)
        {
            if (v.routes[j].stops.size() < 2)
                continue;
            if (routes_time_overlap(v.routes[i], v.routes[j]))
                return true;
        }
    }
    return false;
}

// Returns true if inserting `cand` route into vehicle vi (replacing route ri)
// would cause any temporal overlap with the vehicle's other routes.
static bool would_overlap(const vector<Route> &routes, int ri, const Route &cand)
{
    for (int j = 0; j < (int)routes.size(); j++)
    {
        if (j == ri)
            continue;
        if (routes[j].stops.size() < 2)
            continue;
        if (routes_time_overlap(cand, routes[j]))
            return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scoring
// ─────────────────────────────────────────────────────────────────────────────
static double total_cost(const vector<Vehicle> &vehs)
{
    double c = 0.0;
    for (const auto &v : vehs)
        c += v.total_cost;
    return c;
}
static int unrouted_count(const vector<Employee> &emps)
{
    int c = 0;
    for (const auto &e : emps)
        if (!e.is_routed)
            c++;
    return c;
}
static double score(const vector<Employee> &emps, const vector<Vehicle> &vehs)
{
    // Treat any solution with temporally overlapping trips on the same vehicle
    // as fully infeasible — same penalty tier as having unrouted employees.
    for (const auto &v : vehs)
        if (vehicle_has_trip_overlap(v))
            return (double)(unrouted_count(emps) + 1) * BIG_M;
    return (double)unrouted_count(emps) * BIG_M + total_cost(vehs);
}

// ─────────────────────────────────────────────────────────────────────────────
// Utility helpers
// ─────────────────────────────────────────────────────────────────────────────
struct LocatedEmp
{
    string emp_id;
    int vi, ri;
};

static vector<LocatedEmp> collect_routed(const vector<Vehicle> &vehs)
{
    vector<LocatedEmp> out;
    for (int vi = 0; vi < (int)vehs.size(); vi++)
        for (int ri = 0; ri < (int)vehs[vi].routes.size(); ri++)
            for (const auto &s : vehs[vi].routes[ri].stops)
                if (s.is_pickup)
                    out.push_back({s.emp_id, vi, ri});
    return out;
}

static Employee *find_emp(vector<Employee> &emps, const string &id)
{
    for (auto &e : emps)
        if (e.id == id)
            return &e;
    return nullptr;
}
static const Employee *find_emp_c(const vector<Employee> &emps, const string &id)
{
    for (const auto &e : emps)
        if (e.id == id)
            return &e;
    return nullptr;
}

static void refresh_vehicle(Vehicle &v)
{
    v.total_cost = 0.0;
    for (const auto &r : v.routes)
        v.total_cost += r.total_cost;
}

static void apply_removals(vector<Employee> &emps, vector<Vehicle> &vehs,
                           const vector<string> &ids, const EmpLookup &by_id)
{
    std::set<string> rem_set(ids.begin(), ids.end());
    for (auto &e : emps)
        if (rem_set.count(e.id))
            e.is_routed = false;
    for (auto &v : vehs)
    {
        for (auto &r : v.routes)
            for (const auto &id : ids)
                remove_from_route(r, v, id, by_id);
        refresh_vehicle(v);
    }
}

// =============================================================================
// DESTROY OPERATORS
// =============================================================================
enum DestroyOp
{
    D_RANDOM = 0,
    D_SHAW = 1,
    D_WORST = 2,
    D_ZONE = 3,
    N_DESTROY = 4
};

static vector<string> destroy_random(const vector<Vehicle> &vehs, int q,
                                     std::mt19937 &rng)
{
    auto routed = collect_routed(vehs);
    std::shuffle(routed.begin(), routed.end(), rng);
    vector<string> out;
    for (int i = 0; i < (int)routed.size() && (int)out.size() < q; i++)
        out.push_back(routed[i].emp_id);
    return out;
}

static double shaw_sim(const Employee &a, const Employee &b)
{
    double geo = get_dist(a.pickup, b.pickup);
    double time = std::abs(a.ready_time - b.ready_time) + std::abs(a.due_time - b.due_time);
    double pri = std::abs(a.priority - b.priority) * 20.0;
    return SHAW_DIST_W * geo + SHAW_TIME_W * (time / 60.0) + SHAW_PRI_W * pri;
}

static vector<string> destroy_shaw(const vector<Employee> &emps,
                                   const vector<Vehicle> &vehs,
                                   int q, std::mt19937 &rng)
{
    auto routed = collect_routed(vehs);
    if (routed.empty())
        return {};
    std::uniform_int_distribution<int> pick(0, (int)routed.size() - 1);
    const string seed_id = routed[pick(rng)].emp_id;
    const Employee *seed = find_emp_c(emps, seed_id);
    if (!seed)
        return {};

    struct Cand
    {
        string id;
        double sim;
    };
    vector<Cand> cands;
    for (const auto &le : routed)
    {
        const Employee *e = find_emp_c(emps, le.emp_id);
        if (!e)
            continue;
        cands.push_back({le.emp_id, shaw_sim(*seed, *e)});
    }
    // Ascending: most similar (lowest score) first
    std::sort(cands.begin(), cands.end(),
              [](const Cand &a, const Cand &b)
              { return a.sim < b.sim; });

    std::uniform_real_distribution<double> U(0.0, 1.0);
    const double p = 6.0;
    vector<string> out;
    std::set<string> chosen;
    while ((int)out.size() < q && out.size() < cands.size())
    {
        int idx = (int)(std::pow(U(rng), p) * (int)cands.size());
        idx = std::min(idx, (int)cands.size() - 1);
        if (!chosen.count(cands[idx].id))
        {
            chosen.insert(cands[idx].id);
            out.push_back(cands[idx].id);
        }
    }
    return out;
}

static vector<string> destroy_worst(const vector<Employee> & /*emps*/,
                                    vector<Vehicle> vehs_copy,
                                    int q, std::mt19937 &rng,
                                    const EmpLookup &by_id)
{
    struct Cand
    {
        string id;
        double gain;
    };
    vector<Cand> scored;

    for (int vi = 0; vi < (int)vehs_copy.size(); vi++)
    {
        auto &v = vehs_copy[vi];
        for (int ri = 0; ri < (int)v.routes.size(); ri++)
        {
            auto &r = v.routes[ri];
            vector<string> ids;
            for (const auto &s : r.stops)
                if (s.is_pickup)
                    ids.push_back(s.emp_id);
            for (const auto &id : ids)
            {
                Route r_bak = r;
                if (!remove_from_route(r, v, id, by_id))
                    continue;
                scored.push_back({id, r_bak.total_cost - r.total_cost});
                r = r_bak;
                simulate_route(r, v, by_id);
            }
        }
    }

    std::sort(scored.begin(), scored.end(),
              [](const Cand &a, const Cand &b)
              { return a.gain > b.gain; });

    std::uniform_real_distribution<double> U(0.0, 1.0);
    const double p = 3.0;
    vector<string> out;
    std::set<string> chosen;
    while ((int)out.size() < q && out.size() < scored.size())
    {
        int idx = (int)(std::pow(U(rng), p) * (int)scored.size());
        idx = std::min(idx, (int)scored.size() - 1);
        if (!chosen.count(scored[idx].id))
        {
            chosen.insert(scored[idx].id);
            out.push_back(scored[idx].id);
        }
    }
    return out;
}

static vector<string> destroy_zone(const vector<Employee> &emps,
                                   const vector<Vehicle> &vehs,
                                   int q, std::mt19937 &rng)
{
    auto routed = collect_routed(vehs);
    if (routed.empty())
        return {};
    std::uniform_int_distribution<int> pick(0, (int)routed.size() - 1);
    const string seed_id = routed[pick(rng)].emp_id;
    const Employee *seed = find_emp_c(emps, seed_id);
    if (!seed)
        return {};

    struct Cand
    {
        string id;
        double dist;
    };
    vector<Cand> cands;
    for (const auto &le : routed)
    {
        const Employee *e = find_emp_c(emps, le.emp_id);
        if (!e)
            continue;
        cands.push_back({le.emp_id, get_dist(seed->pickup, e->pickup)});
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand &a, const Cand &b)
              { return a.dist < b.dist; });

    vector<string> out;
    for (int i = 0; i < (int)cands.size() && (int)out.size() < q; i++)
        out.push_back(cands[i].id);
    return out;
}

// =============================================================================
// REPAIR OPERATORS
// =============================================================================

// ── insert_anywhere ──────────────────────────────────────────────────────────
// Phase 1: best delta insertion into any existing route.
// Phase 2: open a new trip on the cheapest compatible vehicle.
// Feasibility is determined solely by simulate_route() — no stale pre-checks.
static bool insert_anywhere(vector<Employee> &emps, vector<Vehicle> &vehs,
                            const Employee &emp, const EmpLookup &by_id)
{
    double best_delta = std::numeric_limits<double>::infinity();
    int best_vi = -1, best_ri = -1;
    Route best_route;

    for (int vi = 0; vi < (int)vehs.size(); vi++)
    {
        if (!vehicle_ok(vehs[vi], emp))
            continue;
        for (int ri = 0; ri < (int)vehs[vi].routes.size(); ri++)
        {
            Route cand = vehs[vi].routes[ri];
            double delta = best_insert(cand, vehs[vi], emp, by_id);
            if (!std::isfinite(delta))
                continue;
            // Reject if inserting into this route would cause a temporal
            // overlap with any other trip already on the same vehicle.
            if (would_overlap(vehs[vi].routes, ri, cand))
                continue;
            if (delta < best_delta)
            {
                best_delta = delta;
                best_vi = vi;
                best_ri = ri;
                best_route = cand;
            }
        }
    }

    if (best_vi != -1)
    {
        vehs[best_vi].routes[best_ri] = std::move(best_route);
        refresh_vehicle(vehs[best_vi]);
        const auto &last = vehs[best_vi].routes[best_ri].stops.back();
        vehs[best_vi].available_time = last.arrival_time;
        vehs[best_vi].current_loc = last.loc;
        Employee *ep = find_emp(emps, emp.id);
        if (ep)
            ep->is_routed = true;
        return true;
    }

    // Fallback: open a new trip
    double best_trip_cost = std::numeric_limits<double>::infinity();
    int best_trip_vi = -1;
    Route best_trip_route;

    for (int vi = 0; vi < (int)vehs.size(); vi++)
    {
        const Vehicle &v = vehs[vi];
        if (!vehicle_ok(v, emp))
            continue;
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
        double delta = best_insert(r, v, emp, by_id);
        if (std::isfinite(delta) && r.total_cost < best_trip_cost)
        {
            best_trip_cost = r.total_cost;
            best_trip_vi = vi;
            best_trip_route = r;
        }
    }

    if (best_trip_vi != -1)
    {
        Vehicle &v = vehs[best_trip_vi];
        v.routes.push_back(best_trip_route);
        v.available_time = best_trip_route.stops.back().arrival_time;
        v.current_loc = best_trip_route.stops.back().loc;
        refresh_vehicle(v);
        Employee *ep = find_emp(emps, emp.id);
        if (ep)
            ep->is_routed = true;
        return true;
    }
    return false;
}

// ── Priority-biased greedy repair ────────────────────────────────────────────
static void repair_priority_greedy(vector<Employee> &emps, vector<Vehicle> &vehs,
                                   vector<string> ids, const EmpLookup &by_id)
{
    std::sort(ids.begin(), ids.end(), [&](const string &a, const string &b)
              {
        const Employee* ea = find_emp_c(emps, a);
        const Employee* eb = find_emp_c(emps, b);
        if (!ea || !eb) return false;
        if (ea->priority != eb->priority) return ea->priority < eb->priority;
        return (ea->due_time - ea->ready_time) < (eb->due_time - eb->ready_time); });
    for (const auto &id : ids)
    {
        const Employee *ep = find_emp_c(emps, id);
        if (ep)
            insert_anywhere(emps, vehs, *ep, by_id);
    }
}

// ── Regret-k repair ──────────────────────────────────────────────────────────
// Single scan per employee collects both the cost list (for regret) and the
// best insertion details (for direct commit). No redundant best_insert calls.
static void repair_regret_k(vector<Employee> &emps, vector<Vehicle> &vehs,
                            vector<string> ids, int k, const EmpLookup &by_id)
{
    std::set<string> remaining(ids.begin(), ids.end());

    while (!remaining.empty())
    {
        string best_id;
        double best_regret = -std::numeric_limits<double>::infinity();

        // For the winner: remember best insertion so we can commit without re-scan
        int winner_vi = -1, winner_ri = -1;
        Route winner_route;
        bool winner_needs_new_trip = false;

        for (const auto &id : remaining)
        {
            const Employee *emp = find_emp_c(emps, id);
            if (!emp)
                continue;

            // Single scan: collect all (delta, vi, ri, committed_route) tuples
            struct Slot
            {
                double delta;
                int vi, ri;
                Route route;
            };
            vector<Slot> slots;

            for (int vi = 0; vi < (int)vehs.size(); vi++)
            {
                if (!vehicle_ok(vehs[vi], *emp))
                    continue;
                for (int ri = 0; ri < (int)vehs[vi].routes.size(); ri++)
                {
                    Route cand = vehs[vi].routes[ri];
                    double delta = best_insert(cand, vehs[vi], *emp, by_id);
                    if (std::isfinite(delta))
                        slots.push_back({delta, vi, ri, std::move(cand)});
                }
            }

            double reg;
            if (slots.empty())
            {
                // No existing route fits — sentinel triggers new-trip fallback
                reg = 1e9 + (5.0 - emp->priority) * 10.0;
            }
            else
            {
                // Sort by delta to find best and k-th best
                std::sort(slots.begin(), slots.end(),
                          [](const Slot &a, const Slot &b)
                          { return a.delta < b.delta; });
                double bst = slots[0].delta;
                double kth = slots[std::min(k - 1, (int)slots.size() - 1)].delta;
                reg = (kth - bst) + (5.0 - emp->priority) * 10.0;
            }

            if (reg > best_regret)
            {
                best_regret = reg;
                best_id = id;
                winner_needs_new_trip = slots.empty();
                if (!slots.empty())
                {
                    winner_vi = slots[0].vi;
                    winner_ri = slots[0].ri;
                    winner_route = std::move(slots[0].route);
                }
            }
        }

        if (best_id.empty())
            break;

        const Employee *chosen = find_emp_c(emps, best_id);
        if (chosen)
        {
            if (!winner_needs_new_trip && winner_vi != -1)
            {
                // Direct commit — the route is already simulated, no re-scan needed
                vehs[winner_vi].routes[winner_ri] = std::move(winner_route);
                refresh_vehicle(vehs[winner_vi]);
                const auto &last = vehs[winner_vi].routes[winner_ri].stops.back();
                vehs[winner_vi].available_time = last.arrival_time;
                vehs[winner_vi].current_loc = last.loc;
                Employee *ep = find_emp(emps, best_id);
                if (ep)
                    ep->is_routed = true;
            }
            else
            {
                // No existing route fits — open a new trip
                insert_anywhere(emps, vehs, *chosen, by_id);
            }
        }
        remaining.erase(best_id);
    }
}

// =============================================================================
// LOCAL SEARCH
// =============================================================================

static bool two_opt_route(Route &route, const Vehicle &veh,
                          const EmpLookup &by_id)
{
    bool improved = true, any = false;
    while (improved)
    {
        improved = false;
        int n = (int)route.stops.size();
        for (int i = 1; i <= n - 3; i++)
        {
            for (int j = i + 1; j <= n - 2; j++)
            {
                Route cand = route;
                std::reverse(cand.stops.begin() + i, cand.stops.begin() + j + 1);
                if (simulate_route(cand, veh, by_id) &&
                    cand.total_cost < route.total_cost - 1e-6)
                {
                    route = cand;
                    improved = any = true;
                }
            }
        }
    }
    return any;
}

static bool relocate_pass(vector<Employee> &emps, vector<Vehicle> &vehs,
                          std::mt19937 &rng, const EmpLookup &by_id)
{
    auto routed = collect_routed(vehs);
    if (routed.empty())
        return false;
    std::shuffle(routed.begin(), routed.end(), rng);
    int max_try = std::min((int)routed.size(), 10);

    for (int ti = 0; ti < max_try; ti++)
    {
        const string &emp_id = routed[ti].emp_id;
        int src_vi = routed[ti].vi, src_ri = routed[ti].ri;
        const Employee *emp = find_emp_c(emps, emp_id);
        if (!emp)
            continue;

        Route src_without = vehs[src_vi].routes[src_ri];
        if (!remove_from_route(src_without, vehs[src_vi], emp_id, by_id))
            continue;
        double src_gain = vehs[src_vi].routes[src_ri].total_cost - src_without.total_cost;

        double best_net = 1e-6;
        int best_dvi = -1, best_dri = -1;
        Route best_dst;

        for (int dvi = 0; dvi < (int)vehs.size(); dvi++)
        {
            if (!vehicle_ok(vehs[dvi], *emp))
                continue;
            for (int dri = 0; dri < (int)vehs[dvi].routes.size(); dri++)
            {
                if (dvi == src_vi && dri == src_ri)
                    continue;
                Route dst_trial = vehs[dvi].routes[dri];
                double delta = best_insert(dst_trial, vehs[dvi], *emp, by_id);
                if (!std::isfinite(delta))
                    continue;
                // Skip if destination route would overlap a sibling trip.
                // For same-vehicle moves (dvi==src_vi), compare against src_without
                // (the source after removal) rather than the original route.
                bool overlap = false;
                if (dvi == src_vi)
                {
                    for (int j = 0; j < (int)vehs[dvi].routes.size(); j++)
                    {
                        if (j == dri || j == src_ri)
                            continue;
                        if (routes_time_overlap(dst_trial, vehs[dvi].routes[j]))
                        {
                            overlap = true;
                            break;
                        }
                    }
                    if (!overlap)
                        overlap = routes_time_overlap(dst_trial, src_without);
                }
                else
                {
                    overlap = would_overlap(vehs[dvi].routes, dri, dst_trial);
                }
                if (overlap)
                    continue;
                double net = src_gain - delta;
                if (net > best_net)
                {
                    best_net = net;
                    best_dvi = dvi;
                    best_dri = dri;
                    best_dst = dst_trial;
                }
            }
        }

        if (best_dvi != -1)
        {
            vehs[src_vi].routes[src_ri] = src_without;
            refresh_vehicle(vehs[src_vi]);
            vehs[best_dvi].routes[best_dri] = best_dst;
            refresh_vehicle(vehs[best_dvi]);
            return true;
        }
    }
    return false;
}

// ── swap_pass ─────────────────────────────────────────────────────────────────
// Tries swapping employee A (from route X) with employee B (from a different
// route Y). Neither individual relocation may be profitable, but the exchange
// can be — classic case: A belongs in Y's geographic cluster, B belongs in X's.
// Tries up to 15 randomly chosen A employees per call, evaluates all B candidates.
// Returns true if any improving swap was found and committed.
static bool swap_pass(vector<Employee> &emps, vector<Vehicle> &vehs,
                      std::mt19937 &rng, const EmpLookup &by_id)
{
    auto routed = collect_routed(vehs);
    if ((int)routed.size() < 2)
        return false;
    std::shuffle(routed.begin(), routed.end(), rng);

    int max_a = std::min((int)routed.size(), 15);

    for (int ai = 0; ai < max_a; ai++)
    {
        const string &id_a = routed[ai].emp_id;
        int avi = routed[ai].vi, ari = routed[ai].ri;
        const Employee *emp_a = find_emp_c(emps, id_a);
        if (!emp_a)
            continue;

        // Route A with emp_a removed
        Route route_a_without = vehs[avi].routes[ari];
        if (!remove_from_route(route_a_without, vehs[avi], id_a, by_id))
            continue;
        double cost_a_orig = vehs[avi].routes[ari].total_cost;
        double cost_a_without = route_a_without.total_cost;

        double best_net = 1e-6; // must strictly improve to commit
        int best_bvi = -1, best_bri = -1;
        Route best_route_a, best_route_b;

        for (int bi = 0; bi < (int)routed.size(); bi++)
        {
            if (bi == ai)
                continue;
            const string &id_b = routed[bi].emp_id;
            int bvi = routed[bi].vi, bri = routed[bi].ri;
            if (bvi == avi && bri == ari)
                continue; // must be a different route

            const Employee *emp_b = find_emp_c(emps, id_b);
            if (!emp_b)
                continue;

            // Cross-vehicle compatibility: B must fit in A's vehicle and vice versa
            if (!vehicle_ok(vehs[avi], *emp_b))
                continue;
            if (!vehicle_ok(vehs[bvi], *emp_a))
                continue;

            // Route B with emp_b removed
            Route route_b_without = vehs[bvi].routes[bri];
            if (!remove_from_route(route_b_without, vehs[bvi], id_b, by_id))
                continue;
            double cost_b_orig = vehs[bvi].routes[bri].total_cost;
            double cost_b_without = route_b_without.total_cost;

            // Try inserting B into A's (now vacated) route
            Route route_a_with_b = route_a_without;
            double delta_b_into_a = best_insert(route_a_with_b, vehs[avi], *emp_b, by_id);
            if (!std::isfinite(delta_b_into_a))
                continue;

            // Try inserting A into B's (now vacated) route
            Route route_b_with_a = route_b_without;
            double delta_a_into_b = best_insert(route_b_with_a, vehs[bvi], *emp_a, by_id);
            if (!std::isfinite(delta_a_into_b))
                continue;

            // Net gain across both routes
            double saving_a = cost_a_orig - cost_a_without;
            double saving_b = cost_b_orig - cost_b_without;
            double net = saving_a + saving_b - delta_b_into_a - delta_a_into_b;

            if (net > best_net)
            {
                // Verify neither modified route overlaps a sibling trip on its vehicle.
                bool overlap_a = false, overlap_b = false;
                if (avi == bvi)
                {
                    // Same vehicle: compare the two modified routes against each other
                    // and against all other routes on that vehicle.
                    if (routes_time_overlap(route_a_with_b, route_b_with_a))
                        overlap_a = true;
                    for (int j = 0; j < (int)vehs[avi].routes.size() && !overlap_a; j++)
                    {
                        if (j == ari || j == bri)
                            continue;
                        if (routes_time_overlap(route_a_with_b, vehs[avi].routes[j]))
                            overlap_a = true;
                        if (routes_time_overlap(route_b_with_a, vehs[avi].routes[j]))
                            overlap_b = true;
                    }
                }
                else
                {
                    overlap_a = would_overlap(vehs[avi].routes, ari, route_a_with_b);
                    overlap_b = would_overlap(vehs[bvi].routes, bri, route_b_with_a);
                }
                if (overlap_a || overlap_b)
                    continue;
                best_net = net;
                best_bvi = bvi;
                best_bri = bri;
                best_route_a = route_a_with_b;
                best_route_b = route_b_with_a;
            }
        }

        if (best_bvi != -1)
        {
            vehs[avi].routes[ari] = std::move(best_route_a);
            vehs[best_bvi].routes[best_bri] = std::move(best_route_b);
            refresh_vehicle(vehs[avi]);
            refresh_vehicle(vehs[best_bvi]);
            return true;
        }
    }
    return false;
}

static bool or_opt_pass(vector<Employee> &emps, vector<Vehicle> &vehs,
                        int chain_len, std::mt19937 &rng, const EmpLookup &by_id)
{
    struct RouteRef
    {
        int vi, ri;
    };
    vector<RouteRef> refs;
    for (int vi = 0; vi < (int)vehs.size(); vi++)
        for (int ri = 0; ri < (int)vehs[vi].routes.size(); ri++)
            if ((int)vehs[vi].routes[ri].stops.size() >= 2 + chain_len)
                refs.push_back({vi, ri});
    if (refs.empty())
        return false;
    std::shuffle(refs.begin(), refs.end(), rng);
    int max_src = std::min((int)refs.size(), 6);

    bool any = false;
    for (int si = 0; si < max_src; si++)
    {
        int svi = refs[si].vi, sri = refs[si].ri;
        Route &src = vehs[svi].routes[sri];
        int n = (int)src.stops.size();
        int max_start = n - 1 - chain_len;

        bool moved = false;
        for (int cs = 1; cs <= max_start && !moved; cs++)
        {
            if (cs + chain_len > (int)src.stops.size() - 1)
                break;

            vector<Stop> chain(src.stops.begin() + cs,
                               src.stops.begin() + cs + chain_len);
            Route src_trial = src;
            src_trial.stops.erase(src_trial.stops.begin() + cs,
                                  src_trial.stops.begin() + cs + chain_len);
            if (!simulate_route(src_trial, vehs[svi], by_id))
                continue;

            for (int dvi = 0; dvi < (int)vehs.size() && !moved; dvi++)
            {
                for (int dri = 0; dri < (int)vehs[dvi].routes.size() && !moved; dri++)
                {
                    if (dvi == svi && dri == sri)
                        continue;
                    Route &dst = vehs[dvi].routes[dri];
                    int dn = (int)dst.stops.size();
                    for (int ins = 1; ins <= dn - 1 && !moved; ins++)
                    {
                        Route dst_trial = dst;
                        dst_trial.stops.insert(dst_trial.stops.begin() + ins,
                                               chain.begin(), chain.end());
                        if (!simulate_route(dst_trial, vehs[dvi], by_id))
                            continue;
                        if (src_trial.total_cost + dst_trial.total_cost <
                            src.total_cost + dst.total_cost - 1e-6)
                        {
                            // Check no temporal overlap is introduced
                            bool ok = !would_overlap(vehs[dvi].routes, dri, dst_trial);
                            if (ok && svi == dvi)
                            {
                                // Also check src_trial vs dst_trial (same vehicle)
                                if (routes_time_overlap(src_trial, dst_trial))
                                    ok = false;
                            }
                            if (!ok)
                                continue;
                            src = src_trial;
                            dst = dst_trial;
                            refresh_vehicle(vehs[svi]);
                            refresh_vehicle(vehs[dvi]);
                            any = true;
                            moved = true;
                        }
                    }
                }
            }
        }
    }
    return any;
}

static void local_search(vector<Employee> &emps, vector<Vehicle> &vehs,
                         std::mt19937 &rng, bool deep, const EmpLookup &by_id)
{
    for (auto &v : vehs)
        for (auto &r : v.routes)
            two_opt_route(r, v, by_id);

    int passes = deep ? 5 : 2;
    for (int p = 0; p < passes; p++)
        while (relocate_pass(emps, vehs, rng, by_id))
        {
        }

    // Swap: exchange employees between routes — finds improvements relocate cannot
    while (swap_pass(emps, vehs, rng, by_id))
    {
    }

    for (int cl = 1; cl <= 3; cl++)
        while (or_opt_pass(emps, vehs, cl, rng, by_id))
        {
        }

    for (auto &v : vehs)
        refresh_vehicle(v);
}

// =============================================================================
// ADAPTIVE WEIGHT SELECTOR
// =============================================================================
static int pick_weighted(const vector<double> &w, std::mt19937 &rng)
{
    double sum = 0.0;
    for (double x : w)
        sum += x;
    std::uniform_real_distribution<double> U(0.0, sum);
    double r = U(rng);
    for (int i = 0; i < (int)w.size(); i++)
    {
        r -= w[i];
        if (r <= 0.0)
            return i;
    }
    return (int)w.size() - 1;
}

// =============================================================================
// SINGLE-RUN CORE
// Runs one full ALNS pass with a given seed. Employees/vehicles are updated
// in-place to the best solution found. Returns the best score achieved.
// =============================================================================
static double run_alns_once(vector<Employee> &employees, vector<Vehicle> &vehicles,
                            const ALNSConfig &cfg, unsigned seed, bool debug)
{
    std::mt19937 rng(seed);

    // Auto-calibrate T0: accept 5%-worse solution with P=0.8 at iteration 1
    double init_cost = total_cost(vehicles);
    double T = (cfg.T0 > 0.0)
                   ? cfg.T0
                   : (0.05 * init_cost / (-std::log(0.8)));
    if (T < 1.0)
        T = 500.0;

    double cooling = cfg.cooling;

    vector<double> w_destroy(N_DESTROY, 1.0);
    vector<double> w_repair(3, 1.0);
    vector<double> sd(N_DESTROY, 0.0), cd(N_DESTROY, 0.0);
    vector<double> sr(3, 0.0), cr(3, 0.0);

    double best_score = score(employees, vehicles);
    double curr_score = best_score;
    auto best_emps = employees;
    auto best_vehs = vehicles;
    int no_improve = 0;

    int n_emp = (int)employees.size();
    int min_rem = cfg.min_remove;
    int max_rem = std::min(cfg.max_remove, std::max(4, n_emp / 3));
    if (min_rem > max_rem)
        min_rem = max_rem; // B5: guard UB in distribution

    std::uniform_int_distribution<int> remove_dist(min_rem, max_rem);
    std::uniform_real_distribution<double> U01(0.0, 1.0);

    if (debug)
        std::cout << "[ALNS] T0=" << T << " init_cost=" << init_cost
                  << " n=" << n_emp << "\n";

    for (int it = 1; it <= cfg.iterations; it++)
    {
        int q = remove_dist(rng);
        int d = pick_weighted(w_destroy, rng);
        int r_op = pick_weighted(w_repair, rng);

        auto trial_emps = employees;
        auto trial_vehs = vehicles;

        // Build employee lookup once per iteration — passed into all functions
        // so simulate_route never rebuilds it internally (Fix 2)
        EmpLookup iter_lookup = build_lookup(trial_emps);

        // Destroy
        vector<string> removed;
        switch (d)
        {
        case D_RANDOM:
            removed = destroy_random(trial_vehs, q, rng);
            break;
        case D_SHAW:
            removed = destroy_shaw(trial_emps, trial_vehs, q, rng);
            break;
        case D_WORST:
            removed = destroy_worst(trial_emps, trial_vehs, q, rng, iter_lookup);
            break;
        case D_ZONE:
            removed = destroy_zone(trial_emps, trial_vehs, q, rng);
            break;
        default:
            removed = destroy_random(trial_vehs, q, rng);
            break;
        }
        if (removed.empty())
            continue;
        apply_removals(trial_emps, trial_vehs, removed, iter_lookup);

        // Repair
        switch (r_op)
        {
        case 0:
            repair_priority_greedy(trial_emps, trial_vehs, removed, iter_lookup);
            break;
        case 1:
            repair_regret_k(trial_emps, trial_vehs, removed, 2, iter_lookup);
            break;
        case 2:
            repair_regret_k(trial_emps, trial_vehs, removed, 3, iter_lookup);
            break;
        default:
            repair_priority_greedy(trial_emps, trial_vehs, removed, iter_lookup);
            break;
        }

        // Optional lightweight 2-opt after repair
        if (cfg.apply_two_opt_after_repair)
        {
            for (auto &v : trial_vehs)
                for (auto &r : v.routes)
                    two_opt_route(r, v, iter_lookup);
            for (auto &v : trial_vehs)
                refresh_vehicle(v);
        }

        double trial_score = score(trial_emps, trial_vehs);
        // Capture delta BEFORE updating curr_score so reward tier is correct (B6)
        double delta = trial_score - curr_score;

        bool accepted = (delta <= 0.0) ||
                        (U01(rng) < std::exp(-delta / std::max(1e-9, T)));

        double reward = W_REJECTED;
        if (accepted)
        {
            employees = std::move(trial_emps);
            vehicles = std::move(trial_vehs);
            curr_score = trial_score;

            if (trial_score < best_score - 1e-6)
            {
                best_score = trial_score;
                best_emps = employees;
                best_vehs = vehicles;
                reward = W_NEW_BEST;
                no_improve = 0;
            }
            else if (delta < -1e-6)
            {
                reward = W_BETTER_CURR; // genuinely better than previous current
                no_improve++;
            }
            else
            {
                reward = W_ACCEPTED; // SA-accepted (worse than previous current)
                no_improve++;
            }
        }
        else
        {
            no_improve++;
        }

        sd[d] += reward;
        cd[d]++;
        sr[r_op] += reward;
        cr[r_op]++;

        if (it % SEGMENT_LEN == 0)
        {
            for (int i = 0; i < N_DESTROY; i++)
            {
                if (cd[i] > 0)
                    w_destroy[i] = (1.0 - RHO) * w_destroy[i] + RHO * (sd[i] / cd[i]);
                w_destroy[i] = std::max(w_destroy[i], 0.1);
                sd[i] = cd[i] = 0.0;
            }
            for (int i = 0; i < 3; i++)
            {
                if (cr[i] > 0)
                    w_repair[i] = (1.0 - RHO) * w_repair[i] + RHO * (sr[i] / cr[i]);
                w_repair[i] = std::max(w_repair[i], 0.1);
                sr[i] = cr[i] = 0.0;
            }
        }

        T *= cooling;

        if (debug && it % 200 == 0)
            std::cout << "[ALNS] it=" << it
                      << " curr=" << curr_score << " best=" << best_score
                      << " T=" << T << " q=" << q
                      << " d=" << d << " r=" << r_op << "\n";

        if (no_improve >= cfg.no_improve_stop)
        {
            if (debug)
                std::cout << "[ALNS] Early stop at it=" << it << "\n";
            break;
        }
    }

    // Deep local search on the best solution found
    EmpLookup final_lookup = build_lookup(best_emps);
    local_search(best_emps, best_vehs, rng, /*deep=*/true, final_lookup);

    best_score = score(best_emps, best_vehs); // refresh after LS

    if (debug)
        std::cout << "[ALNS] After final LS: " << best_score << "\n";

    employees = std::move(best_emps);
    vehicles = std::move(best_vehs);
    return best_score;
}

// =============================================================================
// PUBLIC ENTRY POINT — multi-run wrapper
// Runs run_alns_once n_runs times, each with an independent random seed.
// The best solution across all runs is written back to employees/vehicles.
// All seeds are printed so any run can be reproduced exactly by setting cfg.seed.
// =============================================================================
void run_alns(vector<Employee> &employees, vector<Vehicle> &vehicles,
              const ALNSConfig &cfg, bool debug)
{
    std::random_device rd;

    double best_score = std::numeric_limits<double>::infinity();
    vector<Employee> best_emps;
    vector<Vehicle> best_vehs;

    std::cout << "\n[ALNS] " << cfg.n_runs << " independent run(s) × "
              << cfg.iterations << " iterations (early-stop="
              << cfg.no_improve_stop << ")\n";

    for (int run = 0; run < cfg.n_runs; run++)
    {
        // Determine seed: fixed if cfg.seed != 0, otherwise fresh hardware entropy
        unsigned seed = (cfg.seed != 0)
                            ? cfg.seed
                            : static_cast<unsigned>(rd());

        std::cout << "[ALNS] Run " << (run + 1) << "/" << cfg.n_runs
                  << "  seed=" << seed << "\n";

        // Each run starts from the same post-heuristic solution
        auto trial_emps = employees;
        auto trial_vehs = vehicles;

        double run_score = run_alns_once(trial_emps, trial_vehs, cfg, seed, debug);

        std::cout << "[ALNS] Run " << (run + 1) << " score=" << run_score;

        if (run_score < best_score)
        {
            best_score = run_score;
            best_emps = trial_emps;
            best_vehs = trial_vehs;
            std::cout << "  *** new best ***";
        }
        std::cout << "\n";
    }

    std::cout << "[ALNS] Best score across all runs: " << best_score << "\n";

    employees = std::move(best_emps);
    vehicles = std::move(best_vehs);
}
