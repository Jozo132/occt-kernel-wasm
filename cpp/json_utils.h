#pragma once

#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace occt_kernel::mini_json {

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;

    bool boolean = false;
    double number = 0.0;
    std::string string;
    Array array;
    Object object;

    static Value makeNull()
    {
        Value value;
        value.kind = Kind::Null;
        return value;
    }

    static Value makeBool(bool input)
    {
        Value value;
        value.kind = Kind::Bool;
        value.boolean = input;
        return value;
    }

    static Value makeNumber(double input)
    {
        Value value;
        value.kind = Kind::Number;
        value.number = input;
        return value;
    }

    static Value makeString(const std::string& input)
    {
        Value value;
        value.kind = Kind::String;
        value.string = input;
        return value;
    }

    static Value makeArray()
    {
        Value value;
        value.kind = Kind::Array;
        return value;
    }

    static Value makeObject()
    {
        Value value;
        value.kind = Kind::Object;
        return value;
    }

    const Value* get(const std::string& key) const
    {
        for (const auto& entry : object) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(const std::string& source)
        : current_(source.data()), end_(source.data() + source.size())
    {
    }

    Value parse()
    {
        const char token = peek();
        if (token == '{') {
            return parseObject();
        }
        if (token == '[') {
            return parseArray();
        }
        if (token == '"') {
            return parseString();
        }
        if (token == 't' || token == 'f') {
            return parseBool();
        }
        if (token == 'n') {
            parseNull();
            return Value::makeNull();
        }
        if (token == '\0') {
            throw std::runtime_error("Unexpected end of JSON input");
        }
        return parseNumber();
    }

    bool atEnd()
    {
        skipWhitespace();
        return current_ >= end_;
    }

private:
    const char* current_;
    const char* end_;

    void skipWhitespace()
    {
        while (current_ < end_ && (*current_ == ' ' || *current_ == '\t' || *current_ == '\n' || *current_ == '\r')) {
            ++current_;
        }
    }

    char peek()
    {
        skipWhitespace();
        return current_ < end_ ? *current_ : '\0';
    }

    char consume()
    {
        return current_ < end_ ? *current_++ : '\0';
    }

    void expectLiteral(const char* literal)
    {
        for (const char* cursor = literal; *cursor != '\0'; ++cursor) {
            if (consume() != *cursor) {
                throw std::runtime_error("Invalid JSON literal");
            }
        }
    }

    void parseNull()
    {
        expectLiteral("null");
    }

    Value parseObject()
    {
        consume();
        Value value = Value::makeObject();
        skipWhitespace();
        if (peek() == '}') {
            consume();
            return value;
        }

        while (true) {
            if (peek() != '"') {
                throw std::runtime_error("Expected string key in JSON object");
            }
            const std::string key = parseString().string;
            skipWhitespace();
            if (consume() != ':') {
                throw std::runtime_error("Expected ':' in JSON object");
            }
            value.object.push_back({ key, parse() });
            skipWhitespace();

            const char next = consume();
            if (next == '}') {
                break;
            }
            if (next != ',') {
                throw std::runtime_error("Expected ',' or '}' in JSON object");
            }
        }

        return value;
    }

    Value parseArray()
    {
        consume();
        Value value = Value::makeArray();
        skipWhitespace();
        if (peek() == ']') {
            consume();
            return value;
        }

        while (true) {
            value.array.push_back(parse());
            skipWhitespace();

            const char next = consume();
            if (next == ']') {
                break;
            }
            if (next != ',') {
                throw std::runtime_error("Expected ',' or ']' in JSON array");
            }
        }

        return value;
    }

    Value parseString()
    {
        consume();
        std::string output;
        while (current_ < end_ && *current_ != '"') {
            if (*current_ == '\\') {
                ++current_;
                if (current_ >= end_) {
                    break;
                }
                const char escaped = *current_++;
                switch (escaped) {
                case '"': output += '"'; break;
                case '\\': output += '\\'; break;
                case '/': output += '/'; break;
                case 'b': output += '\b'; break;
                case 'f': output += '\f'; break;
                case 'n': output += '\n'; break;
                case 'r': output += '\r'; break;
                case 't': output += '\t'; break;
                default: output += escaped; break;
                }
                continue;
            }
            output += *current_++;
        }
        if (consume() != '"') {
            throw std::runtime_error("Unterminated JSON string");
        }
        return Value::makeString(output);
    }

    Value parseBool()
    {
        const bool value = peek() == 't';
        expectLiteral(value ? "true" : "false");
        return Value::makeBool(value);
    }

    Value parseNumber()
    {
        char* parseEnd = nullptr;
        const double value = std::strtod(current_, &parseEnd);
        if (parseEnd == nullptr || parseEnd == current_) {
            throw std::runtime_error("Invalid JSON number");
        }
        current_ = parseEnd;
        return Value::makeNumber(value);
    }
};

inline Value parse(const std::string& source)
{
    Parser parser(source);
    Value value = parser.parse();
    if (!parser.atEnd()) {
        throw std::runtime_error("Unexpected trailing JSON content");
    }
    return value;
}

inline const Value& requireMember(const Value& object, const std::string& key, const std::string& context)
{
    if (object.kind != Value::Kind::Object) {
        throw std::runtime_error("Expected object for " + context);
    }

    const Value* member = object.get(key);
    if (member == nullptr) {
        throw std::runtime_error("Missing key '" + key + "' in " + context);
    }
    return *member;
}

inline const Value& requireObject(const Value& value, const std::string& context)
{
    if (value.kind != Value::Kind::Object) {
        throw std::runtime_error("Expected object for " + context);
    }
    return value;
}

inline const Value& requireArray(const Value& value, const std::string& context)
{
    if (value.kind != Value::Kind::Array) {
        throw std::runtime_error("Expected array for " + context);
    }
    return value;
}

inline double requireNumber(const Value& value, const std::string& context)
{
    if (value.kind != Value::Kind::Number) {
        throw std::runtime_error("Expected number for " + context);
    }
    return value.number;
}

inline std::string requireString(const Value& value, const std::string& context)
{
    if (value.kind != Value::Kind::String) {
        throw std::runtime_error("Expected string for " + context);
    }
    return value.string;
}

inline bool requireBool(const Value& value, const std::string& context)
{
    if (value.kind != Value::Kind::Bool) {
        throw std::runtime_error("Expected boolean for " + context);
    }
    return value.boolean;
}

inline std::array<double, 2> requirePoint2(const Value& value, const std::string& context)
{
    const Value& array = requireArray(value, context);
    if (array.array.size() != 2) {
        throw std::runtime_error("Expected 2-element array for " + context);
    }
    return {
        requireNumber(array.array[0], context + "[0]"),
        requireNumber(array.array[1], context + "[1]"),
    };
}

inline std::array<double, 3> requirePoint3(const Value& value, const std::string& context)
{
    const Value& array = requireArray(value, context);
    if (array.array.size() != 3) {
        throw std::runtime_error("Expected 3-element array for " + context);
    }
    return {
        requireNumber(array.array[0], context + "[0]"),
        requireNumber(array.array[1], context + "[1]"),
        requireNumber(array.array[2], context + "[2]"),
    };
}

} // namespace occt_kernel::mini_json