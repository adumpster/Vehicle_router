#include "file_utils.h"
#include <fstream>
#include <sstream>
#include <stdexcept>

std::string read_file_to_string(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
