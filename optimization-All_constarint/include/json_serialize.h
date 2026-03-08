#pragma once
#include <ostream>
#include "mini_json.h"

void write_json(std::ostream& os, const mini_json::Value& v, int indent = 0);
