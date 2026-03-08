#include "time_utils.h"
#include <sstream>
#include <iomanip>

using namespace std;

int parse_time(std::string t_str) {
    if (t_str.empty() || t_str.find(':') == std::string::npos) return 0;
    int h = std::stoi(t_str.substr(0, 2));
    int m = std::stoi(t_str.substr(3, 2));
    return h * 60 + m;
}

std::string format_time(int minutes) {
    int h = (minutes / 60) % 24;
    int m = minutes % 60;
    stringstream ss;
    ss << setfill('0') << setw(2) << h << ":" << setw(2) << m;
    return ss.str();
}
