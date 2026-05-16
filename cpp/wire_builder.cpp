/**
 * @file wire_builder.cpp
 * @brief JSON spatial-wire to OCCT wire conversion.
 */

#include "wire_builder.h"
#include "json_utils.h"

#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <Geom_BezierCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_TrimmedCurve.hxx>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <NCollection_Array1.hxx>
#include <TopExp.hxx>
#include <TopoDS_Vertex.hxx>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace occt_kernel {

namespace {

using mini_json::Value;
using PointArray = NCollection_Array1<gp_Pnt>;
using RealArray = NCollection_Array1<double>;
using IntegerArray = NCollection_Array1<int>;

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

gp_Pnt parsePoint3Value(const Value& value, const std::string& context)
{
    const auto point = mini_json::requirePoint3(value, context);
    return gp_Pnt(point[0], point[1], point[2]);
}

gp_Dir parseDirection3Value(const Value& value, const std::string& context)
{
    const auto point = mini_json::requirePoint3(value, context);
    gp_Vec vector(point[0], point[1], point[2]);
    if (vector.SquareMagnitude() <= 0.0) {
        throw std::runtime_error(context + " must not be the zero vector");
    }
    return gp_Dir(vector.X(), vector.Y(), vector.Z());
}

PointArray buildControlPointArray(const Value& controlPointsValue, const std::string& context, std::size_t minimumCount)
{
    const Value& controlPoints = mini_json::requireArray(controlPointsValue, context);
    if (controlPoints.array.size() < minimumCount) {
        throw std::runtime_error(context + " must contain at least " + std::to_string(minimumCount) + " points");
    }

    PointArray poles(1, static_cast<int>(controlPoints.array.size()));
    for (std::size_t index = 0; index < controlPoints.array.size(); ++index) {
        poles(static_cast<int>(index) + 1) = parsePoint3Value(controlPoints.array[index], context + "[" + std::to_string(index) + "]");
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
        const double number = mini_json::requireNumber(values.array[index], context + "[" + std::to_string(index) + "]");
        if (!std::isfinite(number)) {
            throw std::runtime_error(context + " must contain only finite values");
        }
        result.push_back(number);
    }
    return result;
}

std::vector<int> buildIntegerVector(const Value& valuesValue,
                                    const std::string& context,
                                    std::size_t minimumCount,
                                    int minimumValue)
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

Handle(Geom_BezierCurve) buildBezierCurve(const Value& segment, const std::string& context)
{
    const PointArray poles = buildControlPointArray(
        mini_json::requireMember(segment, "controlPoints", context),
        context + ".controlPoints",
        2);
    return new Geom_BezierCurve(poles);
}

Handle(Geom_BSplineCurve) buildBSplineCurve(const Value& segment, const std::string& context)
{
    const PointArray poles = buildControlPointArray(
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

    RealArray knotArray(1, static_cast<int>(knots.size()));
    IntegerArray multiplicityArray(1, static_cast<int>(multiplicities.size()));
    for (std::size_t index = 0; index < knots.size(); ++index) {
        knotArray(static_cast<int>(index) + 1) = knots[index];
        multiplicityArray(static_cast<int>(index) + 1) = multiplicities[index];
    }

    return new Geom_BSplineCurve(poles, knotArray, multiplicityArray, degree, false);
}

gp_Dir makePerpendicularDirection(const gp_Dir& normal)
{
    gp_Vec reference = std::abs(normal.X()) < 0.9 ? gp_Vec(1.0, 0.0, 0.0) : gp_Vec(0.0, 1.0, 0.0);
    gp_Vec cross = gp_Vec(normal.X(), normal.Y(), normal.Z()).Crossed(reference);
    if (cross.SquareMagnitude() <= 1.0e-12) {
        reference = gp_Vec(0.0, 0.0, 1.0);
        cross = gp_Vec(normal.X(), normal.Y(), normal.Z()).Crossed(reference);
    }
    if (cross.SquareMagnitude() <= 1.0e-12) {
        throw std::runtime_error("Unable to derive a perpendicular xDirection for circle segment");
    }
    return gp_Dir(cross.X(), cross.Y(), cross.Z());
}

TopoDS_Wire buildSpatialWireFromValue(const Value& value, const std::string& context, bool requireClosed)
{
    const Value* segmentsValue = nullptr;
    std::string segmentsContext = context;
    if (value.kind == Value::Kind::Array) {
        segmentsValue = &value;
    } else if (value.kind == Value::Kind::Object) {
        segmentsValue = &mini_json::requireMember(value, "segments", context);
        segmentsContext = context + ".segments";
    } else {
        throw std::runtime_error("Expected object or array for " + context);
    }

    const Value& segments = mini_json::requireArray(*segmentsValue, segmentsContext);
    if (segments.array.empty()) {
        throw std::runtime_error("Spatial wire must contain at least one segment");
    }

    BRepBuilderAPI_MakeWire mkWire;
    for (std::size_t index = 0; index < segments.array.size(); ++index) {
        const std::string segmentContext = segmentsContext + "[" + std::to_string(index) + "]";
        const Value& segment = mini_json::requireObject(segments.array[index], segmentContext);
        const std::string type = mini_json::requireString(mini_json::requireMember(segment, "type", segmentContext), segmentContext + ".type");

        if (type == "line") {
            BRepBuilderAPI_MakeEdge mkEdge(
                parsePoint3Value(mini_json::requireMember(segment, "start", segmentContext), segmentContext + ".start"),
                parsePoint3Value(mini_json::requireMember(segment, "end", segmentContext), segmentContext + ".end"));
            mkWire.Add(mkEdge.Edge());
        } else if (type == "arc") {
            Handle(Geom_TrimmedCurve) curve = GC_MakeArcOfCircle(
                parsePoint3Value(mini_json::requireMember(segment, "start", segmentContext), segmentContext + ".start"),
                parsePoint3Value(mini_json::requireMember(segment, "mid", segmentContext), segmentContext + ".mid"),
                parsePoint3Value(mini_json::requireMember(segment, "end", segmentContext), segmentContext + ".end"));
            BRepBuilderAPI_MakeEdge mkEdge(curve);
            mkWire.Add(mkEdge.Edge());
        } else if (type == "circle") {
            const gp_Pnt center = parsePoint3Value(mini_json::requireMember(segment, "center", segmentContext), segmentContext + ".center");
            const gp_Dir normal = parseDirection3Value(mini_json::requireMember(segment, "normal", segmentContext), segmentContext + ".normal");
            const double radius = mini_json::requireNumber(mini_json::requireMember(segment, "radius", segmentContext), segmentContext + ".radius");
            if (!std::isfinite(radius) || radius <= 0.0) {
                throw std::runtime_error(segmentContext + ".radius must be > 0");
            }
            gp_Dir xDirection = makePerpendicularDirection(normal);
            if (const Value* xDirectionValue = segment.get("xDirection")) {
                xDirection = parseDirection3Value(*xDirectionValue, segmentContext + ".xDirection");
                if (std::abs(normal.Dot(xDirection)) > 1.0 - 1.0e-9) {
                    throw std::runtime_error(segmentContext + ".xDirection must not be parallel to normal");
                }
            }
            BRepBuilderAPI_MakeEdge mkEdge(gp_Circ(gp_Ax2(center, normal, xDirection), radius));
            mkWire.Add(mkEdge.Edge());
        } else if (type == "bezier") {
            BRepBuilderAPI_MakeEdge mkEdge(buildBezierCurve(segment, segmentContext));
            mkWire.Add(mkEdge.Edge());
        } else if (type == "bspline") {
            BRepBuilderAPI_MakeEdge mkEdge(buildBSplineCurve(segment, segmentContext));
            mkWire.Add(mkEdge.Edge());
        } else {
            throw std::runtime_error("Unsupported spatial wire segment type: " + type);
        }
    }

    if (!mkWire.IsDone()) {
        throw std::runtime_error("Failed to build wire from spatial segments");
    }

    const TopoDS_Wire wire = mkWire.Wire();
    if (requireClosed) {
        TopoDS_Vertex firstVertex;
        TopoDS_Vertex lastVertex;
        TopExp::Vertices(wire, firstVertex, lastVertex);
        if (!firstVertex.IsNull() && !lastVertex.IsNull() && !firstVertex.IsSame(lastVertex)) {
            throw std::runtime_error("Spatial wire must be closed");
        }
    }

    return wire;
}

} // namespace

TopoDS_Wire buildSpatialWireFromJson(const std::string& wireJson, bool requireClosed)
{
    return buildSpatialWireFromValue(mini_json::parse(wireJson), "wire", requireClosed);
}

} // namespace occt_kernel