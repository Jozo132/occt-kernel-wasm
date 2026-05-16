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
#include "json_utils.h"

// OCCT geometry
#include <gp_Pnt2d.hxx>
#include <gp_Pnt.hxx>
#include <gp_Circ.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <Geom_BezierCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Curve.hxx>

// OCCT topology builders
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GCE2d_MakeArcOfCircle.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

namespace occt_kernel {

// ---------------------------------------------------------------------------
// Profile → wire
// ---------------------------------------------------------------------------

namespace {

using mini_json::Value;

struct BuiltWire {
    TopoDS_Wire wire;
    double signedArea = 0.0;
    bool hasSignedArea = false;
    bool isCircleWire = false;
};

double computeSignedArea(const std::vector<std::pair<double, double>>& points)
{
    if (points.size() < 3) {
        return 0.0;
    }

    double twiceArea = 0.0;
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto& current = points[index];
        const auto& next = points[(index + 1) % points.size()];
        twiceArea += (current.first * next.second) - (next.first * current.second);
    }
    return 0.5 * twiceArea;
}

void appendPoint(std::vector<std::pair<double, double>>& points, double x, double y)
{
    if (!points.empty() && points.back().first == x && points.back().second == y) {
        return;
    }
    points.push_back({ x, y });
}

int requireIntegerNumber(const Value& value, const std::string& context, int minimum = std::numeric_limits<int>::min())
{
    const double number = mini_json::requireNumber(value, context);
    if (!std::isfinite(number) || std::floor(number) != number) {
        throw std::runtime_error("Expected integer for " + context);
    }

    const int result = static_cast<int>(number);
    if (result < minimum) {
        throw std::runtime_error(context + " must be >= " + std::to_string(minimum));
    }
    return result;
}

TColgp_Array1OfPnt buildControlPointArray(const Value& controlPointsValue, const std::string& context, std::size_t minimumCount)
{
    const Value& controlPoints = mini_json::requireArray(controlPointsValue, context);
    if (controlPoints.array.size() < minimumCount) {
        throw std::runtime_error(context + " must contain at least " + std::to_string(minimumCount) + " points");
    }

    TColgp_Array1OfPnt poles(1, static_cast<int>(controlPoints.array.size()));
    for (std::size_t index = 0; index < controlPoints.array.size(); ++index) {
        const auto point = mini_json::requirePoint2(controlPoints.array[index], context + "[" + std::to_string(index) + "]");
        poles(static_cast<int>(index) + 1) = gp_Pnt(point[0], point[1], 0.0);
    }
    return poles;
}

std::vector<double> buildNumberVector(const Value& valuesValue, const std::string& context, std::size_t minimumCount)
{
    const Value& values = mini_json::requireArray(valuesValue, context);
    if (values.array.size() < minimumCount) {
        throw std::runtime_error(context + " must contain at least " + std::to_string(minimumCount) + " values");
    }

    std::vector<double> result;
    result.reserve(values.array.size());
    for (std::size_t index = 0; index < values.array.size(); ++index) {
        const double value = mini_json::requireNumber(values.array[index], context + "[" + std::to_string(index) + "]");
        if (!std::isfinite(value)) {
            throw std::runtime_error(context + " must contain only finite values");
        }
        result.push_back(value);
    }
    return result;
}

std::vector<int> buildIntegerVector(const Value& valuesValue, const std::string& context, std::size_t minimumCount, int minimumValue)
{
    const Value& values = mini_json::requireArray(valuesValue, context);
    if (values.array.size() < minimumCount) {
        throw std::runtime_error(context + " must contain at least " + std::to_string(minimumCount) + " values");
    }

    std::vector<int> result;
    result.reserve(values.array.size());
    for (std::size_t index = 0; index < values.array.size(); ++index) {
        result.push_back(requireIntegerNumber(values.array[index], context + "[" + std::to_string(index) + "]", minimumValue));
    }
    return result;
}

void requireStrictlyIncreasing(const std::vector<double>& values, const std::string& context)
{
    for (std::size_t index = 1; index < values.size(); ++index) {
        if (values[index] <= values[index - 1]) {
            throw std::runtime_error(context + " must be strictly increasing");
        }
    }
}

void appendCurveSampledPoints(const Handle(Geom_Curve)& curve, std::vector<std::pair<double, double>>& points, int sampleCount = 16)
{
    if (curve.IsNull()) {
        throw std::runtime_error("Curve handle is null");
    }

    const double first = curve->FirstParameter();
    const double last = curve->LastParameter();
    const int steps = sampleCount < 2 ? 2 : sampleCount;
    for (int step = 0; step <= steps; ++step) {
        const double u = first + (last - first) * (static_cast<double>(step) / static_cast<double>(steps));
        const gp_Pnt point = curve->Value(u);
        appendPoint(points, point.X(), point.Y());
    }
}

Handle(Geom_BezierCurve) buildBezierCurve(const Value& segment, const std::string& context)
{
    const TColgp_Array1OfPnt poles = buildControlPointArray(
        mini_json::requireMember(segment, "controlPoints", context),
        context + ".controlPoints",
        2);
    return new Geom_BezierCurve(poles);
}

Handle(Geom_BSplineCurve) buildBSplineCurve(const Value& segment, const std::string& context)
{
    const TColgp_Array1OfPnt poles = buildControlPointArray(
        mini_json::requireMember(segment, "controlPoints", context),
        context + ".controlPoints",
        2);
    const int degree = requireIntegerNumber(mini_json::requireMember(segment, "degree", context), context + ".degree", 1);
    const std::vector<double> knots = buildNumberVector(
        mini_json::requireMember(segment, "knots", context),
        context + ".knots",
        2);
    const std::vector<int> multiplicities = buildIntegerVector(
        mini_json::requireMember(segment, "multiplicities", context),
        context + ".multiplicities",
        2,
        1);

    if (knots.size() != multiplicities.size()) {
        throw std::runtime_error(context + ".knots and " + context + ".multiplicities must have the same length");
    }
    requireStrictlyIncreasing(knots, context + ".knots");

    int multiplicitySum = 0;
    for (const int multiplicity : multiplicities) {
        multiplicitySum += multiplicity;
    }
    if (multiplicitySum - degree - 1 != poles.Length()) {
        throw std::runtime_error(context + " has inconsistent controlPoints/degree/multiplicities");
    }

    TColStd_Array1OfReal knotArray(1, static_cast<int>(knots.size()));
    TColStd_Array1OfInteger multiplicityArray(1, static_cast<int>(multiplicities.size()));
    for (std::size_t index = 0; index < knots.size(); ++index) {
        knotArray(static_cast<int>(index) + 1) = knots[index];
        multiplicityArray(static_cast<int>(index) + 1) = multiplicities[index];
    }

    return new Geom_BSplineCurve(poles, knotArray, multiplicityArray, degree, Standard_False);
}

BuiltWire buildWireFromSegments(const Value& segmentsValue, const std::string& context)
{
    const Value& segments = mini_json::requireArray(segmentsValue, context);
    if (segments.array.empty()) {
        throw std::runtime_error("Profile wire must contain at least one segment");
    }

    BRepBuilderAPI_MakeWire mkWire;
    std::vector<std::pair<double, double>> sampledPoints;
    bool isCircleWire = false;

    for (std::size_t index = 0; index < segments.array.size(); ++index) {
        const Value& segment = mini_json::requireObject(segments.array[index], context + "[" + std::to_string(index) + "]");
        const std::string type = mini_json::requireString(mini_json::requireMember(segment, "type", context), context + ".type");

        if (type == "line") {
            const auto start = mini_json::requirePoint2(mini_json::requireMember(segment, "start", context), context + ".start");
            const auto end = mini_json::requirePoint2(mini_json::requireMember(segment, "end", context), context + ".end");
            const double sx = start[0];
            const double sy = start[1];
            const double ex = end[0];
            const double ey = end[1];
            appendPoint(sampledPoints, sx, sy);
            appendPoint(sampledPoints, ex, ey);
            gp_Pnt p1(sx, sy, 0), p2(ex, ey, 0);
            BRepBuilderAPI_MakeEdge mkEdge(p1, p2);
            if (!mkEdge.IsDone()) throw std::runtime_error("Failed to build line edge");
            mkWire.Add(mkEdge.Edge());
        } else if (type == "arc") {
            const auto start = mini_json::requirePoint2(mini_json::requireMember(segment, "start", context), context + ".start");
            const auto mid = mini_json::requirePoint2(mini_json::requireMember(segment, "mid", context), context + ".mid");
            const auto end = mini_json::requirePoint2(mini_json::requireMember(segment, "end", context), context + ".end");
            const double sx = start[0];
            const double sy = start[1];
            const double mx = mid[0];
            const double my = mid[1];
            const double ex = end[0];
            const double ey = end[1];
            gp_Pnt p1(sx, sy, 0), pm(mx, my, 0), p2(ex, ey, 0);
            Handle(Geom_TrimmedCurve) arc = GC_MakeArcOfCircle(p1, pm, p2);
            appendCurveSampledPoints(arc, sampledPoints, 12);
            BRepBuilderAPI_MakeEdge mkEdge(arc);
            if (!mkEdge.IsDone()) throw std::runtime_error("Failed to build arc edge");
            mkWire.Add(mkEdge.Edge());
        } else if (type == "circle") {
            const auto centre = mini_json::requirePoint2(mini_json::requireMember(segment, "centre", context), context + ".centre");
            const double cx = centre[0];
            const double cy = centre[1];
            const double r = mini_json::requireNumber(mini_json::requireMember(segment, "radius", context), context + ".radius");
            appendPoint(sampledPoints, cx + r, cy);
            appendPoint(sampledPoints, cx, cy + r);
            appendPoint(sampledPoints, cx - r, cy);
            appendPoint(sampledPoints, cx, cy - r);
            isCircleWire = segments.array.size() == 1;
            gp_Ax2 ax(gp_Pnt(cx, cy, 0), gp_Dir(0, 0, 1));
            gp_Circ circ(ax, r);
            BRepBuilderAPI_MakeEdge mkEdge(circ);
            if (!mkEdge.IsDone()) throw std::runtime_error("Failed to build circle edge");
            mkWire.Add(mkEdge.Edge());
        } else if (type == "bezier") {
            Handle(Geom_BezierCurve) curve = buildBezierCurve(segment, context);
            appendCurveSampledPoints(curve, sampledPoints, 16);
            BRepBuilderAPI_MakeEdge mkEdge(curve);
            if (!mkEdge.IsDone()) throw std::runtime_error("Failed to build bezier edge");
            mkWire.Add(mkEdge.Edge());
        } else if (type == "bspline") {
            Handle(Geom_BSplineCurve) curve = buildBSplineCurve(segment, context);
            appendCurveSampledPoints(curve, sampledPoints, 16);
            BRepBuilderAPI_MakeEdge mkEdge(curve);
            if (!mkEdge.IsDone()) throw std::runtime_error("Failed to build bspline edge");
            mkWire.Add(mkEdge.Edge());
        } else {
            throw std::runtime_error("Unknown profile segment type: " + type);
        }
    }

    if (!mkWire.IsDone()) {
        throw std::runtime_error("Failed to build wire from profile (wire not closed or edges not connected)");
    }

    BuiltWire result;
    result.wire = mkWire.Wire();
    result.signedArea = computeSignedArea(sampledPoints);
    result.hasSignedArea = std::abs(result.signedArea) > 1.0e-9;
    result.isCircleWire = isCircleWire;
    return result;
}

BuiltWire buildWireFromWireValue(const Value& wireValue, const std::string& context)
{
    if (wireValue.kind == Value::Kind::Array) {
        return buildWireFromSegments(wireValue, context);
    }
    const Value& wireObject = mini_json::requireObject(wireValue, context);
    return buildWireFromSegments(mini_json::requireMember(wireObject, "segments", context), context + ".segments");
}

std::vector<BuiltWire> buildWiresFromProfileValue(const Value& root)
{
    std::vector<BuiltWire> wires;

    if (const Value* wiresValue = root.get("wires")) {
        const Value& wiresArray = mini_json::requireArray(*wiresValue, "profile.wires");
        if (wiresArray.array.empty()) {
            throw std::runtime_error("Profile must contain at least one wire");
        }
        for (std::size_t index = 0; index < wiresArray.array.size(); ++index) {
            wires.push_back(buildWireFromWireValue(wiresArray.array[index], "profile.wires[" + std::to_string(index) + "]"));
        }
        return wires;
    }

    if (const Value* outerValue = root.get("outer")) {
        wires.push_back(buildWireFromWireValue(*outerValue, "profile.outer"));
        if (const Value* holesValue = root.get("holes")) {
            const Value& holesArray = mini_json::requireArray(*holesValue, "profile.holes");
            for (std::size_t index = 0; index < holesArray.array.size(); ++index) {
                wires.push_back(buildWireFromWireValue(holesArray.array[index], "profile.holes[" + std::to_string(index) + "]"));
            }
        }
        return wires;
    }

    if (const Value* segmentsValue = root.get("segments")) {
        wires.push_back(buildWireFromSegments(*segmentsValue, "profile.segments"));
        return wires;
    }

    throw std::runtime_error("Profile JSON must contain 'segments', 'outer', or 'wires'");
}

} // namespace

TopoDS_Wire buildWireFromProfile(const std::string& profileJson)
{
    const Value root = mini_json::parse(profileJson);
    const std::vector<BuiltWire> wires = buildWiresFromProfileValue(root);
    if (wires.empty()) {
        throw std::runtime_error("Profile must contain at least one wire");
    }
    return wires.front().wire;
}

TopoDS_Face buildFaceFromProfile(const std::string& profileJson)
{
    const Value root = mini_json::parse(profileJson);
    const std::vector<BuiltWire> wires = buildWiresFromProfileValue(root);
    if (wires.empty()) {
        throw std::runtime_error("Profile must contain at least one wire");
    }

    BRepBuilderAPI_MakeFace mkFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), wires.front().wire, Standard_True);
    if (!mkFace.IsDone()) {
        throw std::runtime_error("Failed to build planar face from outer wire");
    }

    for (std::size_t index = 1; index < wires.size(); ++index) {
        TopoDS_Wire holeWire = wires[index].wire;
        const bool needsReverse = wires[index].isCircleWire
            || (wires.front().hasSignedArea && wires[index].hasSignedArea && (wires.front().signedArea * wires[index].signedArea) > 0.0);
        if (needsReverse) {
            holeWire.Reverse();
        }
        mkFace.Add(holeWire);
    }

    return mkFace.Face();
}

} // namespace occt_kernel
