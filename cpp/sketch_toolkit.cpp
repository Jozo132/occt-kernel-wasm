#include "sketch_toolkit.h"

#include "json_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace occt_kernel {

namespace {

using mini_json::Value;

struct SolveOptions {
    int32_t maxIterations = 32;
    double residualTolerance = 1.0e-6;
    double stepTolerance = 1.0e-8;
    double damping = 1.0e-8;
    std::string algorithm = "lm";
    bool algorithmFallback = false;
};

struct PointState {
    int32_t entityId = 0;
    double x = 0.0;
    double y = 0.0;
    bool fixed = false;
};

struct LineSegmentState {
    int32_t entityId = 0;
    int32_t startPointId = 0;
    int32_t endPointId = 0;
};

struct ArcState {
    int32_t entityId = 0;
    int32_t centerPointId = 0;
    int32_t startPointId = 0;
    int32_t endPointId = 0;
    double radius = 0.0;
    double referenceStartRadians = 0.0;
    double referenceSweepRadians = 0.0;
};

struct CircleState {
    int32_t entityId = 0;
    int32_t centerPointId = 0;
    double radius = 0.0;
};

struct FixConstraintState {
    int32_t pointId = 0;
    double targetX = 0.0;
    double targetY = 0.0;
};

struct CoincidentConstraintState {
    int32_t pointA = 0;
    int32_t pointB = 0;
};

struct DistanceConstraintState {
    int32_t pointA = 0;
    int32_t pointB = 0;
    double target = 0.0;
};

struct DistancePointLineConstraintState {
    int32_t pointId = 0;
    int32_t lineId = 0;
    double target = 0.0;
};

struct RadiusConstraintState {
    int32_t entityId = 0;
    double target = 0.0;
};

struct PointOnCurveConstraintState {
    int32_t pointId = 0;
    int32_t curveId = 0;
};

struct AxisConstraintState {
    bool horizontal = false;
    int32_t startPointId = 0;
    int32_t endPointId = 0;
};

struct LinePairConstraintState {
    int32_t lineA = 0;
    int32_t lineB = 0;
};

struct CurvePairConstraintState {
    int32_t curveA = 0;
    int32_t curveB = 0;
};

struct TangentConstraintState {
    bool lineCurve = false;
    int32_t lineId = 0;
    int32_t curveA = 0;
    int32_t curveB = 0;
};

struct AngleConstraintState {
    int32_t lineA = 0;
    int32_t lineB = 0;
    double target = 0.0;
};

double normalizeAngle(double angle)
{
    while (angle > M_PI) {
        angle -= 2.0 * M_PI;
    }
    while (angle < -M_PI) {
        angle += 2.0 * M_PI;
    }
    return angle;
}

double wrapAngleNear(double angle, double reference)
{
    while (angle - reference > M_PI) {
        angle -= 2.0 * M_PI;
    }
    while (angle - reference < -M_PI) {
        angle += 2.0 * M_PI;
    }
    return angle;
}

double wrapSweepNear(double sweep, double reference)
{
    const double fullTurn = 2.0 * M_PI;
    while (sweep - reference > fullTurn) {
        sweep -= fullTurn;
    }
    while (sweep - reference < -fullTurn) {
        sweep += fullTurn;
    }
    return sweep;
}

std::string makeSessionId()
{
    static int32_t counter = 0;
    counter += 1;

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    std::ostringstream builder;
    builder << "sketch_session_" << millis << '_' << counter;
    return builder.str();
}

const Value* findMember(const Value& object, const std::string& key)
{
    if (object.kind != Value::Kind::Object) {
        return nullptr;
    }
    return object.get(key);
}

int32_t requireInt32(const Value& value, const std::string& context)
{
    const double numeric = mini_json::requireNumber(value, context);
    const double rounded = std::round(numeric);
    if (std::abs(numeric - rounded) > 1.0e-9) {
        throw std::runtime_error("Expected integer for " + context);
    }
    return static_cast<int32_t>(rounded);
}

bool getOptionalBoolMember(const Value& object, const std::string& key, bool fallback)
{
    const Value* member = findMember(object, key);
    if (member == nullptr) {
        return fallback;
    }
    return mini_json::requireBool(*member, key);
}

double getOptionalNumberMember(const Value& object, const std::string& key, double fallback)
{
    const Value* member = findMember(object, key);
    if (member == nullptr) {
        return fallback;
    }
    return mini_json::requireNumber(*member, key);
}

std::string getOptionalStringMember(const Value& object, const std::string& key)
{
    const Value* member = findMember(object, key);
    if (member == nullptr) {
        return std::string();
    }
    return mini_json::requireString(*member, key);
}

bool tryParseDouble(const std::string& text, double* value)
{
    char* end = nullptr;
    const double numeric = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || (end != nullptr && *end != '\0')) {
        return false;
    }
    *value = numeric;
    return true;
}

double resolveScalarSource(const Value& value, const std::unordered_map<std::string, std::string>& parameters, const std::string& context)
{
    if (value.kind == Value::Kind::Number) {
        return value.number;
    }
    if (value.kind != Value::Kind::String) {
        throw std::runtime_error("Expected numeric or parameter string for " + context);
    }

    const auto parameterIt = parameters.find(value.string);
    if (parameterIt != parameters.end()) {
        const Value parameterValue = mini_json::parse(parameterIt->second);
        if (parameterValue.kind == Value::Kind::Number) {
            return parameterValue.number;
        }
        if (parameterValue.kind == Value::Kind::String) {
            double nestedNumber = 0.0;
            if (tryParseDouble(parameterValue.string, &nestedNumber)) {
                return nestedNumber;
            }
        }
        throw std::runtime_error("Parameter '" + value.string + "' is not numeric for " + context);
    }

    double literalNumber = 0.0;
    if (tryParseDouble(value.string, &literalNumber)) {
        return literalNumber;
    }

    throw std::runtime_error("Unknown scalar source '" + value.string + "' for " + context);
}

void appendEscapedJsonString(std::ostringstream& output, const std::string& value)
{
    for (const char ch : value) {
        switch (ch) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default: output << ch; break;
        }
    }
}

void appendJsonValue(std::ostringstream& output, const Value& value)
{
    switch (value.kind) {
    case Value::Kind::Null:
        output << "null";
        break;
    case Value::Kind::Bool:
        output << (value.boolean ? "true" : "false");
        break;
    case Value::Kind::Number:
        output << std::setprecision(17) << value.number;
        break;
    case Value::Kind::String:
        output << '"';
        appendEscapedJsonString(output, value.string);
        output << '"';
        break;
    case Value::Kind::Array:
        output << '[';
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            if (index > 0) {
                output << ',';
            }
            appendJsonValue(output, value.array[index]);
        }
        output << ']';
        break;
    case Value::Kind::Object:
        output << '{';
        for (std::size_t index = 0; index < value.object.size(); ++index) {
            if (index > 0) {
                output << ',';
            }
            output << '"';
            appendEscapedJsonString(output, value.object[index].first);
            output << "\":";
            appendJsonValue(output, value.object[index].second);
        }
        output << '}';
        break;
    }
}

std::string stringifyJsonValue(const Value& value)
{
    std::ostringstream output;
    appendJsonValue(output, value);
    return output.str();
}

void setObjectNumber(Value* object, const std::string& key, double numeric)
{
    if (object == nullptr || object->kind != Value::Kind::Object) {
        throw std::runtime_error("Expected object while updating numeric member");
    }
    for (auto& entry : object->object) {
        if (entry.first == key) {
            entry.second = Value::makeNumber(numeric);
            return;
        }
    }
    object->object.push_back({ key, Value::makeNumber(numeric) });
}

double estimateStructuralDof(const Value& entitySpec, const std::string& kind)
{
    if (kind == "point") {
        return getOptionalBoolMember(entitySpec, "fixed", false) ? 0.0 : 2.0;
    }
    if (kind == "line-segment" || kind == "polyline" || kind == "external-reference") {
        return 0.0;
    }
    if (kind == "infinite-line" || kind == "coordinate-system") {
        return 3.0;
    }
    if (kind == "circle") {
        return 1.0;
    }
    if (kind == "arc") {
        return findMember(entitySpec, "startPoint") != nullptr && findMember(entitySpec, "endPoint") != nullptr ? 1.0 : 3.0;
    }
    if (kind == "ellipse") {
        return 3.0;
    }
    if (kind == "elliptical-arc") {
        return 5.0;
    }
    if (kind == "bspline") {
        const Value* controlPoints = findMember(entitySpec, "controlPoints");
        const Value* weights = findMember(entitySpec, "weights");
        const std::size_t controlPointCount = controlPoints != nullptr && controlPoints->kind == Value::Kind::Array ? controlPoints->array.size() : 0U;
        const std::size_t weightCount = weights != nullptr && weights->kind == Value::Kind::Array ? weights->array.size() : 0U;
        return static_cast<double>(controlPointCount * 2U + weightCount);
    }
    return 0.0;
}

int32_t estimateConstraintRank(const std::string& kind)
{
    if (kind == "fix" || kind == "coincident") {
        return 2;
    }
    return 1;
}

int32_t estimateImplicitConstraintRank(const Value& entitySpec, const std::string& kind)
{
    if (kind != "arc") {
        return 0;
    }

    int32_t rank = 0;
    if (findMember(entitySpec, "startPoint") != nullptr) {
        rank += 1;
    }
    if (findMember(entitySpec, "endPoint") != nullptr) {
        rank += 1;
    }
    return rank;
}

std::string deriveStructuralStatus(double structuralDof, int32_t drivingConstraintCount)
{
    if (drivingConstraintCount == 0) {
        return "underdefined";
    }
    if (structuralDof > static_cast<double>(drivingConstraintCount)) {
        return "underdefined";
    }
    if (std::abs(structuralDof - static_cast<double>(drivingConstraintCount)) <= 1.0e-9) {
        return "fully-defined";
    }
    return "overdefined";
}

void accumulateResidual(
    const std::vector<int32_t>& variableIndices,
    const std::vector<double>& coefficients,
    double residual,
    std::vector<double>* normalMatrix,
    std::vector<double>* gradient,
    double* maxResidual)
{
    *maxResidual = std::max(*maxResidual, std::abs(residual));
    const std::size_t variableCount = gradient->size();
    for (std::size_t row = 0; row < variableIndices.size(); ++row) {
        const int32_t rowIndex = variableIndices[row];
        if (rowIndex < 0) {
            continue;
        }
        (*gradient)[static_cast<std::size_t>(rowIndex)] += coefficients[row] * residual;
        for (std::size_t column = 0; column < variableIndices.size(); ++column) {
            const int32_t columnIndex = variableIndices[column];
            if (columnIndex < 0) {
                continue;
            }
            (*normalMatrix)[static_cast<std::size_t>(rowIndex) * variableCount + static_cast<std::size_t>(columnIndex)] += coefficients[row] * coefficients[column];
        }
    }
}

bool solveLinearSystem(std::vector<double> matrix, std::vector<double> rhs, std::vector<double>* solution)
{
    const std::size_t n = rhs.size();
    if (matrix.size() != n * n) {
        throw std::runtime_error("Normal equation matrix dimensions are inconsistent");
    }
    if (n == 0) {
        solution->clear();
        return true;
    }

    for (std::size_t pivot = 0; pivot < n; ++pivot) {
        std::size_t bestRow = pivot;
        double bestValue = std::abs(matrix[pivot * n + pivot]);
        for (std::size_t row = pivot + 1; row < n; ++row) {
            const double candidate = std::abs(matrix[row * n + pivot]);
            if (candidate > bestValue) {
                bestValue = candidate;
                bestRow = row;
            }
        }

        if (bestValue < 1.0e-12) {
            return false;
        }

        if (bestRow != pivot) {
            for (std::size_t column = pivot; column < n; ++column) {
                std::swap(matrix[pivot * n + column], matrix[bestRow * n + column]);
            }
            std::swap(rhs[pivot], rhs[bestRow]);
        }

        const double pivotValue = matrix[pivot * n + pivot];
        for (std::size_t row = pivot + 1; row < n; ++row) {
            const double factor = matrix[row * n + pivot] / pivotValue;
            if (std::abs(factor) < 1.0e-18) {
                continue;
            }
            matrix[row * n + pivot] = 0.0;
            for (std::size_t column = pivot + 1; column < n; ++column) {
                matrix[row * n + column] -= factor * matrix[pivot * n + column];
            }
            rhs[row] -= factor * rhs[pivot];
        }
    }

    solution->assign(n, 0.0);
    for (std::size_t reverse = 0; reverse < n; ++reverse) {
        const std::size_t row = n - 1 - reverse;
        double sum = rhs[row];
        for (std::size_t column = row + 1; column < n; ++column) {
            sum -= matrix[row * n + column] * (*solution)[column];
        }
        (*solution)[row] = sum / matrix[row * n + row];
    }

    return true;
}

std::string joinKinds(const std::vector<std::string>& kinds)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < kinds.size(); ++index) {
        if (index > 0) {
            output << ", ";
        }
        output << kinds[index];
    }
    return output.str();
}

std::string buildDiagnosticsJson(
    const std::string& structuralState,
    double structuralDof,
    int32_t drivingConstraintCount,
    int32_t drivenConstraintCount,
    const std::vector<std::string>& unsupportedEntityKinds,
    const std::vector<std::string>& unsupportedConstraintKinds,
    bool algorithmFallback,
    bool converged,
    double maxScaledResidual)
{
    std::ostringstream output;
    output << '{';
    output << "\"state\":\"" << structuralState << "\",";
    output << "\"dof\":{";
    output << "\"structuralDof\":" << std::setprecision(17) << structuralDof << ',';
    output << "\"drivingConstraintCount\":" << drivingConstraintCount << ',';
    output << "\"drivenConstraintCount\":" << drivenConstraintCount;
    output << "},";
    output << "\"items\":[";

    bool firstItem = true;
    auto appendItem = [&](const char* code, const char* severity, const std::string& message, const std::string& suggestedAction = std::string()) {
        if (!firstItem) {
            output << ',';
        }
        firstItem = false;
        output << '{';
        output << "\"code\":\"" << code << "\",";
        output << "\"severity\":\"" << severity << "\",";
        output << "\"message\":\"";
        appendEscapedJsonString(output, message);
        output << '"';
        if (!suggestedAction.empty()) {
            output << ",\"suggestedAction\":\"";
            appendEscapedJsonString(output, suggestedAction);
            output << '"';
        }
        output << '}';
    };

    appendItem(
        "NATIVE_POINT_SOLVER",
        "info",
        "The native sketch-toolkit solve slice currently supports point, point-line, fix, circle or arc radius, point-on-circle or point-on-arc, tangent, and angle constraints together with point-distance, horizontal or vertical line-segment constraints, and line-segment parallel, perpendicular, or equal-length relations with a damped Gauss-Newton step.",
        "Extend the native residual compiler with additional spline and algebraic constraints as the next migration slice.");

    if (algorithmFallback) {
        appendItem(
            "ALGORITHM_FALLBACK",
            "warning",
            "The requested solve algorithm is not implemented for the current native sketch solve slice, so it fell back to LM.");
    }

    if (!unsupportedEntityKinds.empty()) {
        appendItem(
            "UNSUPPORTED_ENTITY_KINDS",
            "warning",
            "Some entity kinds are stored but not solved natively yet: " + joinKinds(unsupportedEntityKinds),
            "Keep those entities in the document, but expect them to be ignored by the current native solve subset.");
    }

    if (!unsupportedConstraintKinds.empty()) {
        appendItem(
            "UNSUPPORTED_CONSTRAINT_KINDS",
            "warning",
            "Some constraint kinds are stored but not solved natively yet: " + joinKinds(unsupportedConstraintKinds),
            "Add matching residual and Jacobian blocks before expecting those constraints to drive geometry.");
    }

    if (drivenConstraintCount > 0) {
        appendItem(
            "DRIVEN_CONSTRAINTS_SKIPPED",
            "info",
            std::to_string(drivenConstraintCount) + " driven constraints were excluded from residual assembly and are reported as measured outputs only.",
            "Inspect solveResult.drivenDimensions for measured values; convert a constraint back to driving only when it should actively move geometry.");
    }

    if (!converged) {
        appendItem(
            "SOLVE_DID_NOT_CONVERGE",
            "warning",
            "The native sketch-solver slice did not converge within the configured tolerance; the best current iterate was kept.",
            "Increase the iteration budget, add stronger anchoring, or reduce unsupported content before expecting a stable solve.");
    } else if (maxScaledResidual <= 1.0e-6) {
        appendItem(
            "SUPPORTED_CONSTRAINTS_SATISFIED",
            "info",
            "All constraints supported by the current native sketch solver met the configured residual tolerance.");
    }

    output << "],";
    output << "\"freeDirections\":";
    if (structuralState == "underdefined") {
        output << "[{\"label\":\"structural-free-motion\",\"entityIds\":[]}]";
    } else {
        output << "[]";
    }
    output << '}';
    return output.str();
}

} // namespace

SketchToolkitNative::SketchToolkitNative()
    : m_sessionId(makeSessionId()) {}

std::string SketchToolkitNative::getSessionId() const
{
    return m_sessionId;
}

int32_t SketchToolkitNative::createSketch(const std::string& createParamsJson)
{
    SketchRecord record;
    record.id = m_nextSketchId++;
    record.createParamsJson = createParamsJson.empty() ? std::string("{}") : createParamsJson;

    m_sketches.emplace(record.id, std::move(record));
    return record.id;
}

void SketchToolkitNative::disposeSketch(int32_t sketchId)
{
    if (m_sketches.erase(sketchId) == 0) {
        throwError("INVALID_HANDLE", "Sketch handle does not exist in this sketch toolkit session");
    }
}

int32_t SketchToolkitNative::addEntity(int32_t sketchId, const std::string& entitySpecJson)
{
    auto& sketch = requireSketch(sketchId);
    const int32_t entityId = m_nextEntityId++;
    sketch.entities.push_back({ entityId, entitySpecJson.empty() ? std::string("{}") : entitySpecJson });
    return entityId;
}

void SketchToolkitNative::removeEntity(int32_t sketchId, int32_t entityId)
{
    auto& sketch = requireSketch(sketchId);
    const auto originalSize = sketch.entities.size();

    sketch.entities.erase(
        std::remove_if(sketch.entities.begin(), sketch.entities.end(), [entityId](const StoredItem& item) {
            return item.id == entityId;
        }),
        sketch.entities.end());

    if (sketch.entities.size() == originalSize) {
        throwError("INVALID_HANDLE", "Entity handle does not exist in the selected sketch");
    }
}

int32_t SketchToolkitNative::addConstraint(int32_t sketchId, const std::string& constraintSpecJson)
{
    auto& sketch = requireSketch(sketchId);
    const int32_t constraintId = m_nextConstraintId++;
    sketch.constraints.push_back({ constraintId, constraintSpecJson.empty() ? std::string("{}") : constraintSpecJson });
    return constraintId;
}

void SketchToolkitNative::removeConstraint(int32_t sketchId, int32_t constraintId)
{
    auto& sketch = requireSketch(sketchId);
    const auto originalSize = sketch.constraints.size();

    sketch.constraints.erase(
        std::remove_if(sketch.constraints.begin(), sketch.constraints.end(), [constraintId](const StoredItem& item) {
            return item.id == constraintId;
        }),
        sketch.constraints.end());

    if (sketch.constraints.size() == originalSize) {
        throwError("INVALID_HANDLE", "Constraint handle does not exist in the selected sketch");
    }
}

void SketchToolkitNative::setParameter(int32_t sketchId, const std::string& name, const std::string& valueJson)
{
    if (name.empty()) {
        throwError("INVALID_PARAMS", "Parameter name must not be empty");
    }

    auto& sketch = requireSketch(sketchId);
    sketch.parameters[name] = valueJson.empty() ? std::string("null") : valueJson;
}

std::string SketchToolkitNative::solveSketch(int32_t sketchId, const std::string& optionsJson)
{
    auto& sketch = requireSketch(sketchId);

    SolveOptions options;
    if (!optionsJson.empty()) {
        const Value optionsRoot = mini_json::requireObject(mini_json::parse(optionsJson), "solve options");
        const std::string requestedAlgorithm = getOptionalStringMember(optionsRoot, "algorithm");
        if (requestedAlgorithm == "gauss-newton") {
            options.algorithm = "gauss-newton";
            options.damping = 0.0;
        } else if (!requestedAlgorithm.empty() && requestedAlgorithm != "auto" && requestedAlgorithm != "lm") {
            options.algorithmFallback = true;
        }

        options.maxIterations = std::max<int32_t>(1, static_cast<int32_t>(std::llround(getOptionalNumberMember(optionsRoot, "maxIterations", options.maxIterations))));
        options.residualTolerance = std::max(1.0e-12, getOptionalNumberMember(optionsRoot, "residualTolerance", options.residualTolerance));
        options.stepTolerance = std::max(1.0e-14, getOptionalNumberMember(optionsRoot, "stepTolerance", options.stepTolerance));
    }

    std::unordered_map<int32_t, PointState> points;
    std::unordered_map<int32_t, LineSegmentState> lineSegments;
    std::unordered_map<int32_t, CircleState> circles;
    std::unordered_map<int32_t, ArcState> arcs;
    std::vector<std::string> unsupportedEntityKinds;
    std::vector<std::string> unsupportedConstraintKinds;
    std::vector<FixConstraintState> fixConstraints;
    std::vector<CoincidentConstraintState> coincidentConstraints;
    std::vector<DistanceConstraintState> distanceConstraints;
    std::vector<DistancePointLineConstraintState> pointLineDistanceConstraints;
    std::vector<RadiusConstraintState> radiusConstraints;
    std::vector<PointOnCurveConstraintState> pointOnCurveConstraints;
    std::vector<AxisConstraintState> axisConstraints;
    std::vector<LinePairConstraintState> parallelConstraints;
    std::vector<LinePairConstraintState> perpendicularConstraints;
    std::vector<LinePairConstraintState> equalLengthConstraints;
    std::vector<CurvePairConstraintState> equalRadiusConstraints;
    std::vector<TangentConstraintState> tangentConstraints;
    std::vector<AngleConstraintState> angleConstraints;

    double structuralDof = 0.0;
    int32_t drivingConstraintCount = 0;
    int32_t drivenConstraintCount = 0;

    for (const auto& item : sketch.entities) {
        const Value spec = mini_json::requireObject(mini_json::parse(item.json), "entity spec");
        const std::string kind = mini_json::requireString(mini_json::requireMember(spec, "kind", "entity spec"), "entity kind");
        structuralDof += estimateStructuralDof(spec, kind);
        drivingConstraintCount += estimateImplicitConstraintRank(spec, kind);

        if (kind == "point") {
            PointState point;
            point.entityId = item.id;
            point.x = mini_json::requireNumber(mini_json::requireMember(spec, "x", "point entity"), "point.x");
            point.y = mini_json::requireNumber(mini_json::requireMember(spec, "y", "point entity"), "point.y");
            point.fixed = getOptionalBoolMember(spec, "fixed", false);
            points[item.id] = point;
        } else if (kind == "line-segment") {
            LineSegmentState segment;
            segment.entityId = item.id;
            segment.startPointId = requireInt32(mini_json::requireMember(spec, "start", "line-segment entity"), "line-segment.start");
            segment.endPointId = requireInt32(mini_json::requireMember(spec, "end", "line-segment entity"), "line-segment.end");
            lineSegments[item.id] = segment;
        } else if (kind == "circle") {
            CircleState circle;
            circle.entityId = item.id;
            circle.centerPointId = requireInt32(mini_json::requireMember(spec, "center", "circle entity"), "circle.center");
            circle.radius = resolveScalarSource(mini_json::requireMember(spec, "radius", "circle entity"), sketch.parameters, "circle.radius");
            circles[item.id] = circle;
        } else if (kind == "arc") {
            ArcState arc;
            arc.entityId = item.id;
            arc.centerPointId = requireInt32(mini_json::requireMember(spec, "center", "arc entity"), "arc.center");
            arc.radius = resolveScalarSource(mini_json::requireMember(spec, "radius", "arc entity"), sketch.parameters, "arc.radius");
            const Value* startPoint = findMember(spec, "startPoint");
            const Value* endPoint = findMember(spec, "endPoint");
            arc.startPointId = startPoint != nullptr ? requireInt32(*startPoint, "arc.startPoint") : 0;
            arc.endPointId = endPoint != nullptr ? requireInt32(*endPoint, "arc.endPoint") : 0;
            arc.referenceStartRadians = getOptionalNumberMember(spec, "startRadians", 0.0);
            arc.referenceSweepRadians = getOptionalNumberMember(spec, "sweepRadians", 0.0);
            arcs[item.id] = arc;
        } else if (std::find(unsupportedEntityKinds.begin(), unsupportedEntityKinds.end(), kind) == unsupportedEntityKinds.end()) {
            unsupportedEntityKinds.push_back(kind);
        }
    }

    for (const auto& item : sketch.constraints) {
        const Value spec = mini_json::requireObject(mini_json::parse(item.json), "constraint spec");
        const std::string kind = mini_json::requireString(mini_json::requireMember(spec, "kind", "constraint spec"), "constraint kind");
        const std::string drivingState = getOptionalStringMember(spec, "drivingState");
        const int32_t rank = estimateConstraintRank(kind);
        if (drivingState == "driven") {
            drivenConstraintCount += rank;
        } else if (drivingState != "suppressed") {
            drivingConstraintCount += rank;
        }

        if (drivingState == "suppressed" || drivingState == "driven") {
            continue;
        }

        if (kind == "fix") {
            const int32_t entityId = requireInt32(mini_json::requireMember(spec, "entity", "fix constraint"), "fix.entity");
            const auto pointIt = points.find(entityId);
            if (pointIt != points.end()) {
                fixConstraints.push_back({ entityId, pointIt->second.x, pointIt->second.y });
            } else if (std::find(unsupportedConstraintKinds.begin(), unsupportedConstraintKinds.end(), kind) == unsupportedConstraintKinds.end()) {
                unsupportedConstraintKinds.push_back(kind);
            }
        } else if (kind == "coincident") {
            coincidentConstraints.push_back({
                requireInt32(mini_json::requireMember(spec, "pointA", "coincident constraint"), "coincident.pointA"),
                requireInt32(mini_json::requireMember(spec, "pointB", "coincident constraint"), "coincident.pointB"),
            });
        } else if (kind == "distance-point-point") {
            distanceConstraints.push_back({
                requireInt32(mini_json::requireMember(spec, "pointA", "distance-point-point constraint"), "distance-point-point.pointA"),
                requireInt32(mini_json::requireMember(spec, "pointB", "distance-point-point constraint"), "distance-point-point.pointB"),
                resolveScalarSource(mini_json::requireMember(spec, "value", "distance-point-point constraint"), sketch.parameters, "distance-point-point.value"),
            });
        } else if (kind == "distance-point-line") {
            const int32_t pointId = requireInt32(mini_json::requireMember(spec, "point", "distance-point-line constraint"), "distance-point-line.point");
            const int32_t lineId = requireInt32(mini_json::requireMember(spec, "line", "distance-point-line constraint"), "distance-point-line.line");
            if (points.find(pointId) == points.end() || lineSegments.find(lineId) == lineSegments.end()) {
                if (std::find(unsupportedConstraintKinds.begin(), unsupportedConstraintKinds.end(), kind) == unsupportedConstraintKinds.end()) {
                    unsupportedConstraintKinds.push_back(kind);
                }
            } else {
                pointLineDistanceConstraints.push_back({
                    pointId,
                    lineId,
                    resolveScalarSource(mini_json::requireMember(spec, "value", "distance-point-line constraint"), sketch.parameters, "distance-point-line.value"),
                });
            }
        } else if (kind == "radius") {
            const int32_t entityId = requireInt32(mini_json::requireMember(spec, "entity", "radius constraint"), "radius.entity");
            if (circles.find(entityId) == circles.end() && arcs.find(entityId) == arcs.end()) {
                if (std::find(unsupportedConstraintKinds.begin(), unsupportedConstraintKinds.end(), kind) == unsupportedConstraintKinds.end()) {
                    unsupportedConstraintKinds.push_back(kind);
                }
            } else {
                radiusConstraints.push_back({
                    entityId,
                    resolveScalarSource(mini_json::requireMember(spec, "value", "radius constraint"), sketch.parameters, "radius.value"),
                });
            }
        } else if (kind == "point-on-circle" || kind == "point-on-arc") {
            const int32_t pointId = requireInt32(mini_json::requireMember(spec, "point", "point-on-curve constraint"), "point-on-curve.point");
            const int32_t entityId = requireInt32(mini_json::requireMember(spec, "entity", "point-on-curve constraint"), "point-on-curve.entity");
            if (points.find(pointId) == points.end() || (circles.find(entityId) == circles.end() && arcs.find(entityId) == arcs.end())) {
                if (std::find(unsupportedConstraintKinds.begin(), unsupportedConstraintKinds.end(), kind) == unsupportedConstraintKinds.end()) {
                    unsupportedConstraintKinds.push_back(kind);
                }
            } else {
                pointOnCurveConstraints.push_back({ pointId, entityId });
            }
        } else if (kind == "horizontal" || kind == "vertical") {
            const int32_t entityId = requireInt32(mini_json::requireMember(spec, "entity", "axis constraint"), "axis.entity");
            const auto segmentIt = lineSegments.find(entityId);
            if (segmentIt != lineSegments.end()) {
                axisConstraints.push_back({
                    kind == "horizontal",
                    segmentIt->second.startPointId,
                    segmentIt->second.endPointId,
                });
            } else if (std::find(unsupportedConstraintKinds.begin(), unsupportedConstraintKinds.end(), kind) == unsupportedConstraintKinds.end()) {
                unsupportedConstraintKinds.push_back(kind);
            }
        } else if (kind == "parallel" || kind == "perpendicular" || kind == "equal-length") {
            const int32_t lineA = requireInt32(mini_json::requireMember(spec, "entityA", "line-pair constraint"), "line-pair.entityA");
            const int32_t lineB = requireInt32(mini_json::requireMember(spec, "entityB", "line-pair constraint"), "line-pair.entityB");
            if (lineSegments.find(lineA) == lineSegments.end() || lineSegments.find(lineB) == lineSegments.end()) {
                if (std::find(unsupportedConstraintKinds.begin(), unsupportedConstraintKinds.end(), kind) == unsupportedConstraintKinds.end()) {
                    unsupportedConstraintKinds.push_back(kind);
                }
            } else if (kind == "parallel") {
                parallelConstraints.push_back({ lineA, lineB });
            } else if (kind == "perpendicular") {
                perpendicularConstraints.push_back({ lineA, lineB });
            } else {
                equalLengthConstraints.push_back({ lineA, lineB });
            }
        } else if (kind == "equal-radius") {
            const int32_t curveA = requireInt32(mini_json::requireMember(spec, "entityA", "equal-radius constraint"), "equal-radius.entityA");
            const int32_t curveB = requireInt32(mini_json::requireMember(spec, "entityB", "equal-radius constraint"), "equal-radius.entityB");
            if ((circles.find(curveA) == circles.end() && arcs.find(curveA) == arcs.end())
                || (circles.find(curveB) == circles.end() && arcs.find(curveB) == arcs.end())) {
                if (std::find(unsupportedConstraintKinds.begin(), unsupportedConstraintKinds.end(), kind) == unsupportedConstraintKinds.end()) {
                    unsupportedConstraintKinds.push_back(kind);
                }
            } else {
                equalRadiusConstraints.push_back({ curveA, curveB });
            }
        } else if (kind == "tangent") {
            const int32_t entityA = requireInt32(mini_json::requireMember(spec, "entityA", "tangent constraint"), "tangent.entityA");
            const int32_t entityB = requireInt32(mini_json::requireMember(spec, "entityB", "tangent constraint"), "tangent.entityB");
            const bool entityALine = lineSegments.find(entityA) != lineSegments.end();
            const bool entityBLine = lineSegments.find(entityB) != lineSegments.end();
            const bool entityACurve = circles.find(entityA) != circles.end() || arcs.find(entityA) != arcs.end();
            const bool entityBCurve = circles.find(entityB) != circles.end() || arcs.find(entityB) != arcs.end();

            if (entityALine && entityBCurve) {
                tangentConstraints.push_back({ true, entityA, entityB, 0 });
            } else if (entityBLine && entityACurve) {
                tangentConstraints.push_back({ true, entityB, entityA, 0 });
            } else if (entityACurve && entityBCurve) {
                tangentConstraints.push_back({ false, 0, entityA, entityB });
            } else if (std::find(unsupportedConstraintKinds.begin(), unsupportedConstraintKinds.end(), kind) == unsupportedConstraintKinds.end()) {
                unsupportedConstraintKinds.push_back(kind);
            }
        } else if (kind == "angle") {
            const int32_t lineA = requireInt32(mini_json::requireMember(spec, "lineA", "angle constraint"), "angle.lineA");
            const int32_t lineB = requireInt32(mini_json::requireMember(spec, "lineB", "angle constraint"), "angle.lineB");
            if (lineSegments.find(lineA) == lineSegments.end() || lineSegments.find(lineB) == lineSegments.end()) {
                if (std::find(unsupportedConstraintKinds.begin(), unsupportedConstraintKinds.end(), kind) == unsupportedConstraintKinds.end()) {
                    unsupportedConstraintKinds.push_back(kind);
                }
            } else {
                angleConstraints.push_back({
                    lineA,
                    lineB,
                    resolveScalarSource(mini_json::requireMember(spec, "value", "angle constraint"), sketch.parameters, "angle.value"),
                });
            }
        } else if (std::find(unsupportedConstraintKinds.begin(), unsupportedConstraintKinds.end(), kind) == unsupportedConstraintKinds.end()) {
            unsupportedConstraintKinds.push_back(kind);
        }
    }

    const auto requirePoint = [&](int32_t pointId) -> PointState& {
        const auto pointIt = points.find(pointId);
        if (pointIt == points.end()) {
            throwError("INVALID_PARAMS", "Constraint references a point entity that does not exist in this sketch");
        }
        return pointIt->second;
    };

    const auto requireSegment = [&](int32_t segmentId) -> const LineSegmentState& {
        const auto segmentIt = lineSegments.find(segmentId);
        if (segmentIt == lineSegments.end()) {
            throwError("INVALID_PARAMS", "Constraint references a line-segment entity that does not exist in this sketch");
        }
        return segmentIt->second;
    };

    const auto requireCircle = [&](int32_t circleId) -> CircleState& {
        const auto circleIt = circles.find(circleId);
        if (circleIt == circles.end()) {
            throwError("INVALID_PARAMS", "Constraint references a circle entity that does not exist in this sketch");
        }
        return circleIt->second;
    };

    const auto requireArc = [&](int32_t arcId) -> ArcState& {
        const auto arcIt = arcs.find(arcId);
        if (arcIt == arcs.end()) {
            throwError("INVALID_PARAMS", "Constraint references an arc entity that does not exist in this sketch");
        }
        return arcIt->second;
    };

    struct CurveLikeAccess {
        int32_t centerPointId = 0;
        double* radius = nullptr;
    };

    const auto requireCurveLike = [&](int32_t entityId) -> CurveLikeAccess {
        const auto circleIt = circles.find(entityId);
        if (circleIt != circles.end()) {
            return { circleIt->second.centerPointId, &circleIt->second.radius };
        }

        const auto arcIt = arcs.find(entityId);
        if (arcIt != arcs.end()) {
            return { arcIt->second.centerPointId, &arcIt->second.radius };
        }

        throwError("INVALID_PARAMS", "Constraint references a curve-like entity that does not exist in this sketch");
        return {};
    };

    std::unordered_map<int32_t, int32_t> xIndex;
    std::unordered_map<int32_t, int32_t> yIndex;
    std::unordered_map<int32_t, int32_t> radiusIndex;
    int32_t variableCount = 0;
    for (const auto& entry : points) {
        if (entry.second.fixed) {
            xIndex[entry.first] = -1;
            yIndex[entry.first] = -1;
        } else {
            xIndex[entry.first] = variableCount++;
            yIndex[entry.first] = variableCount++;
        }
    }
    for (const auto& entry : circles) {
        radiusIndex[entry.first] = variableCount++;
    }
    for (const auto& entry : arcs) {
        radiusIndex[entry.first] = variableCount++;
    }

    double maxResidual = 0.0;
    int32_t iterations = 0;

    for (; iterations < options.maxIterations; ++iterations) {
        std::vector<double> normalMatrix(static_cast<std::size_t>(variableCount) * static_cast<std::size_t>(variableCount), 0.0);
        std::vector<double> gradient(static_cast<std::size_t>(variableCount), 0.0);
        maxResidual = 0.0;

        for (const auto& constraint : fixConstraints) {
            PointState& point = requirePoint(constraint.pointId);

            accumulateResidual(
                { xIndex[constraint.pointId] },
                { 1.0 },
                point.x - constraint.targetX,
                &normalMatrix,
                &gradient,
                &maxResidual);
            accumulateResidual(
                { yIndex[constraint.pointId] },
                { 1.0 },
                point.y - constraint.targetY,
                &normalMatrix,
                &gradient,
                &maxResidual);
        }

        const auto accumulatePointOnCurveResidual = [&](int32_t pointId, int32_t curveId) {
            PointState& point = requirePoint(pointId);
            CurveLikeAccess curve = requireCurveLike(curveId);
            PointState& center = requirePoint(curve.centerPointId);

            double dx = point.x - center.x;
            double dy = point.y - center.y;
            double distance = std::sqrt(dx * dx + dy * dy);
            if (distance < 1.0e-12) {
                dx = 1.0;
                dy = 0.0;
                distance = 1.0;
            }

            accumulateResidual(
                {
                    xIndex[pointId],
                    yIndex[pointId],
                    xIndex[curve.centerPointId],
                    yIndex[curve.centerPointId],
                    radiusIndex[curveId],
                },
                {
                    dx / distance,
                    dy / distance,
                    -dx / distance,
                    -dy / distance,
                    -1.0,
                },
                distance - *curve.radius,
                &normalMatrix,
                &gradient,
                &maxResidual);
        };

        for (const auto& constraint : coincidentConstraints) {
            PointState& pointA = requirePoint(constraint.pointA);
            PointState& pointB = requirePoint(constraint.pointB);

            accumulateResidual(
                { xIndex[constraint.pointA], xIndex[constraint.pointB] },
                { 1.0, -1.0 },
                pointA.x - pointB.x,
                &normalMatrix,
                &gradient,
                &maxResidual);
            accumulateResidual(
                { yIndex[constraint.pointA], yIndex[constraint.pointB] },
                { 1.0, -1.0 },
                pointA.y - pointB.y,
                &normalMatrix,
                &gradient,
                &maxResidual);
        }

        for (const auto& constraint : axisConstraints) {
            PointState& pointA = requirePoint(constraint.startPointId);
            PointState& pointB = requirePoint(constraint.endPointId);
            if (constraint.horizontal) {
                accumulateResidual(
                    { yIndex[constraint.startPointId], yIndex[constraint.endPointId] },
                    { -1.0, 1.0 },
                    pointB.y - pointA.y,
                    &normalMatrix,
                    &gradient,
                    &maxResidual);
            } else {
                accumulateResidual(
                    { xIndex[constraint.startPointId], xIndex[constraint.endPointId] },
                    { -1.0, 1.0 },
                    pointB.x - pointA.x,
                    &normalMatrix,
                    &gradient,
                    &maxResidual);
            }
        }

        for (const auto& constraint : distanceConstraints) {
            PointState& pointA = requirePoint(constraint.pointA);
            PointState& pointB = requirePoint(constraint.pointB);
            double dx = pointA.x - pointB.x;
            double dy = pointA.y - pointB.y;
            double distance = std::sqrt(dx * dx + dy * dy);
            if (distance < 1.0e-12) {
                dx = 1.0;
                dy = 0.0;
                distance = 1.0;
            }

            accumulateResidual(
                {
                    xIndex[constraint.pointA],
                    yIndex[constraint.pointA],
                    xIndex[constraint.pointB],
                    yIndex[constraint.pointB],
                },
                { dx / distance, dy / distance, -dx / distance, -dy / distance },
                distance - constraint.target,
                &normalMatrix,
                &gradient,
                &maxResidual);
        }

        for (const auto& constraint : radiusConstraints) {
            CurveLikeAccess curve = requireCurveLike(constraint.entityId);

            accumulateResidual(
                { radiusIndex[constraint.entityId] },
                { 1.0 },
                *curve.radius - constraint.target,
                &normalMatrix,
                &gradient,
                &maxResidual);
        }

        for (const auto& constraint : pointOnCurveConstraints) {
            accumulatePointOnCurveResidual(constraint.pointId, constraint.curveId);
        }

        for (const auto& entry : arcs) {
            const ArcState& arc = entry.second;
            if (arc.startPointId != 0) {
                accumulatePointOnCurveResidual(arc.startPointId, entry.first);
            }
            if (arc.endPointId != 0) {
                accumulatePointOnCurveResidual(arc.endPointId, entry.first);
            }
        }

        for (const auto& constraint : pointLineDistanceConstraints) {
            const LineSegmentState& line = requireSegment(constraint.lineId);
            PointState& point = requirePoint(constraint.pointId);
            PointState& a = requirePoint(line.startPointId);
            PointState& b = requirePoint(line.endPointId);

            double dx = b.x - a.x;
            double dy = b.y - a.y;
            double len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.0e-12) {
                dx = 1.0;
                dy = 0.0;
                len = 1.0;
            }

            const double pxa = point.x - a.x;
            const double pya = point.y - a.y;
            const double area = dx * pya - dy * pxa;
            const double side = area >= 0.0 ? 1.0 : -1.0;
            const double residual = side * area - constraint.target * len;

            accumulateResidual(
                {
                    xIndex[constraint.pointId],
                    yIndex[constraint.pointId],
                    xIndex[line.startPointId],
                    yIndex[line.startPointId],
                    xIndex[line.endPointId],
                    yIndex[line.endPointId],
                },
                {
                    side * (-dy),
                    side * dx,
                    side * (b.y - point.y) + constraint.target * (dx / len),
                    side * (point.x - b.x) + constraint.target * (dy / len),
                    side * pya - constraint.target * (dx / len),
                    side * (-pxa) - constraint.target * (dy / len),
                },
                residual,
                &normalMatrix,
                &gradient,
                &maxResidual);
        }

        for (const auto& constraint : tangentConstraints) {
            if (constraint.lineCurve) {
                const LineSegmentState& line = requireSegment(constraint.lineId);
                CurveLikeAccess curve = requireCurveLike(constraint.curveA);
                PointState& center = requirePoint(curve.centerPointId);
                PointState& a = requirePoint(line.startPointId);
                PointState& b = requirePoint(line.endPointId);

                double dx = b.x - a.x;
                double dy = b.y - a.y;
                double len = std::sqrt(dx * dx + dy * dy);
                if (len < 1.0e-12) {
                    dx = 1.0;
                    dy = 0.0;
                    len = 1.0;
                }

                const double pxa = center.x - a.x;
                const double pya = center.y - a.y;
                const double area = dx * pya - dy * pxa;
                const double side = area >= 0.0 ? 1.0 : -1.0;
                const double residual = side * area - *curve.radius * len;

                accumulateResidual(
                    {
                        xIndex[curve.centerPointId],
                        yIndex[curve.centerPointId],
                        xIndex[line.startPointId],
                        yIndex[line.startPointId],
                        xIndex[line.endPointId],
                        yIndex[line.endPointId],
                        radiusIndex[constraint.curveA],
                    },
                    {
                        side * (-dy),
                        side * dx,
                        side * (b.y - center.y) + *curve.radius * (dx / len),
                        side * (center.x - b.x) + *curve.radius * (dy / len),
                        side * pya - *curve.radius * (dx / len),
                        side * (-pxa) - *curve.radius * (dy / len),
                        -len,
                    },
                    side * area - *curve.radius * len,
                    &normalMatrix,
                    &gradient,
                    &maxResidual);
                continue;
            }

            CurveLikeAccess curveA = requireCurveLike(constraint.curveA);
            CurveLikeAccess curveB = requireCurveLike(constraint.curveB);
            PointState& centerA = requirePoint(curveA.centerPointId);
            PointState& centerB = requirePoint(curveB.centerPointId);

            double dx = centerA.x - centerB.x;
            double dy = centerA.y - centerB.y;
            double distance = std::sqrt(dx * dx + dy * dy);
            if (distance < 1.0e-12) {
                dx = 1.0;
                dy = 0.0;
                distance = 1.0;
            }

            accumulateResidual(
                {
                    xIndex[curveA.centerPointId],
                    yIndex[curveA.centerPointId],
                    xIndex[curveB.centerPointId],
                    yIndex[curveB.centerPointId],
                    radiusIndex[constraint.curveA],
                    radiusIndex[constraint.curveB],
                },
                {
                    dx / distance,
                    dy / distance,
                    -dx / distance,
                    -dy / distance,
                    -1.0,
                    -1.0,
                },
                distance - *curveA.radius - *curveB.radius,
                &normalMatrix,
                &gradient,
                &maxResidual);
        }

        for (const auto& constraint : parallelConstraints) {
            const LineSegmentState& lineA = requireSegment(constraint.lineA);
            const LineSegmentState& lineB = requireSegment(constraint.lineB);
            PointState& a0 = requirePoint(lineA.startPointId);
            PointState& a1 = requirePoint(lineA.endPointId);
            PointState& b0 = requirePoint(lineB.startPointId);
            PointState& b1 = requirePoint(lineB.endPointId);

            const double ax = a1.x - a0.x;
            const double ay = a1.y - a0.y;
            const double bx = b1.x - b0.x;
            const double by = b1.y - b0.y;

            accumulateResidual(
                {
                    xIndex[lineA.startPointId],
                    yIndex[lineA.startPointId],
                    xIndex[lineA.endPointId],
                    yIndex[lineA.endPointId],
                    xIndex[lineB.startPointId],
                    yIndex[lineB.startPointId],
                    xIndex[lineB.endPointId],
                    yIndex[lineB.endPointId],
                },
                {
                    -by,
                    bx,
                    by,
                    -bx,
                    ay,
                    -ax,
                    -ay,
                    ax,
                },
                ax * by - ay * bx,
                &normalMatrix,
                &gradient,
                &maxResidual);
        }

        for (const auto& constraint : perpendicularConstraints) {
            const LineSegmentState& lineA = requireSegment(constraint.lineA);
            const LineSegmentState& lineB = requireSegment(constraint.lineB);
            PointState& a0 = requirePoint(lineA.startPointId);
            PointState& a1 = requirePoint(lineA.endPointId);
            PointState& b0 = requirePoint(lineB.startPointId);
            PointState& b1 = requirePoint(lineB.endPointId);

            const double ax = a1.x - a0.x;
            const double ay = a1.y - a0.y;
            const double bx = b1.x - b0.x;
            const double by = b1.y - b0.y;

            accumulateResidual(
                {
                    xIndex[lineA.startPointId],
                    yIndex[lineA.startPointId],
                    xIndex[lineA.endPointId],
                    yIndex[lineA.endPointId],
                    xIndex[lineB.startPointId],
                    yIndex[lineB.startPointId],
                    xIndex[lineB.endPointId],
                    yIndex[lineB.endPointId],
                },
                {
                    -bx,
                    -by,
                    bx,
                    by,
                    -ax,
                    -ay,
                    ax,
                    ay,
                },
                ax * bx + ay * by,
                &normalMatrix,
                &gradient,
                &maxResidual);
        }

        for (const auto& constraint : equalLengthConstraints) {
            const LineSegmentState& lineA = requireSegment(constraint.lineA);
            const LineSegmentState& lineB = requireSegment(constraint.lineB);
            PointState& a0 = requirePoint(lineA.startPointId);
            PointState& a1 = requirePoint(lineA.endPointId);
            PointState& b0 = requirePoint(lineB.startPointId);
            PointState& b1 = requirePoint(lineB.endPointId);

            double ax = a1.x - a0.x;
            double ay = a1.y - a0.y;
            double bx = b1.x - b0.x;
            double by = b1.y - b0.y;
            double lenA = std::sqrt(ax * ax + ay * ay);
            double lenB = std::sqrt(bx * bx + by * by);
            if (lenA < 1.0e-12) {
                ax = 1.0;
                ay = 0.0;
                lenA = 1.0;
            }
            if (lenB < 1.0e-12) {
                bx = 1.0;
                by = 0.0;
                lenB = 1.0;
            }

            accumulateResidual(
                {
                    xIndex[lineA.startPointId],
                    yIndex[lineA.startPointId],
                    xIndex[lineA.endPointId],
                    yIndex[lineA.endPointId],
                    xIndex[lineB.startPointId],
                    yIndex[lineB.startPointId],
                    xIndex[lineB.endPointId],
                    yIndex[lineB.endPointId],
                },
                {
                    -ax / lenA,
                    -ay / lenA,
                    ax / lenA,
                    ay / lenA,
                    bx / lenB,
                    by / lenB,
                    -bx / lenB,
                    -by / lenB,
                },
                lenA - lenB,
                &normalMatrix,
                &gradient,
                &maxResidual);
        }

        for (const auto& constraint : equalRadiusConstraints) {
            CurveLikeAccess curveA = requireCurveLike(constraint.curveA);
            CurveLikeAccess curveB = requireCurveLike(constraint.curveB);

            accumulateResidual(
                {
                    radiusIndex[constraint.curveA],
                    radiusIndex[constraint.curveB],
                },
                {
                    1.0,
                    -1.0,
                },
                *curveA.radius - *curveB.radius,
                &normalMatrix,
                &gradient,
                &maxResidual);
        }

        for (const auto& constraint : angleConstraints) {
            const LineSegmentState& lineA = requireSegment(constraint.lineA);
            const LineSegmentState& lineB = requireSegment(constraint.lineB);
            PointState& a0 = requirePoint(lineA.startPointId);
            PointState& a1 = requirePoint(lineA.endPointId);
            PointState& b0 = requirePoint(lineB.startPointId);
            PointState& b1 = requirePoint(lineB.endPointId);

            double ax = a1.x - a0.x;
            double ay = a1.y - a0.y;
            double bx = b1.x - b0.x;
            double by = b1.y - b0.y;

            if (std::sqrt(ax * ax + ay * ay) < 1.0e-12) {
                ax = 1.0;
                ay = 0.0;
            }
            if (std::sqrt(bx * bx + by * by) < 1.0e-12) {
                bx = 1.0;
                by = 0.0;
            }

            const double cross = ax * by - ay * bx;
            const double dot = ax * bx + ay * by;
            const double denominator = std::max(1.0e-12, cross * cross + dot * dot);
            const double residual = normalizeAngle(std::atan2(cross, dot) - constraint.target);

            const std::array<double, 8> crossCoefficients = {
                -by,
                bx,
                by,
                -bx,
                ay,
                -ax,
                -ay,
                ax,
            };
            const std::array<double, 8> dotCoefficients = {
                -bx,
                -by,
                bx,
                by,
                -ax,
                -ay,
                ax,
                ay,
            };

            std::vector<double> angleCoefficients;
            angleCoefficients.reserve(8);
            for (std::size_t index = 0; index < crossCoefficients.size(); ++index) {
                angleCoefficients.push_back((dot * crossCoefficients[index] - cross * dotCoefficients[index]) / denominator);
            }

            accumulateResidual(
                {
                    xIndex[lineA.startPointId],
                    yIndex[lineA.startPointId],
                    xIndex[lineA.endPointId],
                    yIndex[lineA.endPointId],
                    xIndex[lineB.startPointId],
                    yIndex[lineB.startPointId],
                    xIndex[lineB.endPointId],
                    yIndex[lineB.endPointId],
                },
                angleCoefficients,
                residual,
                &normalMatrix,
                &gradient,
                &maxResidual);
        }

        if (variableCount == 0 || maxResidual <= options.residualTolerance) {
            break;
        }

        for (int32_t diagonal = 0; diagonal < variableCount; ++diagonal) {
            normalMatrix[static_cast<std::size_t>(diagonal) * static_cast<std::size_t>(variableCount) + static_cast<std::size_t>(diagonal)] += options.damping;
        }

        std::vector<double> rhs = gradient;
        for (double& value : rhs) {
            value = -value;
        }

        std::vector<double> delta;
        if (!solveLinearSystem(normalMatrix, rhs, &delta)) {
            break;
        }

        double maxStep = 0.0;
        for (const auto& entry : xIndex) {
            PointState& point = requirePoint(entry.first);
            const int32_t pointXIndex = entry.second;
            if (pointXIndex >= 0) {
                const double xDelta = delta[static_cast<std::size_t>(pointXIndex)];
                point.x += xDelta;
                maxStep = std::max(maxStep, std::abs(xDelta));
            }

            const int32_t pointYIndex = yIndex[entry.first];
            if (pointYIndex >= 0) {
                const double yDelta = delta[static_cast<std::size_t>(pointYIndex)];
                point.y += yDelta;
                maxStep = std::max(maxStep, std::abs(yDelta));
            }
        }

        for (const auto& entry : radiusIndex) {
            const double radiusDelta = delta[static_cast<std::size_t>(entry.second)];
            CurveLikeAccess curve = requireCurveLike(entry.first);
            *curve.radius = std::max(1.0e-9, *curve.radius + radiusDelta);
            maxStep = std::max(maxStep, std::abs(radiusDelta));
        }

        if (maxStep <= options.stepTolerance) {
            break;
        }
    }

    for (auto& item : sketch.entities) {
        const auto pointIt = points.find(item.id);
        if (pointIt != points.end()) {
            Value pointSpec = mini_json::requireObject(mini_json::parse(item.json), "stored point entity");
            setObjectNumber(&pointSpec, "x", pointIt->second.x);
            setObjectNumber(&pointSpec, "y", pointIt->second.y);
            item.json = stringifyJsonValue(pointSpec);
            continue;
        }

        const auto circleIt = circles.find(item.id);
        if (circleIt != circles.end()) {
            Value circleSpec = mini_json::requireObject(mini_json::parse(item.json), "stored circle entity");
            setObjectNumber(&circleSpec, "radius", circleIt->second.radius);
            item.json = stringifyJsonValue(circleSpec);
            continue;
        }

        const auto arcIt = arcs.find(item.id);
        if (arcIt == arcs.end()) {
            continue;
        }

        Value arcSpec = mini_json::requireObject(mini_json::parse(item.json), "stored arc entity");
        setObjectNumber(&arcSpec, "radius", arcIt->second.radius);

        if (arcIt->second.startPointId != 0 && arcIt->second.endPointId != 0) {
            PointState& center = requirePoint(arcIt->second.centerPointId);
            PointState& start = requirePoint(arcIt->second.startPointId);
            PointState& end = requirePoint(arcIt->second.endPointId);
            const double startAngle = wrapAngleNear(std::atan2(start.y - center.y, start.x - center.x), arcIt->second.referenceStartRadians);
            const double endAngle = wrapAngleNear(std::atan2(end.y - center.y, end.x - center.x), arcIt->second.referenceStartRadians + arcIt->second.referenceSweepRadians);
            const double sweepAngle = wrapSweepNear(endAngle - startAngle, arcIt->second.referenceSweepRadians);
            setObjectNumber(&arcSpec, "startRadians", startAngle);
            setObjectNumber(&arcSpec, "sweepRadians", sweepAngle);
        }

        item.json = stringifyJsonValue(arcSpec);
    }

    const bool hasUnsupportedContent = !unsupportedEntityKinds.empty() || !unsupportedConstraintKinds.empty();
    const bool converged = !hasUnsupportedContent && maxResidual <= options.residualTolerance;
    const std::string structuralStatus = deriveStructuralStatus(structuralDof, drivingConstraintCount);
    const std::string status = converged ? structuralStatus : "failed";

    std::ostringstream output;
    output << '{';
    output << "\"converged\":" << (converged ? "true" : "false") << ',';
    output << "\"status\":\"" << status << "\",";
    output << "\"algorithm\":\"" << options.algorithm << "\",";
    output << "\"iterations\":" << iterations << ',';
    output << "\"maxScaledResidual\":" << std::setprecision(17) << maxResidual << ',';
    output << "\"diagnostics\":" << buildDiagnosticsJson(
        structuralStatus,
        structuralDof,
        drivingConstraintCount,
        drivenConstraintCount,
        unsupportedEntityKinds,
        unsupportedConstraintKinds,
        options.algorithmFallback,
        converged,
        maxResidual);
    output << '}';
    return output.str();
}

std::string SketchToolkitNative::getSketchSnapshot(int32_t sketchId) const
{
    const auto& sketch = requireSketch(sketchId);

    std::ostringstream builder;
    builder << '{'
            << "\"sessionId\":\"" << escapeJsonString(m_sessionId) << "\","
            << "\"id\":" << sketch.id << ','
            << "\"createParams\":" << sketch.createParamsJson << ','
            << "\"entities\":" << buildStoredItemsJson(sketch.entities) << ','
            << "\"constraints\":" << buildStoredItemsJson(sketch.constraints) << ','
            << "\"parameters\":" << buildParameterMapJson(sketch.parameters)
            << '}';
    return builder.str();
}

std::string SketchToolkitNative::getLastError() const
{
    return m_lastError;
}

void SketchToolkitNative::clearLastError()
{
    m_lastError.clear();
}

SketchToolkitNative::SketchRecord& SketchToolkitNative::requireSketch(int32_t sketchId)
{
    const auto iterator = m_sketches.find(sketchId);
    if (iterator == m_sketches.end()) {
        throwError("INVALID_HANDLE", "Sketch handle does not exist in this sketch toolkit session");
    }
    return iterator->second;
}

const SketchToolkitNative::SketchRecord& SketchToolkitNative::requireSketch(int32_t sketchId) const
{
    const auto iterator = m_sketches.find(sketchId);
    if (iterator == m_sketches.end()) {
        throwError("INVALID_HANDLE", "Sketch handle does not exist in this sketch toolkit session");
    }
    return iterator->second;
}

[[noreturn]] void SketchToolkitNative::throwError(const std::string& code, const std::string& detail) const
{
    m_lastError = std::string("{\"code\":\"") + escapeJsonString(code) + "\",\"detail\":\"" + escapeJsonString(detail) + "\"}";
    throw std::runtime_error(m_lastError);
}

std::string SketchToolkitNative::buildStoredItemsJson(const std::vector<StoredItem>& items)
{
    std::ostringstream builder;
    builder << '[';

    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        if (index > 0) {
            builder << ',';
        }
        builder << "{\"id\":" << item.id << ",\"spec\":" << item.json << '}';
    }

    builder << ']';
    return builder.str();
}

std::string SketchToolkitNative::buildParameterMapJson(const std::unordered_map<std::string, std::string>& parameters)
{
    std::ostringstream builder;
    builder << '{';

    bool first = true;
    for (const auto& entry : parameters) {
        if (!first) {
            builder << ',';
        }
        first = false;
        builder << '"' << escapeJsonString(entry.first) << "\":" << entry.second;
    }

    builder << '}';
    return builder.str();
}

std::string SketchToolkitNative::escapeJsonString(const std::string& value)
{
    std::ostringstream escaped;
    for (const char ch : value) {
        switch (ch) {
        case '\\': escaped << "\\\\"; break;
        case '"': escaped << "\\\""; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default: escaped << ch; break;
        }
    }
    return escaped.str();
}

} // namespace occt_kernel
