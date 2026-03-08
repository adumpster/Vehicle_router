#include "output_json.h"
#include "config.h"
#include "geo.h"
#include "time_utils.h"
#include "json_serialize.h"


#include <fstream>
#include <iomanip>
#include <sstream>

// Escape strings for JSON safely (quotes, backslashes, newlines, etc.)
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                // Basic ASCII safe output. (If you need full unicode escaping later, we can add it.)
                out += c;
                break;
        }
    }
    return out;
}

static int count_routed(const std::vector<Employee>& emps) {
    int c = 0;
    for (const auto& e : emps) if (e.is_routed) c++;
    return c;
}

static double total_cost_all(const std::vector<Vehicle>& vehs) {
    double c = 0.0;
    for (const auto& v : vehs) c += v.total_cost;
    return c;
}

// Collect passengers from a route in the stop order
static std::vector<std::string> passengers_in_route(const Route& r) {
    std::vector<std::string> ids;
    for (const auto& s : r.stops) {
        if (s.is_pickup) ids.push_back(s.emp_id);
    }
    return ids;
}

static const Stop* find_pickup_stop(const Route& r, const std::string& emp_id) {
    for (const auto& s : r.stops) {
        if (s.is_pickup && s.emp_id == emp_id) return &s;
    }
    return nullptr;
}


bool write_output_json(
    const std::string& filename,
    const std::string& input_json_raw,
    const std::vector<Vehicle>& vehs,
    const std::vector<Employee>& emps
    
) {
    std::ofstream out(filename);
    if (!out.is_open()) return false;

    const int total_emps = (int)emps.size();
    const int routed = count_routed(emps);
    const int unrouted = total_emps - routed;

    // baseline_cost is optional in your input; if present, we can compute totals
    double baseline_total = 0.0;
    for (const auto& e : emps) baseline_total += e.baseline_cost;

    double optimized_total = total_cost_all(vehs);
    double net_savings = baseline_total - optimized_total;
    double savings_pct = (baseline_total > 1e-9) ? (net_savings / baseline_total) * 100.0 : 0.0;

    out << std::fixed << std::setprecision(6);
    out << "{\n";

    out<<" \"input\":"<<input_json_raw<<",\n";

    // summary
    out << "  \"summary\": {\n";
    out << "    \"total_employees\": " << total_emps << ",\n";
    out << "    \"employees_routed\": " << routed << ",\n";
    out << "    \"employees_unrouted\": " << unrouted << ",\n";
    out << "    \"total_baseline_cost\": " << baseline_total << ",\n";
    out << "    \"total_optimized_cost\": " << optimized_total << ",\n";
    out << "    \"net_savings\": " << net_savings << ",\n";
    out << "    \"savings_percentage\": " << savings_pct << "\n";
    out << "  },\n";

    // unrouted details (from global map)
    out << "  \"unrouted_employees\": [\n";
    bool first_unr = true;
    for (const auto& e : emps) {
        if (e.is_routed) continue;
        auto it = g_unrouted_reason.find(e.id);
        std::string reason = (it == g_unrouted_reason.end()) ? "unrouted" : it->second;

        if (!first_unr) out << ",\n";
        first_unr = false;

        out << "    {"
            << "\"employee_id\": \"" << json_escape(e.id) << "\", "
            << "\"reason\": \"" << json_escape(reason) << "\""
            << "}";
    }
    out << "\n  ],\n";

    // vehicles
    out << "  \"vehicles\": [\n";
    for (size_t vi = 0; vi < vehs.size(); vi++) {
        const auto& v = vehs[vi];
        if (vi) out << ",\n";
        out << "    {\n";
        out << "      \"vehicle_id\": \"" << json_escape(v.id) << "\",\n";
        out << "      \"category\": \"" << (v.category == PREMIUM ? "premium" : "normal") << "\",\n";
        out << "      \"capacity\": " << v.capacity << ",\n";
        out << "      \"speed_kmh\": " << v.speed_kmh << ",\n";
        out << "      \"cost_per_km\": " << v.cost_per_km << ",\n";
        out << "      \"total_cost\": " << v.total_cost << ",\n";
        out << "      \"trips\": [\n";

        for (size_t ti = 0; ti < v.routes.size(); ti++) {
            const auto& r = v.routes[ti];
            if (ti) out << ",\n";

            int start_min = r.stops.empty() ? 0 : r.stops.front().departure_time;
            int end_min   = r.stops.empty() ? 0 : r.stops.back().arrival_time;

            out << "        {\n";
            out << "          \"trip_number\": " << (ti + 1) << ",\n";
            out << "          \"load\": " << r.current_capacity << ",\n";
            out << "          \"capacity_limit\": " << r.max_capacity << ",\n";
            out << "          \"start_time\": \"" << format_time(start_min) << "\",\n";
            out << "          \"end_time\": \"" << format_time(end_min) << "\",\n";
            out << "          \"trip_distance_km\": " << r.total_distance << ",\n";
            out << "          \"trip_cost\": " << r.total_cost << ",\n";

            // route node list
            out << "          \"route\": [";
            for (size_t si = 0; si < r.stops.size(); si++) {
                if (si) out << ", ";
                out << "\"" << json_escape(r.stops[si].emp_id) << "\"";
            }
            out << "],\n";

            // passengers
            auto p = passengers_in_route(r);
            out << "          \"passengers\": [";

            int drop_min = r.stops.empty() ? 0 : r.stops.back().arrival_time;
            for (size_t pi = 0; pi < p.size(); pi++) {
                const std::string& eid = p[pi];
                 const Stop* ps = find_pickup_stop(r, eid);

              if (pi) out << ",\n";
    out << "            {"
        << "\"employee_id\": \"" << json_escape(eid) << "\", "
        << "\"pickup_time\": \"" << (ps ? format_time(ps->departure_time) : std::string("00:00")) << "\", "
        << "\"drop_time\": \"" << format_time(drop_min) << "\""
        << "}";
}
            out << "]\n";

            out << "        }";
        }

        out << "\n      ]\n";
        out << "    }";
    }
    out << "\n  ]\n";

    out << "}\n";
    return true;
}
