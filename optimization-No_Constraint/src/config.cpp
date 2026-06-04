#include "config.h"

double ALPHA1 = 1.0;
double ALPHA2 = 0.0;
double LAMBDA = 1.0;
double MU = 1.0;

double W_COST = 0.6; // default: pure distance cost (no time component)
double W_TIME = 0.4;

const double INF = 1e9;
const double PI = 3.14159265358979323846;
const int SERVICE_MIN = 0;
Location OFFICE = {0.0, 0.0};

std::map<std::string, std::string> g_unrouted_reason;
