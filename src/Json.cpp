#include "Json.h"
#include <cctype>
#include <cstdlib>
#include <iomanip>

namespace json {

namespace {

class Parser {
    const std::string& src;
    size_t pos = 0;

    void skipWhitespace() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r' || src[pos] == '\n')) {
            pos++;
        }
    }

    char peek() {
        skipWhitespace();
        if (pos >= src.size()) return '\0';
        return src[pos];
    }

    char get() {
        skipWhitespace();
        if (pos >= src.size()) return '\0';
        return src[pos++];
    }

public:
    Parser(const std::string& input) : src(input) {}

    bool parseValue(Value& val, std::string& err) {
        skipWhitespace();
        char c = peek();
        if (c == '\0') {
            err = "Unexpected end of input";
            return false;
        }

        if (c == '{') return parseObject(val, err);
        if (c == '[') return parseArray(val, err);
        if (c == '"') return parseString(val, err);
        if (c == 't' || c == 'f') return parseBool(val, err);
        if (c == 'n') return parseNull(val, err);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parseNumber(val, err);

        err = std::string("Unexpected token '") + c + "'";
        return false;
    }

    bool parseObject(Value& val, std::string& err) {
        if (get() != '{') { err = "Expected '{'"; return false; }
        val.type = Type::OBJECT;
        val.objVal.clear();

        skipWhitespace();
        if (peek() == '}') {
            get();
            return true;
        }

        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                err = "Expected string key in object";
                return false;
            }

            Value keyVal;
            if (!parseString(keyVal, err)) return false;

            skipWhitespace();
            if (get() != ':') {
                err = "Expected ':' after key";
                return false;
            }

            Value fieldVal;
            if (!parseValue(fieldVal, err)) return false;

            val.objVal[keyVal.strVal] = std::move(fieldVal);

            skipWhitespace();
            char next = get();
            if (next == '}') return true;
            if (next != ',') {
                err = "Expected ',' or '}' in object";
                return false;
            }
        }
    }

    bool parseArray(Value& val, std::string& err) {
        if (get() != '[') { err = "Expected '['"; return false; }
        val.type = Type::ARRAY;
        val.arrVal.clear();

        skipWhitespace();
        if (peek() == ']') {
            get();
            return true;
        }

        while (true) {
            Value itemVal;
            if (!parseValue(itemVal, err)) return false;
            val.arrVal.push_back(std::move(itemVal));

            skipWhitespace();
            char next = get();
            if (next == ']') return true;
            if (next != ',') {
                err = "Expected ',' or ']' in array";
                return false;
            }
        }
    }

    bool parseString(Value& val, std::string& err) {
        if (get() != '"') { err = "Expected '\"'"; return false; }
        val.type = Type::STRING;
        val.strVal.clear();

        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos >= src.size()) { err = "Incomplete escape sequence"; return false; }
                char esc = src[pos++];
                switch (esc) {
                    case '"': val.strVal.push_back('"'); break;
                    case '\\': val.strVal.push_back('\\'); break;
                    case '/': val.strVal.push_back('/'); break;
                    case 'b': val.strVal.push_back('\b'); break;
                    case 'f': val.strVal.push_back('\f'); break;
                    case 'n': val.strVal.push_back('\n'); break;
                    case 'r': val.strVal.push_back('\r'); break;
                    case 't': val.strVal.push_back('\t'); break;
                    case 'u': {
                        if (pos + 4 > src.size()) { err = "Incomplete unicode escape"; return false; }
                        pos += 4; // Skip 4 hex digits for basic JSON parser
                        val.strVal.push_back('?');
                        break;
                    }
                    default: val.strVal.push_back(esc); break;
                }
            } else {
                val.strVal.push_back(c);
            }
        }
        err = "Unterminated string";
        return false;
    }

    bool parseNumber(Value& val, std::string& err) {
        skipWhitespace();
        size_t start = pos;
        if (pos < src.size() && src[pos] == '-') pos++;
        if (pos >= src.size() || !std::isdigit(static_cast<unsigned char>(src[pos]))) {
            err = "Invalid number format";
            return false;
        }

        while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos]))) pos++;

        if (pos < src.size() && src[pos] == '.') {
            pos++;
            if (pos >= src.size() || !std::isdigit(static_cast<unsigned char>(src[pos]))) {
                err = "Invalid fraction in number";
                return false;
            }
            while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos]))) pos++;
        }

        if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
            pos++;
            if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) pos++;
            while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos]))) pos++;
        }

        std::string numStr = src.substr(start, pos - start);
        char* endPtr = nullptr;
        double d = std::strtod(numStr.c_str(), &endPtr);
        if (endPtr != numStr.c_str() + numStr.size()) {
            err = "Failed to convert number";
            return false;
        }

        val.type = Type::NUMBER;
        val.numVal = d;
        return true;
    }

    bool parseBool(Value& val, std::string& err) {
        if (src.compare(pos, 4, "true") == 0) {
            pos += 4;
            val.type = Type::BOOLEAN;
            val.boolVal = true;
            return true;
        }
        if (src.compare(pos, 5, "false") == 0) {
            pos += 5;
            val.type = Type::BOOLEAN;
            val.boolVal = false;
            return true;
        }
        err = "Expected 'true' or 'false'";
        return false;
    }

    bool parseNull(Value& val, std::string& err) {
        if (src.compare(pos, 4, "null") == 0) {
            pos += 4;
            val.type = Type::NULL_VAL;
            return true;
        }
        err = "Expected 'null'";
        return false;
    }
};

void serializeString(const std::string& s, std::ostringstream& oss) {
    oss << '"';
    for (char c : s) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default: oss << c; break;
        }
    }
    oss << '"';
}

void serializeValue(const Value& val, std::ostringstream& oss) {
    switch (val.type) {
        case Type::NULL_VAL: oss << "null"; break;
        case Type::BOOLEAN: oss << (val.boolVal ? "true" : "false"); break;
        case Type::NUMBER: {
            // Check if whole integer
            if (val.numVal == static_cast<int64_t>(val.numVal)) {
                oss << static_cast<int64_t>(val.numVal);
            } else {
                oss << std::setprecision(6) << val.numVal;
            }
            break;
        }
        case Type::STRING: serializeString(val.strVal, oss); break;
        case Type::ARRAY: {
            oss << '[';
            for (size_t i = 0; i < val.arrVal.size(); i++) {
                if (i > 0) oss << ',';
                serializeValue(val.arrVal[i], oss);
            }
            oss << ']';
            break;
        }
        case Type::OBJECT: {
            oss << '{';
            bool first = true;
            for (const auto& [k, v] : val.objVal) {
                if (!first) oss << ',';
                first = false;
                serializeString(k, oss);
                oss << ':';
                serializeValue(v, oss);
            }
            oss << '}';
            break;
        }
    }
}

} // anonymous namespace

std::string Value::serialize() const {
    std::ostringstream oss;
    serializeValue(*this, oss);
    return oss.str();
}

bool parse(const std::string& input, Value& outValue, std::string& outError) {
    Parser parser(input);
    return parser.parseValue(outValue, outError);
}

} // namespace json
