#include "json_serialize.h"
#include <iomanip>

static void indent(std::ostream& os, int n) {
    for (int i = 0; i < n; i++) os << ' ';
}

static std::string escape_json_str(const std::string& s) {
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
            default: out += c; break;
        }
    }
    return out;
}

void write_json(std::ostream& os, const mini_json::Value& v, int ind) {
    using T = mini_json::Value::Type;

    switch (v.type) {
        case T::Null:   os << "null"; return;
        case T::Bool:   os << (v.b ? "true" : "false"); return;
        case T::Number: os << std::setprecision(15) << v.num; return;
        case T::String: os << "\"" << escape_json_str(v.str) << "\""; return;

        case T::Array: {
            os << "[";
            if (!v.arr.empty()) os << "\n";
            for (size_t i = 0; i < v.arr.size(); i++) {
                indent(os, ind + 2);
                write_json(os, v.arr[i], ind + 2);
                if (i + 1 < v.arr.size()) os << ",";
                os << "\n";
            }
            if (!v.arr.empty()) indent(os, ind);
            os << "]";
            return;
        }

        case T::Object: {
            os << "{";
            if (!v.obj.empty()) os << "\n";
            size_t i = 0;
            for (const auto& kv : v.obj) {
                indent(os, ind + 2);
                os << "\"" << escape_json_str(kv.first) << "\": ";
                write_json(os, kv.second, ind + 2);
                if (i + 1 < v.obj.size()) os << ",";
                os << "\n";
                i++;
            }
            if (!v.obj.empty()) indent(os, ind);
            os << "}";
            return;
        }
    }
}
