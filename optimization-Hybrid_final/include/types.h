#pragma once
#include <string>
#include <vector>
#include <map>

using std::string;
using std::vector;
using std::map;

enum VehicleCat { PREMIUM, NORMAL, ANY_CAT };
enum SharingPref { SINGLE, DOUBLE, TRIPLE, ANY_SHARE };

struct Location {
    double lat, lng;
};



struct Employee {
    string id;
    int priority;
    Location pickup;
    Location drop;
    int ready_time;
    int due_time;
    VehicleCat veh_pref;
    SharingPref share_pref;
    bool is_routed;
    double baseline_cost;
};

struct Stop {
    string emp_id;
    Location loc;
    int arrival_time;
    int begin_service;
    int departure_time;
    bool is_pickup;
};

struct Route {
    vector<Stop> stops;
    int current_capacity;
    int max_capacity;
    double total_distance;
    double total_cost;
};

struct Vehicle {
    string id;
    double capacity;
    double cost_per_km;
    double speed_kmh;
    Location depot_loc;
    VehicleCat category;
    int available_time = 0; // minutes since midnight
    // Multi-trip state: chain both time AND physical location across trips.
    // Without this, the vehicle "teleports" back to the depot between trips.
    Location current_loc;
    
    vector<Route> routes;  // Multiple trips
    double total_cost;
};

