#pragma once
#include <string>
#include <vector>
#include <map>
#include <cctype>
#include <stdexcept>

namespace mini_json {
    struct Value {
        enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
        bool b = false;
        double num = 0.0;
        std::string str;
        std::vector<Value> arr;
        std::map<std::string, Value> obj;

        bool is_null() const { return type == Type::Null; }
        bool is_bool() const { return type == Type::Bool; }
        bool is_number() const { return type == Type::Number; }
        bool is_string() const { return type == Type::String; }
        bool is_array() const { return type == Type::Array; }
        bool is_object() const { return type == Type::Object; }

        const Value& at(const std::string& k) const {
            static Value nullv;
            auto it = obj.find(k);
            return it == obj.end() ? nullv : it->second;
        }
        const Value& operator[](const std::string& k) const { return at(k); }

        std::string as_string(const std::string& def = "") const {
            return is_string() ? str : def;
        }
        double as_number(double def = 0.0) const {
            return is_number() ? num : def;
        }
        int as_int(int def = 0) const {
            return is_number() ? static_cast<int>(num) : def;
        }
        bool as_bool(bool def = false) const {
            return is_bool() ? b : def;
        }
    };

    struct Parser {
        const std::string& s;
        size_t i = 0;
        explicit Parser(const std::string& in) : s(in) {}

        void skip_ws() {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
        }

        [[noreturn]] void die(const std::string& msg) {
            throw std::runtime_error("JSON parse error at pos " + std::to_string(i) + ": " + msg);
        }

        bool consume(char c) {
            skip_ws();
            if (i < s.size() && s[i] == c) { i++; return true; }
            return false;
        }

        char peek() {
            skip_ws();
            return i < s.size() ? s[i] : '\0';
        }

        Value parse_value() {
            skip_ws();
            if (i >= s.size()) die("unexpected end");
            char c = s[i];
            if (c == 'n') return parse_null();
            if (c == 't' || c == 'f') return parse_bool();
            if (c == '"') return parse_string();
            if (c == '[') return parse_array();
            if (c == '{') return parse_object();
            if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number();
            die(std::string("unexpected character '") + c + "'");
        }

        Value parse_null() {
            if (s.compare(i, 4, "null") != 0) die("expected null");
            i += 4;
            return Value{};
        }

        Value parse_bool() {
            Value v;
            v.type = Value::Type::Bool;
            if (s.compare(i, 4, "true") == 0) { v.b = true; i += 4; return v; }
            if (s.compare(i, 5, "false") == 0) { v.b = false; i += 5; return v; }
            die("expected true/false");
        }

        Value parse_number() {
            skip_ws();
            size_t start = i;
            if (s[i] == '-') i++;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
            if (i < s.size() && s[i] == '.') {
                i++;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
            }
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                i++;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
            }
            double x = 0.0;
            try {
                x = std::stod(s.substr(start, i - start));
            } catch (...) {
                die("bad number");
            }
            Value v;
            v.type = Value::Type::Number;
            v.num = x;
            return v;
        }

        Value parse_string() {
            skip_ws();
            if (i >= s.size() || s[i] != '"') die("expected string");
            i++; // skip opening quote
            std::string out;
            while (i < s.size()) {
                char c = s[i++];
                if (c == '"') break;
                if (c == '\\') {
                    if (i >= s.size()) die("bad escape");
                    char e = s[i++];
                    switch (e) {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        // NOTE: \uXXXX not implemented (not needed for your testcases)
                        default: die("unsupported escape");
                    }
                } else {
                    out.push_back(c);
                }
            }
            Value v;
            v.type = Value::Type::String;
            v.str = std::move(out);
            return v;
        }

        Value parse_array() {
            Value v;
            v.type = Value::Type::Array;
            if (!consume('[')) die("expected [");
            skip_ws();
            if (consume(']')) return v;
            while (true) {
                v.arr.push_back(parse_value());
                skip_ws();
                if (consume(']')) break;
                if (!consume(',')) die("expected , in array");
            }
            return v;
        }

        Value parse_object() {
            Value v;
            v.type = Value::Type::Object;
            if (!consume('{')) die("expected {");
            skip_ws();
            if (consume('}')) return v;
            while (true) {
                Value key = parse_string();
                if (!consume(':')) die("expected :");
                Value val = parse_value();
                v.obj[key.str] = std::move(val);
                skip_ws();
                if (consume('}')) break;
                if (!consume(',')) die("expected , in object");
            }
            return v;
        }
    };

    inline Value parse(const std::string& text) {
        Parser p(text);
        Value v = p.parse_value();
        p.skip_ws();
        if (p.i != text.size()) throw std::runtime_error("JSON parse error: trailing characters");
        return v;
    }
}

using namespace std;
