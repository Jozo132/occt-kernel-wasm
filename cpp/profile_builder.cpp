/**
 * @file profile_builder.cpp
 * @brief JSON profile → OCCT wire / face conversion.
 *
 * The JSON format mirrors the TypeScript `Profile` type defined in src/types.ts.
 *
 * This file uses a hand-written minimal JSON parser to avoid external dependencies
 * in the WASM build.  Only the subset of JSON needed by the profile format is
 * handled.  For production use a proper JSON library (e.g. nlohmann/json) can
 * replace this parser without affecting the public interface.
 */

#include "profile_builder.h"

// OCCT geometry
#include <gp_Pnt2d.hxx>
#include <gp_Pnt.hxx>
#include <gp_Circ.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>

// OCCT topology builders
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GCE2d_MakeArcOfCircle.hxx>
#include <Geom_TrimmedCurve.hxx>

#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

namespace occt_kernel {

// ---------------------------------------------------------------------------
// Minimal JSON value type
// ---------------------------------------------------------------------------

struct JsonValue;
using JsonArray  = std::vector<JsonValue>;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool        b  = false;
    double      n  = 0.0;
    std::string s;
    JsonArray   arr;
    JsonObject  obj;

    static JsonValue makeNull()   { JsonValue v; v.kind = Kind::Null;   return v; }
    static JsonValue makeBool(bool x) { JsonValue v; v.kind = Kind::Bool; v.b = x; return v; }
    static JsonValue makeNum(double x) { JsonValue v; v.kind = Kind::Number; v.n = x; return v; }
    static JsonValue makeStr(const std::string& x) { JsonValue v; v.kind = Kind::String; v.s = x; return v; }
    static JsonValue makeArr() { JsonValue v; v.kind = Kind::Array; return v; }
    static JsonValue makeObj() { JsonValue v; v.kind = Kind::Object; return v; }

    const JsonValue* get(const std::string& key) const {
        for (auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    double getNum(const std::string& key) const {
        const JsonValue* v = get(key);
        if (!v || v->kind != Kind::Number) {
            throw std::runtime_error("Missing or non-numeric key: " + key);
        }
        return v->n;
    }

    std::pair<double,double> getXY(const std::string& key) const {
        const JsonValue* v = get(key);
        if (!v || v->kind != Kind::Array || v->arr.size() < 2) {
            throw std::runtime_error("Expected 2-element array for key: " + key);
        }
        return { v->arr[0].n, v->arr[1].n };
    }

    std::string getStr(const std::string& key) const {
        const JsonValue* v = get(key);
        if (!v || v->kind != Kind::String) {
            throw std::runtime_error("Missing or non-string key: " + key);
        }
        return v->s;
    }
};

// ---------------------------------------------------------------------------
// Minimal JSON parser
// ---------------------------------------------------------------------------

struct Parser {
    const char* p;
    const char* end;

    explicit Parser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    char peek() { skipWs(); return p < end ? *p : '\0'; }
    char consume() { return p < end ? *p++ : '\0'; }

    JsonValue parse() {
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') { p += 4; return JsonValue::makeNull(); }
        return parseNumber();
    }

    JsonValue parseObject() {
        consume(); // '{'
        JsonValue v = JsonValue::makeObj();
        skipWs();
        if (peek() == '}') { consume(); return v; }
        while (true) {
            skipWs();
            if (peek() != '"') throw std::runtime_error("Expected string key in JSON object");
            std::string key = parseString().s;
            skipWs();
            if (consume() != ':') throw std::runtime_error("Expected ':' in JSON object");
            JsonValue val = parse();
            v.obj.push_back({ key, val });
            skipWs();
            char next = consume();
            if (next == '}') break;
            if (next != ',') throw std::runtime_error("Expected ',' or '}' in JSON object");
        }
        return v;
    }

    JsonValue parseArray() {
        consume(); // '['
        JsonValue v = JsonValue::makeArr();
        skipWs();
        if (peek() == ']') { consume(); return v; }
        while (true) {
            v.arr.push_back(parse());
            skipWs();
            char next = consume();
            if (next == ']') break;
            if (next != ',') throw std::runtime_error("Expected ',' or ']' in JSON array");
        }
        return v;
    }

    JsonValue parseString() {
        consume(); // '"'
        std::string s;
        while (p < end && *p != '"') {
            if (*p == '\\') { ++p; if (p < end) s += *p++; }
            else s += *p++;
        }
        consume(); // closing '"'
        return JsonValue::makeStr(s);
    }

    JsonValue parseBool() {
        bool val = (*p == 't');
        p += val ? 4 : 5;
        return JsonValue::makeBool(val);
    }

    JsonValue parseNumber() {
        char* end_ptr = nullptr;
        double val = std::strtod(p, &end_ptr);
        if (!end_ptr || end_ptr == p) throw std::runtime_error("Invalid JSON number");
        p = end_ptr;
        return JsonValue::makeNum(val);
    }
};

// ---------------------------------------------------------------------------
// Profile → wire
// ---------------------------------------------------------------------------

TopoDS_Wire buildWireFromProfile(const std::string& profileJson) {
    Parser parser(profileJson);
    JsonValue root = parser.parse();

    const JsonValue* segsVal = root.get("segments");
    if (!segsVal || segsVal->kind != JsonValue::Kind::Array) {
        throw std::runtime_error("Profile JSON must contain a 'segments' array");
    }

    BRepBuilderAPI_MakeWire mkWire;

    for (const JsonValue& seg : segsVal->arr) {
        std::string type = seg.getStr("type");

        if (type == "line") {
            auto [sx, sy] = seg.getXY("start");
            auto [ex, ey] = seg.getXY("end");
            gp_Pnt p1(sx, sy, 0), p2(ex, ey, 0);
            BRepBuilderAPI_MakeEdge mkEdge(p1, p2);
            if (!mkEdge.IsDone()) throw std::runtime_error("Failed to build line edge");
            mkWire.Add(mkEdge.Edge());
        } else if (type == "arc") {
            auto [sx, sy]  = seg.getXY("start");
            auto [mx, my]  = seg.getXY("mid");
            auto [ex, ey]  = seg.getXY("end");
            gp_Pnt p1(sx, sy, 0), pm(mx, my, 0), p2(ex, ey, 0);
            Handle(Geom_TrimmedCurve) arc = GC_MakeArcOfCircle(p1, pm, p2);
            BRepBuilderAPI_MakeEdge mkEdge(arc);
            if (!mkEdge.IsDone()) throw std::runtime_error("Failed to build arc edge");
            mkWire.Add(mkEdge.Edge());
        } else if (type == "circle") {
            auto [cx, cy] = seg.getXY("centre");
            double r = seg.getNum("radius");
            gp_Ax2 ax(gp_Pnt(cx, cy, 0), gp_Dir(0, 0, 1));
            gp_Circ circ(ax, r);
            BRepBuilderAPI_MakeEdge mkEdge(circ);
            if (!mkEdge.IsDone()) throw std::runtime_error("Failed to build circle edge");
            mkWire.Add(mkEdge.Edge());
        } else {
            throw std::runtime_error("Unknown profile segment type: " + type);
        }
    }

    if (!mkWire.IsDone()) {
        throw std::runtime_error("Failed to build wire from profile (wire not closed or edges not connected)");
    }
    return mkWire.Wire();
}

TopoDS_Face buildFaceFromProfile(const std::string& profileJson) {
    TopoDS_Wire wire = buildWireFromProfile(profileJson);
    BRepBuilderAPI_MakeFace mkFace(wire, Standard_True);
    if (!mkFace.IsDone()) {
        throw std::runtime_error("Failed to build planar face from profile wire");
    }
    return mkFace.Face();
}

} // namespace occt_kernel
