#pragma once
#include "types.h"
#include <string>
#include <unordered_map>
#include <vector>

double get_dist(Location a, Location b);
double get_dist(const Stop &a, const Stop &b);
int travel_minutes(double dist_km, double speed_kmh);
double recompute_distance_km(const std::vector<Stop> &stops);
void geo_set_distance_overrides_m(
    const std::unordered_map<std::string, std::unordered_map<std::string, double>> &overrides_m);