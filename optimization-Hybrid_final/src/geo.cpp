#include "geo.h"
#include "config.h"
#include <cmath>
#include <string>
#include <unordered_map>

static std::unordered_map<std::string, std::unordered_map<std::string, double>> g_overrides_m;

void geo_set_distance_overrides_m(
    const std::unordered_map<std::string, std::unordered_map<std::string, double>> &overrides_m)
{
    g_overrides_m = overrides_m;
}

static inline bool is_drop_like(const std::string &id)
{
    return id == "drop" || id == "DROP" || id == "END" || id == "OFFICE" || id == "Office";
}

static inline std::string norm_id(const std::string &id)
{
    return is_drop_like(id) ? "drop" : id;
}

static double lookup_override_km(const std::string &from_id, const std::string &to_id)
{
    if (g_overrides_m.empty())
        return -1.0;
    if (from_id.empty() || to_id.empty())
        return -1.0;

    const std::string from = norm_id(from_id);
    const std::string to = norm_id(to_id);

    auto it_from = g_overrides_m.find(from);
    if (it_from != g_overrides_m.end())
    {
        auto it_to = it_from->second.find(to);
        if (it_to != it_from->second.end() && it_to->second >= 0.0)
        {
            return it_to->second / 1000.0;
        }
    }
    auto it_rev_from = g_overrides_m.find(to);
    if (it_rev_from != g_overrides_m.end())
    {
        auto it_rev_to = it_rev_from->second.find(from);
        if (it_rev_to != it_rev_from->second.end() && it_rev_to->second >= 0.0)
        {
            return it_rev_to->second / 1000.0; // meters -> km
        }
    }

    return -1.0;
}
static double haversine_km(Location a, Location b)
{
    const double R = 6371.0;
    const double dLat = (b.lat - a.lat) * PI / 180.0;
    const double dLon = (b.lng - a.lng) * PI / 180.0;
    const double lat1 = a.lat * PI / 180.0;
    const double lat2 = b.lat * PI / 180.0;

    const double a_val =
        std::pow(std::sin(dLat / 2.0), 2) +
        std::pow(std::sin(dLon / 2.0), 2) * std::cos(lat1) * std::cos(lat2);

    const double c = 2.0 * std::asin(std::sqrt(a_val));
    return R * c;
}
double get_dist(Location a, Location b)
{
    return haversine_km(a, b);
}
double get_dist(const Stop &a, const Stop &b)
{
    const double override_km = lookup_override_km(a.emp_id, b.emp_id);
    if (override_km >= 0.0)
        return override_km;
    return haversine_km(a.loc, b.loc);
}
int travel_minutes(double dist_km, double speed_kmh)
{
    if (speed_kmh <= 0.0)
        return (int)1e9;
    return (int)llround((dist_km / speed_kmh) * 60.0);
}
double recompute_distance_km(const std::vector<Stop> &stops)
{
    double dist = 0.0;
    for (size_t i = 1; i < stops.size(); i++)
    {
        dist += get_dist(stops[i - 1], stops[i]);
    }
    return dist;
}