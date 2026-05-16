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
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

// Boolean operations
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>

// Fillets / chamfers
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <TopExp_Explorer.hxx>

// Tessellation
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <Poly_Array1OfTriangle.hxx>
#include <TShort_Array1OfShortReal.hxx>
#include <gp_Vec.hxx>
#include <gp_Pnt2d.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom_Surface.hxx>

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
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
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
    BRepBuilderAPI_Transform transformer(sourceShape, transform, Standard_True);
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
        BRepAdaptor_Surface surface(face, Standard_False);
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
                                             const TopTools_IndexedDataMapOfShapeListOfShape& edgeToFaces) {
    std::vector<TopoDS_Face> faces;
    if (!edgeToFaces.Contains(edge)) {
        return faces;
    }

    const TopTools_ListOfShape& adjacent = edgeToFaces.FindFromKey(edge);
    for (TopTools_ListIteratorOfListOfShape it(adjacent); it.More(); it.Next()) {
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
                                const TopTools_IndexedMapOfShape& faceMap,
                                const TopTools_IndexedDataMapOfShapeListOfShape& edgeToFaces) {
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
        BRepAdaptor_Surface s1(adjacentFaces[0], Standard_False);
        BRepAdaptor_Surface s2(adjacentFaces[1], Standard_False);
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
    TopTools_IndexedMapOfShape faces, edges, vertices;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);

    TopTools_IndexedDataMapOfShapeListOfShape edgeToFaces;
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

    TopTools_IndexedMapOfShape faces, edges, vertices;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);

    TopTools_IndexedDataMapOfShapeListOfShape edgeToFaces;
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
            fixer->FixSolidTool()->CreateOpenSolidMode() = Standard_False;
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
                             sf.GetMessageString());
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        TopTools_IndexedMapOfShape edgeMap, faceMap;
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
    }
    return 0;
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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

        TopTools_IndexedMapOfShape faces, edges, vertices;
        TopExp::MapShapes(shape, TopAbs_FACE,   faces);
        TopExp::MapShapes(shape, TopAbs_EDGE,   edges);
        TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);

        TopTools_IndexedDataMapOfShapeListOfShape edgeToFaces;
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
    }
    return "{}";
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
        "\"checkpointV1\":true"
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

        BRepMesh_IncrementalMesh mesh(shape, linearDeflection, Standard_False, angularDeflection);
        mesh.Perform();

        TopTools_IndexedMapOfShape faceMap, edgeMap;
        TopExp::MapShapes(shape, TopAbs_FACE, faceMap);
        TopExp::MapShapes(shape, TopAbs_EDGE, edgeMap);

        TopTools_IndexedDataMapOfShapeListOfShape edgeToFaces;
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
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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
        throwKernelError("EXPORT_FAILED", sf.GetMessageString());
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
        throwKernelError("EXPORT_FAILED", sf.GetMessageString());
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
        throwKernelError("IMPORT_FAILED", sf.GetMessageString());
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
