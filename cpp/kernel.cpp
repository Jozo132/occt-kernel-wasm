/**
 * @file kernel.cpp
 * @brief Implementation of OcctKernel.
 */

#include "kernel.h"
#include "json_utils.h"
#include "profile_builder.h"

// OCCT foundation
#include <Standard_Version.hxx>
#include <Standard_ErrorHandler.hxx>

// Topology
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS.hxx>
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>

// Primitives
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>

// Prism / Revolution
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepFeat_MakePrism.hxx>
#include <BRepFeat_MakeDPrism.hxx>
#include <BRepFeat_MakeRevol.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

// Boolean operations
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>

// Fillets / chamfers
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <Law_Constant.hxx>
#include <Law_Linear.hxx>
#include <NCollection_Array1.hxx>
#include <TopExp_Explorer.hxx>

// Tessellation
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Array1OfTriangle.hxx>
#include <TShort_Array1OfShortReal.hxx>
#include <gp_Vec.hxx>
#include <gp_Pnt2d.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom_Surface.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BezierCurve.hxx>

// STEP import/export
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <XSControl_WorkSession.hxx>
#include <XSControl_TransferReader.hxx>
#include <Interface_Check.hxx>
#include <Interface_CheckIterator.hxx>
#include <ShapeFix_Shape.hxx>

// Topology iteration
#include <NCollection_IndexedMap.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_List.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopExp.hxx>
#include <TopAbs_ShapeEnum.hxx>

// String / stream helpers
#include <OSD_File.hxx>
#include <OSD_Path.hxx>
#include <OSD_Protection.hxx>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <set>
#include <cstdio>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

namespace occt_kernel {

using ShapeMap = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;
using ShapeList = NCollection_List<TopoDS_Shape>;
using ShapeToShapeListMap =
    NCollection_IndexedDataMap<TopoDS_Shape, ShapeList, TopTools_ShapeMapHasher>;
using Point2dArray = NCollection_Array1<gp_Pnt2d>;

// ---------------------------------------------------------------------------
// Internal error helpers
// ---------------------------------------------------------------------------

static std::string makeErrorJson(const std::string& code, const std::string& detail) {
    // Simple JSON serialisation – avoids pulling in a full JSON library.
    auto escape = [](const std::string& s) -> std::string {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '"')       out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else                out += c;
        }
        return out;
    };
    return "{\"code\":\"" + escape(code) + "\",\"detail\":\"" + escape(detail) + "\"}";
}

static void throwKernelError(const std::string& code, const std::string& detail) {
    throw std::runtime_error(makeErrorJson(code, detail));
}

namespace {

struct PlaneFrame {
    bool hasValue = false;
    gp_Pnt origin = gp_Pnt(0.0, 0.0, 0.0);
    gp_Dir normal = gp_Dir(0.0, 0.0, 1.0);
    gp_Dir xDirection = gp_Dir(1.0, 0.0, 0.0);
};

struct ExtrudeOptions {
    bool hasHeight = false;
    double height = 0.0;
    bool hasVector = false;
    gp_Vec vector = gp_Vec(0.0, 0.0, 0.0);
    PlaneFrame plane;
};

enum class StructuredExtrudeExtentMode {
    Blind,
    UpToNext,
    ThroughAll,
    UpToSurface,
    OffsetFromSurface,
};

struct StructuredExtrudeSurfaceTarget {
    bool hasShapeId = false;
    uint32_t shapeId = 0;
    mini_json::Value faceRef = mini_json::Value::makeObject();
};

struct StructuredExtrudeSpec {
    PlaneFrame plane;
    gp_Dir direction = gp_Dir(0.0, 0.0, 1.0);
    bool hasDraftAngle = false;
    double draftAngleRadians = 0.0;
    StructuredExtrudeExtentMode extentMode = StructuredExtrudeExtentMode::Blind;
    double distance = 0.0;
    double offset = 0.0;
    bool hasSurfaceTarget = false;
    StructuredExtrudeSurfaceTarget surfaceTarget;
};

enum class StructuredRevolveExtentMode {
    Angle,
    UpToSurface,
    FromSurfaceToSurface,
    ThroughAll,
    UpToSurfaceAtAngle,
};

struct StructuredRevolveSlidingEdge {
    int profileEdgeIndex = 0;
    mini_json::Value faceRef = mini_json::Value::makeObject();
};

struct StructuredRevolveSpec {
    PlaneFrame plane;
    gp_Pnt axisOrigin = gp_Pnt(0.0, 0.0, 0.0);
    gp_Dir axisDirection = gp_Dir(0.0, 1.0, 0.0);
    StructuredRevolveExtentMode extentMode = StructuredRevolveExtentMode::Angle;
    double angleRadians = 0.0;
    bool hasSurfaceTarget = false;
    StructuredExtrudeSurfaceTarget surfaceTarget;
    bool hasFromSurface = false;
    StructuredExtrudeSurfaceTarget fromSurfaceTarget;
    bool hasUntilSurface = false;
    StructuredExtrudeSurfaceTarget untilSurfaceTarget;
    std::vector<StructuredRevolveSlidingEdge> slidingEdges;
};

struct RevolveOptions {
    double angleDegrees = 0.0;
    gp_Pnt axisOrigin = gp_Pnt(0.0, 0.0, 0.0);
    gp_Dir axisDirection = gp_Dir(0.0, 1.0, 0.0);
};

struct RotationOptions {
    bool hasValue = false;
    gp_Pnt axisOrigin = gp_Pnt(0.0, 0.0, 0.0);
    gp_Dir axisDirection = gp_Dir(0.0, 1.0, 0.0);
    double angleDegrees = 0.0;
};

struct ShapeTransformOptions {
    bool hasTranslation = false;
    gp_Vec translation = gp_Vec(0.0, 0.0, 0.0);
    RotationOptions rotation;
};

double requireFiniteNumber(double value, const std::string& name)
{
    if (!std::isfinite(value)) {
        throw std::runtime_error(name + " must be finite");
    }
    return value;
}

gp_Pnt parsePoint3(const mini_json::Value& value, const std::string& context)
{
    const std::array<double, 3> point = mini_json::requirePoint3(value, context);
    return gp_Pnt(
        requireFiniteNumber(point[0], context + "[0]"),
        requireFiniteNumber(point[1], context + "[1]"),
        requireFiniteNumber(point[2], context + "[2]")
    );
}

gp_Vec parseVector3(const mini_json::Value& value, const std::string& context, bool allowZero = false)
{
    const std::array<double, 3> vector = mini_json::requirePoint3(value, context);
    const gp_Vec result(
        requireFiniteNumber(vector[0], context + "[0]"),
        requireFiniteNumber(vector[1], context + "[1]"),
        requireFiniteNumber(vector[2], context + "[2]")
    );
    if (!allowZero && result.SquareMagnitude() <= 0.0) {
        throw std::runtime_error(context + " must not be the zero vector");
    }
    return result;
}

gp_Dir parseDirection3(const mini_json::Value& value, const std::string& context)
{
    const gp_Vec vector = parseVector3(value, context, false);
    return gp_Dir(vector.X(), vector.Y(), vector.Z());
}

PlaneFrame parsePlaneFrame(const mini_json::Value& root)
{
    PlaneFrame frame;
    frame.hasValue = true;
    frame.origin = parsePoint3(mini_json::requireMember(root, "origin", "plane"), "plane.origin");
    frame.normal = parseDirection3(mini_json::requireMember(root, "normal", "plane"), "plane.normal");
    frame.xDirection = parseDirection3(mini_json::requireMember(root, "xDirection", "plane"), "plane.xDirection");

    if (std::abs(frame.normal.Dot(frame.xDirection)) > 1.0 - 1.0e-9) {
        throw std::runtime_error("plane.xDirection must not be parallel to plane.normal");
    }

    return frame;
}

ExtrudeOptions parseExtrudeOptions(const std::string& optionsJson)
{
    const mini_json::Value root = mini_json::requireObject(mini_json::parse(optionsJson), "extrude options");
    ExtrudeOptions options;

    if (const mini_json::Value* heightValue = root.get("height")) {
        options.hasHeight = true;
        options.height = mini_json::requireNumber(*heightValue, "extrude.height");
        if (!std::isfinite(options.height) || options.height <= 0.0) {
            throw std::runtime_error("extrude.height must be > 0");
        }
    }

    if (const mini_json::Value* vectorValue = root.get("vector")) {
        options.hasVector = true;
        options.vector = parseVector3(*vectorValue, "extrude.vector");
    }

    if (options.hasHeight == options.hasVector) {
        throw std::runtime_error("Extrude options must specify exactly one of 'height' or 'vector'");
    }

    if (const mini_json::Value* planeValue = root.get("plane")) {
        options.plane = parsePlaneFrame(mini_json::requireObject(*planeValue, "extrude.plane"));
    }

    return options;
}

RevolveOptions parseRevolveOptions(const std::string& optionsJson)
{
    const mini_json::Value root = mini_json::requireObject(mini_json::parse(optionsJson), "revolve options");
    RevolveOptions options;
    options.angleDegrees = mini_json::requireNumber(mini_json::requireMember(root, "angleDegrees", "revolve"), "revolve.angleDegrees");
    if (!std::isfinite(options.angleDegrees) || options.angleDegrees <= 0.0 || options.angleDegrees > 360.0) {
        throw std::runtime_error("revolve.angleDegrees must be in (0, 360]");
    }

    if (const mini_json::Value* axisOriginValue = root.get("axisOrigin")) {
        options.axisOrigin = parsePoint3(*axisOriginValue, "revolve.axisOrigin");
    }
    if (const mini_json::Value* axisDirectionValue = root.get("axisDirection")) {
        options.axisDirection = parseDirection3(*axisDirectionValue, "revolve.axisDirection");
    }

    return options;
}

ShapeTransformOptions parseShapeTransformOptions(const std::string& transformJson)
{
    const mini_json::Value root = mini_json::requireObject(mini_json::parse(transformJson), "shape transform");
    ShapeTransformOptions options;

    if (const mini_json::Value* translationValue = root.get("translation")) {
        options.hasTranslation = true;
        options.translation = parseVector3(*translationValue, "transform.translation", true);
    }

    if (const mini_json::Value* rotationValue = root.get("rotation")) {
        const mini_json::Value& rotation = mini_json::requireObject(*rotationValue, "transform.rotation");
        options.rotation.hasValue = true;
        options.rotation.axisOrigin = parsePoint3(mini_json::requireMember(rotation, "axisOrigin", "transform.rotation"), "transform.rotation.axisOrigin");
        options.rotation.axisDirection = parseDirection3(mini_json::requireMember(rotation, "axisDirection", "transform.rotation"), "transform.rotation.axisDirection");
        options.rotation.angleDegrees = mini_json::requireNumber(mini_json::requireMember(rotation, "angleDegrees", "transform.rotation"), "transform.rotation.angleDegrees");
        if (!std::isfinite(options.rotation.angleDegrees)) {
            throw std::runtime_error("transform.rotation.angleDegrees must be finite");
        }
    }

    if (!options.hasTranslation && !options.rotation.hasValue) {
        throw std::runtime_error("Shape transform must specify translation and/or rotation");
    }

    return options;
}

TopoDS_Shape applyShapeTransform(const TopoDS_Shape& sourceShape, const gp_Trsf& transform, const std::string& context)
{
    BRepBuilderAPI_Transform transformer(sourceShape, transform, true);
    transformer.Build();
    if (!transformer.IsDone() || transformer.Shape().IsNull()) {
        throw std::runtime_error("Failed to apply " + context + " transform");
    }
    return transformer.Shape();
}

TopoDS_Face placeProfileFace(const TopoDS_Face& sourceFace, const PlaneFrame& plane)
{
    if (!plane.hasValue) {
        return sourceFace;
    }

    gp_Trsf transform;
    transform.SetDisplacement(
        gp_Ax3(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0), gp_Dir(1.0, 0.0, 0.0)),
        gp_Ax3(plane.origin, plane.normal, plane.xDirection)
    );

    return TopoDS::Face(applyShapeTransform(sourceFace, transform, "profile placement"));
}

gp_Vec makeExtrusionVector(const ExtrudeOptions& options)
{
    if (options.hasVector) {
        return options.vector;
    }
    if (options.plane.hasValue) {
        return gp_Vec(options.plane.normal.X(), options.plane.normal.Y(), options.plane.normal.Z()) * options.height;
    }
    return gp_Vec(0.0, 0.0, options.height);
}

struct StepImportOptions {
    bool heal = false;
    bool sew = false;
    bool fixSameParameter = false;
    bool fixSolid = false;
    double sewingTolerance = 1.0e-6;
};

struct StepImportMessage {
    std::string phase;
    std::string severity;
    std::string text;
    int entityNumber = 0;
};

struct StepImportRunResult {
    std::string readStatus = "IFSelect_RetVoid";
    std::string transferStatus = "NOT_RUN";
    int rootCount = 0;
    int transferredRootCount = 0;
    bool isValid = false;
    bool wasValidBeforeHealing = false;
    bool healed = false;
    bool hasShape = false;
    TopoDS_Shape shape;
    std::vector<StepImportMessage> messages;
};

struct DeletedEntityRecord {
    std::string kind;
    std::string stableHash;
    std::string deletedBy;
    std::string status = "unresolved";
};

struct RevisionMetadata {
    std::string revisionId;
    std::string operationId;
    std::string sourceFeatureId;
    std::string operationType = "unknown";
    std::vector<std::string> operandRevisionIds;
    std::string parameterHash;
    std::string topologyHash;
    int historySchemaVersion = 1;
    bool createdFromCheckpoint = false;
    std::string entityStatus = "unresolved";
    std::string identityStatus = "unresolved";
    std::vector<std::string> historyWarnings;
    std::vector<DeletedEntityRecord> deletedEntities;
    std::vector<std::string> faceStableHashes;
    std::vector<std::string> edgeStableHashes;
    std::vector<std::string> vertexStableHashes;
};

struct ShapeRecord {
    TopoDS_Shape shape;
    RevisionMetadata revision;
    int refCount = 1;
};

std::string escapeJson(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += ch; break;
        }
    }
    return out;
}

void appendJsonString(std::ostringstream& out, const std::string& value) {
    out << '"' << escapeJson(value) << '"';
}

long long quantizeForIdentity(double value) {
    if (!std::isfinite(value) || std::abs(value) < 1.0e-10) {
        return 0;
    }
    return static_cast<long long>(std::llround(value * 1000000000.0));
}

std::string coordinateKey(double value) {
    return std::to_string(quantizeForIdentity(value));
}

std::string pointKey(const gp_Pnt& point) {
    return coordinateKey(point.X()) + ":" + coordinateKey(point.Y()) + ":" + coordinateKey(point.Z());
}

std::string fnv1a64(const std::string& input) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char ch : input) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }

    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

std::array<double, 6> boundsOfShape(const TopoDS_Shape& shape) {
    Bnd_Box bbox;
    BRepBndLib::Add(shape, bbox);
    if (bbox.IsVoid()) {
        return {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    }

    double xMin, yMin, zMin, xMax, yMax, zMax;
    bbox.Get(xMin, yMin, zMin, xMax, yMax, zMax);
    return {xMin, yMin, zMin, xMax, yMax, zMax};
}

std::string boundsSignature(const TopoDS_Shape& shape) {
    const auto bounds = boundsOfShape(shape);
    std::ostringstream out;
    for (double value : bounds) {
        out << coordinateKey(value) << '|';
    }
    return out.str();
}

std::string surfaceTypeName(GeomAbs_SurfaceType type) {
    switch (type) {
    case GeomAbs_Plane: return "plane";
    case GeomAbs_Cylinder: return "cylinder";
    case GeomAbs_Cone: return "cone";
    case GeomAbs_Sphere: return "sphere";
    case GeomAbs_Torus: return "torus";
    case GeomAbs_BezierSurface: return "bezierSurface";
    case GeomAbs_BSplineSurface: return "bsplineSurface";
    case GeomAbs_SurfaceOfRevolution: return "surfaceOfRevolution";
    case GeomAbs_SurfaceOfExtrusion: return "surfaceOfExtrusion";
    case GeomAbs_OffsetSurface: return "offsetSurface";
    case GeomAbs_OtherSurface: return "otherSurface";
    }
    return "unknownSurface";
}

std::string curveTypeName(GeomAbs_CurveType type) {
    switch (type) {
    case GeomAbs_Line: return "line";
    case GeomAbs_Circle: return "circle";
    case GeomAbs_Ellipse: return "ellipse";
    case GeomAbs_Hyperbola: return "hyperbola";
    case GeomAbs_Parabola: return "parabola";
    case GeomAbs_BezierCurve: return "bezierCurve";
    case GeomAbs_BSplineCurve: return "bsplineCurve";
    case GeomAbs_OffsetCurve: return "offsetCurve";
    case GeomAbs_OtherCurve: return "otherCurve";
    }
    return "unknownCurve";
}

std::string makeFaceStableHash(const TopoDS_Face& face) {
    std::ostringstream signature;
    signature << "face|" << boundsSignature(face);

    try {
        BRepAdaptor_Surface surface(face, false);
        signature << surfaceTypeName(surface.GetType()) << '|';
        if (surface.GetType() == GeomAbs_Plane) {
            gp_Dir normal = surface.Plane().Axis().Direction();
            if (face.Orientation() == TopAbs_REVERSED) {
                normal.Reverse();
            }
            signature << coordinateKey(normal.X()) << ':'
                      << coordinateKey(normal.Y()) << ':'
                      << coordinateKey(normal.Z()) << '|';
        }
    } catch (...) {
        signature << "surfaceUnavailable|";
    }

    return "F:" + fnv1a64(signature.str());
}

std::string makeVertexStableHash(const TopoDS_Vertex& vertex) {
    const gp_Pnt point = BRep_Tool::Pnt(vertex);
    return "V:" + fnv1a64("vertex|" + pointKey(point));
}

std::vector<TopoDS_Face> uniqueAdjacentFaces(const TopoDS_Edge& edge,
                                             const ShapeToShapeListMap& edgeToFaces) {
    std::vector<TopoDS_Face> faces;
    if (!edgeToFaces.Contains(edge)) {
        return faces;
    }

    const ShapeList& adjacent = edgeToFaces.FindFromKey(edge);
    for (ShapeList::Iterator it(adjacent); it.More(); it.Next()) {
        const TopoDS_Face face = TopoDS::Face(it.Value());
        const bool exists = std::any_of(faces.begin(), faces.end(), [&](const TopoDS_Face& current) {
            return current.IsSame(face);
        });
        if (!exists) {
            faces.push_back(face);
        }
    }
    return faces;
}

std::vector<int> faceIdsForEdge(const TopoDS_Edge& edge,
                                const ShapeMap& faceMap,
                                const ShapeToShapeListMap& edgeToFaces) {
    std::vector<int> ids;
    for (const TopoDS_Face& face : uniqueAdjacentFaces(edge, edgeToFaces)) {
        const int id = faceMap.FindIndex(face);
        if (id > 0) {
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::string makeEdgeStableHash(const TopoDS_Edge& edge,
                               const std::vector<std::string>& adjacentFaceHashes) {
    std::ostringstream signature;
    signature << "edge|" << boundsSignature(edge);
    try {
        BRepAdaptor_Curve curve(edge);
        signature << curveTypeName(curve.GetType()) << '|';
        const double first = curve.FirstParameter();
        const double last = curve.LastParameter();
        if (std::isfinite(first) && std::isfinite(last)) {
            signature << pointKey(curve.Value(first)) << '|'
                      << pointKey(curve.Value(last)) << '|';
        }
    } catch (...) {
        signature << "curveUnavailable|";
    }

    std::vector<std::string> sortedFaceHashes = adjacentFaceHashes;
    std::sort(sortedFaceHashes.begin(), sortedFaceHashes.end());
    for (const std::string& hash : sortedFaceHashes) {
        signature << hash << '|';
    }
    return "E:" + fnv1a64(signature.str());
}

bool computeFaceNormalAtEdge(const TopoDS_Face& face, const TopoDS_Edge& edge, gp_Vec& normalOut) {
    try {
        double first = 0.0;
        double last = 0.0;
        Handle(Geom2d_Curve) curveOnSurface = BRep_Tool::CurveOnSurface(edge, face, first, last);
        TopLoc_Location surfaceLocation;
        Handle(Geom_Surface) surface = BRep_Tool::Surface(face, surfaceLocation);
        if (curveOnSurface.IsNull() || surface.IsNull() || !std::isfinite(first) || !std::isfinite(last)) {
            return false;
        }

        const double parameter = 0.5 * (first + last);
        const gp_Pnt2d uv = curveOnSurface->Value(parameter);
        gp_Pnt surfacePoint;
        gp_Vec dU;
        gp_Vec dV;
        surface->D1(uv.X(), uv.Y(), surfacePoint, dU, dV);

        gp_Vec normal = dU.Crossed(dV);
        if (normal.SquareMagnitude() <= 1.0e-18) {
            return false;
        }

        normal.Transform(surfaceLocation.Transformation());
        if (face.Orientation() == TopAbs_REVERSED) {
            normal.Reverse();
        }
        normal.Normalize();
        normalOut = normal;
        return true;
    } catch (...) {
        return false;
    }
}

bool isTopoFaceSeam(const TopoDS_Edge& edge, const std::vector<TopoDS_Face>& adjacentFaces) {
    for (const TopoDS_Face& face : adjacentFaces) {
        try {
            if (BRep_Tool::IsClosed(edge, face)) {
                return true;
            }
        } catch (...) {
        }
    }
    return false;
}

bool isHardEdge(const TopoDS_Edge& edge, const std::vector<TopoDS_Face>& adjacentFaces) {
    if (adjacentFaces.size() > 2) {
        return true;
    }
    if (adjacentFaces.size() != 2) {
        return false;
    }

    gp_Vec n1;
    gp_Vec n2;
    if (computeFaceNormalAtEdge(adjacentFaces[0], edge, n1) &&
        computeFaceNormalAtEdge(adjacentFaces[1], edge, n2)) {
        const double dot = std::max(-1.0, std::min(1.0, n1.Dot(n2)));
        return dot < std::cos(1.0 * M_PI / 180.0);
    }

    try {
        BRepAdaptor_Surface s1(adjacentFaces[0], false);
        BRepAdaptor_Surface s2(adjacentFaces[1], false);
        return s1.GetType() != s2.GetType();
    } catch (...) {
        return true;
    }
}

void appendPointArrayJson(std::ostringstream& out, const std::vector<gp_Pnt>& points) {
    out << '[';
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i > 0) out << ',';
        out << '[' << points[i].X() << ',' << points[i].Y() << ',' << points[i].Z() << ']';
    }
    out << ']';
}

void appendPointJson(std::ostringstream& out, const gp_Pnt& point) {
    out << '[' << point.X() << ',' << point.Y() << ',' << point.Z() << ']';
}

void appendVectorJson(std::ostringstream& out, const gp_Vec& vector) {
    out << '[' << vector.X() << ',' << vector.Y() << ',' << vector.Z() << ']';
}

bool samePointWithinTolerance(const gp_Pnt& a, const gp_Pnt& b) {
    return a.SquareDistance(b) <= 1.0e-18;
}

void appendDedupedPoint(std::vector<gp_Pnt>& points, const gp_Pnt& point) {
    if (points.empty() || !samePointWithinTolerance(points.back(), point)) {
        points.push_back(point);
    }
}

std::vector<gp_Pnt> collectEdgePolyline(const TopoDS_Edge& edge) {
    std::vector<gp_Pnt> points;

    TopLoc_Location loc;
    Handle(Poly_PolygonOnTriangulation) poly;
    Handle(Poly_Triangulation) tri;
    BRep_Tool::PolygonOnTriangulation(edge, poly, tri, loc);
    if (!poly.IsNull() && !tri.IsNull()) {
        gp_Trsf trsf;
        if (!loc.IsIdentity()) {
            trsf = loc.Transformation();
        }
        for (int n = 1; n <= poly->NbNodes(); ++n) {
            appendDedupedPoint(points, tri->Node(poly->Nodes()(n)).Transformed(trsf));
        }
    }

    if (points.size() >= 2) {
        return points;
    }

    points.clear();
    try {
        BRepAdaptor_Curve curve(edge);
        const double first = curve.FirstParameter();
        const double last = curve.LastParameter();
        if (!std::isfinite(first) || !std::isfinite(last)) {
            return points;
        }

        const int samples = curve.GetType() == GeomAbs_Line ? 2 : 24;
        for (int i = 0; i < samples; ++i) {
            const double t = samples == 1 ? first : first + (last - first) * static_cast<double>(i) / static_cast<double>(samples - 1);
            appendDedupedPoint(points, curve.Value(t));
        }
    } catch (...) {
        points.clear();
    }

    return points;
}

std::string chainDedupeKey(const std::vector<gp_Pnt>& points) {
    std::ostringstream forward;
    std::ostringstream reverse;
    for (const gp_Pnt& point : points) {
        forward << pointKey(point) << '|';
    }
    for (auto it = points.rbegin(); it != points.rend(); ++it) {
        reverse << pointKey(*it) << '|';
    }

    const std::string forwardKey = forward.str();
    const std::string reverseKey = reverse.str();
    return std::min(forwardKey, reverseKey);
}

void appendIntArrayJson(std::ostringstream& out, const std::vector<int>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out << ',';
        out << values[i];
    }
    out << ']';
}

void appendStringArrayJson(std::ostringstream& out, const std::vector<std::string>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out << ',';
        appendJsonString(out, values[i]);
    }
    out << ']';
}

std::string optionalStringMember(const mini_json::Value& object, const std::string& key, const std::string& fallback = "") {
    const mini_json::Value* value = object.get(key);
    if (value == nullptr || value->kind == mini_json::Value::Kind::Null) {
        return fallback;
    }
    return mini_json::requireString(*value, key);
}

bool optionalBoolMember(const mini_json::Value& object, const std::string& key, bool fallback = false) {
    const mini_json::Value* value = object.get(key);
    if (value == nullptr || value->kind == mini_json::Value::Kind::Null) {
        return fallback;
    }
    return mini_json::requireBool(*value, key);
}

int optionalIntMember(const mini_json::Value& object, const std::string& key, int fallback = 0) {
    const mini_json::Value* value = object.get(key);
    if (value == nullptr || value->kind == mini_json::Value::Kind::Null) {
        return fallback;
    }
    return static_cast<int>(mini_json::requireNumber(*value, key));
}

double optionalNumberMember(const mini_json::Value& object, const std::string& key, double fallback = 0.0) {
    const mini_json::Value* value = object.get(key);
    if (value == nullptr || value->kind == mini_json::Value::Kind::Null) {
        return fallback;
    }
    return mini_json::requireNumber(*value, key);
}

bool hasMember(const mini_json::Value& object, const std::string& key) {
    return object.kind == mini_json::Value::Kind::Object && object.get(key) != nullptr;
}

void throwStructuredValidation(const std::string& operation,
                               const std::string& path,
                               const std::string& reason,
                               const std::string& unsupportedFeature = "") {
    std::ostringstream detail;
    detail << "{";
    detail << "\"phase\":\"validation\",";
    detail << "\"operation\":"; appendJsonString(detail, operation); detail << ",";
    detail << "\"path\":"; appendJsonString(detail, path); detail << ",";
    detail << "\"reason\":"; appendJsonString(detail, reason);
    if (!unsupportedFeature.empty()) {
        detail << ",\"unsupportedFeature\":"; appendJsonString(detail, unsupportedFeature);
    }
    detail << "}";
    throwKernelError("INVALID_PARAMS", detail.str());
}

void throwStructuredOperationError(const std::string& operation,
                                   const std::string& reason,
                                   const std::vector<std::string>& edgeRefs = {}) {
    std::ostringstream detail;
    detail << "{";
    detail << "\"phase\":\"execution\",";
    detail << "\"operation\":"; appendJsonString(detail, operation); detail << ",";
    detail << "\"reason\":"; appendJsonString(detail, reason); detail << ",";
    detail << "\"failingEdgeRefs\":"; appendStringArrayJson(detail, edgeRefs);
    detail << "}";
    throwKernelError("OPERATION_FAILED", detail.str());
}

void rejectUnknownFields(const mini_json::Value& object,
                         const std::set<std::string>& allowedKeys,
                         const std::string& operation,
                         const std::string& context,
                         bool allowUnknownFields) {
    if (allowUnknownFields || object.kind != mini_json::Value::Kind::Object) {
        return;
    }
    for (const auto& entry : object.object) {
        if (allowedKeys.count(entry.first) == 0) {
            throwStructuredValidation(operation,
                                      context + "." + entry.first,
                                      "Unknown field is not allowed for this schema version",
                                      "unknownField");
        }
    }
}

int requireSchemaVersion(const mini_json::Value& root, const std::string& operation) {
    if (!hasMember(root, "schemaVersion")) {
        throwStructuredValidation(operation, "schemaVersion", "schemaVersion is required");
    }
    const int version = optionalIntMember(root, "schemaVersion", 0);
    if (version != 1) {
        throwStructuredValidation(operation, "schemaVersion", "Only schemaVersion 1 is supported", "schemaVersion");
    }
    return version;
}

double requirePositiveMember(const mini_json::Value& object,
                             const std::string& key,
                             const std::string& operation,
                             const std::string& path) {
    if (!hasMember(object, key)) {
        throwStructuredValidation(operation, path, "Required positive number is missing");
    }
    const double value = optionalNumberMember(object, key, 0.0);
    if (!std::isfinite(value) || value <= 0.0) {
        throwStructuredValidation(operation, path, "Value must be a finite number > 0");
    }
    return value;
}

double optionalAngleRadians(const mini_json::Value& object,
                            const std::string& operation,
                            const std::string& path) {
    if (hasMember(object, "angleRadians")) {
        const double angle = optionalNumberMember(object, "angleRadians", 0.0);
        if (!std::isfinite(angle) || angle <= 0.0 || angle >= M_PI) {
            throwStructuredValidation(operation, path + ".angleRadians", "angleRadians must be finite and in (0, pi)");
        }
        return angle;
    }
    if (hasMember(object, "angleDegrees")) {
        const double angle = optionalNumberMember(object, "angleDegrees", 0.0);
        if (!std::isfinite(angle) || angle <= 0.0 || angle >= 180.0) {
            throwStructuredValidation(operation, path + ".angleDegrees", "angleDegrees must be finite and in (0, 180)");
        }
        return angle * M_PI / 180.0;
    }
    throwStructuredValidation(operation, path, "Distance-angle chamfer requires angleRadians or angleDegrees");
    return 0.0;
}

int requirePositiveIntMember(const mini_json::Value& object,
                             const std::string& key,
                             const std::string& operation,
                             const std::string& path) {
    if (!hasMember(object, key)) {
        throwStructuredValidation(operation, path, "Required positive integer is missing");
    }
    const double value = optionalNumberMember(object, key, 0.0);
    if (!std::isfinite(value) || std::floor(value) != value || value <= 0.0) {
        throwStructuredValidation(operation, path, "Value must be a positive integer");
    }
    return static_cast<int>(value);
}

double optionalSignedAngleRadians(const mini_json::Value& object,
                                  const std::string& operation,
                                  const std::string& path,
                                  bool& hasValue) {
    const bool hasRadians = hasMember(object, "draftAngleRadians");
    const bool hasDegrees = hasMember(object, "draftAngleDegrees");
    if (hasRadians && hasDegrees) {
        throwStructuredValidation(operation,
                                  path,
                                  "Specify only one of draftAngleRadians or draftAngleDegrees");
    }
    if (hasRadians) {
        const double angle = optionalNumberMember(object, "draftAngleRadians", 0.0);
        if (!std::isfinite(angle) || std::abs(angle) >= (M_PI / 2.0)) {
            throwStructuredValidation(operation,
                                      path + ".draftAngleRadians",
                                      "draftAngleRadians must be finite and in (-pi/2, pi/2)");
        }
        hasValue = true;
        return angle;
    }
    if (hasDegrees) {
        const double angle = optionalNumberMember(object, "draftAngleDegrees", 0.0);
        if (!std::isfinite(angle) || std::abs(angle) >= 90.0) {
            throwStructuredValidation(operation,
                                      path + ".draftAngleDegrees",
                                      "draftAngleDegrees must be finite and in (-90, 90)");
        }
        hasValue = true;
        return angle * M_PI / 180.0;
    }
    hasValue = false;
    return 0.0;
}

double requireSignedExtentAngleRadians(const mini_json::Value& object,
                                       const std::string& operation,
                                       const std::string& path) {
    const bool hasRadians = hasMember(object, "angleRadians");
    const bool hasDegrees = hasMember(object, "angleDegrees");
    if (hasRadians && hasDegrees) {
        throwStructuredValidation(operation,
                                  path,
                                  "Specify only one of angleRadians or angleDegrees");
    }
    if (!hasRadians && !hasDegrees) {
        throwStructuredValidation(operation,
                                  path,
                                  "Specify angleRadians or angleDegrees");
    }
    if (hasRadians) {
        const double angle = optionalNumberMember(object, "angleRadians", 0.0);
        if (!std::isfinite(angle) || angle == 0.0 || std::abs(angle) > (2.0 * M_PI)) {
            throwStructuredValidation(operation,
                                      path + ".angleRadians",
                                      "angleRadians must be finite, non-zero, and in [-2pi, 2pi]");
        }
        return angle;
    }

    const double angle = optionalNumberMember(object, "angleDegrees", 0.0);
    if (!std::isfinite(angle) || angle == 0.0 || std::abs(angle) > 360.0) {
        throwStructuredValidation(operation,
                                  path + ".angleDegrees",
                                  "angleDegrees must be finite, non-zero, and in [-360, 360]");
    }
    return angle * M_PI / 180.0;
}

StructuredExtrudeSurfaceTarget parseStructuredExtrudeSurfaceTarget(const mini_json::Value& value,
                                                                   bool allowUnknownFields,
                                                                   const std::string& operation,
                                                                   const std::string& path) {
    const mini_json::Value& surface = mini_json::requireObject(value, path);
    rejectUnknownFields(surface, { "shapeId", "face" }, operation, path, allowUnknownFields);

    StructuredExtrudeSurfaceTarget target;
    if (hasMember(surface, "shapeId")) {
        target.hasShapeId = true;
        target.shapeId = static_cast<uint32_t>(requirePositiveIntMember(surface,
                                                                        "shapeId",
                                                                        operation,
                                                                        path + ".shapeId"));
    }
    target.faceRef = mini_json::requireObject(mini_json::requireMember(surface, "face", path), path + ".face");
    return target;
}

StructuredExtrudeSpec parseStructuredExtrudeSpec(const std::string& specJson,
                                                 const std::string& operation,
                                                 const std::string& pathRoot) {
    const mini_json::Value root = mini_json::requireObject(mini_json::parse(specJson), pathRoot + " spec");
    requireSchemaVersion(root, operation);
    const bool allowUnknownFields = optionalBoolMember(root, "allowUnknownFields", false);
    rejectUnknownFields(root,
                        { "schemaVersion", "allowUnknownFields", "unit", "plane", "direction", "reverseDirection", "draftAngleRadians", "draftAngleDegrees", "extent", "metadata" },
                        operation,
                        pathRoot,
                        allowUnknownFields);

    if (const mini_json::Value* unitValue = root.get("unit")) {
        const mini_json::Value& unit = mini_json::requireObject(*unitValue, pathRoot + ".unit");
        rejectUnknownFields(unit, { "length", "angle" }, operation, pathRoot + ".unit", allowUnknownFields);
        if (hasMember(unit, "length")) {
            const std::string lengthUnit = optionalStringMember(unit, "length");
            if (lengthUnit != "model") {
                throwStructuredValidation(operation,
                                          pathRoot + ".unit.length",
                                          "Only model length units are supported");
            }
        }
        if (hasMember(unit, "angle")) {
            const std::string angleUnit = optionalStringMember(unit, "angle");
            if (angleUnit != "radians" && angleUnit != "degrees") {
                throwStructuredValidation(operation,
                                          pathRoot + ".unit.angle",
                                          "Angle unit must be radians or degrees");
            }
        }
    }

    StructuredExtrudeSpec spec;
    if (const mini_json::Value* planeValue = root.get("plane")) {
        spec.plane = parsePlaneFrame(mini_json::requireObject(*planeValue, pathRoot + ".plane"));
    }

    const gp_Dir sketchNormal = spec.plane.hasValue ? spec.plane.normal : gp_Dir(0.0, 0.0, 1.0);
    if (const mini_json::Value* directionValue = root.get("direction")) {
        spec.direction = parseDirection3(*directionValue, pathRoot + ".direction");
    } else {
        spec.direction = sketchNormal;
    }
    if (optionalBoolMember(root, "reverseDirection", false)) {
        spec.direction.Reverse();
    }

    bool hasDraftAngle = false;
    spec.draftAngleRadians = optionalSignedAngleRadians(root, operation, pathRoot, hasDraftAngle);
    spec.hasDraftAngle = hasDraftAngle && std::abs(spec.draftAngleRadians) > 1.0e-12;
    if (spec.hasDraftAngle && std::abs(spec.direction.Dot(sketchNormal)) < 1.0 - 1.0e-9) {
        throwStructuredValidation(operation,
                                  pathRoot + ".direction",
                                  "Draft extrusion requires direction aligned with the sketch plane normal");
    }

    const mini_json::Value& extent = mini_json::requireObject(mini_json::requireMember(root, "extent", pathRoot), pathRoot + ".extent");
    const std::string extentType = optionalStringMember(extent, "type");
    if (extentType.empty()) {
        throwStructuredValidation(operation,
                                  pathRoot + ".extent.type",
                                  "Extent type is required");
    }

    if (extentType == "blind") {
        rejectUnknownFields(extent, { "type", "distance" }, operation, pathRoot + ".extent", allowUnknownFields);
        spec.extentMode = StructuredExtrudeExtentMode::Blind;
        spec.distance = requirePositiveMember(extent, "distance", operation, pathRoot + ".extent.distance");
    } else if (extentType == "upToNext") {
        rejectUnknownFields(extent, { "type" }, operation, pathRoot + ".extent", allowUnknownFields);
        spec.extentMode = StructuredExtrudeExtentMode::UpToNext;
    } else if (extentType == "throughAll") {
        rejectUnknownFields(extent, { "type" }, operation, pathRoot + ".extent", allowUnknownFields);
        spec.extentMode = StructuredExtrudeExtentMode::ThroughAll;
    } else if (extentType == "upToSurface") {
        rejectUnknownFields(extent, { "type", "surface" }, operation, pathRoot + ".extent", allowUnknownFields);
        spec.extentMode = StructuredExtrudeExtentMode::UpToSurface;
        spec.hasSurfaceTarget = true;
        spec.surfaceTarget = parseStructuredExtrudeSurfaceTarget(mini_json::requireMember(extent, "surface", pathRoot + ".extent"),
                                                                 allowUnknownFields,
                                                                 operation,
                                                                 pathRoot + ".extent.surface");
    } else if (extentType == "offsetFromSurface") {
        rejectUnknownFields(extent, { "type", "surface", "offset" }, operation, pathRoot + ".extent", allowUnknownFields);
        spec.extentMode = StructuredExtrudeExtentMode::OffsetFromSurface;
        spec.hasSurfaceTarget = true;
        spec.surfaceTarget = parseStructuredExtrudeSurfaceTarget(mini_json::requireMember(extent, "surface", pathRoot + ".extent"),
                                                                 allowUnknownFields,
                                                                 operation,
                                                                 pathRoot + ".extent.surface");
        spec.offset = requirePositiveMember(extent, "offset", operation, pathRoot + ".extent.offset");
    } else {
        throwStructuredValidation(operation,
                                  pathRoot + ".extent.type",
                                  "Unsupported extent type");
    }

    return spec;
}

StructuredRevolveSpec parseStructuredRevolveSpec(const std::string& specJson,
                                                 const std::string& operation,
                                                 const std::string& pathRoot) {
    const mini_json::Value root = mini_json::requireObject(mini_json::parse(specJson), pathRoot + " spec");
    requireSchemaVersion(root, operation);
    const bool allowUnknownFields = optionalBoolMember(root, "allowUnknownFields", false);
    rejectUnknownFields(root,
                        { "schemaVersion", "allowUnknownFields", "unit", "plane", "axisOrigin", "axisDirection", "reverseDirection", "slidingEdges", "extent", "metadata" },
                        operation,
                        pathRoot,
                        allowUnknownFields);

    if (const mini_json::Value* unitValue = root.get("unit")) {
        const mini_json::Value& unit = mini_json::requireObject(*unitValue, pathRoot + ".unit");
        rejectUnknownFields(unit, { "length", "angle" }, operation, pathRoot + ".unit", allowUnknownFields);
        if (hasMember(unit, "length")) {
            const std::string lengthUnit = optionalStringMember(unit, "length");
            if (lengthUnit != "model") {
                throwStructuredValidation(operation,
                                          pathRoot + ".unit.length",
                                          "Only model length units are supported");
            }
        }
        if (hasMember(unit, "angle")) {
            const std::string angleUnit = optionalStringMember(unit, "angle");
            if (angleUnit != "radians" && angleUnit != "degrees") {
                throwStructuredValidation(operation,
                                          pathRoot + ".unit.angle",
                                          "Angle unit must be radians or degrees");
            }
        }
    }

    StructuredRevolveSpec spec;
    if (const mini_json::Value* planeValue = root.get("plane")) {
        spec.plane = parsePlaneFrame(mini_json::requireObject(*planeValue, pathRoot + ".plane"));
    }
    if (const mini_json::Value* axisOriginValue = root.get("axisOrigin")) {
        spec.axisOrigin = parsePoint3(*axisOriginValue, pathRoot + ".axisOrigin");
    }
    if (const mini_json::Value* axisDirectionValue = root.get("axisDirection")) {
        spec.axisDirection = parseDirection3(*axisDirectionValue, pathRoot + ".axisDirection");
    }
    if (optionalBoolMember(root, "reverseDirection", false)) {
        spec.axisDirection.Reverse();
    }

    if (const mini_json::Value* slidingEdgesValue = root.get("slidingEdges")) {
        const mini_json::Value& slidingEdges = mini_json::requireArray(*slidingEdgesValue, pathRoot + ".slidingEdges");
        spec.slidingEdges.reserve(slidingEdges.array.size());
        for (std::size_t index = 0; index < slidingEdges.array.size(); ++index) {
            const std::string edgePath = pathRoot + ".slidingEdges[" + std::to_string(index) + "]";
            const mini_json::Value& entry = mini_json::requireObject(slidingEdges.array[index], edgePath);
            rejectUnknownFields(entry, { "profileEdgeIndex", "face" }, operation, edgePath, allowUnknownFields);

            StructuredRevolveSlidingEdge slidingEdge;
            slidingEdge.profileEdgeIndex = requirePositiveIntMember(entry,
                                                                    "profileEdgeIndex",
                                                                    operation,
                                                                    edgePath + ".profileEdgeIndex");
            slidingEdge.faceRef = mini_json::requireObject(mini_json::requireMember(entry, "face", edgePath), edgePath + ".face");
            spec.slidingEdges.push_back(slidingEdge);
        }
    }

    const mini_json::Value& extent = mini_json::requireObject(mini_json::requireMember(root, "extent", pathRoot), pathRoot + ".extent");
    const std::string extentType = optionalStringMember(extent, "type");
    if (extentType.empty()) {
        throwStructuredValidation(operation,
                                  pathRoot + ".extent.type",
                                  "Extent type is required");
    }

    if (extentType == "angle") {
        rejectUnknownFields(extent, { "type", "angleRadians", "angleDegrees" }, operation, pathRoot + ".extent", allowUnknownFields);
        spec.extentMode = StructuredRevolveExtentMode::Angle;
        spec.angleRadians = requireSignedExtentAngleRadians(extent, operation, pathRoot + ".extent");
    } else if (extentType == "upToSurface") {
        rejectUnknownFields(extent, { "type", "surface" }, operation, pathRoot + ".extent", allowUnknownFields);
        spec.extentMode = StructuredRevolveExtentMode::UpToSurface;
        spec.hasSurfaceTarget = true;
        spec.surfaceTarget = parseStructuredExtrudeSurfaceTarget(mini_json::requireMember(extent, "surface", pathRoot + ".extent"),
                                                                 allowUnknownFields,
                                                                 operation,
                                                                 pathRoot + ".extent.surface");
    } else if (extentType == "fromSurfaceToSurface") {
        rejectUnknownFields(extent, { "type", "fromSurface", "untilSurface" }, operation, pathRoot + ".extent", allowUnknownFields);
        spec.extentMode = StructuredRevolveExtentMode::FromSurfaceToSurface;
        spec.hasFromSurface = true;
        spec.fromSurfaceTarget = parseStructuredExtrudeSurfaceTarget(mini_json::requireMember(extent, "fromSurface", pathRoot + ".extent"),
                                                                     allowUnknownFields,
                                                                     operation,
                                                                     pathRoot + ".extent.fromSurface");
        spec.hasUntilSurface = true;
        spec.untilSurfaceTarget = parseStructuredExtrudeSurfaceTarget(mini_json::requireMember(extent, "untilSurface", pathRoot + ".extent"),
                                                                      allowUnknownFields,
                                                                      operation,
                                                                      pathRoot + ".extent.untilSurface");
    } else if (extentType == "throughAll") {
        rejectUnknownFields(extent, { "type" }, operation, pathRoot + ".extent", allowUnknownFields);
        spec.extentMode = StructuredRevolveExtentMode::ThroughAll;
    } else if (extentType == "upToSurfaceAtAngle") {
        rejectUnknownFields(extent, { "type", "surface", "angleRadians", "angleDegrees" }, operation, pathRoot + ".extent", allowUnknownFields);
        spec.extentMode = StructuredRevolveExtentMode::UpToSurfaceAtAngle;
        spec.hasSurfaceTarget = true;
        spec.surfaceTarget = parseStructuredExtrudeSurfaceTarget(mini_json::requireMember(extent, "surface", pathRoot + ".extent"),
                                                                 allowUnknownFields,
                                                                 operation,
                                                                 pathRoot + ".extent.surface");
        spec.angleRadians = requireSignedExtentAngleRadians(extent, operation, pathRoot + ".extent");
    } else {
        throwStructuredValidation(operation,
                                  pathRoot + ".extent.type",
                                  "Unsupported extent type");
    }

    return spec;
}

std::string objectToStableSignature(const mini_json::Value& value) {
    if (value.kind == mini_json::Value::Kind::Null) return "null";
    if (value.kind == mini_json::Value::Kind::Bool) return value.boolean ? "true" : "false";
    if (value.kind == mini_json::Value::Kind::Number) return coordinateKey(value.number);
    if (value.kind == mini_json::Value::Kind::String) return "s:" + value.string;
    if (value.kind == mini_json::Value::Kind::Array) {
        std::ostringstream out;
        out << "[";
        for (const mini_json::Value& entry : value.array) {
            out << objectToStableSignature(entry) << ";";
        }
        out << "]";
        return out.str();
    }

    std::vector<std::pair<std::string, std::string>> members;
    for (const auto& entry : value.object) {
        members.push_back({ entry.first, objectToStableSignature(entry.second) });
    }
    std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::ostringstream out;
    out << "{";
    for (const auto& member : members) {
        out << member.first << ":" << member.second << ";";
    }
    out << "}";
    return out.str();
}

struct ResolvedEdgeRef {
    TopoDS_Edge edge;
    int topoId = 0;
    std::string stableHash;
};

struct ResolvedFaceRef {
    TopoDS_Face face;
    int topoId = 0;
    std::string stableHash;
};

std::vector<std::string> faceStableHashesForShape(const TopoDS_Shape& shape, const RevisionMetadata* revision) {
    ShapeMap faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    std::vector<std::string> hashes(static_cast<std::size_t>(faces.Extent()) + 1);
    for (int i = 1; i <= faces.Extent(); ++i) {
        hashes[static_cast<std::size_t>(i)] = revision != nullptr && revision->faceStableHashes.size() == static_cast<std::size_t>(faces.Extent())
            ? revision->faceStableHashes[static_cast<std::size_t>(i - 1)]
            : makeFaceStableHash(TopoDS::Face(faces(i)));
    }
    return hashes;
}

std::vector<std::string> edgeStableHashesForShape(const TopoDS_Shape& shape, const RevisionMetadata* revision) {
    ShapeMap faces, edges;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    ShapeToShapeListMap edgeToFaces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeToFaces);

    std::vector<std::string> faceHashes = faceStableHashesForShape(shape, revision);
    std::vector<std::string> edgeHashes(static_cast<std::size_t>(edges.Extent()) + 1);
    for (int i = 1; i <= edges.Extent(); ++i) {
        if (revision != nullptr && revision->edgeStableHashes.size() == static_cast<std::size_t>(edges.Extent())) {
            edgeHashes[static_cast<std::size_t>(i)] = revision->edgeStableHashes[static_cast<std::size_t>(i - 1)];
            continue;
        }
        std::vector<std::string> adjacentFaceHashes;
        for (int faceId : faceIdsForEdge(TopoDS::Edge(edges(i)), faces, edgeToFaces)) {
            adjacentFaceHashes.push_back(faceHashes[static_cast<std::size_t>(faceId)]);
        }
        edgeHashes[static_cast<std::size_t>(i)] = makeEdgeStableHash(TopoDS::Edge(edges(i)), adjacentFaceHashes);
    }
    return edgeHashes;
}

ResolvedEdgeRef resolveEdgeRef(const TopoDS_Shape& shape,
                               const RevisionMetadata* revision,
                               const mini_json::Value& refValue,
                               const std::string& operation,
                               const std::string& path) {
    const mini_json::Value& ref = mini_json::requireObject(refValue, path);
    ShapeMap edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    const std::vector<std::string> edgeHashes = edgeStableHashesForShape(shape, revision);

    if (hasMember(ref, "topoId")) {
        const int topoId = optionalIntMember(ref, "topoId", 0);
        if (topoId < 1 || topoId > edges.Extent()) {
            throwStructuredValidation(operation, path + ".topoId", "Edge topoId is outside the current topology");
        }
        ResolvedEdgeRef result;
        result.edge = TopoDS::Edge(edges(topoId));
        result.topoId = topoId;
        result.stableHash = edgeHashes[static_cast<std::size_t>(topoId)];
        return result;
    }

    if (hasMember(ref, "stableHash")) {
        const std::string stableHash = optionalStringMember(ref, "stableHash");
        for (int i = 1; i <= edges.Extent(); ++i) {
            if (edgeHashes[static_cast<std::size_t>(i)] == stableHash) {
                ResolvedEdgeRef result;
                result.edge = TopoDS::Edge(edges(i));
                result.topoId = i;
                result.stableHash = stableHash;
                return result;
            }
        }
        throwStructuredValidation(operation, path + ".stableHash", "No edge with this stableHash exists in the current topology");
    }

    throwStructuredValidation(operation, path, "Edge reference requires topoId or stableHash");
    return ResolvedEdgeRef();
}

ResolvedFaceRef resolveFaceRef(const TopoDS_Shape& shape,
                               const RevisionMetadata* revision,
                               const mini_json::Value& refValue,
                               const std::string& operation,
                               const std::string& path) {
    const mini_json::Value& ref = mini_json::requireObject(refValue, path);
    ShapeMap faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    const std::vector<std::string> faceHashes = faceStableHashesForShape(shape, revision);

    if (hasMember(ref, "topoId")) {
        const int topoId = optionalIntMember(ref, "topoId", 0);
        if (topoId < 1 || topoId > faces.Extent()) {
            throwStructuredValidation(operation, path + ".topoId", "Face topoId is outside the current topology");
        }
        return { TopoDS::Face(faces(topoId)), topoId, faceHashes[static_cast<std::size_t>(topoId)] };
    }

    if (hasMember(ref, "stableHash")) {
        const std::string stableHash = optionalStringMember(ref, "stableHash");
        for (int i = 1; i <= faces.Extent(); ++i) {
            if (faceHashes[static_cast<std::size_t>(i)] == stableHash) {
                return { TopoDS::Face(faces(i)), i, stableHash };
            }
        }
        throwStructuredValidation(operation, path + ".stableHash", "No face with this stableHash exists in the current topology");
    }

    throwStructuredValidation(operation, path, "Face reference requires topoId or stableHash");
    return ResolvedFaceRef();
}

void appendEntityRefJson(std::ostringstream& out, const ResolvedEdgeRef& edge) {
    out << "{";
    out << "\"topoId\":" << edge.topoId << ",";
    out << "\"stableHash\":"; appendJsonString(out, edge.stableHash);
    out << "}";
}

void appendEntityRefJson(std::ostringstream& out, const ResolvedFaceRef& face) {
    out << "{";
    out << "\"topoId\":" << face.topoId << ",";
    out << "\"stableHash\":"; appendJsonString(out, face.stableHash);
    out << "}";
}

template <typename TFeature>
TopoDS_Shape performStructuredExtrudeExtent(TFeature& feature,
                                            const StructuredExtrudeSpec& spec,
                                            const TopoDS_Shape& baseShape,
                                            const TopoDS_Shape* surfaceLimitShape,
                                            const std::string& operation) {
    switch (spec.extentMode) {
    case StructuredExtrudeExtentMode::Blind:
        feature.Perform(spec.distance);
        break;
    case StructuredExtrudeExtentMode::UpToNext:
        feature.Perform(baseShape);
        break;
    case StructuredExtrudeExtentMode::ThroughAll:
        feature.PerformUntilEnd();
        break;
    case StructuredExtrudeExtentMode::UpToSurface:
        if (surfaceLimitShape == nullptr) {
            throwStructuredValidation(operation, "extent.surface", "Surface limit is required");
        }
        feature.Perform(*surfaceLimitShape);
        break;
    case StructuredExtrudeExtentMode::OffsetFromSurface:
        if (surfaceLimitShape == nullptr) {
            throwStructuredValidation(operation, "extent.surface", "Surface limit is required");
        }
        feature.PerformUntilHeight(*surfaceLimitShape, spec.offset);
        break;
    }

    if (!feature.IsDone() || feature.Shape().IsNull()) {
        throwStructuredOperationError(operation, "Structured extrusion feature failed");
    }
    return feature.Shape();
}

TopoDS_Shape performStructuredRevolveExtent(BRepFeat_MakeRevol& feature,
                                            const StructuredRevolveSpec& spec,
                                            const TopoDS_Shape* surfaceLimitShape,
                                            const TopoDS_Shape* fromSurfaceShape,
                                            const TopoDS_Shape* untilSurfaceShape,
                                            const std::string& operation) {
    switch (spec.extentMode) {
    case StructuredRevolveExtentMode::Angle:
        feature.Perform(spec.angleRadians);
        break;
    case StructuredRevolveExtentMode::UpToSurface:
        if (surfaceLimitShape == nullptr) {
            throwStructuredValidation(operation, "extent.surface", "Surface limit is required");
        }
        feature.Perform(*surfaceLimitShape);
        break;
    case StructuredRevolveExtentMode::FromSurfaceToSurface:
        if (fromSurfaceShape == nullptr) {
            throwStructuredValidation(operation, "extent.fromSurface", "From surface is required");
        }
        if (untilSurfaceShape == nullptr) {
            throwStructuredValidation(operation, "extent.untilSurface", "Until surface is required");
        }
        feature.Perform(*fromSurfaceShape, *untilSurfaceShape);
        break;
    case StructuredRevolveExtentMode::ThroughAll:
        feature.PerformThruAll();
        break;
    case StructuredRevolveExtentMode::UpToSurfaceAtAngle:
        if (surfaceLimitShape == nullptr) {
            throwStructuredValidation(operation, "extent.surface", "Surface limit is required");
        }
        feature.PerformUntilAngle(*surfaceLimitShape, spec.angleRadians);
        break;
    }

    if (!feature.IsDone() || feature.Shape().IsNull()) {
        throwStructuredOperationError(operation, "Structured revolve feature failed");
    }
    return feature.Shape();
}

ChFi3d_FilletShape parseFilletShape(const std::string& value, const std::string& operation, const std::string& path) {
    if (value.empty() || value == "rational") return ChFi3d_Rational;
    if (value == "quasiAngular") return ChFi3d_QuasiAngular;
    if (value == "polynomial") return ChFi3d_Polynomial;
    throwStructuredValidation(operation, path, "Unsupported fillet blendShape", "fillet.blendShape");
    return ChFi3d_Rational;
}

GeomAbs_Shape parseContinuity(const std::string& value, const std::string& operation, const std::string& path) {
    if (value.empty() || value == "C1" || value == "G1") return GeomAbs_C1;
    if (value == "C0" || value == "G0") return GeomAbs_C0;
    if (value == "C2" || value == "G2") return GeomAbs_C2;
    throwStructuredValidation(operation, path, "Unsupported continuity mode", "fillet.continuityModes");
    return GeomAbs_C1;
}

std::string stableEdgeRefString(const ResolvedEdgeRef& edge) {
    return edge.stableHash.empty() ? ("topoId:" + std::to_string(edge.topoId)) : edge.stableHash;
}

std::vector<int> supportFaceIdsForEdge(const TopoDS_Shape& shape, const TopoDS_Edge& edge) {
    ShapeMap faceMap;
    TopExp::MapShapes(shape, TopAbs_FACE, faceMap);
    ShapeToShapeListMap edgeToFaces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeToFaces);
    return faceIdsForEdge(edge, faceMap, edgeToFaces);
}

std::string faceHashForOutputFace(const TopoDS_Shape& maybeFace) {
    if (maybeFace.IsNull() || maybeFace.ShapeType() != TopAbs_FACE) {
        return "";
    }
    return makeFaceStableHash(TopoDS::Face(maybeFace));
}

template <typename BlendBuilder>
void appendBlendFacesJson(std::ostringstream& out,
                          BlendBuilder& builder,
                          const TopoDS_Shape& sourceShape,
                          const std::vector<ResolvedEdgeRef>& edges,
                          const std::vector<std::string>& normalizedParameters,
                          const std::string& faceKind) {
    out << '[';
    bool firstFace = true;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const ShapeList& generated = builder.Generated(edges[i].edge);
        std::vector<std::string> generatedFaceHashes;
        for (ShapeList::Iterator it(generated); it.More(); it.Next()) {
            const std::string stableHash = faceHashForOutputFace(it.Value());
            if (!stableHash.empty()) {
                generatedFaceHashes.push_back(stableHash);
            }
        }

        if (generatedFaceHashes.empty()) {
            if (!firstFace) out << ',';
            firstFace = false;
            out << '{';
            out << "\"kind\":"; appendJsonString(out, faceKind); out << ',';
            out << "\"stableHash\":null,";
            out << "\"sourceEdge\":"; appendEntityRefJson(out, edges[i]); out << ',';
            out << "\"tangentChainEdgeRefs\":["; appendEntityRefJson(out, edges[i]); out << "],";
            out << "\"usedParameters\":" << normalizedParameters[i] << ',';
            out << "\"supportingFaceIds\":"; appendIntArrayJson(out, supportFaceIdsForEdge(sourceShape, edges[i].edge)); out << ',';
            out << "\"terminalCapIds\":[],";
            out << "\"terminalCondition\":\"unresolved\"";
            out << '}';
            continue;
        }

        for (const std::string& stableHash : generatedFaceHashes) {
            if (!firstFace) out << ',';
            firstFace = false;
            out << '{';
            out << "\"kind\":"; appendJsonString(out, faceKind); out << ',';
            out << "\"stableHash\":"; appendJsonString(out, stableHash); out << ',';
            out << "\"sourceEdge\":"; appendEntityRefJson(out, edges[i]); out << ',';
            out << "\"tangentChainEdgeRefs\":["; appendEntityRefJson(out, edges[i]); out << "],";
            out << "\"usedParameters\":" << normalizedParameters[i] << ',';
            out << "\"supportingFaceIds\":"; appendIntArrayJson(out, supportFaceIdsForEdge(sourceShape, edges[i].edge)); out << ',';
            out << "\"terminalCapIds\":[],";
            out << "\"terminalCondition\":\"unresolved\"";
            out << '}';
        }
    }
    out << ']';
}

template <typename BlendBuilder>
std::string makeBlendResultJson(OcctKernel* kernel,
                                uint32_t shapeId,
                                BlendBuilder& builder,
                                const TopoDS_Shape& sourceShape,
                                const std::vector<ResolvedEdgeRef>& edges,
                                const std::vector<std::string>& normalizedParameters,
                                const std::string& faceKind) {
    std::ostringstream generatedFaces;
    appendBlendFacesJson(generatedFaces, builder, sourceShape, edges, normalizedParameters, faceKind);

    std::vector<std::string> deletedEdges;
    for (const ResolvedEdgeRef& edge : edges) {
        deletedEdges.push_back(edge.stableHash);
    }

    std::ostringstream result;
    result << '{';
    result << "\"shapeId\":" << shapeId << ',';
    result << "\"revision\":" << kernel->getRevisionInfo(shapeId) << ',';
    result << "\"topology\":" << kernel->getTopology(shapeId) << ',';
    result << "\"lineage\":{";
    result << "\"generated\":" << generatedFaces.str() << ',';
    result << "\"modified\":[],";
    result << "\"retained\":[],";
    result << "\"deleted\":"; appendStringArrayJson(result, deletedEdges);
    result << "},";
    result << "\"blendFaces\":" << generatedFaces.str() << ',';
    result << "\"status\":{";
    result << "\"isPartial\":false,";
    result << "\"isClipped\":false,";
    result << "\"isHealed\":false,";
    result << "\"isExact\":true";
    result << "}";
    result << '}';
    return result.str();
}

std::vector<std::string> optionalStringArrayMember(const mini_json::Value& object, const std::string& key) {
    std::vector<std::string> values;
    const mini_json::Value* member = object.get(key);
    if (member == nullptr || member->kind == mini_json::Value::Kind::Null) {
        return values;
    }

    const mini_json::Value& array = mini_json::requireArray(*member, key);
    values.reserve(array.array.size());
    for (std::size_t i = 0; i < array.array.size(); ++i) {
        values.push_back(mini_json::requireString(array.array[i], key + "[]"));
    }
    return values;
}

void appendDeletedEntitiesJson(std::ostringstream& out, const std::vector<DeletedEntityRecord>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out << ',';
        out << '{';
        out << "\"kind\":"; appendJsonString(out, values[i].kind); out << ',';
        out << "\"stableHash\":"; appendJsonString(out, values[i].stableHash);
        if (!values[i].deletedBy.empty()) {
            out << ",\"deletedBy\":"; appendJsonString(out, values[i].deletedBy);
        }
        out << ",\"status\":"; appendJsonString(out, values[i].status);
        out << '}';
    }
    out << ']';
}

void appendRevisionMetadataJson(std::ostringstream& out, const RevisionMetadata& revision) {
    out << '{';
    out << "\"revisionId\":"; appendJsonString(out, revision.revisionId); out << ',';
    out << "\"operationId\":";
    if (revision.operationId.empty()) out << "null"; else appendJsonString(out, revision.operationId);
    out << ',';
    out << "\"sourceFeatureId\":";
    if (revision.sourceFeatureId.empty()) out << "null"; else appendJsonString(out, revision.sourceFeatureId);
    out << ',';
    out << "\"operationType\":"; appendJsonString(out, revision.operationType); out << ',';
    out << "\"operandRevisionIds\":"; appendStringArrayJson(out, revision.operandRevisionIds); out << ',';
    out << "\"parameterHash\":";
    if (revision.parameterHash.empty()) out << "null"; else appendJsonString(out, revision.parameterHash);
    out << ',';
    out << "\"topologyHash\":"; appendJsonString(out, revision.topologyHash); out << ',';
    out << "\"historySchemaVersion\":" << revision.historySchemaVersion << ',';
    out << "\"createdFromCheckpoint\":" << (revision.createdFromCheckpoint ? "true" : "false") << ',';
    out << "\"entityStatus\":"; appendJsonString(out, revision.entityStatus); out << ',';
    out << "\"identityStatus\":"; appendJsonString(out, revision.identityStatus); out << ',';
    out << "\"historyWarnings\":"; appendStringArrayJson(out, revision.historyWarnings); out << ',';
    out << "\"deletedEntities\":"; appendDeletedEntitiesJson(out, revision.deletedEntities); out << ',';
    out << "\"faceStableHashes\":"; appendStringArrayJson(out, revision.faceStableHashes); out << ',';
    out << "\"edgeStableHashes\":"; appendStringArrayJson(out, revision.edgeStableHashes); out << ',';
    out << "\"vertexStableHashes\":"; appendStringArrayJson(out, revision.vertexStableHashes);
    out << '}';
}

RevisionMetadata parseRevisionMetadata(const mini_json::Value& value) {
    const mini_json::Value& object = mini_json::requireObject(value, "checkpoint.revision");
    RevisionMetadata revision;
    revision.revisionId = optionalStringMember(object, "revisionId");
    revision.operationId = optionalStringMember(object, "operationId");
    revision.sourceFeatureId = optionalStringMember(object, "sourceFeatureId");
    revision.operationType = optionalStringMember(object, "operationType", "hydrateCheckpoint");
    revision.operandRevisionIds = optionalStringArrayMember(object, "operandRevisionIds");
    revision.parameterHash = optionalStringMember(object, "parameterHash");
    revision.topologyHash = optionalStringMember(object, "topologyHash");
    revision.historySchemaVersion = optionalIntMember(object, "historySchemaVersion", 1);
    revision.createdFromCheckpoint = optionalBoolMember(object, "createdFromCheckpoint", false);
    revision.entityStatus = optionalStringMember(object, "entityStatus", "retained");
    revision.identityStatus = optionalStringMember(object, "identityStatus", "retained");
    revision.historyWarnings = optionalStringArrayMember(object, "historyWarnings");
    revision.faceStableHashes = optionalStringArrayMember(object, "faceStableHashes");
    revision.edgeStableHashes = optionalStringArrayMember(object, "edgeStableHashes");
    revision.vertexStableHashes = optionalStringArrayMember(object, "vertexStableHashes");

    const mini_json::Value* deleted = object.get("deletedEntities");
    if (deleted != nullptr && deleted->kind != mini_json::Value::Kind::Null) {
        const mini_json::Value& array = mini_json::requireArray(*deleted, "checkpoint.revision.deletedEntities");
        for (const mini_json::Value& entryValue : array.array) {
            const mini_json::Value& entry = mini_json::requireObject(entryValue, "deleted entity");
            DeletedEntityRecord record;
            record.kind = optionalStringMember(entry, "kind");
            record.stableHash = optionalStringMember(entry, "stableHash");
            record.deletedBy = optionalStringMember(entry, "deletedBy");
            record.status = optionalStringMember(entry, "status", "unresolved");
            if (!record.kind.empty() && !record.stableHash.empty()) {
                revision.deletedEntities.push_back(record);
            }
        }
    }

    return revision;
}

std::string computeTopologyHash(const TopoDS_Shape& shape) {
    ShapeMap faces, edges, vertices;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);

    ShapeToShapeListMap edgeToFaces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeToFaces);

    std::vector<std::string> faceHashes(static_cast<std::size_t>(faces.Extent()) + 1);
    for (int i = 1; i <= faces.Extent(); ++i) {
        faceHashes[static_cast<std::size_t>(i)] = makeFaceStableHash(TopoDS::Face(faces(i)));
    }

    std::vector<std::string> topologyParts;
    topologyParts.reserve(static_cast<std::size_t>(faces.Extent() + edges.Extent() + vertices.Extent() + 3));
    topologyParts.push_back("faces=" + std::to_string(faces.Extent()));
    topologyParts.push_back("edges=" + std::to_string(edges.Extent()));
    topologyParts.push_back("vertices=" + std::to_string(vertices.Extent()));

    for (int i = 1; i <= faces.Extent(); ++i) {
        topologyParts.push_back(faceHashes[static_cast<std::size_t>(i)]);
    }
    for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge& edge = TopoDS::Edge(edges(i));
        std::vector<std::string> adjacentFaceHashes;
        for (int faceId : faceIdsForEdge(edge, faces, edgeToFaces)) {
            adjacentFaceHashes.push_back(faceHashes[static_cast<std::size_t>(faceId)]);
        }
        topologyParts.push_back(makeEdgeStableHash(edge, adjacentFaceHashes));
    }
    for (int i = 1; i <= vertices.Extent(); ++i) {
        topologyParts.push_back(makeVertexStableHash(TopoDS::Vertex(vertices(i))));
    }

    std::sort(topologyParts.begin(), topologyParts.end());
    std::ostringstream signature;
    for (const std::string& part : topologyParts) {
        signature << part << '|';
    }
    return "T:" + fnv1a64(signature.str());
}

RevisionMetadata makeRevisionMetadata(const TopoDS_Shape& shape,
                                      const std::string& operationType,
                                      const std::string& parameterSignature,
                                      const std::vector<std::string>& operandRevisionIds,
                                      const std::string& entityStatus,
                                      const std::string& identityStatus,
                                      const std::vector<std::string>& warnings = {}) {
    RevisionMetadata revision;
    revision.operationType = operationType;
    revision.operandRevisionIds = operandRevisionIds;
    revision.parameterHash = parameterSignature.empty() ? "P:" + fnv1a64(operationType) : "P:" + fnv1a64(parameterSignature);
    revision.topologyHash = computeTopologyHash(shape);

    std::ostringstream key;
    key << operationType << '|' << revision.parameterHash << '|' << revision.topologyHash << '|';
    for (const std::string& operand : operandRevisionIds) {
        key << operand << '|';
    }
    revision.revisionId = "rev_" + fnv1a64(key.str());
    revision.operationId = "op_" + fnv1a64(operationType + "|" + revision.parameterHash + "|" + revision.revisionId);
    revision.sourceFeatureId = revision.operationId;
    revision.entityStatus = entityStatus;
    revision.identityStatus = identityStatus;
    revision.historyWarnings = warnings;
    return revision;
}

struct StableEntityRecord {
    std::string kind;
    int id = 0;
    std::string stableHash;
};

std::vector<StableEntityRecord> collectStableEntities(const TopoDS_Shape& shape, const RevisionMetadata* revision = nullptr) {
    std::vector<StableEntityRecord> records;

    ShapeMap faces, edges, vertices;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);

    ShapeToShapeListMap edgeToFaces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeToFaces);

    std::vector<std::string> faceHashes(static_cast<std::size_t>(faces.Extent()) + 1);
    for (int i = 1; i <= faces.Extent(); ++i) {
        if (revision != nullptr && revision->faceStableHashes.size() == static_cast<std::size_t>(faces.Extent())) {
            faceHashes[static_cast<std::size_t>(i)] = revision->faceStableHashes[static_cast<std::size_t>(i - 1)];
        } else {
            faceHashes[static_cast<std::size_t>(i)] = makeFaceStableHash(TopoDS::Face(faces(i)));
        }
        records.push_back({ "face", i, faceHashes[static_cast<std::size_t>(i)] });
    }
    for (int i = 1; i <= edges.Extent(); ++i) {
        std::vector<std::string> adjacentFaceHashes;
        for (int faceId : faceIdsForEdge(TopoDS::Edge(edges(i)), faces, edgeToFaces)) {
            adjacentFaceHashes.push_back(faceHashes[static_cast<std::size_t>(faceId)]);
        }
        const std::string stableHash = revision != nullptr && revision->edgeStableHashes.size() == static_cast<std::size_t>(edges.Extent())
            ? revision->edgeStableHashes[static_cast<std::size_t>(i - 1)]
            : makeEdgeStableHash(TopoDS::Edge(edges(i)), adjacentFaceHashes);
        records.push_back({ "edge", i, stableHash });
    }
    for (int i = 1; i <= vertices.Extent(); ++i) {
        const std::string stableHash = revision != nullptr && revision->vertexStableHashes.size() == static_cast<std::size_t>(vertices.Extent())
            ? revision->vertexStableHashes[static_cast<std::size_t>(i - 1)]
            : makeVertexStableHash(TopoDS::Vertex(vertices(i)));
        records.push_back({ "vertex", i, stableHash });
    }

    return records;
}

void addStepImportMessage(std::vector<StepImportMessage>& messages,
                          const std::string& phase,
                          const std::string& severity,
                          const std::string& text,
                          int entityNumber = 0) {
    if (text.empty()) {
        return;
    }

    StepImportMessage message;
    message.phase = phase;
    message.severity = severity;
    message.text = text;
    message.entityNumber = entityNumber;
    messages.push_back(message);
}

std::string stepReturnStatusToString(IFSelect_ReturnStatus status) {
    switch (status) {
    case IFSelect_RetVoid: return "IFSelect_RetVoid";
    case IFSelect_RetDone: return "IFSelect_RetDone";
    case IFSelect_RetError: return "IFSelect_RetError";
    case IFSelect_RetFail: return "IFSelect_RetFail";
    case IFSelect_RetStop: return "IFSelect_RetStop";
    }
    return "IFSelect_RetFail";
}

std::string stepTransferStatusToString(int rootCount, int transferredRootCount, int shapeCount) {
    if (rootCount == 0) {
        return "EMPTY";
    }
    if (transferredRootCount <= 0 || shapeCount <= 0) {
        return "FAILED";
    }
    if (transferredRootCount < rootCount) {
        return "PARTIAL";
    }
    return "DONE";
}

void collectCheckMessages(const Interface_CheckIterator& checks,
                          const std::string& phase,
                          std::vector<StepImportMessage>& messages) {
    for (checks.Start(); checks.More(); checks.Next()) {
        const Handle(Interface_Check)& check = checks.Value();
        const int entityNumber = checks.Number();

        if (check.IsNull()) {
            continue;
        }

        for (int i = 1; i <= check->NbFails(); ++i) {
            addStepImportMessage(messages, phase, "fail", check->CFail(i), entityNumber);
        }
        for (int i = 1; i <= check->NbWarnings(); ++i) {
            addStepImportMessage(messages, phase, "warning", check->CWarning(i), entityNumber);
        }
        for (int i = 1; i <= check->NbInfoMsgs(); ++i) {
            addStepImportMessage(messages, phase, "info", check->CInfoMsg(i), entityNumber);
        }
    }
}

TopoDS_Shape applyImportHealing(const TopoDS_Shape& sourceShape,
                                const StepImportOptions& options,
                                StepImportRunResult& result) {
    TopoDS_Shape shape = sourceShape;
    bool touched = false;

    result.wasValidBeforeHealing = BRepCheck_Analyzer(shape).IsValid();

    if (options.sew) {
        BRepBuilderAPI_Sewing sewing(options.sewingTolerance);
        sewing.Load(shape);
        sewing.Perform();
        const TopoDS_Shape& sewedShape = sewing.SewedShape();
        if (!sewedShape.IsNull()) {
            shape = sewedShape;
            touched = true;
            addStepImportMessage(result.messages,
                                 "heal",
                                 "info",
                                 "Applied sewing with tolerance " + std::to_string(options.sewingTolerance));
            if (sewing.NbFreeEdges() > 0) {
                addStepImportMessage(result.messages,
                                     "heal",
                                     "warning",
                                     "Sewing left " + std::to_string(sewing.NbFreeEdges()) + " free edge(s)");
            }
        }
    }

    if (options.heal || options.fixSameParameter || options.fixSolid) {
        Handle(ShapeFix_Shape) fixer = new ShapeFix_Shape(shape);
        fixer->FixFreeShellMode() = options.heal ? 1 : 0;
        fixer->FixFreeFaceMode() = options.heal ? 1 : 0;
        fixer->FixFreeWireMode() = options.heal ? 1 : 0;
        fixer->FixVertexPositionMode() = options.heal ? 1 : 0;
        fixer->FixVertexTolMode() = options.heal ? 1 : 0;
        fixer->FixSameParameterMode() = (options.heal || options.fixSameParameter) ? 1 : 0;
        fixer->FixSolidMode() = (options.heal || options.fixSolid) ? 1 : 0;
        if (options.fixSolid) {
            fixer->FixSolidTool()->CreateOpenSolidMode() = false;
        }

        if (fixer->Perform()) {
            TopoDS_Shape fixedShape = fixer->Shape();
            if (!fixedShape.IsNull()) {
                shape = fixedShape;
                touched = true;
            }
            addStepImportMessage(result.messages,
                                 "heal",
                                 "info",
                                 "Applied ShapeFix post-processing");
        }
    }

    result.healed = touched;
    result.isValid = BRepCheck_Analyzer(shape).IsValid();

    if (!result.isValid) {
        addStepImportMessage(result.messages,
                             "validation",
                             "warning",
                             "Imported shape is not valid according to BRepCheck_Analyzer");
    } else if (result.healed && !result.wasValidBeforeHealing) {
        addStepImportMessage(result.messages,
                             "validation",
                             "info",
                             "Healing produced a valid shape");
    }

    return shape;
}

bool hasFailureMessage(const StepImportRunResult& result) {
    for (const StepImportMessage& message : result.messages) {
        if (message.severity == "fail") {
            return true;
        }
    }
    return false;
}

std::string inferImportFailureDetail(const StepImportRunResult& result) {
    for (const StepImportMessage& message : result.messages) {
        if (message.severity == "fail") {
            return message.text;
        }
    }
    for (const StepImportMessage& message : result.messages) {
        if (message.severity == "warning") {
            return message.text;
        }
    }
    return "STEP import failed";
}

std::string buildStepImportResultJson(const StepImportRunResult& result, uint32_t shapeId) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"readStatus\":\"" << escapeJson(result.readStatus) << "\",";
    ss << "\"transferStatus\":\"" << escapeJson(result.transferStatus) << "\",";
    ss << "\"rootCount\":" << result.rootCount << ",";
    ss << "\"transferredRootCount\":" << result.transferredRootCount << ",";
    ss << "\"isValid\":" << (result.isValid ? "true" : "false") << ",";
    ss << "\"wasValidBeforeHealing\":" << (result.wasValidBeforeHealing ? "true" : "false") << ",";
    ss << "\"healed\":" << (result.healed ? "true" : "false") << ",";
    if (shapeId != 0) {
        ss << "\"shapeId\":" << shapeId << ",";
    }
    ss << "\"messageList\":[";
    for (std::size_t i = 0; i < result.messages.size(); ++i) {
        const StepImportMessage& message = result.messages[i];
        if (i > 0) {
            ss << ",";
        }
        ss << "{";
        ss << "\"phase\":\"" << escapeJson(message.phase) << "\",";
        ss << "\"severity\":\"" << escapeJson(message.severity) << "\",";
        ss << "\"text\":\"" << escapeJson(message.text) << "\"";
        if (message.entityNumber > 0) {
            ss << ",\"entityNumber\":" << message.entityNumber;
        }
        ss << "}";
    }
    ss << "]";
    ss << "}";
    return ss.str();
}

StepImportRunResult runStepImport(const std::string& content, const StepImportOptions& options) {
    StepImportRunResult result;

    if (content.empty() || content.find_first_not_of(" \t\r\n") == std::string::npos) {
        result.readStatus = "IFSelect_RetError";
        result.transferStatus = "FAILED";
        addStepImportMessage(result.messages, "load", "fail", "STEP content is empty");
        return result;
    }

    try {
        TCollection_AsciiString tmpPath("/tmp/occt_import_tmp.step");
        {
            std::ofstream ofs(tmpPath.ToCString());
            if (!ofs) {
                result.readStatus = "IFSelect_RetError";
                result.transferStatus = "FAILED";
                addStepImportMessage(result.messages,
                                     "load",
                                     "fail",
                                     "Cannot create temporary file for STEP import");
                return result;
            }
            ofs << content;
        }

        STEPControl_Reader reader;
        const IFSelect_ReturnStatus readStatus = reader.ReadFile(tmpPath.ToCString());
        result.readStatus = stepReturnStatusToString(readStatus);

        Handle(XSControl_WorkSession) workSession = reader.WS();
        if (!workSession.IsNull()) {
            collectCheckMessages(workSession->ModelCheckList(), "load", result.messages);
        }

        if (readStatus != IFSelect_RetDone) {
            result.transferStatus = "FAILED";
            if (!hasFailureMessage(result)) {
                addStepImportMessage(result.messages,
                                     "load",
                                     "fail",
                                     "STEPControl_Reader::ReadFile returned " + result.readStatus);
            }
            return result;
        }

        result.rootCount = reader.NbRootsForTransfer();
        result.transferredRootCount = reader.TransferRoots();

        if (!workSession.IsNull() && !workSession->TransferReader().IsNull()) {
            collectCheckMessages(workSession->TransferReader()->LastCheckList(), "transfer", result.messages);
        }

        result.transferStatus = stepTransferStatusToString(result.rootCount,
                                                           result.transferredRootCount,
                                                           reader.NbShapes());

        if (reader.NbShapes() == 0) {
            if (!hasFailureMessage(result)) {
                addStepImportMessage(result.messages,
                                     "transfer",
                                     "fail",
                                     "No shapes found in STEP file");
            }
            return result;
        }

        TopoDS_Shape importedShape = reader.OneShape();
        if (importedShape.IsNull()) {
            result.transferStatus = "FAILED";
            addStepImportMessage(result.messages,
                                 "transfer",
                                 "fail",
                                 "STEP translation produced a null shape");
            return result;
        }

        result.shape = applyImportHealing(importedShape, options, result);
        result.hasShape = !result.shape.IsNull();
        return result;
    } catch (const Standard_Failure& sf) {
        result.transferStatus = "FAILED";
        addStepImportMessage(result.messages,
                             "transfer",
                             "fail",
                             sf.what());
        return result;
    } catch (const std::exception& ex) {
        result.transferStatus = "FAILED";
        addStepImportMessage(result.messages,
                             "transfer",
                             "fail",
                             ex.what());
        return result;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// PIMPL
// ---------------------------------------------------------------------------

struct OcctKernel::Impl {
    std::unordered_map<uint32_t, ShapeRecord> records;
    std::unordered_map<std::string, uint32_t> revisionToHandle;
    uint32_t nextId = 1;
};

// ---------------------------------------------------------------------------
// Ctor / dtor
// ---------------------------------------------------------------------------

OcctKernel::OcctKernel() : _impl(new Impl()) {}
OcctKernel::~OcctKernel() { delete _impl; }

// ---------------------------------------------------------------------------
// Handle management
// ---------------------------------------------------------------------------

uint32_t OcctKernel::storeShape(const TopoDS_Shape& shape) {
    return storeShapeWithMetadata(shape,
                                  "unknown",
                                  "",
                                  {},
                                  "unresolved",
                                  "unresolved",
                                  { "No operation metadata was supplied for this revision" });
}

uint32_t OcctKernel::storeShapeWithMetadata(const TopoDS_Shape& shape,
                                            const std::string& operationType,
                                            const std::string& parameterSignature,
                                            const std::vector<std::string>& operandRevisionIds,
                                            const std::string& entityStatus,
                                            const std::string& identityStatus,
                                            const std::vector<std::string>& warnings) {
    if (shape.IsNull()) {
        throwKernelError("OPERATION_FAILED", "Produced a null shape");
    }

    RevisionMetadata revision = makeRevisionMetadata(shape,
                                                     operationType,
                                                     parameterSignature,
                                                     operandRevisionIds,
                                                     entityStatus,
                                                     identityStatus,
                                                     warnings);

    uint32_t id = _impl->nextId++;
    ShapeRecord record;
    record.shape = shape;
    record.revision = revision;
    record.refCount = 1;
    _impl->records[id] = record;
    _impl->revisionToHandle[revision.revisionId] = id;
    return id;
}

const TopoDS_Shape& OcctKernel::requireShape(uint32_t id) const {
    auto it = _impl->records.find(id);
    if (it == _impl->records.end()) {
        throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
    }
    return it->second.shape;
}

std::string OcctKernel::requireRevisionId(uint32_t id) const {
    auto it = _impl->records.find(id);
    if (it == _impl->records.end()) {
        throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
    }
    return it->second.revision.revisionId;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

uint32_t OcctKernel::createBox(double dx, double dy, double dz) {
    if (dx <= 0 || dy <= 0 || dz <= 0) {
        throwKernelError("INVALID_PARAMS", "Box dimensions must be > 0");
    }
    try {
        BRepPrimAPI_MakeBox mkBox(dx, dy, dz);
        mkBox.Build();
        if (!mkBox.IsDone()) {
            throwKernelError("OPERATION_FAILED", "BRepPrimAPI_MakeBox failed");
        }
        return storeShapeWithMetadata(mkBox.Shape(),
                                      "createBox",
                                      "dx=" + std::to_string(dx) + ";dy=" + std::to_string(dy) + ";dz=" + std::to_string(dz),
                                      {},
                                      "generated",
                                      "generated",
                                      {});
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0; // unreachable
}

uint32_t OcctKernel::createCylinder(double radius, double height) {
    if (radius <= 0 || height <= 0) {
        throwKernelError("INVALID_PARAMS", "Cylinder radius and height must be > 0");
    }
    try {
        BRepPrimAPI_MakeCylinder mkCyl(radius, height);
        mkCyl.Build();
        if (!mkCyl.IsDone()) {
            throwKernelError("OPERATION_FAILED", "BRepPrimAPI_MakeCylinder failed");
        }
        return storeShapeWithMetadata(mkCyl.Shape(),
                                      "createCylinder",
                                      "radius=" + std::to_string(radius) + ";height=" + std::to_string(height),
                                      {},
                                      "generated",
                                      "generated",
                                      {});
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

uint32_t OcctKernel::createSphere(double radius) {
    if (radius <= 0) {
        throwKernelError("INVALID_PARAMS", "Sphere radius must be > 0");
    }
    try {
        BRepPrimAPI_MakeSphere mkSph(radius);
        mkSph.Build();
        if (!mkSph.IsDone()) {
            throwKernelError("OPERATION_FAILED", "BRepPrimAPI_MakeSphere failed");
        }
        return storeShapeWithMetadata(mkSph.Shape(),
                                      "createSphere",
                                      "radius=" + std::to_string(radius),
                                      {},
                                      "generated",
                                      "generated",
                                      {});
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Sketch-based features
// ---------------------------------------------------------------------------

uint32_t OcctKernel::extrudeProfile(const std::string& profileJson, const std::string& optionsJson) {
    try {
        const ExtrudeOptions options = parseExtrudeOptions(optionsJson);
        TopoDS_Face face = placeProfileFace(buildFaceFromProfile(profileJson), options.plane);
        const gp_Vec dir = makeExtrusionVector(options);
        BRepPrimAPI_MakePrism mkPrism(face, dir);
        mkPrism.Build();
        if (!mkPrism.IsDone()) {
            throwKernelError("OPERATION_FAILED", "BRepPrimAPI_MakePrism failed");
        }
        return storeShapeWithMetadata(mkPrism.Shape(),
                                      "extrudeProfile",
                                      profileJson + "|" + optionsJson,
                                      {},
                                      "generated",
                                      "generated",
                                      {});
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

uint32_t OcctKernel::performStructuredExtrudeFeature(uint32_t id,
                                                     const std::string& profileJson,
                                                     const std::string& specJson,
                                                     const std::string& operationType,
                                                     int fuseMode) {
    const std::string pathRoot = operationType == "extrudeCutProfile" ? "extrudeCut" : "extrude";
    auto sourceIt = _impl->records.find(id);
    if (sourceIt == _impl->records.end()) {
        throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
    }

    const StructuredExtrudeSpec spec = parseStructuredExtrudeSpec(specJson, operationType, pathRoot);
    const TopoDS_Shape& baseShape = sourceIt->second.shape;
    const RevisionMetadata* baseRevision = &sourceIt->second.revision;
    TopoDS_Face profileFace = placeProfileFace(buildFaceFromProfile(profileJson), spec.plane);

    TopoDS_Shape surfaceLimitShape;
    const TopoDS_Shape* surfaceLimitShapePtr = nullptr;
    if (spec.hasSurfaceTarget) {
        const TopoDS_Shape* targetShape = &baseShape;
        const RevisionMetadata* targetRevision = baseRevision;
        if (spec.surfaceTarget.hasShapeId) {
            auto targetIt = _impl->records.find(spec.surfaceTarget.shapeId);
            if (targetIt == _impl->records.end()) {
                throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(spec.surfaceTarget.shapeId));
            }
            targetShape = &targetIt->second.shape;
            targetRevision = &targetIt->second.revision;
        }
        const ResolvedFaceRef targetFace = resolveFaceRef(*targetShape,
                                                          targetRevision,
                                                          spec.surfaceTarget.faceRef,
                                                          operationType,
                                                          pathRoot + ".extent.surface.face");
        surfaceLimitShape = targetFace.face;
        surfaceLimitShapePtr = &surfaceLimitShape;
    }

    TopoDS_Shape resultShape;
    if (spec.hasDraftAngle) {
        BRepFeat_MakeDPrism feature;
        feature.Init(baseShape, profileFace, profileFace, spec.draftAngleRadians, fuseMode, true);
        resultShape = performStructuredExtrudeExtent(feature,
                                                     spec,
                                                     baseShape,
                                                     surfaceLimitShapePtr,
                                                     operationType);
    } else {
        BRepFeat_MakePrism feature;
        feature.Init(baseShape, profileFace, profileFace, spec.direction, fuseMode, true);
        resultShape = performStructuredExtrudeExtent(feature,
                                                     spec,
                                                     baseShape,
                                                     surfaceLimitShapePtr,
                                                     operationType);
    }

    return storeShapeWithMetadata(resultShape,
                                  operationType,
                                  "source=" + baseRevision->revisionId + ";" + profileJson + "|" + specJson,
                                  { baseRevision->revisionId },
                                  "unresolved",
                                  "unresolved",
                                  { "Structured extrude feature lineage is not yet proven; generated/modified/deleted identity is reported as unresolved" });
}

uint32_t OcctKernel::extrudeProfileWithSpec(uint32_t id, const std::string& profileJson, const std::string& specJson) {
    try {
        return performStructuredExtrudeFeature(id, profileJson, specJson, "extrudeProfile", 1);
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

uint32_t OcctKernel::extrudeCutProfileWithSpec(uint32_t id, const std::string& profileJson, const std::string& specJson) {
    try {
        return performStructuredExtrudeFeature(id, profileJson, specJson, "extrudeCutProfile", 0);
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

uint32_t OcctKernel::revolveProfile(const std::string& profileJson, const std::string& optionsJson) {
    try {
        const RevolveOptions options = parseRevolveOptions(optionsJson);
        TopoDS_Face face = buildFaceFromProfile(profileJson);
        gp_Ax1 axis(options.axisOrigin, options.axisDirection);
        const double angleRad = options.angleDegrees * M_PI / 180.0;
        BRepPrimAPI_MakeRevol mkRevol(face, axis, angleRad);
        mkRevol.Build();
        if (!mkRevol.IsDone()) {
            throwKernelError("OPERATION_FAILED", "BRepPrimAPI_MakeRevol failed");
        }
        return storeShapeWithMetadata(mkRevol.Shape(),
                                      "revolveProfile",
                                      profileJson + "|" + optionsJson,
                                      {},
                                      "generated",
                                      "generated",
                                      {});
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

uint32_t OcctKernel::performStructuredRevolveFeature(uint32_t id,
                                                     const std::string& profileJson,
                                                     const std::string& specJson,
                                                     const std::string& operationType,
                                                     int fuseMode) {
    const std::string pathRoot = operationType == "revolveCutProfile" ? "revolveCut" : "revolve";
    auto sourceIt = _impl->records.find(id);
    if (sourceIt == _impl->records.end()) {
        throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
    }

    const StructuredRevolveSpec spec = parseStructuredRevolveSpec(specJson, operationType, pathRoot);
    const TopoDS_Shape& baseShape = sourceIt->second.shape;
    const RevisionMetadata* baseRevision = &sourceIt->second.revision;
    TopoDS_Face profileFace = placeProfileFace(buildFaceFromProfile(profileJson), spec.plane);

    auto resolveSurfaceTarget = [&](const StructuredExtrudeSurfaceTarget& target,
                                    const std::string& facePath) -> TopoDS_Shape {
        const TopoDS_Shape* targetShape = &baseShape;
        const RevisionMetadata* targetRevision = baseRevision;
        if (target.hasShapeId) {
            auto targetIt = _impl->records.find(target.shapeId);
            if (targetIt == _impl->records.end()) {
                throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(target.shapeId));
            }
            targetShape = &targetIt->second.shape;
            targetRevision = &targetIt->second.revision;
        }
        const ResolvedFaceRef targetFace = resolveFaceRef(*targetShape,
                                                          targetRevision,
                                                          target.faceRef,
                                                          operationType,
                                                          facePath);
        return targetFace.face;
    };

    TopoDS_Shape surfaceLimitShape;
    const TopoDS_Shape* surfaceLimitShapePtr = nullptr;
    if (spec.hasSurfaceTarget) {
        surfaceLimitShape = resolveSurfaceTarget(spec.surfaceTarget, pathRoot + ".extent.surface.face");
        surfaceLimitShapePtr = &surfaceLimitShape;
    }

    TopoDS_Shape fromSurfaceShape;
    const TopoDS_Shape* fromSurfaceShapePtr = nullptr;
    if (spec.hasFromSurface) {
        fromSurfaceShape = resolveSurfaceTarget(spec.fromSurfaceTarget, pathRoot + ".extent.fromSurface.face");
        fromSurfaceShapePtr = &fromSurfaceShape;
    }

    TopoDS_Shape untilSurfaceShape;
    const TopoDS_Shape* untilSurfaceShapePtr = nullptr;
    if (spec.hasUntilSurface) {
        untilSurfaceShape = resolveSurfaceTarget(spec.untilSurfaceTarget, pathRoot + ".extent.untilSurface.face");
        untilSurfaceShapePtr = &untilSurfaceShape;
    }

    gp_Ax1 axis(spec.axisOrigin, spec.axisDirection);
    BRepFeat_MakeRevol feature;
    feature.Init(baseShape, profileFace, profileFace, axis, fuseMode, true);

    if (!spec.slidingEdges.empty()) {
        ShapeMap profileEdges;
        TopExp::MapShapes(profileFace, TopAbs_EDGE, profileEdges);
        for (std::size_t index = 0; index < spec.slidingEdges.size(); ++index) {
            const StructuredRevolveSlidingEdge& slidingEdge = spec.slidingEdges[index];
            if (slidingEdge.profileEdgeIndex < 1 || slidingEdge.profileEdgeIndex > profileEdges.Extent()) {
                throwStructuredValidation(operationType,
                                          pathRoot + ".slidingEdges[" + std::to_string(index) + "].profileEdgeIndex",
                                          "Profile edge index is outside the current sketch topology");
            }
            const ResolvedFaceRef onFace = resolveFaceRef(baseShape,
                                                         baseRevision,
                                                         slidingEdge.faceRef,
                                                         operationType,
                                                         pathRoot + ".slidingEdges[" + std::to_string(index) + "].face");
            feature.Add(TopoDS::Edge(profileEdges(slidingEdge.profileEdgeIndex)), onFace.face);
        }
    }

    const TopoDS_Shape resultShape = performStructuredRevolveExtent(feature,
                                                                    spec,
                                                                    surfaceLimitShapePtr,
                                                                    fromSurfaceShapePtr,
                                                                    untilSurfaceShapePtr,
                                                                    operationType);

    return storeShapeWithMetadata(resultShape,
                                  operationType,
                                  "source=" + baseRevision->revisionId + ";" + profileJson + "|" + specJson,
                                  { baseRevision->revisionId },
                                  "unresolved",
                                  "unresolved",
                                  { "Structured revolve feature lineage is not yet proven; generated/modified/deleted identity is reported as unresolved" });
}

uint32_t OcctKernel::revolveProfileWithSpec(uint32_t id, const std::string& profileJson, const std::string& specJson) {
    try {
        return performStructuredRevolveFeature(id, profileJson, specJson, "revolveProfile", 1);
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

uint32_t OcctKernel::revolveCutProfileWithSpec(uint32_t id, const std::string& profileJson, const std::string& specJson) {
    try {
        return performStructuredRevolveFeature(id, profileJson, specJson, "revolveCutProfile", 0);
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Booleans
// ---------------------------------------------------------------------------

static uint32_t runBoolean(
    OcctKernel* self,
    uint32_t id1, uint32_t id2,
    const char* opName,
    BRepAlgoAPI_BooleanOperation& op
) {
    (void)self;
    (void)opName;
    op.Build();
    if (!op.IsDone() || op.Shape().IsNull()) {
        throwKernelError("OPERATION_FAILED",
            std::string(opName) + " failed or produced a null shape");
    }
    return 0; // caller replaces this
}

uint32_t OcctKernel::booleanUnion(uint32_t id1, uint32_t id2) {
    try {
        const TopoDS_Shape& s1 = requireShape(id1);
        const TopoDS_Shape& s2 = requireShape(id2);
        BRepAlgoAPI_Fuse op(s1, s2);
        runBoolean(this, id1, id2, "BooleanUnion", op);
        return storeShapeWithMetadata(op.Shape(),
                          "booleanUnion",
                          "base=" + requireRevisionId(id1) + ";tool=" + requireRevisionId(id2),
                          { requireRevisionId(id1), requireRevisionId(id2) },
                          "unresolved",
                          "unresolved",
                          { "Boolean subshape lineage is not yet proven; generated/modified/deleted identity is reported as unresolved" });
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

uint32_t OcctKernel::booleanSubtract(uint32_t id1, uint32_t id2) {
    try {
        const TopoDS_Shape& s1 = requireShape(id1);
        const TopoDS_Shape& s2 = requireShape(id2);
        BRepAlgoAPI_Cut op(s1, s2);
        runBoolean(this, id1, id2, "BooleanSubtract", op);
        return storeShapeWithMetadata(op.Shape(),
                          "booleanSubtract",
                          "base=" + requireRevisionId(id1) + ";tool=" + requireRevisionId(id2),
                          { requireRevisionId(id1), requireRevisionId(id2) },
                          "unresolved",
                          "unresolved",
                          { "Boolean subshape lineage is not yet proven; generated/modified/deleted identity is reported as unresolved" });
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

uint32_t OcctKernel::booleanIntersect(uint32_t id1, uint32_t id2) {
    try {
        const TopoDS_Shape& s1 = requireShape(id1);
        const TopoDS_Shape& s2 = requireShape(id2);
        BRepAlgoAPI_Common op(s1, s2);
        runBoolean(this, id1, id2, "BooleanIntersect", op);
        return storeShapeWithMetadata(op.Shape(),
                          "booleanIntersect",
                          "base=" + requireRevisionId(id1) + ";tool=" + requireRevisionId(id2),
                          { requireRevisionId(id1), requireRevisionId(id2) },
                          "unresolved",
                          "unresolved",
                          { "Boolean subshape lineage is not yet proven; generated/modified/deleted identity is reported as unresolved" });
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Modifiers
// ---------------------------------------------------------------------------

uint32_t OcctKernel::filletEdges(uint32_t id, double radius) {
    if (radius <= 0) {
        throwKernelError("INVALID_PARAMS", "Fillet radius must be > 0");
    }
    try {
        const TopoDS_Shape& shape = requireShape(id);
        BRepFilletAPI_MakeFillet mkFillet(shape);
        for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next()) {
            mkFillet.Add(radius, TopoDS::Edge(ex.Current()));
        }
        mkFillet.Build();
        if (!mkFillet.IsDone()) {
            throwKernelError("OPERATION_FAILED", "BRepFilletAPI_MakeFillet failed");
        }
        return storeShapeWithMetadata(mkFillet.Shape(),
                                      "filletEdges",
                                      "source=" + requireRevisionId(id) + ";radius=" + std::to_string(radius),
                                      { requireRevisionId(id) },
                                      "unresolved",
                                      "unresolved",
                                      { "Fillet subshape lineage is not yet proven; generated/modified/deleted identity is reported as unresolved" });
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

uint32_t OcctKernel::chamferEdges(uint32_t id, double distance) {
    if (distance <= 0) {
        throwKernelError("INVALID_PARAMS", "Chamfer distance must be > 0");
    }
    try {
        const TopoDS_Shape& shape = requireShape(id);
        BRepFilletAPI_MakeChamfer mkChamfer(shape);
        ShapeMap edgeMap, faceMap;
        TopExp::MapShapes(shape, TopAbs_EDGE, edgeMap);
        TopExp::MapShapes(shape, TopAbs_FACE, faceMap);

        // Associate each edge with an adjacent face for the chamfer operation.
        for (int i = 1; i <= edgeMap.Extent(); ++i) {
            const TopoDS_Edge& edge = TopoDS::Edge(edgeMap(i));
            // Find a face adjacent to this edge
            for (int j = 1; j <= faceMap.Extent(); ++j) {
                TopExp_Explorer ex(faceMap(j), TopAbs_EDGE);
                for (; ex.More(); ex.Next()) {
                    if (ex.Current().IsSame(edge)) {
                        mkChamfer.Add(distance, distance, edge, TopoDS::Face(faceMap(j)));
                        goto nextEdge;
                    }
                }
            }
            nextEdge:;
        }
        mkChamfer.Build();
        if (!mkChamfer.IsDone()) {
            throwKernelError("OPERATION_FAILED", "BRepFilletAPI_MakeChamfer failed");
        }
        return storeShapeWithMetadata(mkChamfer.Shape(),
                                      "chamferEdges",
                                      "source=" + requireRevisionId(id) + ";distance=" + std::to_string(distance),
                                      { requireRevisionId(id) },
                                      "unresolved",
                                      "unresolved",
                                      { "Chamfer subshape lineage is not yet proven; generated/modified/deleted identity is reported as unresolved" });
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

std::string OcctKernel::filletEdgesWithSpec(uint32_t id, const std::string& specJson) {
    const std::string operation = "filletEdges";
    try {
        auto sourceIt = _impl->records.find(id);
        if (sourceIt == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }
        const TopoDS_Shape& shape = sourceIt->second.shape;
        const RevisionMetadata* sourceRevision = &sourceIt->second.revision;

        const mini_json::Value root = mini_json::requireObject(mini_json::parse(specJson), "fillet spec");
        requireSchemaVersion(root, operation);
        const bool allowUnknownFields = optionalBoolMember(root, "allowUnknownFields", false);
        rejectUnknownFields(root,
                            { "schemaVersion", "allowUnknownFields", "unit", "edges", "radius", "radiusMode", "startRadius", "endRadius", "stations", "law", "tangentPropagation", "limits", "cornerMode", "blendShape", "continuity", "angularTolerance", "overflowMode", "metadata" },
                            operation,
                            "fillet",
                            allowUnknownFields);

        if (hasMember(root, "limits")) {
            throwStructuredValidation(operation, "fillet.limits", "Partial-edge fillets are not exposed by this OCCT build", "fillet.partialEdge");
        }
        if (optionalStringMember(root, "cornerMode", "rollingBall") != "rollingBall") {
            throwStructuredValidation(operation, "fillet.cornerMode", "Only rollingBall corner handling is supported by this OCCT build", "fillet.cornerModes");
        }
        if (optionalStringMember(root, "overflowMode", "fail") != "fail") {
            throwStructuredValidation(operation, "fillet.overflowMode", "Only fail-fast overflow handling is supported", "fillet.overflowModes");
        }
        if (!optionalBoolMember(root, "tangentPropagation", true)) {
            throwStructuredValidation(operation, "fillet.tangentPropagation", "Disabling tangent propagation is not exposed by this OCCT build", "fillet.nonPropagatingEdges");
        }

        BRepFilletAPI_MakeFillet mkFillet(shape, parseFilletShape(optionalStringMember(root, "blendShape", "rational"), operation, "fillet.blendShape"));
        const std::string continuity = optionalStringMember(root, "continuity", "C1");
        const double angularTolerance = optionalNumberMember(root, "angularTolerance", 1.0e-2);
        if (!std::isfinite(angularTolerance) || angularTolerance <= 0.0) {
            throwStructuredValidation(operation, "fillet.angularTolerance", "angularTolerance must be finite and > 0");
        }
        mkFillet.SetContinuity(parseContinuity(continuity, operation, "fillet.continuity"), angularTolerance);

        std::vector<ResolvedEdgeRef> appliedEdges;
        std::vector<std::string> normalizedParameters;
        auto addFilletForEdge = [&](const mini_json::Value& entryValue, const std::string& path) {
            const mini_json::Value& entry = mini_json::requireObject(entryValue, path);
            rejectUnknownFields(entry,
                                { "edge", "edgeRef", "topoId", "stableHash", "radius", "radiusMode", "startRadius", "endRadius", "stations", "law", "tangentPropagation", "limits", "cornerMode", "blendShape", "continuity", "angularTolerance", "overflowMode", "metadata" },
                                operation,
                                path,
                                allowUnknownFields);
            if (hasMember(entry, "limits")) {
                throwStructuredValidation(operation, path + ".limits", "Partial-edge fillets are not exposed by this OCCT build", "fillet.partialEdge");
            }
            if (!optionalBoolMember(entry, "tangentPropagation", true)) {
                throwStructuredValidation(operation, path + ".tangentPropagation", "Disabling tangent propagation is not exposed by this OCCT build", "fillet.nonPropagatingEdges");
            }
            if (optionalStringMember(entry, "cornerMode", optionalStringMember(root, "cornerMode", "rollingBall")) != "rollingBall") {
                throwStructuredValidation(operation, path + ".cornerMode", "Only rollingBall corner handling is supported by this OCCT build", "fillet.cornerModes");
            }

            const mini_json::Value* edgeRefValue = entry.get("edge");
            if (edgeRefValue == nullptr) edgeRefValue = entry.get("edgeRef");
            const mini_json::Value& effectiveEdgeRef = edgeRefValue == nullptr ? entry : *edgeRefValue;
            ResolvedEdgeRef edge = resolveEdgeRef(shape, sourceRevision, effectiveEdgeRef, operation, path + ".edge");

            const mini_json::Value* lawValue = entry.get("law");
            const mini_json::Value* stationValue = entry.get("stations");
            if (lawValue == nullptr) lawValue = root.get("law");
            if (stationValue == nullptr) stationValue = root.get("stations");
            std::string mode = optionalStringMember(entry, "radiusMode");
            if (mode.empty()) {
                if (lawValue != nullptr) mode = "law";
                else if (stationValue != nullptr || hasMember(entry, "startRadius") || hasMember(entry, "endRadius") || hasMember(root, "startRadius") || hasMember(root, "endRadius")) mode = "variable";
                else mode = "constant";
            }

            std::ostringstream normalized;
            normalized << "{";
            normalized << "\"mode\":"; appendJsonString(normalized, mode); normalized << ",";

            if (mode == "constant") {
                const double radius = hasMember(entry, "radius")
                    ? requirePositiveMember(entry, "radius", operation, path + ".radius")
                    : requirePositiveMember(root, "radius", operation, "fillet.radius");
                mkFillet.Add(radius, edge.edge);
                normalized << "\"radius\":" << radius;
            } else if (mode == "variable" || mode == "startEnd") {
                if (stationValue != nullptr) {
                    const mini_json::Value& stations = mini_json::requireArray(*stationValue, path + ".stations");
                    if (stations.array.size() < 2) {
                        throwStructuredValidation(operation, path + ".stations", "At least two radius stations are required");
                    }
                    Point2dArray table(1, static_cast<int>(stations.array.size()));
                    double previousT = -1.0;
                    normalized << "\"stations\":[";
                    for (std::size_t i = 0; i < stations.array.size(); ++i) {
                        const mini_json::Value& station = mini_json::requireObject(stations.array[i], path + ".stations[]");
                        const double t = optionalNumberMember(station, "t", optionalNumberMember(station, "u", -1.0));
                        const double radius = requirePositiveMember(station, "radius", operation, path + ".stations[].radius");
                        if (!std::isfinite(t) || t < 0.0 || t > 1.0 || t <= previousT) {
                            throwStructuredValidation(operation, path + ".stations[].t", "Station t values must be finite, increasing, and in [0, 1]");
                        }
                        previousT = t;
                        table.SetValue(static_cast<int>(i + 1), gp_Pnt2d(t, radius));
                        if (i > 0) normalized << ',';
                        normalized << "{\"t\":" << t << ",\"radius\":" << radius << "}";
                    }
                    normalized << "]";
                    mkFillet.Add(table, edge.edge);
                } else {
                    const double startRadius = hasMember(entry, "startRadius")
                        ? requirePositiveMember(entry, "startRadius", operation, path + ".startRadius")
                        : requirePositiveMember(root, "startRadius", operation, "fillet.startRadius");
                    const double endRadius = hasMember(entry, "endRadius")
                        ? requirePositiveMember(entry, "endRadius", operation, path + ".endRadius")
                        : requirePositiveMember(root, "endRadius", operation, "fillet.endRadius");
                    mkFillet.Add(startRadius, endRadius, edge.edge);
                    normalized << "\"startRadius\":" << startRadius << ",\"endRadius\":" << endRadius;
                }
            } else if (mode == "law") {
                if (lawValue == nullptr) {
                    throwStructuredValidation(operation, path + ".law", "law radiusMode requires a law object");
                }
                const mini_json::Value& law = mini_json::requireObject(*lawValue, path + ".law");
                const std::string lawType = optionalStringMember(law, "type", "linear");
                if (lawType == "constant") {
                    const double radius = requirePositiveMember(law, "radius", operation, path + ".law.radius");
                    mkFillet.Add(radius, edge.edge);
                    normalized << "\"law\":{\"type\":\"constant\",\"radius\":" << radius << "}";
                } else if (lawType == "linear") {
                    const double startRadius = requirePositiveMember(law, "startRadius", operation, path + ".law.startRadius");
                    const double endRadius = requirePositiveMember(law, "endRadius", operation, path + ".law.endRadius");
                    mkFillet.Add(startRadius, endRadius, edge.edge);
                    normalized << "\"law\":{\"type\":\"linear\",\"startRadius\":" << startRadius << ",\"endRadius\":" << endRadius << "}";
                } else {
                    throwStructuredValidation(operation, path + ".law.type", "Only constant and linear radius laws are supported", "fillet.radiusLawTypes");
                }
            } else {
                throwStructuredValidation(operation, path + ".radiusMode", "Unsupported fillet radiusMode", "fillet.radiusModes");
            }

            normalized << ",\"tangentPropagation\":true}";
            appliedEdges.push_back(edge);
            normalizedParameters.push_back(normalized.str());
        };

        if (const mini_json::Value* edgesValue = root.get("edges")) {
            const mini_json::Value& edges = mini_json::requireArray(*edgesValue, "fillet.edges");
            if (edges.array.empty()) {
                throwStructuredValidation(operation, "fillet.edges", "At least one edge selection is required");
            }
            for (std::size_t i = 0; i < edges.array.size(); ++i) {
                addFilletForEdge(edges.array[i], "fillet.edges[" + std::to_string(i) + "]");
            }
        } else {
            ShapeMap edges;
            TopExp::MapShapes(shape, TopAbs_EDGE, edges);
            if (edges.Extent() == 0) {
                throwStructuredValidation(operation, "fillet.edges", "Shape has no edges to fillet");
            }
            for (int i = 1; i <= edges.Extent(); ++i) {
                mini_json::Value entry;
                entry.kind = mini_json::Value::Kind::Object;
                mini_json::Value topo;
                topo.kind = mini_json::Value::Kind::Number;
                topo.number = static_cast<double>(i);
                entry.object.push_back({ "topoId", topo });
                addFilletForEdge(entry, "fillet.edges[" + std::to_string(i - 1) + "]");
            }
        }

        mkFillet.Build();
        if (!mkFillet.IsDone() || mkFillet.Shape().IsNull()) {
            std::vector<std::string> failing;
            for (const ResolvedEdgeRef& edge : appliedEdges) failing.push_back(stableEdgeRefString(edge));
            throwStructuredOperationError(operation, "BRepFilletAPI_MakeFillet failed", failing);
        }

        const uint32_t resultId = storeShapeWithMetadata(mkFillet.Shape(),
                                                         "filletEdges",
                                                         "source=" + sourceRevision->revisionId + ";spec=" + fnv1a64(objectToStableSignature(root)),
                                                         { sourceRevision->revisionId },
                                                         "unresolved",
                                                         "unresolved",
                                                         { "Fillet generated/modified/deleted lineage is reported from OCCT history when available; unresolved entries are explicit in the result payload" });
        auto resultIt = _impl->records.find(resultId);
        if (resultIt != _impl->records.end()) {
            for (const ResolvedEdgeRef& edge : appliedEdges) {
                resultIt->second.revision.deletedEntities.push_back({ "edge", edge.stableHash, resultIt->second.revision.operationId, "unresolved" });
            }
        }
        return makeBlendResultJson(this, resultId, mkFillet, shape, appliedEdges, normalizedParameters, "filletFace");
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwStructuredOperationError(operation, sf.what());
    }
    return "{}";
}

std::string OcctKernel::chamferEdgesWithSpec(uint32_t id, const std::string& specJson) {
    const std::string operation = "chamferEdges";
    try {
        auto sourceIt = _impl->records.find(id);
        if (sourceIt == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }
        const TopoDS_Shape& shape = sourceIt->second.shape;
        const RevisionMetadata* sourceRevision = &sourceIt->second.revision;

        const mini_json::Value root = mini_json::requireObject(mini_json::parse(specJson), "chamfer spec");
        requireSchemaVersion(root, operation);
        const bool allowUnknownFields = optionalBoolMember(root, "allowUnknownFields", false);
        rejectUnknownFields(root,
                            { "schemaVersion", "allowUnknownFields", "unit", "edges", "mode", "distance", "distance1", "distance2", "angleRadians", "angleDegrees", "referenceFace", "tangentPropagation", "limits", "cornerMode", "overflowMode", "metadata" },
                            operation,
                            "chamfer",
                            allowUnknownFields);
        if (hasMember(root, "limits")) {
            throwStructuredValidation(operation, "chamfer.limits", "Partial-edge chamfers are not exposed by this OCCT build", "chamfer.partialEdge");
        }
        if (!optionalBoolMember(root, "tangentPropagation", true)) {
            throwStructuredValidation(operation, "chamfer.tangentPropagation", "Disabling tangent propagation is not exposed by this OCCT build", "chamfer.nonPropagatingEdges");
        }
        if (optionalStringMember(root, "cornerMode", "rollingBall") != "rollingBall") {
            throwStructuredValidation(operation, "chamfer.cornerMode", "Only rollingBall corner handling is supported by this OCCT build", "chamfer.cornerModes");
        }
        if (optionalStringMember(root, "overflowMode", "fail") != "fail") {
            throwStructuredValidation(operation, "chamfer.overflowMode", "Only fail-fast overflow handling is supported", "chamfer.overflowModes");
        }

        BRepFilletAPI_MakeChamfer mkChamfer(shape);
        std::vector<ResolvedEdgeRef> appliedEdges;
        std::vector<std::string> normalizedParameters;

        auto addChamferForEdge = [&](const mini_json::Value& entryValue, const std::string& path) {
            const mini_json::Value& entry = mini_json::requireObject(entryValue, path);
            rejectUnknownFields(entry,
                                { "edge", "edgeRef", "topoId", "stableHash", "mode", "distance", "distance1", "distance2", "angleRadians", "angleDegrees", "referenceFace", "tangentPropagation", "limits", "cornerMode", "overflowMode", "metadata" },
                                operation,
                                path,
                                allowUnknownFields);
            if (hasMember(entry, "limits")) {
                throwStructuredValidation(operation, path + ".limits", "Partial-edge chamfers are not exposed by this OCCT build", "chamfer.partialEdge");
            }
            if (!optionalBoolMember(entry, "tangentPropagation", true)) {
                throwStructuredValidation(operation, path + ".tangentPropagation", "Disabling tangent propagation is not exposed by this OCCT build", "chamfer.nonPropagatingEdges");
            }
            if (optionalStringMember(entry, "cornerMode", optionalStringMember(root, "cornerMode", "rollingBall")) != "rollingBall") {
                throwStructuredValidation(operation, path + ".cornerMode", "Only rollingBall corner handling is supported by this OCCT build", "chamfer.cornerModes");
            }

            const mini_json::Value* edgeRefValue = entry.get("edge");
            if (edgeRefValue == nullptr) edgeRefValue = entry.get("edgeRef");
            const mini_json::Value& effectiveEdgeRef = edgeRefValue == nullptr ? entry : *edgeRefValue;
            ResolvedEdgeRef edge = resolveEdgeRef(shape, sourceRevision, effectiveEdgeRef, operation, path + ".edge");
            const std::string mode = optionalStringMember(entry, "mode", optionalStringMember(root, "mode", "symmetric"));

            std::ostringstream normalized;
            normalized << "{";
            normalized << "\"mode\":"; appendJsonString(normalized, mode); normalized << ",";

            if (mode == "symmetric") {
                const double distance = hasMember(entry, "distance")
                    ? requirePositiveMember(entry, "distance", operation, path + ".distance")
                    : requirePositiveMember(root, "distance", operation, "chamfer.distance");
                mkChamfer.Add(distance, edge.edge);
                normalized << "\"distance\":" << distance;
            } else if (mode == "twoDistance") {
                const double distance1 = hasMember(entry, "distance1")
                    ? requirePositiveMember(entry, "distance1", operation, path + ".distance1")
                    : requirePositiveMember(root, "distance1", operation, "chamfer.distance1");
                const double distance2 = hasMember(entry, "distance2")
                    ? requirePositiveMember(entry, "distance2", operation, path + ".distance2")
                    : requirePositiveMember(root, "distance2", operation, "chamfer.distance2");
                const mini_json::Value* referenceFaceValue = entry.get("referenceFace");
                if (referenceFaceValue == nullptr) referenceFaceValue = root.get("referenceFace");
                if (referenceFaceValue == nullptr) {
                    throwStructuredValidation(operation, path + ".referenceFace", "twoDistance chamfers require a referenceFace for the distance1 side");
                }
                ResolvedFaceRef referenceFace = resolveFaceRef(shape, sourceRevision, *referenceFaceValue, operation, path + ".referenceFace");
                mkChamfer.Add(distance1, distance2, edge.edge, referenceFace.face);
                normalized << "\"distance1\":" << distance1 << ",\"distance2\":" << distance2 << ",\"referenceFace\":";
                appendEntityRefJson(normalized, referenceFace);
            } else if (mode == "distanceAngle") {
                const double distance = hasMember(entry, "distance")
                    ? requirePositiveMember(entry, "distance", operation, path + ".distance")
                    : requirePositiveMember(root, "distance", operation, "chamfer.distance");
                const mini_json::Value* angleSource = (hasMember(entry, "angleRadians") || hasMember(entry, "angleDegrees")) ? &entry : &root;
                const double angle = optionalAngleRadians(*angleSource, operation, angleSource == &entry ? path : "chamfer");
                const mini_json::Value* referenceFaceValue = entry.get("referenceFace");
                if (referenceFaceValue == nullptr) referenceFaceValue = root.get("referenceFace");
                if (referenceFaceValue == nullptr) {
                    throwStructuredValidation(operation, path + ".referenceFace", "distanceAngle chamfers require a referenceFace for the measured side");
                }
                ResolvedFaceRef referenceFace = resolveFaceRef(shape, sourceRevision, *referenceFaceValue, operation, path + ".referenceFace");
                mkChamfer.AddDA(distance, angle, edge.edge, referenceFace.face);
                normalized << "\"distance\":" << distance << ",\"angleRadians\":" << angle << ",\"referenceFace\":";
                appendEntityRefJson(normalized, referenceFace);
            } else {
                throwStructuredValidation(operation, path + ".mode", "Unsupported chamfer mode", "chamfer.modes");
            }

            normalized << ",\"tangentPropagation\":true}";
            appliedEdges.push_back(edge);
            normalizedParameters.push_back(normalized.str());
        };

        if (const mini_json::Value* edgesValue = root.get("edges")) {
            const mini_json::Value& edges = mini_json::requireArray(*edgesValue, "chamfer.edges");
            if (edges.array.empty()) {
                throwStructuredValidation(operation, "chamfer.edges", "At least one edge selection is required");
            }
            for (std::size_t i = 0; i < edges.array.size(); ++i) {
                addChamferForEdge(edges.array[i], "chamfer.edges[" + std::to_string(i) + "]");
            }
        } else {
            ShapeMap edges;
            TopExp::MapShapes(shape, TopAbs_EDGE, edges);
            if (edges.Extent() == 0) {
                throwStructuredValidation(operation, "chamfer.edges", "Shape has no edges to chamfer");
            }
            for (int i = 1; i <= edges.Extent(); ++i) {
                mini_json::Value entry;
                entry.kind = mini_json::Value::Kind::Object;
                mini_json::Value topo;
                topo.kind = mini_json::Value::Kind::Number;
                topo.number = static_cast<double>(i);
                entry.object.push_back({ "topoId", topo });
                addChamferForEdge(entry, "chamfer.edges[" + std::to_string(i - 1) + "]");
            }
        }

        mkChamfer.Build();
        if (!mkChamfer.IsDone() || mkChamfer.Shape().IsNull()) {
            std::vector<std::string> failing;
            for (const ResolvedEdgeRef& edge : appliedEdges) failing.push_back(stableEdgeRefString(edge));
            throwStructuredOperationError(operation, "BRepFilletAPI_MakeChamfer failed", failing);
        }

        const uint32_t resultId = storeShapeWithMetadata(mkChamfer.Shape(),
                                                         "chamferEdges",
                                                         "source=" + sourceRevision->revisionId + ";spec=" + fnv1a64(objectToStableSignature(root)),
                                                         { sourceRevision->revisionId },
                                                         "unresolved",
                                                         "unresolved",
                                                         { "Chamfer generated/modified/deleted lineage is reported from OCCT history when available; unresolved entries are explicit in the result payload" });
        auto resultIt = _impl->records.find(resultId);
        if (resultIt != _impl->records.end()) {
            for (const ResolvedEdgeRef& edge : appliedEdges) {
                resultIt->second.revision.deletedEntities.push_back({ "edge", edge.stableHash, resultIt->second.revision.operationId, "unresolved" });
            }
        }
        return makeBlendResultJson(this, resultId, mkChamfer, shape, appliedEdges, normalizedParameters, "chamferFace");
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwStructuredOperationError(operation, sf.what());
    }
    return "{}";
}

uint32_t OcctKernel::transformShape(uint32_t id, const std::string& transformJson) {
    try {
        const ShapeTransformOptions options = parseShapeTransformOptions(transformJson);
        TopoDS_Shape shape = requireShape(id);
        auto sourceIt = _impl->records.find(id);
        if (sourceIt == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }
        const std::vector<StableEntityRecord> sourceEntities = collectStableEntities(sourceIt->second.shape, &sourceIt->second.revision);

        if (options.rotation.hasValue) {
            gp_Trsf rotation;
            rotation.SetRotation(
                gp_Ax1(options.rotation.axisOrigin, options.rotation.axisDirection),
                options.rotation.angleDegrees * M_PI / 180.0
            );
            shape = applyShapeTransform(shape, rotation, "rotation");
        }

        if (options.hasTranslation) {
            gp_Trsf translation;
            translation.SetTranslation(options.translation);
            shape = applyShapeTransform(shape, translation, "translation");
        }

        const uint32_t resultId = storeShapeWithMetadata(shape,
                                                        "transformShape",
                                                        "source=" + requireRevisionId(id) + ";transform=" + transformJson,
                                                        { requireRevisionId(id) },
                                                        "retained",
                                                        "retained",
                                                        {});
        RevisionMetadata& revision = _impl->records[resultId].revision;
        for (const StableEntityRecord& entity : sourceEntities) {
            if (entity.kind == "face") {
                revision.faceStableHashes.push_back(entity.stableHash);
            } else if (entity.kind == "edge") {
                revision.edgeStableHashes.push_back(entity.stableHash);
            } else if (entity.kind == "vertex") {
                revision.vertexStableHashes.push_back(entity.stableHash);
            }
        }
        return resultId;
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::string OcctKernel::getTopology(uint32_t id) {
    try {
        auto recordIt = _impl->records.find(id);
        if (recordIt == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }
        const ShapeRecord& record = recordIt->second;
        const TopoDS_Shape& shape = record.shape;
        const RevisionMetadata& revision = record.revision;

        ShapeMap faces, edges, vertices;
        TopExp::MapShapes(shape, TopAbs_FACE,   faces);
        TopExp::MapShapes(shape, TopAbs_EDGE,   edges);
        TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);

        ShapeToShapeListMap edgeToFaces;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeToFaces);

        std::vector<std::string> faceHashes(static_cast<std::size_t>(faces.Extent()) + 1);
        for (int i = 1; i <= faces.Extent(); ++i) {
            if (revision.faceStableHashes.size() == static_cast<std::size_t>(faces.Extent())) {
                faceHashes[static_cast<std::size_t>(i)] = revision.faceStableHashes[static_cast<std::size_t>(i - 1)];
            } else {
                faceHashes[static_cast<std::size_t>(i)] = makeFaceStableHash(TopoDS::Face(faces(i)));
            }
        }

        std::vector<std::string> edgeHashes(static_cast<std::size_t>(edges.Extent()) + 1);
        std::vector<std::vector<int>> edgeFaceIds(static_cast<std::size_t>(edges.Extent()) + 1);
        for (int i = 1; i <= edges.Extent(); ++i) {
            const TopoDS_Edge& edge = TopoDS::Edge(edges(i));
            edgeFaceIds[static_cast<std::size_t>(i)] = faceIdsForEdge(edge, faces, edgeToFaces);

            std::vector<std::string> adjacentFaceHashes;
            for (int faceId : edgeFaceIds[static_cast<std::size_t>(i)]) {
                adjacentFaceHashes.push_back(faceHashes[static_cast<std::size_t>(faceId)]);
            }
            edgeHashes[static_cast<std::size_t>(i)] = revision.edgeStableHashes.size() == static_cast<std::size_t>(edges.Extent())
                ? revision.edgeStableHashes[static_cast<std::size_t>(i - 1)]
                : makeEdgeStableHash(edge, adjacentFaceHashes);
        }

        std::vector<std::string> vertexHashes(static_cast<std::size_t>(vertices.Extent()) + 1);
        for (int i = 1; i <= vertices.Extent(); ++i) {
            vertexHashes[static_cast<std::size_t>(i)] = revision.vertexStableHashes.size() == static_cast<std::size_t>(vertices.Extent())
                ? revision.vertexStableHashes[static_cast<std::size_t>(i - 1)]
                : makeVertexStableHash(TopoDS::Vertex(vertices(i)));
        }

        Bnd_Box bbox;
        BRepBndLib::Add(shape, bbox);
        double xMin, yMin, zMin, xMax, yMax, zMax;
        bbox.Get(xMin, yMin, zMin, xMax, yMax, zMax);

        BRepCheck_Analyzer analyzer(shape);
        bool isValid = analyzer.IsValid();

        std::ostringstream ss;
        ss << "{";
        ss << "\"revisionId\":"; appendJsonString(ss, revision.revisionId); ss << ",";
        ss << "\"operationId\":";
        if (revision.operationId.empty()) ss << "null"; else appendJsonString(ss, revision.operationId);
        ss << ",";
        ss << "\"sourceFeatureId\":";
        if (revision.sourceFeatureId.empty()) ss << "null"; else appendJsonString(ss, revision.sourceFeatureId);
        ss << ",";
        ss << "\"operationType\":"; appendJsonString(ss, revision.operationType); ss << ",";
        ss << "\"operandRevisionIds\":"; appendStringArrayJson(ss, revision.operandRevisionIds); ss << ",";
        ss << "\"parameterHash\":";
        if (revision.parameterHash.empty()) ss << "null"; else appendJsonString(ss, revision.parameterHash);
        ss << ",";
        ss << "\"topologyHash\":"; appendJsonString(ss, revision.topologyHash); ss << ",";
        ss << "\"historySchemaVersion\":" << revision.historySchemaVersion << ",";
        ss << "\"createdFromCheckpoint\":" << (revision.createdFromCheckpoint ? "true" : "false") << ",";
        ss << "\"identityStatus\":"; appendJsonString(ss, revision.identityStatus); ss << ",";
        ss << "\"historyWarnings\":"; appendStringArrayJson(ss, revision.historyWarnings); ss << ",";
        ss << "\"faceCount\":"   << faces.Extent()    << ",";
        ss << "\"edgeCount\":"   << edges.Extent()    << ",";
        ss << "\"vertexCount\":" << vertices.Extent() << ",";
        ss << "\"boundingBox\":{";
        ss << "\"xMin\":"  << xMin << ",";
        ss << "\"yMin\":"  << yMin << ",";
        ss << "\"zMin\":"  << zMin << ",";
        ss << "\"xMax\":"  << xMax << ",";
        ss << "\"yMax\":"  << yMax << ",";
        ss << "\"zMax\":"  << zMax;
        ss << "},";
        ss << "\"isValid\":" << (isValid ? "true" : "false") << ",";

        auto appendLineage = [&](const char* lineageKind) {
            if (revision.entityStatus == lineageKind) {
                appendStringArrayJson(ss, revision.operandRevisionIds);
            } else {
                ss << "[]";
            }
        };

        ss << "\"faces\":[";
        for (int i = 1; i <= faces.Extent(); ++i) {
            if (i > 1) ss << ",";
            ss << "{";
            ss << "\"id\":" << i << ",";
            ss << "\"stableHash\":"; appendJsonString(ss, faceHashes[static_cast<std::size_t>(i)]); ss << ",";
            ss << "\"role\":\"unknown\",";
            ss << "\"sourceFeatureId\":";
            if (revision.sourceFeatureId.empty()) ss << "null"; else appendJsonString(ss, revision.sourceFeatureId);
            ss << ",\"generatedFrom\":"; appendLineage("generated");
            ss << ",\"modifiedFrom\":"; appendLineage("modified");
            ss << ",\"retainedFrom\":"; appendLineage("retained");
            ss << ",\"status\":"; appendJsonString(ss, revision.entityStatus); ss << ",";
            ss << "\"shared\":{\"sourceFeatureId\":";
            if (revision.sourceFeatureId.empty()) ss << "null"; else appendJsonString(ss, revision.sourceFeatureId);
            ss << "}";
            ss << "}";
        }
        ss << "],";

        ss << "\"edges\":[";
        for (int i = 1; i <= edges.Extent(); ++i) {
            if (i > 1) ss << ",";
            ss << "{";
            ss << "\"id\":" << i << ",";
            ss << "\"stableHash\":"; appendJsonString(ss, edgeHashes[static_cast<std::size_t>(i)]); ss << ",";
            ss << "\"topoFaceIds\":";
            appendIntArrayJson(ss, edgeFaceIds[static_cast<std::size_t>(i)]);
            ss << ",\"generatedFrom\":"; appendLineage("generated");
            ss << ",\"modifiedFrom\":"; appendLineage("modified");
            ss << ",\"retainedFrom\":"; appendLineage("retained");
            ss << ",\"status\":"; appendJsonString(ss, revision.entityStatus);
            ss << "}";
        }
        ss << "],";

        ss << "\"vertices\":[";
        for (int i = 1; i <= vertices.Extent(); ++i) {
            if (i > 1) ss << ",";
            ss << "{";
            ss << "\"id\":" << i << ",";
            ss << "\"stableHash\":"; appendJsonString(ss, vertexHashes[static_cast<std::size_t>(i)]); ss << ",";
            ss << "\"status\":"; appendJsonString(ss, revision.entityStatus);
            ss << "}";
        }
        ss << "],";
        ss << "\"deletedEntities\":";
        appendDeletedEntitiesJson(ss, revision.deletedEntities);
        ss << "}";
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return "{}";
}

std::string OcctKernel::getRevisionInfo(uint32_t id) {
    try {
        auto it = _impl->records.find(id);
        if (it == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }

        std::ostringstream ss;
        appendRevisionMetadataJson(ss, it->second.revision);
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return "{}";
}

std::string OcctKernel::resolveStableEntity(uint32_t id, const std::string& stableHash) {
    try {
        auto it = _impl->records.find(id);
        if (it == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }

        for (const StableEntityRecord& entity : collectStableEntities(it->second.shape, &it->second.revision)) {
            if (entity.stableHash == stableHash) {
                std::ostringstream ss;
                ss << "{";
                ss << "\"found\":true,";
                ss << "\"status\":\"active\",";
                ss << "\"kind\":"; appendJsonString(ss, entity.kind); ss << ",";
                ss << "\"id\":" << entity.id << ",";
                ss << "\"stableHash\":"; appendJsonString(ss, entity.stableHash); ss << ",";
                ss << "\"revisionId\":"; appendJsonString(ss, it->second.revision.revisionId);
                ss << "}";
                return ss.str();
            }
        }

        for (const DeletedEntityRecord& deleted : it->second.revision.deletedEntities) {
            if (deleted.stableHash == stableHash) {
                std::ostringstream ss;
                ss << "{";
                ss << "\"found\":false,";
                ss << "\"status\":\"deleted\",";
                ss << "\"kind\":"; appendJsonString(ss, deleted.kind); ss << ",";
                ss << "\"stableHash\":"; appendJsonString(ss, deleted.stableHash); ss << ",";
                ss << "\"revisionId\":"; appendJsonString(ss, it->second.revision.revisionId);
                if (!deleted.deletedBy.empty()) {
                    ss << ",\"deletedBy\":"; appendJsonString(ss, deleted.deletedBy);
                }
                ss << "}";
                return ss.str();
            }
        }

        std::ostringstream ss;
        ss << "{";
        ss << "\"found\":false,";
        ss << "\"status\":\"unresolved\",";
        ss << "\"stableHash\":"; appendJsonString(ss, stableHash); ss << ",";
        ss << "\"revisionId\":"; appendJsonString(ss, it->second.revision.revisionId); ss << ",";
        ss << "\"message\":\"Stable entity is not present in this resident revision\"";
        ss << "}";
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return "{}";
}

std::string OcctKernel::mapEntitiesAcrossRevisions(const std::string& fromRevisionId,
                                                   const std::string& toRevisionId,
                                                   const std::string& stableHashesJson) {
    try {
        auto fromHandleIt = _impl->revisionToHandle.find(fromRevisionId);
        auto toHandleIt = _impl->revisionToHandle.find(toRevisionId);
        if (fromHandleIt == _impl->revisionToHandle.end()) {
            throwKernelError("INVALID_HANDLE", "No resident revision " + fromRevisionId);
        }
        if (toHandleIt == _impl->revisionToHandle.end()) {
            throwKernelError("INVALID_HANDLE", "No resident revision " + toRevisionId);
        }

        const ShapeRecord& fromRecord = _impl->records.at(fromHandleIt->second);
        const ShapeRecord& toRecord = _impl->records.at(toHandleIt->second);
        const std::vector<StableEntityRecord> fromEntities = collectStableEntities(fromRecord.shape, &fromRecord.revision);
        const std::vector<StableEntityRecord> toEntities = collectStableEntities(toRecord.shape, &toRecord.revision);

        std::set<std::string> fromHashes;
        std::set<std::string> toHashes;
        for (const StableEntityRecord& entity : fromEntities) fromHashes.insert(entity.stableHash);
        for (const StableEntityRecord& entity : toEntities) toHashes.insert(entity.stableHash);

        const mini_json::Value parsedHashes = mini_json::parse(stableHashesJson);
        const mini_json::Value& hashes = mini_json::requireArray(parsedHashes, "stable hashes");

        std::ostringstream ss;
        ss << "{";
        ss << "\"fromRevisionId\":"; appendJsonString(ss, fromRevisionId); ss << ",";
        ss << "\"toRevisionId\":"; appendJsonString(ss, toRevisionId); ss << ",";
        ss << "\"mappings\":[";
        for (std::size_t i = 0; i < hashes.array.size(); ++i) {
            const std::string stableHash = mini_json::requireString(hashes.array[i], "stable hash");
            if (i > 0) ss << ",";
            ss << "{";
            ss << "\"stableHash\":"; appendJsonString(ss, stableHash); ss << ",";
            if (toHashes.count(stableHash) > 0) {
                ss << "\"status\":\"mapped\",\"mappedStableHash\":"; appendJsonString(ss, stableHash);
            } else {
                bool deleted = false;
                for (const DeletedEntityRecord& entity : toRecord.revision.deletedEntities) {
                    if (entity.stableHash == stableHash) {
                        deleted = true;
                        break;
                    }
                }
                if (deleted) {
                    ss << "\"status\":\"deleted\",\"mappedStableHash\":null";
                } else if (fromHashes.count(stableHash) > 0) {
                    ss << "\"status\":\"unresolved\",\"mappedStableHash\":null,"
                       << "\"message\":\"No proven descendant exists in the target resident revision\"";
                } else {
                    ss << "\"status\":\"missing\",\"mappedStableHash\":null,"
                       << "\"message\":\"Stable hash is not present in the source resident revision\"";
                }
            }
            ss << "}";
        }
        ss << "]}";
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return "{}";
}

std::string OcctKernel::evaluateEdge(uint32_t id, const std::string& edgeRefJson, double t) {
    const std::string operation = "evaluateEdge";
    try {
        auto it = _impl->records.find(id);
        if (it == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }
        const mini_json::Value edgeRef = mini_json::requireObject(mini_json::parse(edgeRefJson), "edgeRef");
        ResolvedEdgeRef edge = resolveEdgeRef(it->second.shape, &it->second.revision, edgeRef, operation, "edgeRef");
        BRepAdaptor_Curve curve(edge.edge);
        const double first = curve.FirstParameter();
        const double last = curve.LastParameter();
        const bool nativeParameter = optionalStringMember(edgeRef, "parameterMode", "normalized") == "native" || !optionalBoolMember(edgeRef, "normalized", true);
        const double parameter = nativeParameter ? t : first + (last - first) * t;
        if (!std::isfinite(parameter) || parameter < std::min(first, last) - 1.0e-12 || parameter > std::max(first, last) + 1.0e-12) {
            throwStructuredValidation(operation, "t", "Edge parameter is outside the curve domain");
        }

        gp_Pnt point;
        gp_Vec tangent;
        curve.D1(parameter, point, tangent);
        const bool hasTangent = tangent.SquareMagnitude() > 1.0e-18;
        if (hasTangent) tangent.Normalize();

        std::ostringstream ss;
        ss << "{";
        ss << "\"edge\":"; appendEntityRefJson(ss, edge); ss << ",";
        ss << "\"curveType\":"; appendJsonString(ss, curveTypeName(curve.GetType())); ss << ",";
        ss << "\"parameter\":" << parameter << ",";
        ss << "\"normalizedParameter\":" << ((parameter - first) / (last - first)) << ",";
        ss << "\"domain\":{\"first\":" << first << ",\"last\":" << last << "},";
        ss << "\"point\":"; appendPointJson(ss, point); ss << ",";
        ss << "\"tangent\":";
        if (hasTangent) appendVectorJson(ss, tangent); else ss << "null";
        ss << "}";
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return "{}";
}

std::string OcctKernel::sampleEdge(uint32_t id, const std::string& edgeRefJson, const std::string& optionsJson) {
    const std::string operation = "sampleEdge";
    try {
        auto it = _impl->records.find(id);
        if (it == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }
        const mini_json::Value edgeRef = mini_json::requireObject(mini_json::parse(edgeRefJson), "edgeRef");
        const mini_json::Value options = optionsJson.empty()
            ? mini_json::Value::makeObject()
            : mini_json::requireObject(mini_json::parse(optionsJson), "sampleEdge options");
        const int count = optionalIntMember(options, "count", 16);
        if (count < 2 || count > 4096) {
            throwStructuredValidation(operation, "options.count", "sample count must be in [2, 4096]");
        }
        const bool includeTangents = optionalBoolMember(options, "includeTangents", true);
        const bool normalized = optionalBoolMember(options, "normalized", true);
        const double start = optionalNumberMember(options, "start", normalized ? 0.0 : std::numeric_limits<double>::quiet_NaN());
        const double end = optionalNumberMember(options, "end", normalized ? 1.0 : std::numeric_limits<double>::quiet_NaN());

        ResolvedEdgeRef edge = resolveEdgeRef(it->second.shape, &it->second.revision, edgeRef, operation, "edgeRef");
        BRepAdaptor_Curve curve(edge.edge);
        const double first = curve.FirstParameter();
        const double last = curve.LastParameter();
        const double parameterStart = normalized ? first + (last - first) * start : (std::isnan(start) ? first : start);
        const double parameterEnd = normalized ? first + (last - first) * end : (std::isnan(end) ? last : end);
        if (!std::isfinite(parameterStart) || !std::isfinite(parameterEnd) || parameterStart == parameterEnd) {
            throwStructuredValidation(operation, "options", "sample range must be finite and non-empty");
        }

        std::ostringstream ss;
        ss << "{";
        ss << "\"edge\":"; appendEntityRefJson(ss, edge); ss << ",";
        ss << "\"curveType\":"; appendJsonString(ss, curveTypeName(curve.GetType())); ss << ",";
        ss << "\"domain\":{\"first\":" << first << ",\"last\":" << last << "},";
        ss << "\"samples\":[";
        for (int i = 0; i < count; ++i) {
            if (i > 0) ss << ',';
            const double alpha = static_cast<double>(i) / static_cast<double>(count - 1);
            const double parameter = parameterStart + (parameterEnd - parameterStart) * alpha;
            gp_Pnt point;
            gp_Vec tangent;
            curve.D1(parameter, point, tangent);
            const bool hasTangent = tangent.SquareMagnitude() > 1.0e-18;
            if (hasTangent) tangent.Normalize();
            ss << "{";
            ss << "\"parameter\":" << parameter << ",";
            ss << "\"normalizedParameter\":" << ((parameter - first) / (last - first)) << ",";
            ss << "\"point\":"; appendPointJson(ss, point);
            if (includeTangents) {
                ss << ",\"tangent\":";
                if (hasTangent) appendVectorJson(ss, tangent); else ss << "null";
            }
            ss << "}";
        }
        ss << "]}";
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return "{}";
}

std::string OcctKernel::getEdgeCurve(uint32_t id, const std::string& edgeRefJson) {
    const std::string operation = "getEdgeCurve";
    try {
        auto it = _impl->records.find(id);
        if (it == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }
        const mini_json::Value edgeRef = mini_json::requireObject(mini_json::parse(edgeRefJson), "edgeRef");
        ResolvedEdgeRef edge = resolveEdgeRef(it->second.shape, &it->second.revision, edgeRef, operation, "edgeRef");
        BRepAdaptor_Curve curve(edge.edge);
        const double first = curve.FirstParameter();
        const double last = curve.LastParameter();
        std::ostringstream ss;
        ss << "{";
        ss << "\"edge\":"; appendEntityRefJson(ss, edge); ss << ",";
        ss << "\"curveType\":"; appendJsonString(ss, curveTypeName(curve.GetType())); ss << ",";
        ss << "\"domain\":{\"first\":" << first << ",\"last\":" << last << "},";
        ss << "\"startPoint\":"; appendPointJson(ss, curve.Value(first)); ss << ",";
        ss << "\"endPoint\":"; appendPointJson(ss, curve.Value(last));

        if (curve.GetType() == GeomAbs_Line) {
            const gp_Lin line = curve.Line();
            ss << ",\"line\":{\"origin\":"; appendPointJson(ss, line.Location()); ss << ",\"direction\":"; appendVectorJson(ss, gp_Vec(line.Direction())); ss << "}";
        } else if (curve.GetType() == GeomAbs_Circle) {
            const gp_Circ circle = curve.Circle();
            ss << ",\"circle\":{\"center\":"; appendPointJson(ss, circle.Location()); ss << ",\"radius\":" << circle.Radius() << ",\"normal\":"; appendVectorJson(ss, gp_Vec(circle.Axis().Direction())); ss << "}";
        } else if (curve.GetType() == GeomAbs_BSplineCurve) {
            Handle(Geom_BSplineCurve) bspline = curve.BSpline();
            ss << ",\"bspline\":{\"degree\":" << bspline->Degree() << ",\"periodic\":" << (bspline->IsPeriodic() ? "true" : "false") << ",\"poles\":[";
            for (int i = 1; i <= bspline->NbPoles(); ++i) {
                if (i > 1) ss << ',';
                appendPointJson(ss, bspline->Pole(i));
            }
            ss << "],\"weights\":[";
            for (int i = 1; i <= bspline->NbPoles(); ++i) {
                if (i > 1) ss << ',';
                ss << bspline->Weight(i);
            }
            ss << "],\"knots\":[";
            for (int i = 1; i <= bspline->NbKnots(); ++i) {
                if (i > 1) ss << ',';
                ss << bspline->Knot(i);
            }
            ss << "],\"multiplicities\":[";
            for (int i = 1; i <= bspline->NbKnots(); ++i) {
                if (i > 1) ss << ',';
                ss << bspline->Multiplicity(i);
            }
            ss << "]}";
        } else if (curve.GetType() == GeomAbs_BezierCurve) {
            Handle(Geom_BezierCurve) bezier = curve.Bezier();
            ss << ",\"bezier\":{\"degree\":" << bezier->Degree() << ",\"poles\":[";
            for (int i = 1; i <= bezier->NbPoles(); ++i) {
                if (i > 1) ss << ',';
                appendPointJson(ss, bezier->Pole(i));
            }
            ss << "],\"weights\":[";
            for (int i = 1; i <= bezier->NbPoles(); ++i) {
                if (i > 1) ss << ',';
                ss << bezier->Weight(i);
            }
            ss << "]}";
        }

        ss << "}";
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return "{}";
}

std::string OcctKernel::evaluateFace(uint32_t id, const std::string& faceRefJson, double u, double v) {
    const std::string operation = "evaluateFace";
    try {
        auto it = _impl->records.find(id);
        if (it == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }
        const mini_json::Value faceRef = mini_json::requireObject(mini_json::parse(faceRefJson), "faceRef");
        ResolvedFaceRef face = resolveFaceRef(it->second.shape, &it->second.revision, faceRef, operation, "faceRef");
        BRepAdaptor_Surface surface(face.face, false);
        const double firstU = surface.FirstUParameter();
        const double lastU = surface.LastUParameter();
        const double firstV = surface.FirstVParameter();
        const double lastV = surface.LastVParameter();
        const bool nativeParameter = optionalStringMember(faceRef, "parameterMode", "normalized") == "native" || !optionalBoolMember(faceRef, "normalized", true);
        const double parameterU = nativeParameter ? u : firstU + (lastU - firstU) * u;
        const double parameterV = nativeParameter ? v : firstV + (lastV - firstV) * v;
        if (!std::isfinite(parameterU) || !std::isfinite(parameterV)) {
            throwStructuredValidation(operation, "uv", "Face parameters must be finite");
        }

        gp_Pnt point;
        gp_Vec du;
        gp_Vec dv;
        surface.D1(parameterU, parameterV, point, du, dv);
        gp_Vec normal = du.Crossed(dv);
        const bool hasNormal = normal.SquareMagnitude() > 1.0e-18;
        if (hasNormal) {
            if (face.face.Orientation() == TopAbs_REVERSED) normal.Reverse();
            normal.Normalize();
        }

        std::ostringstream ss;
        ss << "{";
        ss << "\"face\":"; appendEntityRefJson(ss, face); ss << ",";
        ss << "\"surfaceType\":"; appendJsonString(ss, surfaceTypeName(surface.GetType())); ss << ",";
        ss << "\"uv\":[" << parameterU << ',' << parameterV << "],";
        ss << "\"normalizedUv\":[" << ((parameterU - firstU) / (lastU - firstU)) << ',' << ((parameterV - firstV) / (lastV - firstV)) << "],";
        ss << "\"domain\":{\"u\":[" << firstU << ',' << lastU << "],\"v\":[" << firstV << ',' << lastV << "]},";
        ss << "\"point\":"; appendPointJson(ss, point); ss << ",";
        ss << "\"dU\":"; appendVectorJson(ss, du); ss << ",";
        ss << "\"dV\":"; appendVectorJson(ss, dv); ss << ",";
        ss << "\"normal\":";
        if (hasNormal) appendVectorJson(ss, normal); else ss << "null";
        ss << "}";
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return "{}";
}

std::string OcctKernel::getOperationSchema() const {
    return "{"
        "\"schemaVersion\":1,"
        "\"operations\":{"
        "\"extrudeProfile\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"direction\":true,\"draft\":true,\"plane\":true,\"reverseDirection\":true,\"endConditions\":[\"blind\",\"upToNext\",\"throughAll\",\"upToSurface\",\"offsetFromSurface\"],\"surfaceTarget\":true,\"curvedSurfaceTarget\":true}},"
        "\"extrudeCutProfile\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"direction\":true,\"draft\":true,\"plane\":true,\"reverseDirection\":true,\"endConditions\":[\"blind\",\"upToNext\",\"throughAll\",\"upToSurface\",\"offsetFromSurface\"],\"surfaceTarget\":true,\"curvedSurfaceTarget\":true}},"
        "\"revolveProfile\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"plane\":true,\"axis\":true,\"reverseDirection\":true,\"signedAngle\":true,\"endConditions\":[\"angle\",\"upToSurface\",\"fromSurfaceToSurface\",\"throughAll\",\"upToSurfaceAtAngle\"],\"surfaceTarget\":true,\"curvedSurfaceTarget\":true,\"slidingEdges\":true}},"
        "\"revolveCutProfile\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"plane\":true,\"axis\":true,\"reverseDirection\":true,\"signedAngle\":true,\"endConditions\":[\"angle\",\"upToSurface\",\"fromSurfaceToSurface\",\"throughAll\",\"upToSurfaceAtAngle\"],\"surfaceTarget\":true,\"curvedSurfaceTarget\":true,\"slidingEdges\":true}},"
        "\"filletEdges\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"constantRadius\":true,\"startEndRadius\":true,\"stationRadii\":true,\"lawRadius\":[\"constant\",\"linear\"],\"tangentPropagation\":true,\"partialEdges\":false,\"setbackCorners\":false,\"blendShape\":[\"rational\",\"quasiAngular\",\"polynomial\"],\"continuity\":[\"C0\",\"C1\",\"C2\"],\"overflowModes\":[\"fail\"]}},"
        "\"chamferEdges\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"symmetric\":true,\"twoDistance\":true,\"distanceAngle\":true,\"referenceFace\":true,\"tangentPropagation\":true,\"partialEdges\":false,\"setbackCorners\":false,\"overflowModes\":[\"fail\"]}},"
        "\"evaluateEdge\":{\"schemaVersion\":1,\"nativeExact\":true,\"parameterModes\":[\"normalized\",\"native\"]},"
        "\"sampleEdge\":{\"schemaVersion\":1,\"nativeExact\":true,\"parameterModes\":[\"normalized\",\"native\"]},"
        "\"getEdgeCurve\":{\"schemaVersion\":1,\"nativeExact\":true},"
        "\"evaluateFace\":{\"schemaVersion\":1,\"nativeExact\":true,\"parameterModes\":[\"normalized\",\"native\"]}"
        "}"
        "}";
}

std::string OcctKernel::getCapabilities() const {
    return "{"
        "\"featureEdgesV1\":true,"
        "\"rawEdgeSegmentsV1\":true,"
        "\"triangleNormalsV1\":true,"
        "\"triangleFaceMappingV1\":true,"
        "\"topologySubshapesV1\":true,"
        "\"geometricStableHashesV1\":true,"
        "\"revisionInfoV1\":true,"
        "\"entityResolutionV1\":true,"
        "\"entityRemapV1\":true,"
        "\"revisionRetentionV1\":true,"
        "\"historyV1\":true,"
        "\"stableNamingV1\":false,"
        "\"checkpointV1\":true,"
        "\"operations\":{"
        "\"structuredSpecsV1\":true,"
        "\"operationSchemaV1\":true,"
        "\"nativeExactBlendOpsV1\":true,"
        "\"exactSubshapeEvaluationV1\":true"
        "},"
        "\"extrudeProfile\":{"
        "\"schemaVersion\":1,"
        "\"nativeExact\":true,"
        "\"direction\":true,"
        "\"draft\":true,"
        "\"plane\":true,"
        "\"reverseDirection\":true,"
        "\"endConditions\":[\"blind\",\"upToNext\",\"throughAll\",\"upToSurface\",\"offsetFromSurface\"],"
        "\"surfaceTarget\":true,"
        "\"curvedSurfaceTarget\":true"
        "},"
        "\"extrudeCutProfile\":{"
        "\"schemaVersion\":1,"
        "\"nativeExact\":true,"
        "\"direction\":true,"
        "\"draft\":true,"
        "\"plane\":true,"
        "\"reverseDirection\":true,"
        "\"endConditions\":[\"blind\",\"upToNext\",\"throughAll\",\"upToSurface\",\"offsetFromSurface\"],"
        "\"surfaceTarget\":true,"
        "\"curvedSurfaceTarget\":true"
        "},"
        "\"revolveProfile\":{"
        "\"schemaVersion\":1,"
        "\"nativeExact\":true,"
        "\"plane\":true,"
        "\"axis\":true,"
        "\"reverseDirection\":true,"
        "\"signedAngle\":true,"
        "\"endConditions\":[\"angle\",\"upToSurface\",\"fromSurfaceToSurface\",\"throughAll\",\"upToSurfaceAtAngle\"],"
        "\"surfaceTarget\":true,"
        "\"curvedSurfaceTarget\":true,"
        "\"slidingEdges\":true"
        "},"
        "\"revolveCutProfile\":{"
        "\"schemaVersion\":1,"
        "\"nativeExact\":true,"
        "\"plane\":true,"
        "\"axis\":true,"
        "\"reverseDirection\":true,"
        "\"signedAngle\":true,"
        "\"endConditions\":[\"angle\",\"upToSurface\",\"fromSurfaceToSurface\",\"throughAll\",\"upToSurfaceAtAngle\"],"
        "\"surfaceTarget\":true,"
        "\"curvedSurfaceTarget\":true,"
        "\"slidingEdges\":true"
        "},"
        "\"fillet\":{"
        "\"schemaVersion\":1,"
        "\"nativeExact\":true,"
        "\"constantRadius\":true,"
        "\"startEndRadius\":true,"
        "\"stationRadii\":true,"
        "\"lawRadius\":[\"constant\",\"linear\"],"
        "\"tangentPropagation\":true,"
        "\"partialEdges\":false,"
        "\"setbackCorners\":false,"
        "\"blendShape\":[\"rational\",\"quasiAngular\",\"polynomial\"],"
        "\"continuity\":[\"C0\",\"C1\",\"C2\"],"
        "\"overflowModes\":[\"fail\"]"
        "},"
        "\"chamfer\":{"
        "\"schemaVersion\":1,"
        "\"nativeExact\":true,"
        "\"symmetric\":true,"
        "\"twoDistance\":true,"
        "\"distanceAngle\":true,"
        "\"referenceFace\":true,"
        "\"tangentPropagation\":true,"
        "\"partialEdges\":false,"
        "\"setbackCorners\":false,"
        "\"overflowModes\":[\"fail\"]"
        "},"
        "\"subshapeEvaluation\":{"
        "\"evaluateEdge\":true,"
        "\"sampleEdge\":true,"
        "\"getEdgeCurve\":true,"
        "\"evaluateFace\":true,"
        "\"parameterModes\":[\"normalized\",\"native\"]"
        "}"
        "}";
}

bool OcctKernel::checkValidity(uint32_t id) {
    try {
        const TopoDS_Shape& shape = requireShape(id);
        BRepCheck_Analyzer analyzer(shape);
        return analyzer.IsValid();
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Tessellation
// ---------------------------------------------------------------------------

std::string OcctKernel::tessellate(uint32_t id, double linearDeflection, double angularDeflection) {
    if (linearDeflection  <= 0) throwKernelError("INVALID_PARAMS", "linearDeflection must be > 0");
    if (angularDeflection <= 0) throwKernelError("INVALID_PARAMS", "angularDeflection must be > 0");

    try {
        auto recordIt = _impl->records.find(id);
        if (recordIt == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }
        const ShapeRecord& record = recordIt->second;
        const TopoDS_Shape& shape = record.shape;
        const RevisionMetadata& revision = record.revision;

        BRepMesh_IncrementalMesh mesh(shape, linearDeflection, false, angularDeflection);
        mesh.Perform();

        ShapeMap faceMap, edgeMap;
        TopExp::MapShapes(shape, TopAbs_FACE, faceMap);
        TopExp::MapShapes(shape, TopAbs_EDGE, edgeMap);

        ShapeToShapeListMap edgeToFaces;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeToFaces);

        std::vector<std::string> faceHashes(static_cast<std::size_t>(faceMap.Extent()) + 1);
        for (int i = 1; i <= faceMap.Extent(); ++i) {
            if (revision.faceStableHashes.size() == static_cast<std::size_t>(faceMap.Extent())) {
                faceHashes[static_cast<std::size_t>(i)] = revision.faceStableHashes[static_cast<std::size_t>(i - 1)];
            } else {
                faceHashes[static_cast<std::size_t>(i)] = makeFaceStableHash(TopoDS::Face(faceMap(i)));
            }
        }

        std::ostringstream positions_ss, normals_ss, indices_ss, triangle_normals_ss;
        std::ostringstream triangle_face_ids_ss, triangle_face_groups_ss, triangle_hashes_ss;
        std::ostringstream raw_edges_ss, feature_edges_ss;
        positions_ss          << "[";
        normals_ss            << "[";
        indices_ss            << "[";
        triangle_normals_ss   << "[";
        triangle_face_ids_ss  << "[";
        triangle_face_groups_ss << "[";
        triangle_hashes_ss    << "[";
        raw_edges_ss          << "[";
        feature_edges_ss      << "[";

        uint32_t globalIndex = 0;
        bool firstPos = true;
        bool firstIdx = true;
        bool firstTriangleNormal = true;
        bool firstTriangleFaceId = true;
        bool firstTriangleFaceGroup = true;
        bool firstTriangleHash = true;
        bool firstRawEdge = true;

        for (int faceIndex = 1; faceIndex <= faceMap.Extent(); ++faceIndex) {
            const TopoDS_Face& face = TopoDS::Face(faceMap(faceIndex));
            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull()) continue;

            gp_Trsf trsf;
            if (!loc.IsIdentity()) trsf = loc.Transformation();
            bool reversed = (face.Orientation() == TopAbs_REVERSED);

            // Nodes
            std::vector<gp_Pnt> nodePoints(static_cast<std::size_t>(tri->NbNodes()) + 1);
            for (int n = 1; n <= tri->NbNodes(); ++n) {
                gp_Pnt pt = tri->Node(n).Transformed(trsf);
                nodePoints[static_cast<std::size_t>(n)] = pt;
                if (!firstPos) { positions_ss << ","; normals_ss << ","; }
                firstPos = false;
                positions_ss << pt.X() << "," << pt.Y() << "," << pt.Z();
                // Compute normal from triangulation if available, otherwise use 0,0,1
                if (tri->HasNormals()) {
                    gp_Dir nDir = tri->Normal(n);
                    gp_Vec nVec(nDir.X(), nDir.Y(), nDir.Z());
                    nVec.Transform(trsf);
                    if (reversed) {
                        nVec.Reverse();
                    }
                    double len = nVec.Magnitude();
                    if (len > 1e-9) nVec /= len;
                    normals_ss << nVec.X() << "," << nVec.Y() << "," << nVec.Z();
                } else {
                    normals_ss << (reversed ? "0,0,-1" : "0,0,1");
                }
            }

            // Triangles
            for (int t = 1; t <= tri->NbTriangles(); ++t) {
                int n1, n2, n3;
                tri->Triangle(t).Get(n1, n2, n3);
                if (reversed) std::swap(n2, n3);
                uint32_t i1 = globalIndex + (uint32_t)(n1 - 1);
                uint32_t i2 = globalIndex + (uint32_t)(n2 - 1);
                uint32_t i3 = globalIndex + (uint32_t)(n3 - 1);
                if (!firstIdx) indices_ss << ",";
                firstIdx = false;
                indices_ss << i1 << "," << i2 << "," << i3;

                gp_Vec a(nodePoints[static_cast<std::size_t>(n1)], nodePoints[static_cast<std::size_t>(n2)]);
                gp_Vec b(nodePoints[static_cast<std::size_t>(n1)], nodePoints[static_cast<std::size_t>(n3)]);
                gp_Vec triangleNormal = a.Crossed(b);
                if (triangleNormal.SquareMagnitude() > 1.0e-18) {
                    triangleNormal.Normalize();
                } else {
                    triangleNormal = gp_Vec(0.0, 0.0, reversed ? -1.0 : 1.0);
                }

                if (!firstTriangleNormal) triangle_normals_ss << ",";
                firstTriangleNormal = false;
                triangle_normals_ss << triangleNormal.X() << "," << triangleNormal.Y() << "," << triangleNormal.Z();

                if (!firstTriangleFaceId) triangle_face_ids_ss << ",";
                firstTriangleFaceId = false;
                triangle_face_ids_ss << faceIndex;

                if (!firstTriangleFaceGroup) triangle_face_groups_ss << ",";
                firstTriangleFaceGroup = false;
                triangle_face_groups_ss << faceIndex;

                if (!firstTriangleHash) triangle_hashes_ss << ",";
                firstTriangleHash = false;
                appendJsonString(triangle_hashes_ss, faceHashes[static_cast<std::size_t>(faceIndex)]);
            }

            globalIndex += (uint32_t)tri->NbNodes();
        }

        for (int edgeIndex = 1; edgeIndex <= edgeMap.Extent(); ++edgeIndex) {
            const TopoDS_Edge& edge = TopoDS::Edge(edgeMap(edgeIndex));
            const std::vector<gp_Pnt> points = collectEdgePolyline(edge);
            for (const gp_Pnt& pt : points) {
                if (!firstRawEdge) raw_edges_ss << ",";
                firstRawEdge = false;
                raw_edges_ss << pt.X() << "," << pt.Y() << "," << pt.Z();
            }
        }

        std::set<std::string> emittedFeatureKeys;
        int chainId = 1;
        bool firstFeatureEdge = true;
        for (int edgeIndex = 1; edgeIndex <= edgeMap.Extent(); ++edgeIndex) {
            const TopoDS_Edge& edge = TopoDS::Edge(edgeMap(edgeIndex));
            const std::vector<TopoDS_Face> adjacentFaces = uniqueAdjacentFaces(edge, edgeToFaces);
            const bool boundary = adjacentFaces.size() < 2;
            const bool seam = isTopoFaceSeam(edge, adjacentFaces);
            const bool sharp = isHardEdge(edge, adjacentFaces);
            if (!boundary && !seam && !sharp) {
                continue;
            }

            const std::vector<gp_Pnt> points = collectEdgePolyline(edge);
            if (points.size() < 2) {
                continue;
            }

            const std::string dedupeKey = chainDedupeKey(points);
            if (!emittedFeatureKeys.insert(dedupeKey).second) {
                continue;
            }

            std::vector<int> faceIds = faceIdsForEdge(edge, faceMap, edgeToFaces);
            std::vector<std::string> adjacentFaceHashes;
            for (int faceId : faceIds) {
                adjacentFaceHashes.push_back(faceHashes[static_cast<std::size_t>(faceId)]);
            }
            const std::string edgeHash = revision.edgeStableHashes.size() == static_cast<std::size_t>(edgeMap.Extent())
                ? revision.edgeStableHashes[static_cast<std::size_t>(edgeIndex - 1)]
                : makeEdgeStableHash(edge, adjacentFaceHashes);
            const bool closed = samePointWithinTolerance(points.front(), points.back());

            if (!firstFeatureEdge) feature_edges_ss << ",";
            firstFeatureEdge = false;
            feature_edges_ss << "{";
            feature_edges_ss << "\"points\":"; appendPointArrayJson(feature_edges_ss, points); feature_edges_ss << ",";
            feature_edges_ss << "\"isClosed\":" << (closed ? "true" : "false") << ",";
            feature_edges_ss << "\"chainId\":" << chainId++ << ",";
            feature_edges_ss << "\"faceIndices\":"; appendIntArrayJson(feature_edges_ss, faceIds); feature_edges_ss << ",";
            feature_edges_ss << "\"topoFaceIds\":"; appendIntArrayJson(feature_edges_ss, faceIds); feature_edges_ss << ",";
            feature_edges_ss << "\"isBoundary\":" << (boundary ? "true" : "false") << ",";
            feature_edges_ss << "\"isSharp\":" << (sharp ? "true" : "false") << ",";
            feature_edges_ss << "\"isSeam\":" << (seam ? "true" : "false") << ",";
            feature_edges_ss << "\"stableHash\":"; appendJsonString(feature_edges_ss, edgeHash);
            feature_edges_ss << "}";
        }

        positions_ss            << "]";
        normals_ss              << "]";
        indices_ss              << "]";
        triangle_normals_ss     << "]";
        triangle_face_ids_ss    << "]";
        triangle_face_groups_ss << "]";
        triangle_hashes_ss      << "]";
        raw_edges_ss            << "]";
        feature_edges_ss        << "]";

        std::ostringstream result;
        result << "{";
        result << "\"positions\":"    << positions_ss.str() << ",";
        result << "\"normals\":"      << normals_ss.str()   << ",";
        result << "\"indices\":"      << indices_ss.str()   << ",";
        result << "\"triangleNormals\":" << triangle_normals_ss.str() << ",";
        result << "\"triangleTopoFaceIds\":" << triangle_face_ids_ss.str() << ",";
        result << "\"triangleFaceGroups\":" << triangle_face_groups_ss.str() << ",";
        result << "\"triangleStableHashes\":" << triangle_hashes_ss.str() << ",";
        result << "\"featureEdges\":" << feature_edges_ss.str() << ",";
        result << "\"rawEdgeSegments\":" << raw_edges_ss.str();
        result << "}";
        return result.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return "{}";
}

// ---------------------------------------------------------------------------
// Import / export
// ---------------------------------------------------------------------------

uint32_t OcctKernel::importStep(const std::string& content) {
    const StepImportRunResult result = runStepImport(content, StepImportOptions());
    if (!result.hasShape) {
        throwKernelError("IMPORT_FAILED", inferImportFailureDetail(result));
    }

    return storeShapeWithMetadata(result.shape,
                                  "importStep",
                                  "content=" + fnv1a64(content),
                                  {},
                                  "generated",
                                  "unresolved",
                                  { "STEP import does not yet provide source semantic stable naming; imported identity is geometry-derived" });
}

std::string OcctKernel::importStepDetailed(const std::string& content,
                                           bool heal,
                                           bool sew,
                                           bool fixSameParameter,
                                           bool fixSolid,
                                           double sewingTolerance) {
    StepImportOptions options;
    options.heal = heal;
    options.sew = sew;
    options.fixSameParameter = fixSameParameter;
    options.fixSolid = fixSolid;
    options.sewingTolerance = sewingTolerance > 0 ? sewingTolerance : 1.0e-6;

    StepImportRunResult result = runStepImport(content, options);
    uint32_t shapeId = 0;
    if (result.hasShape) {
        shapeId = storeShapeWithMetadata(result.shape,
                                         "importStepDetailed",
                                         "content=" + fnv1a64(content) +
                                             ";heal=" + (heal ? "1" : "0") +
                                             ";sew=" + (sew ? "1" : "0") +
                                             ";fixSameParameter=" + (fixSameParameter ? "1" : "0") +
                                             ";fixSolid=" + (fixSolid ? "1" : "0") +
                                             ";sewingTolerance=" + std::to_string(options.sewingTolerance),
                                         {},
                                         "generated",
                                         result.healed ? "unresolved" : "generated",
                                         { result.healed
                                             ? "STEP heal/sew/fixup stage changed topology; detailed per-subshape lineage is unresolved"
                                             : "STEP import identity is geometry-derived because source semantic stable naming is not available" });
    }
    return buildStepImportResultJson(result, shapeId);
}

std::string OcctKernel::exportStep(uint32_t id) {
    try {
        const TopoDS_Shape& shape = requireShape(id);
        TCollection_AsciiString tmpPath("/tmp/occt_export_tmp.step");
        STEPControl_Writer writer;
        Interface_Static::SetCVal("write.step.schema", "AP214");
        IFSelect_ReturnStatus status = writer.Transfer(shape, STEPControl_AsIs);
        if (status != IFSelect_RetDone) {
            throwKernelError("EXPORT_FAILED", "STEPControl_Writer::Transfer failed");
        }
        status = writer.Write(tmpPath.ToCString());
        if (status != IFSelect_RetDone) {
            throwKernelError("EXPORT_FAILED", "STEPControl_Writer::Write failed");
        }
        std::ifstream ifs(tmpPath.ToCString());
        if (!ifs) throwKernelError("EXPORT_FAILED", "Cannot read exported STEP file");
        std::ostringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("EXPORT_FAILED", sf.what());
    }
    return "";
}

std::string OcctKernel::createCheckpoint(uint32_t id) {
    try {
        auto it = _impl->records.find(id);
        if (it == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }

        TCollection_AsciiString tmpPath("/tmp/occt_checkpoint_tmp.brep");
        if (!BRepTools::Write(it->second.shape, tmpPath.ToCString())) {
            throwKernelError("EXPORT_FAILED", "BRepTools::Write failed while creating checkpoint");
        }

        std::ifstream ifs(tmpPath.ToCString(), std::ios::binary);
        if (!ifs) {
            throwKernelError("EXPORT_FAILED", "Cannot read checkpoint CBREP file");
        }
        std::ostringstream brep;
        brep << ifs.rdbuf();

        std::ostringstream revisionJson;
        appendRevisionMetadataJson(revisionJson, it->second.revision);

        std::ostringstream ss;
        ss << "{";
        ss << "\"checkpointSchemaVersion\":1,";
        ss << "\"brep\":"; appendJsonString(ss, brep.str()); ss << ",";
        ss << "\"revision\":" << revisionJson.str();
        ss << "}";
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("EXPORT_FAILED", sf.what());
    }
    return "{}";
}

uint32_t OcctKernel::hydrateCheckpoint(const std::string& checkpointJson) {
    try {
        const mini_json::Value root = mini_json::requireObject(mini_json::parse(checkpointJson), "checkpoint");
        const std::string brep = mini_json::requireString(mini_json::requireMember(root, "brep", "checkpoint"), "checkpoint.brep");
        RevisionMetadata revision = parseRevisionMetadata(mini_json::requireMember(root, "revision", "checkpoint"));

        TCollection_AsciiString tmpPath("/tmp/occt_hydrate_checkpoint_tmp.brep");
        {
            std::ofstream ofs(tmpPath.ToCString(), std::ios::binary);
            if (!ofs) {
                throwKernelError("IMPORT_FAILED", "Cannot create temporary file for checkpoint hydrate");
            }
            ofs << brep;
        }

        TopoDS_Shape shape;
        BRep_Builder builder;
        if (!BRepTools::Read(shape, tmpPath.ToCString(), builder) || shape.IsNull()) {
            throwKernelError("IMPORT_FAILED", "BRepTools::Read failed while hydrating checkpoint");
        }

        revision.createdFromCheckpoint = true;
        if (revision.topologyHash.empty()) {
            revision.topologyHash = computeTopologyHash(shape);
        }
        if (revision.revisionId.empty()) {
            revision.revisionId = "rev_" + fnv1a64("hydrateCheckpoint|" + revision.topologyHash);
        }
        if (revision.operationId.empty()) {
            revision.operationId = "op_" + fnv1a64("hydrateCheckpoint|" + revision.revisionId);
        }
        if (revision.operationType.empty()) {
            revision.operationType = "hydrateCheckpoint";
        }
        if (revision.entityStatus.empty()) {
            revision.entityStatus = "retained";
        }
        if (revision.identityStatus.empty()) {
            revision.identityStatus = "retained";
        }

        const uint32_t id = _impl->nextId++;
        ShapeRecord record;
        record.shape = shape;
        record.revision = revision;
        record.refCount = 1;
        _impl->records[id] = record;
        _impl->revisionToHandle[revision.revisionId] = id;
        return id;
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("IMPORT_FAILED", sf.what());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

void OcctKernel::disposeShape(uint32_t id) {
    auto it = _impl->records.find(id);
    if (it == _impl->records.end()) {
        return;
    }
    _impl->revisionToHandle.erase(it->second.revision.revisionId);
    _impl->records.erase(it);
}

void OcctKernel::retainRevision(uint32_t id) {
    auto it = _impl->records.find(id);
    if (it == _impl->records.end()) {
        throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
    }
    ++it->second.refCount;
}

bool OcctKernel::releaseRevision(uint32_t id) {
    auto it = _impl->records.find(id);
    if (it == _impl->records.end()) {
        throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
    }
    --it->second.refCount;
    if (it->second.refCount > 0) {
        return false;
    }

    _impl->revisionToHandle.erase(it->second.revision.revisionId);
    _impl->records.erase(it);
    return true;
}

} // namespace occt_kernel
