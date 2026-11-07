#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <sstream>
#include <cstdint>
#include <stdexcept>

namespace json {

enum class Type {
    NULL_VAL,
    BOOLEAN,
    NUMBER,
    STRING,
    ARRAY,
    OBJECT
};

class Value {
public:
    Type type = Type::NULL_VAL;
    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<Value> arrVal;
    std::unordered_map<std::string, Value> objVal;

    Value() : type(Type::NULL_VAL) {}
    Value(bool b) : type(Type::BOOLEAN), boolVal(b) {}
    Value(double d) : type(Type::NUMBER), numVal(d) {}
    Value(int i) : type(Type::NUMBER), numVal(static_cast<double>(i)) {}
    Value(int64_t i) : type(Type::NUMBER), numVal(static_cast<double>(i)) {}
    Value(uint64_t i) : type(Type::NUMBER), numVal(static_cast<double>(i)) {}
    Value(const char* s) : type(Type::STRING), strVal(s) {}
    Value(std::string s) : type(Type::STRING), strVal(std::move(s)) {}
    Value(Type t) : type(t) {}

    bool isNull() const { return type == Type::NULL_VAL; }
    bool isBool() const { return type == Type::BOOLEAN; }
    bool isNumber() const { return type == Type::NUMBER; }
    bool isString() const { return type == Type::STRING; }
    bool isArray() const { return type == Type::ARRAY; }
    bool isObject() const { return type == Type::OBJECT; }

    bool asBool(bool def = false) const { return isBool() ? boolVal : def; }
    double asDouble(double def = 0.0) const { return isNumber() ? numVal : def; }
    int64_t asInt(int64_t def = 0) const { return isNumber() ? static_cast<int64_t>(numVal) : def; }
    const std::string& asString(const std::string& def = "") const { return isString() ? strVal : def; }

    bool contains(const std::string& key) const {
        if (!isObject()) return false;
        return objVal.find(key) != objVal.end();
    }

    const Value& operator[](const std::string& key) const {
        static const Value nullVal;
        if (!isObject()) return nullVal;
        auto it = objVal.find(key);
        if (it != objVal.end()) return it->second;
        return nullVal;
    }

    Value& operator[](const std::string& key) {
        if (!isObject()) {
            type = Type::OBJECT;
            objVal.clear();
        }
        return objVal[key];
    }

    const Value& operator[](size_t index) const {
        static const Value nullVal;
        if (!isArray() || index >= arrVal.size()) return nullVal;
        return arrVal[index];
    }

    void push_back(Value v) {
        if (!isArray()) {
            type = Type::ARRAY;
            arrVal.clear();
        }
        arrVal.push_back(std::move(v));
    }

    size_t size() const {
        if (isArray()) return arrVal.size();
        if (isObject()) return objVal.size();
        return 0;
    }

    std::string serialize() const;
};

// Parser
bool parse(const std::string& input, Value& outValue, std::string& outError);

} // namespace json
