/**
 * @file kernel.cpp
 * @brief Implementation of OcctKernel.
 */

#include "kernel.h"
#include "json_utils.h"
#include "profile_builder.h"
#include "wire_builder.h"

// OCCT foundation
#include <Standard_Version.hxx>
#include <Standard_ErrorHandler.hxx>
#include <Precision.hxx>

// Topology
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS.hxx>
#include <BRep_Builder.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GProp_GProps.hxx>

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
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
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
#include <BRepAlgoAPI_Section.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepExtrema_SupportType.hxx>

// Fillets / chamfers
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <Law_Constant.hxx>
#include <Law_Linear.hxx>
#include <NCollection_Array1.hxx>
#include <TopExp_Explorer.hxx>

// Canonical B-spline conversion used as a native retry basis for blends on
// swept support surfaces (Geom_SurfaceOfLinearExtrusion / Revolution / Offset).
#include <ShapeBuild_ReShape.hxx>
#include <ShapeCustom.hxx>
#include <ShapeCustom_ConvertToBSpline.hxx>
#include <BRepTools_Modifier.hxx>
#include <NCollection_DataMap.hxx>

// Tessellation
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Array1OfTriangle.hxx>
#include <TShort_Array1OfShortReal.hxx>
#include <gp_Vec.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Circ2d.hxx>
#include <gp_Elips.hxx>
#include <gp_Elips2d.hxx>
#include <gp_Lin2d.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom2dAdaptor_Curve.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_BezierCurve.hxx>
#include <Geom2dConvert.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Surface.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BezierCurve.hxx>
#include <GeomConvert.hxx>
#include <Approx_ParametrizationType.hxx>
#include <GeomAbs_Shape.hxx>
#include <BRepBuilderAPI_TransitionMode.hxx>
#include <BRepFill_TypeOfContact.hxx>

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
#include <TopAbs_State.hxx>
#include <TopAbs_ShapeEnum.hxx>

// String / stream helpers
#include <OSD_File.hxx>
#include <OSD_Path.hxx>
#include <OSD_Protection.hxx>

#include <fstream>
#include <memory>
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

static std::string g_lastKernelErrorJson;

static void throwKernelError(const std::string& code, const std::string& detail) {
    g_lastKernelErrorJson = makeErrorJson(code, detail);
    throw std::runtime_error(g_lastKernelErrorJson);
}

static bool looksLikeKernelErrorJson(const std::string& text) {
    return !text.empty()
        && text.front() == '{'
        && text.find("\"code\"") != std::string::npos
        && text.find("\"detail\"") != std::string::npos;
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

enum class StructuredSweepTrihedronMode {
    CorrectedFrenet,
    Frenet,
    Discrete,
    FixedTrihedron,
    FixedBinormal,
    AuxiliarySpine,
};

struct StructuredSweepSpec {
    PlaneFrame plane;
    std::string spineJson;
    StructuredSweepTrihedronMode trihedronMode = StructuredSweepTrihedronMode::CorrectedFrenet;
    PlaneFrame trihedronFrame;
    gp_Dir binormal = gp_Dir(0.0, 0.0, 1.0);
    std::string auxiliarySpineJson;
    bool curvilinearEquivalence = true;
    BRepFill_TypeOfContact auxiliaryContact = BRepFill_NoContact;
    bool sectionWithContact = false;
    bool sectionWithCorrection = false;
    bool solid = true;
    bool forceApproxC1 = false;
    BRepBuilderAPI_TransitionMode transitionMode = BRepBuilderAPI_Transformed;
    bool hasTolerance = false;
    double tol3d = 1.0e-4;
    double boundTol = 1.0e-4;
    double angularTol = 1.0e-2;
    bool hasMaxDegree = false;
    int maxDegree = 0;
    bool hasMaxSegments = false;
    int maxSegments = 0;
    bool cut = false;
};

enum class StructuredLoftSectionKind {
    Profile,
    Wire,
    Point,
};

struct StructuredLoftSection {
    StructuredLoftSectionKind kind = StructuredLoftSectionKind::Profile;
    std::string profileJson;
    PlaneFrame plane;
    std::string wireJson;
    gp_Pnt point = gp_Pnt(0.0, 0.0, 0.0);
};

struct StructuredLoftSpec {
    bool solid = true;
    bool ruled = false;
    double pres3d = 1.0e-6;
    bool hasCheckCompatibility = false;
    bool checkCompatibility = true;
    bool hasSmoothing = false;
    bool smoothing = false;
    bool hasParametrization = false;
    Approx_ParametrizationType parametrization = Approx_ChordLength;
    bool hasContinuity = false;
    GeomAbs_Shape continuity = GeomAbs_C2;
    bool hasCriteriumWeight = false;
    double criteriumWeight1 = 1.0;
    double criteriumWeight2 = 1.0;
    double criteriumWeight3 = 1.0;
    bool hasMaxDegree = false;
    int maxDegree = 0;
    bool hasMutableInput = false;
    bool mutableInput = true;
    bool cut = false;
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

void throwStructuredValidation(const std::string& operation,
                               const std::string& path,
                               const std::string& reason,
                               const std::string& unsupportedFeature);

void throwStructuredOperationError(const std::string& operation,
                                   const std::string& reason,
                                   const std::vector<std::string>& edgeRefs);

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

TopoDS_Wire placeProfileWire(const TopoDS_Wire& sourceWire, const PlaneFrame& plane)
{
    if (!plane.hasValue) {
        return sourceWire;
    }

    gp_Trsf transform;
    transform.SetDisplacement(
        gp_Ax3(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0), gp_Dir(1.0, 0.0, 0.0)),
        gp_Ax3(plane.origin, plane.normal, plane.xDirection)
    );

    return TopoDS::Wire(applyShapeTransform(sourceWire, transform, "profile placement"));
}

void requireSingleWireProfile(const std::string& profileJson,
                              const std::string& operation,
                              const std::string& path)
{
    const mini_json::Value root = mini_json::requireObject(mini_json::parse(profileJson), path + " profile");
    const mini_json::Value& wires = mini_json::requireArray(mini_json::requireMember(root, "wires", path), path + ".wires");
    if (wires.array.size() != 1) {
        throwStructuredValidation(operation,
                                  path,
                                  "Only single closed-wire sections are supported for this operation",
                                  "profile.multiWireSections");
    }
}

TopoDS_Shape composeFeatureWithBase(const TopoDS_Shape& baseShape,
                                    const TopoDS_Shape& featureShape,
                                    bool cut,
                                    const std::string& operation)
{
    if (cut) {
        BRepAlgoAPI_Cut booleanOp(baseShape, featureShape);
        booleanOp.Build();
        if (!booleanOp.IsDone() || booleanOp.Shape().IsNull()) {
            throwStructuredOperationError(operation, "Boolean cut failed for the built feature", {});
        }
        return booleanOp.Shape();
    }

    BRepAlgoAPI_Fuse booleanOp(baseShape, featureShape);
    booleanOp.Build();
    if (!booleanOp.IsDone() || booleanOp.Shape().IsNull()) {
        throwStructuredOperationError(operation, "Boolean fuse failed for the built feature", {});
    }
    return booleanOp.Shape();
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
    BRepBndLib::AddOptimal(shape, bbox, true, false);
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

const char* shapeTypeName(TopAbs_ShapeEnum type) {
    switch (type) {
    case TopAbs_COMPOUND: return "compound";
    case TopAbs_COMPSOLID: return "compSolid";
    case TopAbs_SOLID: return "solid";
    case TopAbs_SHELL: return "shell";
    case TopAbs_FACE: return "face";
    case TopAbs_WIRE: return "wire";
    case TopAbs_EDGE: return "edge";
    case TopAbs_VERTEX: return "vertex";
    case TopAbs_SHAPE: return "shape";
    }
    return "unknown";
}

const char* containmentStateName(TopAbs_State state) {
    switch (state) {
    case TopAbs_IN: return "in";
    case TopAbs_OUT: return "out";
    case TopAbs_ON: return "on";
    case TopAbs_UNKNOWN: return "unknown";
    }
    return "unknown";
}

std::vector<int> childIdsForShape(const TopoDS_Shape& parent,
                                  const ShapeMap& childMap,
                                  TopAbs_ShapeEnum childType) {
    ShapeMap local;
    TopExp::MapShapes(parent, childType, local);
    std::vector<int> ids;
    ids.reserve(static_cast<std::size_t>(local.Extent()));
    for (int index = 1; index <= local.Extent(); ++index) {
        const int mappedId = childMap.FindIndex(local(index));
        if (mappedId > 0) {
            ids.push_back(mappedId);
        }
    }
    return ids;
}

std::vector<int> ancestorIdsForShape(const TopoDS_Shape& child,
                                     const ShapeMap& ancestorMap,
                                     const ShapeToShapeListMap& childToAncestors) {
    std::vector<int> ids;
    if (!childToAncestors.Contains(child)) {
        return ids;
    }

    const ShapeList& ancestors = childToAncestors.FindFromKey(child);
    for (ShapeList::Iterator it(ancestors); it.More(); it.Next()) {
        const int mappedId = ancestorMap.FindIndex(it.Value());
        if (mappedId > 0 && std::find(ids.begin(), ids.end(), mappedId) == ids.end()) {
            ids.push_back(mappedId);
        }
    }
    return ids;
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

void appendPoint2Json(std::ostringstream& out, const gp_Pnt2d& point) {
    out << '[' << point.X() << ',' << point.Y() << ']';
}

void appendDirection2Json(std::ostringstream& out, const gp_Dir2d& direction) {
    out << '[' << direction.X() << ',' << direction.Y() << ']';
}

void appendPlaneFrameJson(std::ostringstream& out, const gp_Pln& plane) {
    out << "{\"origin\":";
    appendPointJson(out, plane.Location());
    out << ",\"normal\":";
    appendVectorJson(out, gp_Vec(plane.Axis().Direction()));
    out << ",\"xDirection\":";
    appendVectorJson(out, gp_Vec(plane.XAxis().Direction()));
    out << '}';
}

double normalizedCurveParameter(double value, double first, double last) {
    const double range = last - first;
    if (std::abs(range) <= 1.0e-18) return 0.0;
    return (value - first) / range;
}

struct OrientedCurveRange {
    double first = 0.0;
    double last = 0.0;
    bool reversed = false;
};

OrientedCurveRange orientCurveRangeForWire(const TopoDS_Edge& edge, const BRepAdaptor_Curve& curve) {
    const double domainFirst = curve.FirstParameter();
    const double domainLast = curve.LastParameter();
    bool reversed = edge.Orientation() == TopAbs_REVERSED;

    const TopoDS_Vertex startVertex = TopExp::FirstVertex(edge, true);
    const TopoDS_Vertex endVertex = TopExp::LastVertex(edge, true);
    if (!startVertex.IsNull() && !endVertex.IsNull() && !startVertex.IsSame(endVertex)) {
        const gp_Pnt wireStart = BRep_Tool::Pnt(startVertex);
        const gp_Pnt wireEnd = BRep_Tool::Pnt(endVertex);
        const gp_Pnt curveStart = curve.Value(domainFirst);
        const gp_Pnt curveEnd = curve.Value(domainLast);
        const double directError = curveStart.SquareDistance(wireStart) + curveEnd.SquareDistance(wireEnd);
        const double reverseError = curveStart.SquareDistance(wireEnd) + curveEnd.SquareDistance(wireStart);
        reversed = reverseError + 1.0e-18 < directError;
    }

    return {
        reversed ? domainLast : domainFirst,
        reversed ? domainFirst : domainLast,
        reversed,
    };
}

void appendCurveDomainJson(std::ostringstream& out, double first, double last) {
    out << "{\"first\":" << first << ",\"last\":" << last << '}';
}

void appendCurveTrimJson(std::ostringstream& out, double trimFirst, double trimLast, double domainFirst, double domainLast) {
    out << "{\"first\":" << trimFirst
        << ",\"last\":" << trimLast
        << ",\"normalizedFirst\":" << normalizedCurveParameter(trimFirst, domainFirst, domainLast)
        << ",\"normalizedLast\":" << normalizedCurveParameter(trimLast, domainFirst, domainLast)
        << '}';
}

void appendBsplineCurveJson(std::ostringstream& out, const Handle(Geom_BSplineCurve)& bspline) {
    out << "{\"degree\":" << bspline->Degree()
        << ",\"periodic\":" << (bspline->IsPeriodic() ? "true" : "false")
        << ",\"poles\":[";
    for (int i = 1; i <= bspline->NbPoles(); ++i) {
        if (i > 1) out << ',';
        appendPointJson(out, bspline->Pole(i));
    }
    out << "],\"weights\":[";
    for (int i = 1; i <= bspline->NbPoles(); ++i) {
        if (i > 1) out << ',';
        out << bspline->Weight(i);
    }
    out << "],\"knots\":[";
    for (int i = 1; i <= bspline->NbKnots(); ++i) {
        if (i > 1) out << ',';
        out << bspline->Knot(i);
    }
    out << "],\"multiplicities\":[";
    for (int i = 1; i <= bspline->NbKnots(); ++i) {
        if (i > 1) out << ',';
        out << bspline->Multiplicity(i);
    }
    out << "]}";
}

void appendBezierCurveJson(std::ostringstream& out, const Handle(Geom_BezierCurve)& bezier) {
    out << "{\"degree\":" << bezier->Degree() << ",\"poles\":[";
    for (int i = 1; i <= bezier->NbPoles(); ++i) {
        if (i > 1) out << ',';
        appendPointJson(out, bezier->Pole(i));
    }
    out << "],\"weights\":[";
    for (int i = 1; i <= bezier->NbPoles(); ++i) {
        if (i > 1) out << ',';
        out << bezier->Weight(i);
    }
    out << "]}";
}

void appendBsplineCurve2dJson(std::ostringstream& out, const Handle(Geom2d_BSplineCurve)& bspline) {
    out << "{\"degree\":" << bspline->Degree()
        << ",\"periodic\":" << (bspline->IsPeriodic() ? "true" : "false")
        << ",\"poles\":[";
    for (int i = 1; i <= bspline->NbPoles(); ++i) {
        if (i > 1) out << ',';
        appendPoint2Json(out, bspline->Pole(i));
    }
    out << "],\"weights\":[";
    for (int i = 1; i <= bspline->NbPoles(); ++i) {
        if (i > 1) out << ',';
        out << bspline->Weight(i);
    }
    out << "],\"knots\":[";
    for (int i = 1; i <= bspline->NbKnots(); ++i) {
        if (i > 1) out << ',';
        out << bspline->Knot(i);
    }
    out << "],\"multiplicities\":[";
    for (int i = 1; i <= bspline->NbKnots(); ++i) {
        if (i > 1) out << ',';
        out << bspline->Multiplicity(i);
    }
    out << "]}";
}

void appendBezierCurve2dJson(std::ostringstream& out, const Handle(Geom2d_BezierCurve)& bezier) {
    out << "{\"degree\":" << bezier->Degree() << ",\"poles\":[";
    for (int i = 1; i <= bezier->NbPoles(); ++i) {
        if (i > 1) out << ',';
        appendPoint2Json(out, bezier->Pole(i));
    }
    out << "],\"weights\":[";
    for (int i = 1; i <= bezier->NbPoles(); ++i) {
        if (i > 1) out << ',';
        out << bezier->Weight(i);
    }
    out << "]}";
}

void appendSpatialCurveDescriptorJson(std::ostringstream& out,
                                      const TopoDS_Edge& edge,
                                      double domainFirst,
                                      double domainLast,
                                      double trimFirst,
                                      double trimLast) {
    BRepAdaptor_Curve curve(edge);
    const gp_Pnt startPoint = curve.Value(trimFirst);
    const gp_Pnt midPoint = curve.Value(trimFirst + (trimLast - trimFirst) * 0.5);
    const gp_Pnt endPoint = curve.Value(trimLast);

    out << '{';
    out << "\"curveType\":";
    appendJsonString(out, curveTypeName(curve.GetType()));
    out << ",\"domain\":";
    appendCurveDomainJson(out, domainFirst, domainLast);
    out << ",\"trim\":";
    appendCurveTrimJson(out, trimFirst, trimLast, domainFirst, domainLast);
    out << ",\"startPoint\":";
    appendPointJson(out, startPoint);
    out << ",\"midPoint\":";
    appendPointJson(out, midPoint);
    out << ",\"endPoint\":";
    appendPointJson(out, endPoint);

    if (curve.GetType() == GeomAbs_Line) {
        const gp_Lin line = curve.Line();
        out << ",\"line\":{\"origin\":";
        appendPointJson(out, line.Location());
        out << ",\"direction\":";
        appendVectorJson(out, gp_Vec(line.Direction()));
        out << '}';
    } else if (curve.GetType() == GeomAbs_Circle) {
        const gp_Circ circle = curve.Circle();
        out << ",\"circle\":{\"center\":";
        appendPointJson(out, circle.Location());
        out << ",\"radius\":" << circle.Radius() << ",\"normal\":";
        appendVectorJson(out, gp_Vec(circle.Axis().Direction()));
        out << ",\"xDirection\":";
        appendVectorJson(out, gp_Vec(circle.XAxis().Direction()));
        out << '}';
    } else if (curve.GetType() == GeomAbs_Ellipse) {
        const gp_Elips ellipse = curve.Ellipse();
        out << ",\"ellipse\":{\"center\":";
        appendPointJson(out, ellipse.Location());
        out << ",\"majorRadius\":" << ellipse.MajorRadius() << ",\"minorRadius\":" << ellipse.MinorRadius() << ",\"normal\":";
        appendVectorJson(out, gp_Vec(ellipse.Axis().Direction()));
        out << ",\"xDirection\":";
        appendVectorJson(out, gp_Vec(ellipse.XAxis().Direction()));
        out << '}';
    } else if (curve.GetType() == GeomAbs_BSplineCurve) {
        out << ",\"bspline\":";
        appendBsplineCurveJson(out, curve.BSpline());
    } else if (curve.GetType() == GeomAbs_BezierCurve) {
        out << ",\"bezier\":";
        appendBezierCurveJson(out, curve.Bezier());
    } else {
        Standard_Real exactFirst = 0.0;
        Standard_Real exactLast = 0.0;
        Handle(Geom_Curve) exactCurve = BRep_Tool::Curve(TopoDS::Edge(edge.Oriented(TopAbs_FORWARD)), exactFirst, exactLast);
        if (!exactCurve.IsNull()) {
            Handle(Geom_BSplineCurve) bspline = GeomConvert::CurveToBSplineCurve(exactCurve);
            if (!bspline.IsNull()) {
                out << ",\"bspline\":";
                appendBsplineCurveJson(out, bspline);
            }
        }
    }

    out << '}';
}

void appendPlanarCurveDescriptorJson(std::ostringstream& out,
                                     const Handle(Geom2d_Curve)& curveHandle,
                                     double domainFirst,
                                     double domainLast,
                                     double trimFirst,
                                     double trimLast) {
    const double domainMin = std::min(domainFirst, domainLast);
    const double domainMax = std::max(domainFirst, domainLast);
    Geom2dAdaptor_Curve curve(curveHandle, domainMin, domainMax);
    const gp_Pnt2d startPoint = curve.Value(trimFirst);
    const gp_Pnt2d midPoint = curve.Value(trimFirst + (trimLast - trimFirst) * 0.5);
    const gp_Pnt2d endPoint = curve.Value(trimLast);

    out << '{';
    out << "\"curveType\":";
    appendJsonString(out, curveTypeName(curve.GetType()));
    out << ",\"domain\":";
    appendCurveDomainJson(out, domainFirst, domainLast);
    out << ",\"trim\":";
    appendCurveTrimJson(out, trimFirst, trimLast, domainFirst, domainLast);
    out << ",\"startPoint\":";
    appendPoint2Json(out, startPoint);
    out << ",\"midPoint\":";
    appendPoint2Json(out, midPoint);
    out << ",\"endPoint\":";
    appendPoint2Json(out, endPoint);

    if (curve.GetType() == GeomAbs_Line) {
        const gp_Lin2d line = curve.Line();
        out << ",\"line\":{\"origin\":";
        appendPoint2Json(out, line.Location());
        out << ",\"direction\":";
        appendDirection2Json(out, line.Direction());
        out << '}';
    } else if (curve.GetType() == GeomAbs_Circle) {
        const gp_Circ2d circle = curve.Circle();
        out << ",\"circle\":{\"center\":";
        appendPoint2Json(out, circle.Location());
        out << ",\"radius\":" << circle.Radius() << '}';
    } else if (curve.GetType() == GeomAbs_Ellipse) {
        const gp_Elips2d ellipse = curve.Ellipse();
        out << ",\"ellipse\":{\"center\":";
        appendPoint2Json(out, ellipse.Location());
        out << ",\"xDirection\":";
        appendDirection2Json(out, ellipse.XAxis().Direction());
        out << ",\"majorRadius\":" << ellipse.MajorRadius() << ",\"minorRadius\":" << ellipse.MinorRadius() << '}';
    } else if (curve.GetType() == GeomAbs_BSplineCurve) {
        out << ",\"bspline\":";
        appendBsplineCurve2dJson(out, curve.BSpline());
    } else if (curve.GetType() == GeomAbs_BezierCurve) {
        out << ",\"bezier\":";
        appendBezierCurve2dJson(out, curve.Bezier());
    } else {
        Handle(Geom2d_BSplineCurve) bspline = Geom2dConvert::CurveToBSplineCurve(curveHandle);
        if (!bspline.IsNull()) {
            out << ",\"bspline\":";
            appendBsplineCurve2dJson(out, bspline);
        }
    }

    out << '}';
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

StructuredSweepSpec parseStructuredSweepSpec(const std::string& specJson,
                                             const std::string& operation,
                                             const std::string& pathRoot) {
    const mini_json::Value root = mini_json::requireObject(mini_json::parse(specJson), pathRoot + " spec");
    requireSchemaVersion(root, operation);
    const bool allowUnknownFields = optionalBoolMember(root, "allowUnknownFields", false);
    rejectUnknownFields(root,
                        { "schemaVersion", "allowUnknownFields", "unit", "plane", "spineJson", "trihedronMode", "sectionWithContact", "sectionWithCorrection", "solid", "forceApproxC1", "transitionMode", "tolerance", "maxDegree", "maxSegments", "cut", "metadata" },
                        operation,
                        pathRoot,
                        allowUnknownFields);

    if (const mini_json::Value* unitValue = root.get("unit")) {
        const mini_json::Value& unit = mini_json::requireObject(*unitValue, pathRoot + ".unit");
        rejectUnknownFields(unit, { "length", "angle" }, operation, pathRoot + ".unit", allowUnknownFields);
        if (hasMember(unit, "length") && optionalStringMember(unit, "length") != "model") {
            throwStructuredValidation(operation, pathRoot + ".unit.length", "Only model length units are supported");
        }
        if (hasMember(unit, "angle")) {
            const std::string angleUnit = optionalStringMember(unit, "angle");
            if (angleUnit != "radians" && angleUnit != "degrees") {
                throwStructuredValidation(operation, pathRoot + ".unit.angle", "Angle unit must be radians or degrees");
            }
        }
    }

    StructuredSweepSpec spec;
    if (const mini_json::Value* planeValue = root.get("plane")) {
        spec.plane = parsePlaneFrame(mini_json::requireObject(*planeValue, pathRoot + ".plane"));
    }

    spec.spineJson = mini_json::requireString(mini_json::requireMember(root, "spineJson", pathRoot), pathRoot + ".spineJson");
    spec.sectionWithContact = optionalBoolMember(root, "sectionWithContact", false);
    spec.sectionWithCorrection = optionalBoolMember(root, "sectionWithCorrection", false);
    spec.solid = !hasMember(root, "solid") || optionalBoolMember(root, "solid", true);
    spec.forceApproxC1 = optionalBoolMember(root, "forceApproxC1", false);
    spec.cut = optionalBoolMember(root, "cut", false);

    if (spec.cut && !spec.solid) {
        throwStructuredValidation(operation,
                                  pathRoot + ".solid",
                                  "Cut sweep operations require solid sections");
    }

    if (hasMember(root, "transitionMode")) {
        const std::string transitionMode = optionalStringMember(root, "transitionMode");
        if (transitionMode == "transformed") {
            spec.transitionMode = BRepBuilderAPI_Transformed;
        } else if (transitionMode == "rightCorner") {
            spec.transitionMode = BRepBuilderAPI_RightCorner;
        } else if (transitionMode == "roundCorner") {
            spec.transitionMode = BRepBuilderAPI_RoundCorner;
        } else {
            throwStructuredValidation(operation,
                                      pathRoot + ".transitionMode",
                                      "Unsupported sweep transition mode");
        }
    }

    if (const mini_json::Value* toleranceValue = root.get("tolerance")) {
        const mini_json::Value& tolerance = mini_json::requireObject(*toleranceValue, pathRoot + ".tolerance");
        rejectUnknownFields(tolerance, { "tol3d", "boundTol", "angularTol" }, operation, pathRoot + ".tolerance", allowUnknownFields);
        spec.hasTolerance = true;
        if (hasMember(tolerance, "tol3d")) {
            spec.tol3d = requirePositiveMember(tolerance, "tol3d", operation, pathRoot + ".tolerance.tol3d");
        }
        if (hasMember(tolerance, "boundTol")) {
            spec.boundTol = requirePositiveMember(tolerance, "boundTol", operation, pathRoot + ".tolerance.boundTol");
        }
        if (hasMember(tolerance, "angularTol")) {
            spec.angularTol = requirePositiveMember(tolerance, "angularTol", operation, pathRoot + ".tolerance.angularTol");
        }
    }

    if (hasMember(root, "maxDegree")) {
        spec.hasMaxDegree = true;
        spec.maxDegree = requirePositiveIntMember(root, "maxDegree", operation, pathRoot + ".maxDegree");
    }
    if (hasMember(root, "maxSegments")) {
        spec.hasMaxSegments = true;
        spec.maxSegments = requirePositiveIntMember(root, "maxSegments", operation, pathRoot + ".maxSegments");
    }

    if (const mini_json::Value* trihedronModeValue = root.get("trihedronMode")) {
        const mini_json::Value& trihedronMode = mini_json::requireObject(*trihedronModeValue, pathRoot + ".trihedronMode");
        const std::string trihedronType = optionalStringMember(trihedronMode, "type");
        if (trihedronType.empty()) {
            throwStructuredValidation(operation, pathRoot + ".trihedronMode.type", "Trihedron mode type is required");
        }

        if (trihedronType == "correctedFrenet") {
            rejectUnknownFields(trihedronMode, { "type" }, operation, pathRoot + ".trihedronMode", allowUnknownFields);
            spec.trihedronMode = StructuredSweepTrihedronMode::CorrectedFrenet;
        } else if (trihedronType == "frenet") {
            rejectUnknownFields(trihedronMode, { "type" }, operation, pathRoot + ".trihedronMode", allowUnknownFields);
            spec.trihedronMode = StructuredSweepTrihedronMode::Frenet;
        } else if (trihedronType == "discrete") {
            rejectUnknownFields(trihedronMode, { "type" }, operation, pathRoot + ".trihedronMode", allowUnknownFields);
            spec.trihedronMode = StructuredSweepTrihedronMode::Discrete;
        } else if (trihedronType == "fixedTrihedron") {
            rejectUnknownFields(trihedronMode, { "type", "frame" }, operation, pathRoot + ".trihedronMode", allowUnknownFields);
            spec.trihedronMode = StructuredSweepTrihedronMode::FixedTrihedron;
            spec.trihedronFrame = parsePlaneFrame(mini_json::requireObject(mini_json::requireMember(trihedronMode, "frame", pathRoot + ".trihedronMode"), pathRoot + ".trihedronMode.frame"));
        } else if (trihedronType == "fixedBinormal") {
            rejectUnknownFields(trihedronMode, { "type", "binormal" }, operation, pathRoot + ".trihedronMode", allowUnknownFields);
            spec.trihedronMode = StructuredSweepTrihedronMode::FixedBinormal;
            spec.binormal = parseDirection3(mini_json::requireMember(trihedronMode, "binormal", pathRoot + ".trihedronMode"), pathRoot + ".trihedronMode.binormal");
        } else if (trihedronType == "auxiliarySpine") {
            rejectUnknownFields(trihedronMode, { "type", "spineJson", "curvilinearEquivalence", "contact" }, operation, pathRoot + ".trihedronMode", allowUnknownFields);
            spec.trihedronMode = StructuredSweepTrihedronMode::AuxiliarySpine;
            spec.auxiliarySpineJson = mini_json::requireString(mini_json::requireMember(trihedronMode, "spineJson", pathRoot + ".trihedronMode"), pathRoot + ".trihedronMode.spineJson");
            spec.curvilinearEquivalence = !hasMember(trihedronMode, "curvilinearEquivalence") || optionalBoolMember(trihedronMode, "curvilinearEquivalence", true);
            if (hasMember(trihedronMode, "contact")) {
                const std::string contact = optionalStringMember(trihedronMode, "contact");
                if (contact == "none") {
                    spec.auxiliaryContact = BRepFill_NoContact;
                } else if (contact == "contact") {
                    spec.auxiliaryContact = BRepFill_Contact;
                } else if (contact == "contactOnBorder") {
                    spec.auxiliaryContact = BRepFill_ContactOnBorder;
                } else {
                    throwStructuredValidation(operation,
                                              pathRoot + ".trihedronMode.contact",
                                              "Unsupported auxiliary-spine contact mode");
                }
            }
        } else {
            throwStructuredValidation(operation,
                                      pathRoot + ".trihedronMode.type",
                                      "Unsupported trihedron mode");
        }
    }

    return spec;
}

std::vector<StructuredLoftSection> parseStructuredLoftSections(const std::string& sectionsJson,
                                                               bool allowUnknownFields,
                                                               const std::string& operation,
                                                               const std::string& pathRoot) {
    const mini_json::Value sections = mini_json::requireArray(mini_json::parse(sectionsJson), pathRoot + ".sections");
    if (sections.array.size() < 2) {
        throwStructuredValidation(operation, pathRoot + ".sections", "At least two loft sections are required");
    }

    std::vector<StructuredLoftSection> result;
    result.reserve(sections.array.size());
    for (std::size_t index = 0; index < sections.array.size(); ++index) {
        const std::string sectionPath = pathRoot + ".sections[" + std::to_string(index) + "]";
        const mini_json::Value& section = mini_json::requireObject(sections.array[index], sectionPath);
        const std::string type = optionalStringMember(section, "type");
        if (type.empty()) {
            throwStructuredValidation(operation, sectionPath + ".type", "Section type is required");
        }

        StructuredLoftSection entry;
        if (type == "profile") {
            rejectUnknownFields(section, { "type", "profileJson", "plane" }, operation, sectionPath, allowUnknownFields);
            entry.kind = StructuredLoftSectionKind::Profile;
            entry.profileJson = mini_json::requireString(mini_json::requireMember(section, "profileJson", sectionPath), sectionPath + ".profileJson");
            if (const mini_json::Value* planeValue = section.get("plane")) {
                entry.plane = parsePlaneFrame(mini_json::requireObject(*planeValue, sectionPath + ".plane"));
            }
        } else if (type == "wire") {
            rejectUnknownFields(section, { "type", "wireJson" }, operation, sectionPath, allowUnknownFields);
            entry.kind = StructuredLoftSectionKind::Wire;
            entry.wireJson = mini_json::requireString(mini_json::requireMember(section, "wireJson", sectionPath), sectionPath + ".wireJson");
        } else if (type == "point") {
            rejectUnknownFields(section, { "type", "point" }, operation, sectionPath, allowUnknownFields);
            entry.kind = StructuredLoftSectionKind::Point;
            entry.point = parsePoint3(mini_json::requireMember(section, "point", sectionPath), sectionPath + ".point");
        } else {
            throwStructuredValidation(operation, sectionPath + ".type", "Unsupported loft section type");
        }

        result.push_back(entry);
    }

    bool hasWireSection = false;
    for (std::size_t index = 0; index < result.size(); ++index) {
        if (result[index].kind == StructuredLoftSectionKind::Point) {
            if (index != 0 && index + 1 != result.size()) {
                throwStructuredValidation(operation,
                                          pathRoot + ".sections[" + std::to_string(index) + "]",
                                          "Point loft sections are only allowed at the start or end");
            }
        } else {
            hasWireSection = true;
        }
    }
    if (!hasWireSection) {
        throwStructuredValidation(operation, pathRoot + ".sections", "At least one wire/profile section is required");
    }

    return result;
}

StructuredLoftSpec parseStructuredLoftSpec(const std::string& specJson,
                                           const std::string& operation,
                                           const std::string& pathRoot,
                                           bool& allowUnknownFields) {
    const mini_json::Value root = mini_json::requireObject(mini_json::parse(specJson), pathRoot + " spec");
    requireSchemaVersion(root, operation);
    allowUnknownFields = optionalBoolMember(root, "allowUnknownFields", false);
    rejectUnknownFields(root,
                        { "schemaVersion", "allowUnknownFields", "solid", "ruled", "pres3d", "checkCompatibility", "smoothing", "parametrization", "continuity", "criteriumWeight", "maxDegree", "mutableInput", "cut", "metadata" },
                        operation,
                        pathRoot,
                        allowUnknownFields);

    StructuredLoftSpec spec;
    spec.solid = !hasMember(root, "solid") || optionalBoolMember(root, "solid", true);
    spec.ruled = optionalBoolMember(root, "ruled", false);
    spec.cut = optionalBoolMember(root, "cut", false);
    if (hasMember(root, "pres3d")) {
        spec.pres3d = requirePositiveMember(root, "pres3d", operation, pathRoot + ".pres3d");
    }
    if (spec.cut && !spec.solid) {
        throwStructuredValidation(operation,
                                  pathRoot + ".solid",
                                  "Cut loft operations require solid sections");
    }

    if (hasMember(root, "checkCompatibility")) {
        spec.hasCheckCompatibility = true;
        spec.checkCompatibility = optionalBoolMember(root, "checkCompatibility", true);
    }
    if (hasMember(root, "smoothing")) {
        spec.hasSmoothing = true;
        spec.smoothing = optionalBoolMember(root, "smoothing", false);
    }
    if (hasMember(root, "parametrization")) {
        spec.hasParametrization = true;
        const std::string parametrization = optionalStringMember(root, "parametrization");
        if (parametrization == "chordLength") {
            spec.parametrization = Approx_ChordLength;
        } else if (parametrization == "centripetal") {
            spec.parametrization = Approx_Centripetal;
        } else if (parametrization == "isoParametric") {
            spec.parametrization = Approx_IsoParametric;
        } else {
            throwStructuredValidation(operation,
                                      pathRoot + ".parametrization",
                                      "Unsupported loft parametrization mode");
        }
    }
    if (hasMember(root, "continuity")) {
        spec.hasContinuity = true;
        const std::string continuity = optionalStringMember(root, "continuity");
        if (continuity == "C0") {
            spec.continuity = GeomAbs_C0;
        } else if (continuity == "G1") {
            spec.continuity = GeomAbs_G1;
        } else if (continuity == "C1") {
            spec.continuity = GeomAbs_C1;
        } else if (continuity == "G2") {
            spec.continuity = GeomAbs_G2;
        } else if (continuity == "C2") {
            spec.continuity = GeomAbs_C2;
        } else if (continuity == "C3") {
            spec.continuity = GeomAbs_C3;
        } else if (continuity == "CN") {
            spec.continuity = GeomAbs_CN;
        } else {
            throwStructuredValidation(operation,
                                      pathRoot + ".continuity",
                                      "Unsupported loft continuity mode");
        }
    }
    if (const mini_json::Value* weightsValue = root.get("criteriumWeight")) {
        const mini_json::Value& weights = mini_json::requireObject(*weightsValue, pathRoot + ".criteriumWeight");
        rejectUnknownFields(weights, { "w1", "w2", "w3" }, operation, pathRoot + ".criteriumWeight", allowUnknownFields);
        spec.hasCriteriumWeight = true;
        spec.criteriumWeight1 = requirePositiveMember(weights, "w1", operation, pathRoot + ".criteriumWeight.w1");
        spec.criteriumWeight2 = requirePositiveMember(weights, "w2", operation, pathRoot + ".criteriumWeight.w2");
        spec.criteriumWeight3 = requirePositiveMember(weights, "w3", operation, pathRoot + ".criteriumWeight.w3");
    }
    if (hasMember(root, "maxDegree")) {
        spec.hasMaxDegree = true;
        spec.maxDegree = requirePositiveIntMember(root, "maxDegree", operation, pathRoot + ".maxDegree");
    }
    if (hasMember(root, "mutableInput")) {
        spec.hasMutableInput = true;
        spec.mutableInput = optionalBoolMember(root, "mutableInput", true);
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

std::vector<std::string> vertexStableHashesForShape(const TopoDS_Shape& shape, const RevisionMetadata* revision) {
    ShapeMap vertices;
    TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);

    std::vector<std::string> vertexHashes(static_cast<std::size_t>(vertices.Extent()) + 1);
    for (int i = 1; i <= vertices.Extent(); ++i) {
        vertexHashes[static_cast<std::size_t>(i)] = revision != nullptr && revision->vertexStableHashes.size() == static_cast<std::size_t>(vertices.Extent())
            ? revision->vertexStableHashes[static_cast<std::size_t>(i - 1)]
            : makeVertexStableHash(TopoDS::Vertex(vertices(i)));
    }
    return vertexHashes;
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

void appendEdgeRefArrayJson(std::ostringstream& out, const std::vector<ResolvedEdgeRef>& edges) {
    out << '[';
    for (std::size_t i = 0; i < edges.size(); ++i) {
        if (i > 0) out << ',';
        appendEntityRefJson(out, edges[i]);
    }
    out << ']';
}

void appendFaceRefArrayJson(std::ostringstream& out, const std::vector<ResolvedFaceRef>& faces) {
    out << '[';
    for (std::size_t i = 0; i < faces.size(); ++i) {
        if (i > 0) out << ',';
        appendEntityRefJson(out, faces[i]);
    }
    out << ']';
}

void appendSupportRefJson(std::ostringstream& out,
                          const ShapeRecord& record,
                          const TopoDS_Shape& support,
                          BRepExtrema_SupportType supportType,
                          bool hasEdgeParameter = false,
                          double edgeParameter = 0.0,
                          bool hasFaceUv = false,
                          double faceU = 0.0,
                          double faceV = 0.0) {
    out << "{";

    switch (supportType) {
    case BRepExtrema_IsVertex: {
        out << "\"kind\":\"vertex\"";
        ShapeMap vertices;
        TopExp::MapShapes(record.shape, TopAbs_VERTEX, vertices);
        const int topoId = vertices.FindIndex(support);
        if (topoId > 0) {
            const std::vector<std::string> hashes = vertexStableHashesForShape(record.shape, &record.revision);
            out << ",\"topoId\":" << topoId;
            out << ",\"stableHash\":"; appendJsonString(out, hashes[static_cast<std::size_t>(topoId)]);
        }
        break;
    }
    case BRepExtrema_IsOnEdge: {
        out << "\"kind\":\"edge\"";
        ShapeMap edges;
        TopExp::MapShapes(record.shape, TopAbs_EDGE, edges);
        const int topoId = edges.FindIndex(support);
        if (topoId > 0) {
            const std::vector<std::string> hashes = edgeStableHashesForShape(record.shape, &record.revision);
            out << ",\"topoId\":" << topoId;
            out << ",\"stableHash\":"; appendJsonString(out, hashes[static_cast<std::size_t>(topoId)]);
        }
        if (hasEdgeParameter) {
            out << ",\"parameter\":" << edgeParameter;
        }
        break;
    }
    case BRepExtrema_IsInFace: {
        out << "\"kind\":\"face\"";
        ShapeMap faces;
        TopExp::MapShapes(record.shape, TopAbs_FACE, faces);
        const int topoId = faces.FindIndex(support);
        if (topoId > 0) {
            const std::vector<std::string> hashes = faceStableHashesForShape(record.shape, &record.revision);
            out << ",\"topoId\":" << topoId;
            out << ",\"stableHash\":"; appendJsonString(out, hashes[static_cast<std::size_t>(topoId)]);
        }
        if (hasFaceUv) {
            out << ",\"uv\":[" << faceU << "," << faceV << "]";
        }
        break;
    }
    }

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

bool faceContainsEdge(const TopoDS_Face& face, const TopoDS_Edge& edge) {
    for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
        if (ex.Current().IsSame(edge)) {
            return true;
        }
    }
    return false;
}

ResolvedFaceRef resolvedFaceByTopoId(const TopoDS_Shape& shape,
                                     const RevisionMetadata* revision,
                                     int topoId) {
    ShapeMap faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    const std::vector<std::string> hashes = faceStableHashesForShape(shape, revision);
    if (topoId < 1 || topoId > faces.Extent()) {
        return ResolvedFaceRef();
    }
    return { TopoDS::Face(faces(topoId)), topoId, hashes[static_cast<std::size_t>(topoId)] };
}

bool isSplineLikeEdge(const TopoDS_Edge& edge) {
    try {
        BRepAdaptor_Curve curve(edge);
        const GeomAbs_CurveType type = curve.GetType();
        return type == GeomAbs_BSplineCurve || type == GeomAbs_BezierCurve || type == GeomAbs_OtherCurve;
    } catch (const Standard_Failure&) {
        return false;
    }
}

bool isPlanarFace(const TopoDS_Face& face) {
    try {
        return BRepAdaptor_Surface(face, false).GetType() == GeomAbs_Plane;
    } catch (const Standard_Failure&) {
        return false;
    }
}

ResolvedFaceRef selectSymmetricChamferBuilderFace(const TopoDS_Shape& shape,
                                                  const RevisionMetadata* revision,
                                                  const ResolvedEdgeRef& edge,
                                                  const ResolvedFaceRef& requestedFace) {
    if (!isSplineLikeEdge(edge.edge)) {
        return requestedFace;
    }
    for (int faceId : supportFaceIdsForEdge(shape, edge.edge)) {
        ResolvedFaceRef candidate = resolvedFaceByTopoId(shape, revision, faceId);
        if (!candidate.face.IsNull() && !isPlanarFace(candidate.face)) {
            return candidate;
        }
    }
    return requestedFace;
}

void throwReferenceFaceValidationError(const std::string& operation,
                                       const std::string& path,
                                       const TopoDS_Shape& shape,
                                       const ResolvedEdgeRef& edge,
                                       const ResolvedFaceRef& face) {
    std::ostringstream detail;
    detail << "{";
    detail << "\"phase\":\"validation\",";
    detail << "\"operation\":"; appendJsonString(detail, operation); detail << ",";
    detail << "\"path\":"; appendJsonString(detail, path); detail << ",";
    detail << "\"reason\":\"referenceFace must be one of the selected edge's adjacent faces\",";
    detail << "\"edge\":"; appendEntityRefJson(detail, edge); detail << ",";
    detail << "\"referenceFace\":"; appendEntityRefJson(detail, face); detail << ",";
    detail << "\"adjacentFaceIds\":"; appendIntArrayJson(detail, supportFaceIdsForEdge(shape, edge.edge));
    detail << "}";
    throwKernelError("INVALID_PARAMS", detail.str());
}

void requireReferenceFaceAdjacentToEdge(const TopoDS_Shape& shape,
                                        const ResolvedEdgeRef& edge,
                                        const ResolvedFaceRef& face,
                                        const std::string& operation,
                                        const std::string& path) {
    if (!faceContainsEdge(face.face, edge.edge)) {
        throwReferenceFaceValidationError(operation, path, shape, edge, face);
    }
}

template <typename BlendBuilder>
std::string blendBuilderDiagnosticsJson(BlendBuilder& builder,
                                        const std::vector<ResolvedEdgeRef>& edges) {
    try {
        std::ostringstream out;
        out << "{";
        out << "\"isDone\":" << (builder.IsDone() ? "true" : "false") << ",";
        const int contourCount = builder.NbContours();
        out << "\"contourCount\":" << contourCount << ",";
        out << "\"contours\":[";
        for (int contour = 1; contour <= contourCount; ++contour) {
            if (contour > 1) out << ',';
            out << "{";
            out << "\"index\":" << contour << ",";
            out << "\"edgeCount\":" << builder.NbEdges(contour) << ",";
            out << "\"length\":" << builder.Length(contour) << ",";
            out << "\"closed\":" << (builder.Closed(contour) ? "true" : "false") << ",";
            out << "\"closedAndTangent\":" << (builder.ClosedAndTangent(contour) ? "true" : "false") << ",";
            out << "\"requestedEdgeTopoIds\":[";
            bool first = true;
            for (const ResolvedEdgeRef& edge : edges) {
                if (builder.Contour(edge.edge) == contour) {
                    if (!first) out << ',';
                    first = false;
                    out << edge.topoId;
                }
            }
            out << "]}";
        }
        out << "]}";
        return out.str();
    } catch (const Standard_Failure& sf) {
        std::ostringstream out;
        out << "{\"diagnosticsError\":"; appendJsonString(out, sf.what()); out << "}";
        return out.str();
    }
}

void throwStructuredBlendOperationError(const std::string& operation,
                                        const std::string& reason,
                                        const TopoDS_Shape& shape,
                                        const RevisionMetadata* revision,
                                        const std::vector<ResolvedEdgeRef>& edgeRefs,
                                        const std::vector<ResolvedFaceRef>& faceRefs,
                                        const std::string& builderDiagnosticsJson) {
    ShapeMap edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    const bool shapeValid = BRepCheck_Analyzer(shape).IsValid();

    std::ostringstream detail;
    detail << "{";
    detail << "\"phase\":\"execution\",";
    detail << "\"operation\":"; appendJsonString(detail, operation); detail << ",";
    detail << "\"message\":"; appendJsonString(detail, reason); detail << ",";
    detail << "\"reason\":"; appendJsonString(detail, reason); detail << ",";
    detail << "\"requestedEdgeRefs\":"; appendEdgeRefArrayJson(detail, edgeRefs); detail << ",";
    detail << "\"failingEdgeRefs\":[";
    for (std::size_t i = 0; i < edgeRefs.size(); ++i) {
        if (i > 0) detail << ',';
        appendJsonString(detail, stableEdgeRefString(edgeRefs[i]));
    }
    detail << "],";
    detail << "\"resolvedFaceRefs\":"; appendFaceRefArrayJson(detail, faceRefs); detail << ",";
    detail << "\"shapeValid\":" << (shapeValid ? "true" : "false") << ",";
    detail << "\"shapeEdgeCount\":" << edges.Extent() << ",";
    detail << "\"builder\":" << (builderDiagnosticsJson.empty() ? "{}" : builderDiagnosticsJson);
    if (revision != nullptr) {
        detail << ",\"revisionId\":"; appendJsonString(detail, revision->revisionId);
        detail << ",\"operationId\":";
        if (revision->operationId.empty()) detail << "null"; else appendJsonString(detail, revision->operationId);
        detail << ",\"topologyHash\":"; appendJsonString(detail, revision->topologyHash);
    }
    detail << "}";
    throwKernelError("OPERATION_FAILED", detail.str());
}

template <typename ShapeListT>
void appendHistoryResultIndices(const ShapeListT& list,
                                const ShapeMap& resultMap,
                                TopAbs_ShapeEnum kind,
                                std::vector<int>& indices);

template <typename Builder>
std::vector<int> historyResultIndices(Builder& builder,
                                      const TopoDS_Shape& sourceShape,
                                      const ShapeMap& resultMap,
                                      TopAbs_ShapeEnum kind);

void appendBlendOutputFaceRefJson(std::ostringstream& out, const ResolvedFaceRef& face) {
    out << '{';
    out << "\"stableHash\":"; appendJsonString(out, face.stableHash); out << ',';
    out << "\"topoFaceId\":" << face.topoId;
    out << '}';
}

void appendBlendOutputFaceRefsJson(std::ostringstream& out, const std::vector<ResolvedFaceRef>& faces) {
    out << '[';
    for (std::size_t i = 0; i < faces.size(); ++i) {
        if (i > 0) out << ',';
        appendBlendOutputFaceRefJson(out, faces[i]);
    }
    out << ']';
}

template <typename BlendBuilder>
std::vector<ResolvedFaceRef> resolveBlendOutputFaces(BlendBuilder& builder,
                                                     const ShapeMap& resultFaces,
                                                     const std::vector<std::string>& faceHashes,
                                                     const ResolvedEdgeRef& edge) {
    std::vector<int> finalFaceIds;
    const ShapeList& generatedFaces = builder.Generated(edge.edge);
    appendHistoryResultIndices(generatedFaces, resultFaces, TopAbs_FACE, finalFaceIds);

    std::sort(finalFaceIds.begin(), finalFaceIds.end());

    std::vector<ResolvedFaceRef> resolvedFaces;
    resolvedFaces.reserve(finalFaceIds.size());
    for (int faceId : finalFaceIds) {
        if (faceId < 1 || faceId > resultFaces.Extent()) {
            continue;
        }
        resolvedFaces.push_back({ TopoDS::Face(resultFaces(faceId)), faceId, faceHashes[static_cast<std::size_t>(faceId)] });
    }
    return resolvedFaces;
}

template <typename BlendBuilder>
void appendBlendFacesJson(std::ostringstream& out,
                          BlendBuilder& builder,
                          const TopoDS_Shape& sourceShape,
                          const TopoDS_Shape& resultShape,
                          const RevisionMetadata& resultRevision,
                          const std::vector<ResolvedEdgeRef>& edges,
                          const std::vector<std::string>& normalizedParameters,
                          const std::string& faceKind) {
    ShapeMap resultFaces;
    TopExp::MapShapes(resultShape, TopAbs_FACE, resultFaces);
    const std::vector<std::string> faceHashes = faceStableHashesForShape(resultShape, &resultRevision);

    out << '[';
    bool firstFace = true;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const std::vector<ResolvedFaceRef> finalFaces = resolveBlendOutputFaces(builder, resultFaces, faceHashes, edges[i]);

        if (!firstFace) out << ',';
        firstFace = false;
        out << '{';
        out << "\"kind\":"; appendJsonString(out, faceKind); out << ',';
        if (finalFaces.size() == 1) {
            out << "\"stableHash\":"; appendJsonString(out, finalFaces[0].stableHash); out << ',';
            out << "\"topoFaceId\":" << finalFaces[0].topoId << ',';
            out << "\"finalOutputFaceRef\":";
            appendBlendOutputFaceRefJson(out, finalFaces[0]);
            out << ',';
        } else {
            out << "\"stableHash\":null,";
            out << "\"finalOutputFaceRefs\":";
            appendBlendOutputFaceRefsJson(out, finalFaces);
            out << ',';
        }
        out << "\"sourceEdge\":"; appendEntityRefJson(out, edges[i]); out << ',';
        out << "\"tangentChainEdgeRefs\":["; appendEntityRefJson(out, edges[i]); out << "],";
        out << "\"usedParameters\":" << normalizedParameters[i] << ',';
        out << "\"supportingFaceIds\":"; appendIntArrayJson(out, supportFaceIdsForEdge(sourceShape, edges[i].edge)); out << ',';
        out << "\"terminalCapIds\":[],";
        out << "\"terminalCondition\":\"unresolved\"";
        out << '}';
    }
    out << ']';
}

template <typename BlendBuilder>
std::string makeBlendResultJson(OcctKernel* kernel,
                                uint32_t shapeId,
                                const TopoDS_Shape& resultShape,
                                const RevisionMetadata& resultRevision,
                                BlendBuilder& builder,
                                const TopoDS_Shape& sourceShape,
                                const std::vector<ResolvedEdgeRef>& edges,
                                const std::vector<std::string>& normalizedParameters,
                                const std::string& faceKind) {
    std::ostringstream generatedFaces;
    appendBlendFacesJson(generatedFaces, builder, sourceShape, resultShape, resultRevision, edges, normalizedParameters, faceKind);

    std::vector<std::string> deletedEdges;
    for (const ResolvedEdgeRef& edge : edges) {
        deletedEdges.push_back(edge.stableHash);
    }

    const std::string revisionJson = kernel->getRevisionInfo(shapeId);
    const std::string topologyJson = kernel->getTopology(shapeId);

    std::ostringstream result;
    result << '{';
    result << "\"shapeId\":" << shapeId << ',';
    result << "\"revision\":" << revisionJson << ',';
    result << "\"topology\":" << topologyJson << ',';
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

std::string freshStableHash(const std::string& revisionId,
                            const char* kind,
                            const char* prefix,
                            int index) {
    return std::string(prefix) + fnv1a64("stable|" + revisionId + "|" + kind + "|" + std::to_string(index));
}

void assignFreshStableHashes(RevisionMetadata& revision, const TopoDS_Shape& shape) {
    ShapeMap faces, edges, vertices;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);

    revision.faceStableHashes.clear();
    revision.edgeStableHashes.clear();
    revision.vertexStableHashes.clear();
    revision.faceStableHashes.reserve(static_cast<std::size_t>(faces.Extent()));
    revision.edgeStableHashes.reserve(static_cast<std::size_t>(edges.Extent()));
    revision.vertexStableHashes.reserve(static_cast<std::size_t>(vertices.Extent()));

    for (int index = 1; index <= faces.Extent(); ++index) {
        revision.faceStableHashes.push_back(freshStableHash(revision.revisionId, "face", "F:", index));
    }
    for (int index = 1; index <= edges.Extent(); ++index) {
        revision.edgeStableHashes.push_back(freshStableHash(revision.revisionId, "edge", "E:", index));
    }
    for (int index = 1; index <= vertices.Extent(); ++index) {
        revision.vertexStableHashes.push_back(freshStableHash(revision.revisionId, "vertex", "V:", index));
    }
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
    assignFreshStableHashes(revision, shape);
    return revision;
}

struct StableEntityRecord {
    std::string kind;
    int id = 0;
    std::string stableHash;
};

struct StableSourceSubshape {
    TopoDS_Shape shape;
    std::string stableHash;
};

std::string derivedStableHash(const RevisionMetadata& revision,
                              const char* kind,
                              const char* prefix,
                              const char* relation,
                              int index,
                              const std::vector<std::string>& sourceHashes) {
    std::ostringstream signature;
    signature << "derived|" << revision.revisionId << '|'
              << revision.operationId << '|'
              << kind << '|'
              << relation << '|'
              << index << '|';
    for (const std::string& hash : sourceHashes) {
        signature << hash << '|';
    }
    return std::string(prefix) + fnv1a64(signature.str());
}

std::vector<StableSourceSubshape> collectSourceSubshapes(const ShapeRecord& record,
                                                         TopAbs_ShapeEnum kind) {
    std::vector<StableSourceSubshape> result;
    ShapeMap map;
    TopExp::MapShapes(record.shape, kind, map);
    result.reserve(static_cast<std::size_t>(map.Extent()));

    const std::vector<std::string>* hashes = nullptr;
    const char* fallbackPrefix = "";
    const char* fallbackKind = "";
    switch (kind) {
    case TopAbs_FACE:
        hashes = &record.revision.faceStableHashes;
        fallbackPrefix = "F:";
        fallbackKind = "face";
        break;
    case TopAbs_EDGE:
        hashes = &record.revision.edgeStableHashes;
        fallbackPrefix = "E:";
        fallbackKind = "edge";
        break;
    case TopAbs_VERTEX:
        hashes = &record.revision.vertexStableHashes;
        fallbackPrefix = "V:";
        fallbackKind = "vertex";
        break;
    default:
        return result;
    }

    for (int index = 1; index <= map.Extent(); ++index) {
        const std::string stableHash = hashes != nullptr && hashes->size() == static_cast<std::size_t>(map.Extent())
            ? (*hashes)[static_cast<std::size_t>(index - 1)]
            : freshStableHash(record.revision.revisionId, fallbackKind, fallbackPrefix, index);
        result.push_back({ map(index), stableHash });
    }

    return result;
}

template <typename ShapeListT>
void appendHistoryResultIndices(const ShapeListT& list,
                                const ShapeMap& resultMap,
                                TopAbs_ShapeEnum kind,
                                std::vector<int>& indices) {
    for (typename ShapeListT::Iterator it(list); it.More(); it.Next()) {
        const TopoDS_Shape& candidate = it.Value();
        if (candidate.ShapeType() != kind) {
            continue;
        }
        const int mapped = resultMap.FindIndex(candidate);
        if (mapped > 0 && std::find(indices.begin(), indices.end(), mapped) == indices.end()) {
            indices.push_back(mapped);
        }
    }
}

template <typename Builder>
std::vector<int> historyResultIndices(Builder& builder,
                                      const TopoDS_Shape& sourceShape,
                                      const ShapeMap& resultMap,
                                      TopAbs_ShapeEnum kind) {
    std::vector<int> indices;
    appendHistoryResultIndices(builder.Modified(sourceShape), resultMap, kind, indices);
    appendHistoryResultIndices(builder.Generated(sourceShape), resultMap, kind, indices);
    const int retained = resultMap.FindIndex(sourceShape);
    if (retained > 0 && std::find(indices.begin(), indices.end(), retained) == indices.end()) {
        indices.push_back(retained);
    }
    std::sort(indices.begin(), indices.end());
    return indices;
}

template <typename Builder>
std::vector<std::string> assignHistoryStableHashesForKind(const TopoDS_Shape& resultShape,
                                                          const RevisionMetadata& revision,
                                                          const std::vector<const ShapeRecord*>& sourceRecords,
                                                          Builder& builder,
                                                          TopAbs_ShapeEnum kind,
                                                          const char* kindLabel,
                                                          const char* prefix,
                                                          std::vector<DeletedEntityRecord>& deletedEntities) {
    ShapeMap resultMap;
    TopExp::MapShapes(resultShape, kind, resultMap);
    std::vector<std::string> assignments(static_cast<std::size_t>(resultMap.Extent()) + 1);
    if (resultMap.Extent() == 0) {
        return assignments;
    }

    std::vector<std::vector<std::string>> sourceHashesByResult(static_cast<std::size_t>(resultMap.Extent()) + 1);

    for (const ShapeRecord* sourceRecord : sourceRecords) {
        for (const StableSourceSubshape& source : collectSourceSubshapes(*sourceRecord, kind)) {
            const std::vector<int> descendants = historyResultIndices(builder, source.shape, resultMap, kind);
            if (descendants.empty()) {
                deletedEntities.push_back({ kindLabel, source.stableHash, revision.operationId, "deleted" });
                continue;
            }
            for (int descendant : descendants) {
                std::vector<std::string>& sourceHashes = sourceHashesByResult[static_cast<std::size_t>(descendant)];
                if (std::find(sourceHashes.begin(), sourceHashes.end(), source.stableHash) == sourceHashes.end()) {
                    sourceHashes.push_back(source.stableHash);
                }
            }
        }
    }

    std::unordered_map<std::string, std::vector<int>> sourceToResults;
    for (int index = 1; index <= resultMap.Extent(); ++index) {
        std::vector<std::string>& sourceHashes = sourceHashesByResult[static_cast<std::size_t>(index)];
        std::sort(sourceHashes.begin(), sourceHashes.end());
        for (const std::string& stableHash : sourceHashes) {
            sourceToResults[stableHash].push_back(index);
        }
    }

    std::unordered_map<std::string, int> primaryResultBySource;
    for (const auto& entry : sourceToResults) {
        const std::vector<int>& candidates = entry.second;
        const auto singleSourceIt = std::find_if(candidates.begin(), candidates.end(), [&](int candidate) {
            return sourceHashesByResult[static_cast<std::size_t>(candidate)].size() == 1;
        });
        primaryResultBySource[entry.first] = singleSourceIt != candidates.end() ? *singleSourceIt : candidates.front();
    }

    std::set<std::string> usedHashes;
    for (int index = 1; index <= resultMap.Extent(); ++index) {
        const std::vector<std::string>& sourceHashes = sourceHashesByResult[static_cast<std::size_t>(index)];
        if (sourceHashes.empty()) {
            assignments[static_cast<std::size_t>(index)] = freshStableHash(revision.revisionId, kindLabel, prefix, index);
            continue;
        }

        if (sourceHashes.size() == 1) {
            const std::string& inherited = sourceHashes.front();
            auto primaryIt = primaryResultBySource.find(inherited);
            const bool canReuse = primaryIt != primaryResultBySource.end()
                && primaryIt->second == index
                && usedHashes.insert(inherited).second;
            assignments[static_cast<std::size_t>(index)] = canReuse
                ? inherited
                : derivedStableHash(revision, kindLabel, prefix, "split", index, sourceHashes);
            continue;
        }

        assignments[static_cast<std::size_t>(index)] = derivedStableHash(revision, kindLabel, prefix, "merge", index, sourceHashes);
    }

    return assignments;
}

template <typename Builder>
void applyHistoryStableHashes(const TopoDS_Shape& resultShape,
                              RevisionMetadata& revision,
                              const std::vector<const ShapeRecord*>& sourceRecords,
                              Builder& builder) {
    revision.faceStableHashes = assignHistoryStableHashesForKind(resultShape,
                                                                 revision,
                                                                 sourceRecords,
                                                                 builder,
                                                                 TopAbs_FACE,
                                                                 "face",
                                                                 "F:",
                                                                 revision.deletedEntities);
    if (!revision.faceStableHashes.empty()) {
        revision.faceStableHashes.erase(revision.faceStableHashes.begin());
    }

    revision.edgeStableHashes = assignHistoryStableHashesForKind(resultShape,
                                                                 revision,
                                                                 sourceRecords,
                                                                 builder,
                                                                 TopAbs_EDGE,
                                                                 "edge",
                                                                 "E:",
                                                                 revision.deletedEntities);
    if (!revision.edgeStableHashes.empty()) {
        revision.edgeStableHashes.erase(revision.edgeStableHashes.begin());
    }

    revision.vertexStableHashes = assignHistoryStableHashesForKind(resultShape,
                                                                   revision,
                                                                   sourceRecords,
                                                                   builder,
                                                                   TopAbs_VERTEX,
                                                                   "vertex",
                                                                   "V:",
                                                                   revision.deletedEntities);
    if (!revision.vertexStableHashes.empty()) {
        revision.vertexStableHashes.erase(revision.vertexStableHashes.begin());
    }

    revision.identityStatus = "resolved";
}

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

std::string OcctKernel::getLastError() const {
    return g_lastKernelErrorJson;
}

void OcctKernel::clearLastError() {
    g_lastKernelErrorJson.clear();
}

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

uint32_t OcctKernel::performStructuredSweepFeature(uint32_t id,
                                                   const std::string& profileJson,
                                                   const std::string& specJson,
                                                   const std::string& operationType) {
    auto sourceIt = _impl->records.find(id);
    if (sourceIt == _impl->records.end()) {
        throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
    }

    const StructuredSweepSpec spec = parseStructuredSweepSpec(specJson, operationType, "sweep");
    const TopoDS_Shape& baseShape = sourceIt->second.shape;
    const RevisionMetadata* baseRevision = &sourceIt->second.revision;

    requireSingleWireProfile(profileJson, operationType, "profile");
    const TopoDS_Wire profileWire = placeProfileWire(buildWireFromProfile(profileJson), spec.plane);
    const TopoDS_Wire spine = buildSpatialWireFromJson(spec.spineJson, false);

    BRepOffsetAPI_MakePipeShell pipe(spine);
    switch (spec.trihedronMode) {
    case StructuredSweepTrihedronMode::CorrectedFrenet:
        pipe.SetMode(false);
        break;
    case StructuredSweepTrihedronMode::Frenet:
        pipe.SetMode(true);
        break;
    case StructuredSweepTrihedronMode::Discrete:
        pipe.SetDiscreteMode();
        break;
    case StructuredSweepTrihedronMode::FixedTrihedron:
        pipe.SetMode(gp_Ax2(spec.trihedronFrame.origin, spec.trihedronFrame.normal, spec.trihedronFrame.xDirection));
        break;
    case StructuredSweepTrihedronMode::FixedBinormal:
        pipe.SetMode(spec.binormal);
        break;
    case StructuredSweepTrihedronMode::AuxiliarySpine: {
        const TopoDS_Wire auxiliarySpine = buildSpatialWireFromJson(spec.auxiliarySpineJson, false);
        pipe.SetMode(auxiliarySpine, spec.curvilinearEquivalence, spec.auxiliaryContact);
        break;
    }
    }

    pipe.Add(profileWire, spec.sectionWithContact, spec.sectionWithCorrection);
    if (spec.hasTolerance) {
        pipe.SetTolerance(spec.tol3d, spec.boundTol, spec.angularTol);
    }
    if (spec.hasMaxDegree) {
        pipe.SetMaxDegree(spec.maxDegree);
    }
    if (spec.hasMaxSegments) {
        pipe.SetMaxSegments(spec.maxSegments);
    }
    pipe.SetForceApproxC1(spec.forceApproxC1);
    pipe.SetTransitionMode(spec.transitionMode);
    pipe.Build();
    if (!pipe.IsDone() || pipe.Shape().IsNull()) {
        throwStructuredOperationError(operationType, "Sweep build failed");
    }
    if (spec.solid && !pipe.MakeSolid()) {
        throwStructuredOperationError(operationType, "Sweep profile could not be converted into a solid");
    }

    const TopoDS_Shape resultShape = composeFeatureWithBase(baseShape, pipe.Shape(), spec.cut, operationType);
    return storeShapeWithMetadata(resultShape,
                                  operationType,
                                  "source=" + baseRevision->revisionId + ";" + profileJson + "|" + specJson,
                                  { baseRevision->revisionId },
                                  "unresolved",
                                  "unresolved",
                                  { "Structured sweep lineage is composed from an exact builder plus boolean feature composition; generated/modified/deleted identity is reported as unresolved" });
}

uint32_t OcctKernel::sweepProfileWithSpec(uint32_t id, const std::string& profileJson, const std::string& specJson) {
    try {
        const mini_json::Value specRoot = mini_json::requireObject(mini_json::parse(specJson), "sweep spec");
        const std::string operationType = optionalBoolMember(specRoot, "cut", false) ? "sweepCutProfile" : "sweepProfile";
        return performStructuredSweepFeature(id, profileJson, specJson, operationType);
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

uint32_t OcctKernel::performStructuredLoftFeature(uint32_t id,
                                                  const std::string& sectionsJson,
                                                  const std::string& specJson,
                                                  const std::string& operationType) {
    auto sourceIt = _impl->records.find(id);
    if (sourceIt == _impl->records.end()) {
        throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
    }

    bool allowUnknownFields = false;
    const StructuredLoftSpec spec = parseStructuredLoftSpec(specJson, operationType, "loft", allowUnknownFields);
    const std::vector<StructuredLoftSection> sections = parseStructuredLoftSections(sectionsJson,
                                                                                    allowUnknownFields,
                                                                                    operationType,
                                                                                    "loft");
    const TopoDS_Shape& baseShape = sourceIt->second.shape;
    const RevisionMetadata* baseRevision = &sourceIt->second.revision;

    BRepOffsetAPI_ThruSections loft(spec.solid, spec.ruled, spec.pres3d);
    if (spec.hasCheckCompatibility) {
        loft.CheckCompatibility(spec.checkCompatibility);
    }
    if (spec.hasSmoothing) {
        loft.SetSmoothing(spec.smoothing);
    }
    if (spec.hasParametrization) {
        loft.SetParType(spec.parametrization);
    }
    if (spec.hasContinuity) {
        loft.SetContinuity(spec.continuity);
    }
    if (spec.hasCriteriumWeight) {
        loft.SetCriteriumWeight(spec.criteriumWeight1, spec.criteriumWeight2, spec.criteriumWeight3);
    }
    if (spec.hasMaxDegree) {
        loft.SetMaxDegree(spec.maxDegree);
    }
    if (spec.hasMutableInput) {
        loft.SetMutableInput(spec.mutableInput);
    }

    for (std::size_t index = 0; index < sections.size(); ++index) {
        const StructuredLoftSection& section = sections[index];
        switch (section.kind) {
        case StructuredLoftSectionKind::Profile: {
            requireSingleWireProfile(section.profileJson,
                                     operationType,
                                     "loft.sections[" + std::to_string(index) + "]");
            const TopoDS_Wire wire = placeProfileWire(buildWireFromProfile(section.profileJson), section.plane);
            loft.AddWire(wire);
            break;
        }
        case StructuredLoftSectionKind::Wire:
            loft.AddWire(buildSpatialWireFromJson(section.wireJson, true));
            break;
        case StructuredLoftSectionKind::Point:
            loft.AddVertex(BRepBuilderAPI_MakeVertex(section.point).Vertex());
            break;
        }
    }

    loft.Build();
    if (!loft.IsDone() || loft.Shape().IsNull()) {
        throwStructuredOperationError(operationType, "Loft build failed");
    }

    const TopoDS_Shape resultShape = composeFeatureWithBase(baseShape, loft.Shape(), spec.cut, operationType);
    return storeShapeWithMetadata(resultShape,
                                  operationType,
                                  "source=" + baseRevision->revisionId + ";" + sectionsJson + "|" + specJson,
                                  { baseRevision->revisionId },
                                  "unresolved",
                                  "unresolved",
                                  { "Structured loft lineage is composed from an exact builder plus boolean feature composition; generated/modified/deleted identity is reported as unresolved" });
}

uint32_t OcctKernel::loftWithSpec(uint32_t id, const std::string& sectionsJson, const std::string& specJson) {
    try {
        const mini_json::Value specRoot = mini_json::requireObject(mini_json::parse(specJson), "loft spec");
        const std::string operationType = optionalBoolMember(specRoot, "cut", false) ? "loftCut" : "loft";
        return performStructuredLoftFeature(id, sectionsJson, specJson, operationType);
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
        auto sourceIt1 = _impl->records.find(id1);
        auto sourceIt2 = _impl->records.find(id2);
        if (sourceIt1 == _impl->records.end() || sourceIt2 == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "BooleanUnion requires valid resident handles");
        }
        const TopoDS_Shape& s1 = requireShape(id1);
        const TopoDS_Shape& s2 = requireShape(id2);
        BRepAlgoAPI_Fuse op(s1, s2);
        runBoolean(this, id1, id2, "BooleanUnion", op);
        const uint32_t resultId = storeShapeWithMetadata(op.Shape(),
                                                         "booleanUnion",
                                                         "base=" + requireRevisionId(id1) + ";tool=" + requireRevisionId(id2),
                                                         { requireRevisionId(id1), requireRevisionId(id2) },
                                                         "unresolved",
                                                         "resolved",
                                                         { "Stable semantic subshape ids are propagated through exact OCCT boolean history; split and merged descendants receive derived ids." });
        applyHistoryStableHashes(_impl->records[resultId].shape,
                                 _impl->records[resultId].revision,
                                 { &sourceIt1->second, &sourceIt2->second },
                                 op);
        return resultId;
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

uint32_t OcctKernel::booleanSubtract(uint32_t id1, uint32_t id2) {
    try {
        auto sourceIt1 = _impl->records.find(id1);
        auto sourceIt2 = _impl->records.find(id2);
        if (sourceIt1 == _impl->records.end() || sourceIt2 == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "BooleanSubtract requires valid resident handles");
        }
        const TopoDS_Shape& s1 = requireShape(id1);
        const TopoDS_Shape& s2 = requireShape(id2);
        BRepAlgoAPI_Cut op(s1, s2);
        runBoolean(this, id1, id2, "BooleanSubtract", op);
        const uint32_t resultId = storeShapeWithMetadata(op.Shape(),
                                                         "booleanSubtract",
                                                         "base=" + requireRevisionId(id1) + ";tool=" + requireRevisionId(id2),
                                                         { requireRevisionId(id1), requireRevisionId(id2) },
                                                         "unresolved",
                                                         "resolved",
                                                         { "Stable semantic subshape ids are propagated through exact OCCT boolean history; split and merged descendants receive derived ids." });
        applyHistoryStableHashes(_impl->records[resultId].shape,
                                 _impl->records[resultId].revision,
                                 { &sourceIt1->second, &sourceIt2->second },
                                 op);
        return resultId;
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

uint32_t OcctKernel::booleanIntersect(uint32_t id1, uint32_t id2) {
    try {
        auto sourceIt1 = _impl->records.find(id1);
        auto sourceIt2 = _impl->records.find(id2);
        if (sourceIt1 == _impl->records.end() || sourceIt2 == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "BooleanIntersect requires valid resident handles");
        }
        const TopoDS_Shape& s1 = requireShape(id1);
        const TopoDS_Shape& s2 = requireShape(id2);
        BRepAlgoAPI_Common op(s1, s2);
        runBoolean(this, id1, id2, "BooleanIntersect", op);
        const uint32_t resultId = storeShapeWithMetadata(op.Shape(),
                                                         "booleanIntersect",
                                                         "base=" + requireRevisionId(id1) + ";tool=" + requireRevisionId(id2),
                                                         { requireRevisionId(id1), requireRevisionId(id2) },
                                                         "unresolved",
                                                         "resolved",
                                                         { "Stable semantic subshape ids are propagated through exact OCCT boolean history; split and merged descendants receive derived ids." });
        applyHistoryStableHashes(_impl->records[resultId].shape,
                                 _impl->records[resultId].revision,
                                 { &sourceIt1->second, &sourceIt2->second },
                                 op);
        return resultId;
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
        auto sourceIt = _impl->records.find(id);
        if (sourceIt == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }
        const TopoDS_Shape& shape = requireShape(id);
        BRepFilletAPI_MakeFillet mkFillet(shape);
        for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next()) {
            mkFillet.Add(radius, TopoDS::Edge(ex.Current()));
        }
        mkFillet.Build();
        if (!mkFillet.IsDone()) {
            throwKernelError("OPERATION_FAILED", "BRepFilletAPI_MakeFillet failed");
        }
        const uint32_t resultId = storeShapeWithMetadata(mkFillet.Shape(),
                                                         "filletEdges",
                                                         "source=" + requireRevisionId(id) + ";radius=" + std::to_string(radius),
                                                         { requireRevisionId(id) },
                                                         "unresolved",
                                                         "resolved",
                                                         { "Stable semantic subshape ids are propagated through exact OCCT fillet history; split descendants receive derived ids." });
        applyHistoryStableHashes(_impl->records[resultId].shape,
                                 _impl->records[resultId].revision,
                                 { &sourceIt->second },
                                 mkFillet);
        return resultId;
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
        auto sourceIt = _impl->records.find(id);
        if (sourceIt == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }
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
        const uint32_t resultId = storeShapeWithMetadata(mkChamfer.Shape(),
                                                         "chamferEdges",
                                                         "source=" + requireRevisionId(id) + ";distance=" + std::to_string(distance),
                                                         { requireRevisionId(id) },
                                                         "unresolved",
                                                         "resolved",
                                                         { "Stable semantic subshape ids are propagated through exact OCCT chamfer history; split descendants receive derived ids." });
        applyHistoryStableHashes(_impl->records[resultId].shape,
                                 _impl->records[resultId].revision,
                                 { &sourceIt->second },
                                 mkChamfer);
        return resultId;
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return 0;
}

namespace {

// ---------------------------------------------------------------------------
// Canonical B-spline retry basis for blend operations
//
// BRepFilletAPI_MakeFillet / MakeChamfer (the ChFi3d core) are unreliable when
// a blend support face is a swept surface (Geom_SurfaceOfLinearExtrusion,
// Geom_SurfaceOfRevolution, Geom_OffsetSurface), in particular for concave
// b-spline directrices produced by sketch-profile prisms. The standard OCCT
// remedy is to convert exactly those surfaces to their exact B-spline basis
// (ShapeCustom_ConvertToBSpline) and run the blend on the converted shape.
// The conversion is geometrically exact, so analytic faces (planes, cylinders)
// stay untouched and the model does not lose precision.
// ---------------------------------------------------------------------------

struct CanonicalBlendBasis {
    bool available = false;
    TopoDS_Shape shape;
    NCollection_DataMap<TopoDS_Shape, TopoDS_Shape, TopTools_ShapeMapHasher> context;
    BRepTools_Modifier modifier;
};

// Builds the canonical (swept-surfaces-to-bspline) form of `shape`. Returns an
// unavailable basis when nothing needed conversion or conversion failed, in
// which case the caller must surface the original blend failure unchanged.
std::unique_ptr<CanonicalBlendBasis> makeCanonicalBlendBasis(const TopoDS_Shape& shape) {
    auto basis = std::make_unique<CanonicalBlendBasis>();
    try {
        Handle(ShapeCustom_ConvertToBSpline) conversion = new ShapeCustom_ConvertToBSpline;
        conversion->SetExtrusionMode(true);
        conversion->SetRevolutionMode(true);
        conversion->SetOffsetMode(true);
        conversion->SetPlaneMode(false);

        const TopoDS_Shape converted = ShapeCustom::ApplyModifier(shape, conversion, basis->context, basis->modifier);
        if (converted.IsNull() || converted.IsSame(shape) || !basis->modifier.IsDone()) {
            return basis;
        }
        basis->shape = converted;
        basis->available = true;
    } catch (const Standard_Failure&) {
        basis->available = false;
    }
    return basis;
}

// Maps a subshape of the original blend operand onto its counterpart in the
// canonical basis. Subshapes untouched by the conversion map to themselves.
bool tryMapToCanonicalBasis(const CanonicalBlendBasis& basis,
                           const TopoDS_Shape& shape,
                           TopoDS_Shape& mappedShape) {
    const auto mapWithOrientation = [&](const TopoDS_Shape& candidate) -> TopoDS_Shape {
        if (basis.context.IsBound(candidate)) {
            const TopoDS_Shape& mapped = basis.context.Find(candidate);
            if (!mapped.IsNull()) {
                return mapped.Oriented(shape.Orientation());
            }
        }
        try {
            const TopoDS_Shape& mapped = basis.modifier.ModifiedShape(candidate);
            if (!mapped.IsNull()) {
                return mapped.Oriented(shape.Orientation());
            }
        } catch (const Standard_Failure&) {
            // Shape was not part of the initial shape (e.g. builder-generated);
            // treat it as unmapped.
        }
        return TopoDS_Shape();
    };

    const TopoDS_Shape exact = mapWithOrientation(shape);
    if (!exact.IsNull()) {
        mappedShape = exact;
        return true;
    }

    const TopoDS_Shape forward = shape.Oriented(TopAbs_FORWARD);
    if (!forward.IsSame(shape)) {
        const TopoDS_Shape mappedForward = mapWithOrientation(forward);
        if (!mappedForward.IsNull()) {
            mappedShape = mappedForward;
            return true;
        }
    }

    mappedShape = shape;
    return false;
}

TopoDS_Shape mapToCanonicalBasis(const CanonicalBlendBasis& basis, const TopoDS_Shape& shape) {
    TopoDS_Shape mapped;
    if (tryMapToCanonicalBasis(basis, shape, mapped)) {
        return mapped;
    }
    return shape;
}

// History adapter that composes the canonical-basis conversion with the blend
// builder history, so stable-hash lineage keeps resolving against the
// ORIGINAL operand subshapes even though the blend ran on the converted shape.
template <typename Builder>
struct CanonicalBlendHistory {
    const CanonicalBlendBasis& basis;
    Builder& builder;

    ShapeList Modified(const TopoDS_Shape& shape) {
        TopoDS_Shape mapped;
        if (!tryMapToCanonicalBasis(basis, shape, mapped)) {
            return ShapeList();
        }
        ShapeList list = builder.Modified(mapped);
        if (!mapped.IsSame(shape)) {
            // The conversion itself modified this subshape; if the blend then
            // retained it, lineage must still reach the converted counterpart.
            list.Append(mapped);
        }
        return list;
    }

    ShapeList Generated(const TopoDS_Shape& shape) {
        TopoDS_Shape mapped;
        if (!tryMapToCanonicalBasis(basis, shape, mapped)) {
            return ShapeList();
        }
        return builder.Generated(mapped);
    }
};

// Re-resolved edge refs against the canonical basis: same identity (topoId and
// stableHash are reported from the original resolution), converted TopoDS edge.
std::vector<ResolvedEdgeRef> mapEdgeRefsToCanonicalBasis(const CanonicalBlendBasis& basis,
                                                         const std::vector<ResolvedEdgeRef>& edges) {
    ShapeMap basisEdges;
    TopExp::MapShapes(basis.shape, TopAbs_EDGE, basisEdges);
    std::vector<ResolvedEdgeRef> mapped;
    mapped.reserve(edges.size());
    for (const ResolvedEdgeRef& edge : edges) {
        ResolvedEdgeRef copy = edge;
        if (edge.topoId > 0 && edge.topoId <= basisEdges.Extent()) {
            copy.edge = TopoDS::Edge(basisEdges(edge.topoId));
        } else {
            copy.edge = TopoDS::Edge(mapToCanonicalBasis(basis, edge.edge));
        }
        mapped.push_back(copy);
    }
    return mapped;
}

TopoDS_Face mapFaceRefToCanonicalBasis(const CanonicalBlendBasis& basis, const ResolvedFaceRef& face) {
    ShapeMap basisFaces;
    TopExp::MapShapes(basis.shape, TopAbs_FACE, basisFaces);
    if (face.topoId > 0 && face.topoId <= basisFaces.Extent()) {
        return TopoDS::Face(basisFaces(face.topoId));
    }
    return TopoDS::Face(mapToCanonicalBasis(basis, face.face));
}

} // namespace

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

        const ChFi3d_FilletShape filletShape = parseFilletShape(optionalStringMember(root, "blendShape", "rational"), operation, "fillet.blendShape");
        BRepFilletAPI_MakeFillet mkFillet(shape, filletShape);
        const std::string continuity = optionalStringMember(root, "continuity", "C1");
        const double angularTolerance = optionalNumberMember(root, "angularTolerance", 1.0e-2);
        if (!std::isfinite(angularTolerance) || angularTolerance <= 0.0) {
            throwStructuredValidation(operation, "fillet.angularTolerance", "angularTolerance must be finite and > 0");
        }
        const GeomAbs_Shape blendContinuity = parseContinuity(continuity, operation, "fillet.continuity");
        mkFillet.SetContinuity(blendContinuity, angularTolerance);

        std::vector<ResolvedEdgeRef> appliedEdges;
        std::vector<std::string> normalizedParameters;

        // Replayable per-edge radius programme: the same parameters must be
        // re-applied verbatim when the blend is retried on the canonical
        // B-spline basis of the operand.
        struct FilletReplayOp {
            int kind = 0; // 0: constant radius, 1: start/end radii, 2: radius table
            double radius1 = 0.0;
            double radius2 = 0.0;
            std::shared_ptr<Point2dArray> table;
        };
        std::vector<FilletReplayOp> replayOps;
        const auto applyFilletOp = [](BRepFilletAPI_MakeFillet& mk, const FilletReplayOp& op, const TopoDS_Edge& edge) {
            if (op.kind == 0) {
                mk.Add(op.radius1, edge);
            } else if (op.kind == 1) {
                mk.Add(op.radius1, op.radius2, edge);
            } else {
                mk.Add(*op.table, edge);
            }
        };

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
                FilletReplayOp op;
                op.kind = 0;
                op.radius1 = radius;
                applyFilletOp(mkFillet, op, edge.edge);
                replayOps.push_back(op);
                normalized << "\"radius\":" << radius;
            } else if (mode == "variable" || mode == "startEnd") {
                if (stationValue != nullptr) {
                    const mini_json::Value& stations = mini_json::requireArray(*stationValue, path + ".stations");
                    if (stations.array.size() < 2) {
                        throwStructuredValidation(operation, path + ".stations", "At least two radius stations are required");
                    }
                    auto table = std::make_shared<Point2dArray>(1, static_cast<int>(stations.array.size()));
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
                        table->SetValue(static_cast<int>(i + 1), gp_Pnt2d(t, radius));
                        if (i > 0) normalized << ',';
                        normalized << "{\"t\":" << t << ",\"radius\":" << radius << "}";
                    }
                    normalized << "]";
                    FilletReplayOp op;
                    op.kind = 2;
                    op.table = table;
                    applyFilletOp(mkFillet, op, edge.edge);
                    replayOps.push_back(op);
                } else {
                    const double startRadius = hasMember(entry, "startRadius")
                        ? requirePositiveMember(entry, "startRadius", operation, path + ".startRadius")
                        : requirePositiveMember(root, "startRadius", operation, "fillet.startRadius");
                    const double endRadius = hasMember(entry, "endRadius")
                        ? requirePositiveMember(entry, "endRadius", operation, path + ".endRadius")
                        : requirePositiveMember(root, "endRadius", operation, "fillet.endRadius");
                    FilletReplayOp op;
                    op.kind = 1;
                    op.radius1 = startRadius;
                    op.radius2 = endRadius;
                    applyFilletOp(mkFillet, op, edge.edge);
                    replayOps.push_back(op);
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
                    FilletReplayOp op;
                    op.kind = 0;
                    op.radius1 = radius;
                    applyFilletOp(mkFillet, op, edge.edge);
                    replayOps.push_back(op);
                    normalized << "\"law\":{\"type\":\"constant\",\"radius\":" << radius << "}";
                } else if (lawType == "linear") {
                    const double startRadius = requirePositiveMember(law, "startRadius", operation, path + ".law.startRadius");
                    const double endRadius = requirePositiveMember(law, "endRadius", operation, path + ".law.endRadius");
                    FilletReplayOp op;
                    op.kind = 1;
                    op.radius1 = startRadius;
                    op.radius2 = endRadius;
                    applyFilletOp(mkFillet, op, edge.edge);
                    replayOps.push_back(op);
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

        std::string primaryFailureReason;
        try {
            mkFillet.Build();
        } catch (const Standard_Failure& sf) {
            primaryFailureReason = sf.what() != nullptr && sf.what()[0] != '\0'
                ? sf.what()
                : "BRepFilletAPI_MakeFillet raised an exception";
        }
        if (primaryFailureReason.empty() && (!mkFillet.IsDone() || mkFillet.Shape().IsNull())) {
            primaryFailureReason = "BRepFilletAPI_MakeFillet failed";
        }

        const auto finalizeFillet = [&](auto& historyBuilder, const TopoDS_Shape& resultShape) -> std::string {
            const uint32_t resultId = storeShapeWithMetadata(resultShape,
                                                             "filletEdges",
                                                             "source=" + sourceRevision->revisionId + ";spec=" + fnv1a64(objectToStableSignature(root)),
                                                             { sourceRevision->revisionId },
                                                             "unresolved",
                                                             "resolved",
                                                             { "Stable semantic subshape ids are propagated through exact OCCT fillet history; explicit blend lineage remains available in the result payload." });
            auto resultIt = _impl->records.find(resultId);
            if (resultIt == _impl->records.end()) {
                throwKernelError("OPERATION_FAILED", "Stored fillet result is unavailable");
            }
            applyHistoryStableHashes(resultIt->second.shape,
                                     resultIt->second.revision,
                                     { &sourceIt->second },
                                     historyBuilder);
            for (const ResolvedEdgeRef& edge : appliedEdges) {
                const auto duplicate = std::find_if(resultIt->second.revision.deletedEntities.begin(),
                                                    resultIt->second.revision.deletedEntities.end(),
                                                    [&](const DeletedEntityRecord& record) {
                                                        return record.kind == "edge" && record.stableHash == edge.stableHash;
                                                    });
                if (duplicate == resultIt->second.revision.deletedEntities.end()) {
                    resultIt->second.revision.deletedEntities.push_back({ "edge", edge.stableHash, resultIt->second.revision.operationId, "deleted" });
                }
            }
            return makeBlendResultJson(this,
                                       resultId,
                                       resultIt->second.shape,
                                       resultIt->second.revision,
                                       historyBuilder,
                                       shape,
                                       appliedEdges,
                                       normalizedParameters,
                                       "filletFace");
        };

        if (primaryFailureReason.empty()) {
            return finalizeFillet(mkFillet, mkFillet.Shape());
        }

        // Native retry: ChFi3d is unreliable on swept support surfaces
        // (e.g. Geom_SurfaceOfLinearExtrusion walls of sketch-profile prisms
        // with concave b-spline directrices). Convert exactly those surfaces
        // to their exact B-spline basis and rerun the same blend programme on
        // the converted shape, composing both histories for stable identity.
        std::unique_ptr<CanonicalBlendBasis> basis = makeCanonicalBlendBasis(shape);
        if (!basis->available) {
            throwStructuredBlendOperationError(operation,
                                               primaryFailureReason,
                                               shape,
                                               sourceRevision,
                                               appliedEdges,
                                               {},
                                               blendBuilderDiagnosticsJson(mkFillet, appliedEdges));
        }

        const std::vector<ResolvedEdgeRef> basisEdges = mapEdgeRefsToCanonicalBasis(*basis, appliedEdges);
        BRepFilletAPI_MakeFillet mkRetry(basis->shape, filletShape);
        mkRetry.SetContinuity(blendContinuity, angularTolerance);
        for (std::size_t i = 0; i < replayOps.size(); ++i) {
            applyFilletOp(mkRetry, replayOps[i], basisEdges[i].edge);
        }

        std::string retryFailureReason;
        try {
            mkRetry.Build();
        } catch (const Standard_Failure& sf) {
            retryFailureReason = sf.what() != nullptr && sf.what()[0] != '\0'
                ? sf.what()
                : "BRepFilletAPI_MakeFillet raised an exception on the canonical B-spline basis";
        }
        if (retryFailureReason.empty() && (!mkRetry.IsDone() || mkRetry.Shape().IsNull())) {
            retryFailureReason = "BRepFilletAPI_MakeFillet failed on the canonical B-spline basis";
        }
        if (!retryFailureReason.empty()) {
            throwStructuredBlendOperationError(operation,
                                               primaryFailureReason + "; canonical B-spline retry: " + retryFailureReason,
                                               shape,
                                               sourceRevision,
                                               appliedEdges,
                                               {},
                                               blendBuilderDiagnosticsJson(mkRetry, basisEdges));
        }

        CanonicalBlendHistory<BRepFilletAPI_MakeFillet> composedHistory{ *basis, mkRetry };
        return finalizeFillet(composedHistory, mkRetry.Shape());
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception& ex) {
        throwStructuredOperationError(operation, ex.what());
    } catch (const Standard_Failure& sf) {
        throwStructuredOperationError(operation, sf.what());
    } catch (...) {
        throwStructuredOperationError(operation, "Unknown native exception");
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
        std::vector<ResolvedFaceRef> appliedReferenceFaces;
        std::vector<std::string> normalizedParameters;

        // Replayable per-edge chamfer programme for the canonical-basis retry.
        struct ChamferReplayOp {
            int kind = 0; // 0: symmetric, 1: symmetric w/ reference face, 2: twoDistance, 3: distanceAngle
            double value1 = 0.0; // distance / distance1
            double value2 = 0.0; // distance2 / angleRadians
            TopoDS_Face referenceFace; // original-shape face for kinds 1-3
        };
        std::vector<ChamferReplayOp> replayOps;

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
                const mini_json::Value* referenceFaceValue = entry.get("referenceFace");
                if (referenceFaceValue == nullptr) referenceFaceValue = root.get("referenceFace");
                if (referenceFaceValue != nullptr) {
                    ResolvedFaceRef referenceFace = resolveFaceRef(shape, sourceRevision, *referenceFaceValue, operation, path + ".referenceFace");
                    requireReferenceFaceAdjacentToEdge(shape, edge, referenceFace, operation, path + ".referenceFace");
                    const ResolvedFaceRef builderReferenceFace = selectSymmetricChamferBuilderFace(shape, sourceRevision, edge, referenceFace);
                    mkChamfer.Add(edge.edge);
                    const int contour = mkChamfer.Contour(edge.edge);
                    if (contour <= 0) {
                        throwStructuredBlendOperationError(operation,
                                                           "BRepFilletAPI_MakeChamfer did not create a contour for the selected edge",
                                                           shape,
                                                           sourceRevision,
                                                           { edge },
                                                           { builderReferenceFace },
                                                           blendBuilderDiagnosticsJson(mkChamfer, { edge }));
                    }
                    mkChamfer.SetDist(distance, contour, builderReferenceFace.face);
                    appliedReferenceFaces.push_back(builderReferenceFace);
                    ChamferReplayOp op;
                    op.kind = 1;
                    op.value1 = distance;
                    op.referenceFace = builderReferenceFace.face;
                    replayOps.push_back(op);
                    normalized << "\"distance\":" << distance << ",\"referenceFace\":";
                    appendEntityRefJson(normalized, referenceFace);
                    if (builderReferenceFace.topoId != referenceFace.topoId) {
                        normalized << ",\"builderReferenceFace\":";
                        appendEntityRefJson(normalized, builderReferenceFace);
                    }
                } else {
                    mkChamfer.Add(distance, edge.edge);
                    ChamferReplayOp op;
                    op.kind = 0;
                    op.value1 = distance;
                    replayOps.push_back(op);
                    normalized << "\"distance\":" << distance;
                }
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
                requireReferenceFaceAdjacentToEdge(shape, edge, referenceFace, operation, path + ".referenceFace");
                mkChamfer.Add(distance1, distance2, edge.edge, referenceFace.face);
                appliedReferenceFaces.push_back(referenceFace);
                ChamferReplayOp op;
                op.kind = 2;
                op.value1 = distance1;
                op.value2 = distance2;
                op.referenceFace = referenceFace.face;
                replayOps.push_back(op);
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
                requireReferenceFaceAdjacentToEdge(shape, edge, referenceFace, operation, path + ".referenceFace");
                mkChamfer.AddDA(distance, angle, edge.edge, referenceFace.face);
                appliedReferenceFaces.push_back(referenceFace);
                ChamferReplayOp op;
                op.kind = 3;
                op.value1 = distance;
                op.value2 = angle;
                op.referenceFace = referenceFace.face;
                replayOps.push_back(op);
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

        std::string primaryFailureReason;
        try {
            mkChamfer.Build();
        } catch (const Standard_Failure& sf) {
            primaryFailureReason = sf.what() != nullptr && sf.what()[0] != '\0'
                ? sf.what()
                : "BRepFilletAPI_MakeChamfer raised an exception";
        }
        if (primaryFailureReason.empty() && (!mkChamfer.IsDone() || mkChamfer.Shape().IsNull())) {
            primaryFailureReason = "BRepFilletAPI_MakeChamfer failed";
        }

        const auto finalizeChamfer = [&](auto& historyBuilder, const TopoDS_Shape& resultShape) -> std::string {
            const uint32_t resultId = storeShapeWithMetadata(resultShape,
                                                             "chamferEdges",
                                                             "source=" + sourceRevision->revisionId + ";spec=" + fnv1a64(objectToStableSignature(root)),
                                                             { sourceRevision->revisionId },
                                                             "unresolved",
                                                             "resolved",
                                                             { "Stable semantic subshape ids are propagated through exact OCCT chamfer history; explicit blend lineage remains available in the result payload." });
            auto resultIt = _impl->records.find(resultId);
            if (resultIt == _impl->records.end()) {
                throwKernelError("OPERATION_FAILED", "Stored chamfer result is unavailable");
            }
            applyHistoryStableHashes(resultIt->second.shape,
                                     resultIt->second.revision,
                                     { &sourceIt->second },
                                     historyBuilder);
            for (const ResolvedEdgeRef& edge : appliedEdges) {
                const auto duplicate = std::find_if(resultIt->second.revision.deletedEntities.begin(),
                                                    resultIt->second.revision.deletedEntities.end(),
                                                    [&](const DeletedEntityRecord& record) {
                                                        return record.kind == "edge" && record.stableHash == edge.stableHash;
                                                    });
                if (duplicate == resultIt->second.revision.deletedEntities.end()) {
                    resultIt->second.revision.deletedEntities.push_back({ "edge", edge.stableHash, resultIt->second.revision.operationId, "deleted" });
                }
            }
            return makeBlendResultJson(this,
                                       resultId,
                                       resultIt->second.shape,
                                       resultIt->second.revision,
                                       historyBuilder,
                                       shape,
                                       appliedEdges,
                                       normalizedParameters,
                                       "chamferFace");
        };

        if (primaryFailureReason.empty()) {
            return finalizeChamfer(mkChamfer, mkChamfer.Shape());
        }

        // Native retry on the canonical B-spline basis (see filletEdgesWithSpec).
        std::unique_ptr<CanonicalBlendBasis> basis = makeCanonicalBlendBasis(shape);
        if (!basis->available) {
            throwStructuredBlendOperationError(operation,
                                               primaryFailureReason,
                                               shape,
                                               sourceRevision,
                                               appliedEdges,
                                               appliedReferenceFaces,
                                               blendBuilderDiagnosticsJson(mkChamfer, appliedEdges));
        }

        const std::vector<ResolvedEdgeRef> basisEdges = mapEdgeRefsToCanonicalBasis(*basis, appliedEdges);
        BRepFilletAPI_MakeChamfer mkRetry(basis->shape);
        std::string retryFailureReason;
        for (std::size_t i = 0; i < replayOps.size() && retryFailureReason.empty(); ++i) {
            const ChamferReplayOp& op = replayOps[i];
            const TopoDS_Edge& edge = basisEdges[i].edge;
            TopoDS_Face mappedFace;
            if (op.kind != 0) {
                mappedFace = TopoDS::Face(mapToCanonicalBasis(*basis, op.referenceFace));
            }
            if (op.kind == 0) {
                mkRetry.Add(op.value1, edge);
            } else if (op.kind == 1) {
                mkRetry.Add(edge);
                const int contour = mkRetry.Contour(edge);
                if (contour <= 0) {
                    retryFailureReason = "BRepFilletAPI_MakeChamfer did not create a contour for the selected edge on the canonical B-spline basis";
                    break;
                }
                mkRetry.SetDist(op.value1, contour, mappedFace);
            } else if (op.kind == 2) {
                mkRetry.Add(op.value1, op.value2, edge, mappedFace);
            } else {
                mkRetry.AddDA(op.value1, op.value2, edge, mappedFace);
            }
        }

        if (retryFailureReason.empty()) {
            try {
                mkRetry.Build();
            } catch (const Standard_Failure& sf) {
                retryFailureReason = sf.what() != nullptr && sf.what()[0] != '\0'
                    ? sf.what()
                    : "BRepFilletAPI_MakeChamfer raised an exception on the canonical B-spline basis";
            }
            if (retryFailureReason.empty() && (!mkRetry.IsDone() || mkRetry.Shape().IsNull())) {
                retryFailureReason = "BRepFilletAPI_MakeChamfer failed on the canonical B-spline basis";
            }
        }
        if (!retryFailureReason.empty()) {
            throwStructuredBlendOperationError(operation,
                                               primaryFailureReason + "; canonical B-spline retry: " + retryFailureReason,
                                               shape,
                                               sourceRevision,
                                               appliedEdges,
                                               appliedReferenceFaces,
                                               blendBuilderDiagnosticsJson(mkRetry, basisEdges));
        }

        CanonicalBlendHistory<BRepFilletAPI_MakeChamfer> composedHistory{ *basis, mkRetry };
        return finalizeChamfer(composedHistory, mkRetry.Shape());
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception& ex) {
        throwStructuredOperationError(operation, ex.what());
    } catch (const Standard_Failure& sf) {
        throwStructuredOperationError(operation, sf.what());
    } catch (...) {
        throwStructuredOperationError(operation, "Unknown native exception");
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
        revision.faceStableHashes.clear();
        revision.edgeStableHashes.clear();
        revision.vertexStableHashes.clear();
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

        ShapeMap solids, shells, wires, faces, edges, vertices;
        TopExp::MapShapes(shape, TopAbs_SOLID,  solids);
        TopExp::MapShapes(shape, TopAbs_SHELL,  shells);
        TopExp::MapShapes(shape, TopAbs_WIRE,   wires);
        TopExp::MapShapes(shape, TopAbs_FACE,   faces);
        TopExp::MapShapes(shape, TopAbs_EDGE,   edges);
        TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);

        ShapeToShapeListMap edgeToFaces;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeToFaces);
        ShapeToShapeListMap shellToSolids;
        TopExp::MapShapesAndAncestors(shape, TopAbs_SHELL, TopAbs_SOLID, shellToSolids);
        ShapeToShapeListMap wireToFaces;
        TopExp::MapShapesAndAncestors(shape, TopAbs_WIRE, TopAbs_FACE, wireToFaces);

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

        const auto bounds = boundsOfShape(shape);

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
        ss << "\"shapeType\":"; appendJsonString(ss, shapeTypeName(shape.ShapeType())); ss << ",";
        ss << "\"identityStatus\":"; appendJsonString(ss, revision.identityStatus); ss << ",";
        ss << "\"historyWarnings\":"; appendStringArrayJson(ss, revision.historyWarnings); ss << ",";
        ss << "\"solidCount\":"  << solids.Extent()   << ",";
        ss << "\"shellCount\":"  << shells.Extent()   << ",";
        ss << "\"wireCount\":"   << wires.Extent()    << ",";
        ss << "\"faceCount\":"   << faces.Extent()    << ",";
        ss << "\"edgeCount\":"   << edges.Extent()    << ",";
        ss << "\"vertexCount\":" << vertices.Extent() << ",";
        ss << "\"boundingBox\":{";
        ss << "\"xMin\":"  << bounds[0] << ",";
        ss << "\"yMin\":"  << bounds[1] << ",";
        ss << "\"zMin\":"  << bounds[2] << ",";
        ss << "\"xMax\":"  << bounds[3] << ",";
        ss << "\"yMax\":"  << bounds[4] << ",";
        ss << "\"zMax\":"  << bounds[5];
        ss << "},";
        ss << "\"isValid\":" << (isValid ? "true" : "false") << ",";

        auto appendLineage = [&](const char* lineageKind) {
            if (revision.entityStatus == lineageKind) {
                appendStringArrayJson(ss, revision.operandRevisionIds);
            } else {
                ss << "[]";
            }
        };

        ss << "\"solids\":[";
        for (int i = 1; i <= solids.Extent(); ++i) {
            if (i > 1) ss << ",";
            ss << "{";
            ss << "\"id\":" << i << ",";
            ss << "\"shellIds\":";
            appendIntArrayJson(ss, childIdsForShape(solids(i), shells, TopAbs_SHELL));
            ss << ",\"status\":"; appendJsonString(ss, revision.entityStatus);
            ss << "}";
        }
        ss << "],";

        ss << "\"shells\":[";
        for (int i = 1; i <= shells.Extent(); ++i) {
            if (i > 1) ss << ",";
            ss << "{";
            ss << "\"id\":" << i << ",";
            ss << "\"solidIds\":";
            appendIntArrayJson(ss, ancestorIdsForShape(shells(i), solids, shellToSolids));
            ss << ",\"faceIds\":";
            appendIntArrayJson(ss, childIdsForShape(shells(i), faces, TopAbs_FACE));
            ss << ",\"status\":"; appendJsonString(ss, revision.entityStatus);
            ss << "}";
        }
        ss << "],";

        ss << "\"wires\":[";
        for (int i = 1; i <= wires.Extent(); ++i) {
            if (i > 1) ss << ",";
            ss << "{";
            ss << "\"id\":" << i << ",";
            ss << "\"edgeIds\":";
            appendIntArrayJson(ss, childIdsForShape(wires(i), edges, TopAbs_EDGE));
            ss << ",\"topoFaceIds\":";
            appendIntArrayJson(ss, ancestorIdsForShape(wires(i), faces, wireToFaces));
            ss << ",\"status\":"; appendJsonString(ss, revision.entityStatus);
            ss << "}";
        }
        ss << "],";

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

std::string OcctKernel::getPlanarFaceWires(uint32_t id, const std::string& faceRefJson) {
    const std::string operation = "getPlanarFaceWires";
    try {
        auto it = _impl->records.find(id);
        if (it == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }
        const mini_json::Value faceRef = mini_json::requireObject(mini_json::parse(faceRefJson), "faceRef");
        ResolvedFaceRef face = resolveFaceRef(it->second.shape, &it->second.revision, faceRef, operation, "faceRef");
        BRepAdaptor_Surface surface(face.face, false);
        if (surface.GetType() != GeomAbs_Plane) {
            throwStructuredValidation(operation, "faceRef", "Selected face is not planar");
        }

        const gp_Pln plane = surface.Plane();
        const double firstU = surface.FirstUParameter();
        const double lastU = surface.LastUParameter();
        const double firstV = surface.FirstVParameter();
        const double lastV = surface.LastVParameter();

        ShapeMap edges;
        TopExp::MapShapes(it->second.shape, TopAbs_EDGE, edges);
        const std::vector<std::string> edgeHashes = edgeStableHashesForShape(it->second.shape, &it->second.revision);

        std::vector<std::pair<TopoDS_Wire, bool>> wires;
        const TopoDS_Wire outerWire = BRepTools::OuterWire(face.face);
        if (!outerWire.IsNull()) {
            wires.emplace_back(outerWire, true);
        }
        for (TopExp_Explorer ex(face.face, TopAbs_WIRE); ex.More(); ex.Next()) {
            const TopoDS_Wire wire = TopoDS::Wire(ex.Current());
            if (!outerWire.IsNull() && wire.IsSame(outerWire)) continue;
            wires.emplace_back(wire, false);
        }

        std::ostringstream ss;
        ss << '{';
        ss << "\"face\":";
        appendEntityRefJson(ss, face);
        ss << ",\"surfaceType\":\"plane\",\"plane\":";
        appendPlaneFrameJson(ss, plane);
        ss << ",\"domain\":{\"u\":[" << firstU << ',' << lastU << "],\"v\":[" << firstV << ',' << lastV << "]},\"wires\":[";

        for (std::size_t wireIndex = 0; wireIndex < wires.size(); ++wireIndex) {
            if (wireIndex > 0) ss << ',';
            ss << '{';
            ss << "\"kind\":";
            appendJsonString(ss, wires[wireIndex].second ? "outer" : "hole");
            ss << ",\"segments\":[";

            bool firstSegment = true;
            for (BRepTools_WireExplorer wx(wires[wireIndex].first, face.face); wx.More(); wx.Next()) {
                const TopoDS_Edge wireEdge = TopoDS::Edge(wx.Current());
                if (wireEdge.IsNull()) continue;

                BRepAdaptor_Curve curve(wireEdge);
                const OrientedCurveRange orientedRange = orientCurveRangeForWire(wireEdge, curve);

                Standard_Real pcurveFirst = 0.0;
                Standard_Real pcurveLast = 0.0;
                Handle(Geom2d_Curve) pcurve = BRep_Tool::CurveOnSurface(wireEdge, face.face, pcurveFirst, pcurveLast);
                if (pcurve.IsNull()) {
                    throwKernelError("OPERATION_FAILED", "Planar face trim extraction requires face pcurves for each boundary edge");
                }

                const TopoDS_Edge canonicalEdge = TopoDS::Edge(wireEdge.Oriented(TopAbs_FORWARD));
                const int edgeTopoId = edges.FindIndex(canonicalEdge);
                const std::string edgeHash = edgeTopoId > 0 && static_cast<std::size_t>(edgeTopoId) < edgeHashes.size()
                    ? edgeHashes[static_cast<std::size_t>(edgeTopoId)]
                    : std::string();
                const ResolvedEdgeRef edgeRef = { canonicalEdge, edgeTopoId, edgeHash };
                const double planarTrimFirst = orientedRange.reversed ? pcurveLast : pcurveFirst;
                const double planarTrimLast = orientedRange.reversed ? pcurveFirst : pcurveLast;

                if (!firstSegment) ss << ',';
                ss << '{';
                ss << "\"edge\":";
                appendEntityRefJson(ss, edgeRef);
                ss << ",\"orientation\":";
                appendJsonString(ss, orientedRange.reversed ? "reversed" : "forward");
                ss << ",\"planarCurve\":";
                appendPlanarCurveDescriptorJson(ss, pcurve, pcurveFirst, pcurveLast, planarTrimFirst, planarTrimLast);
                ss << ",\"spatialCurve\":";
                appendSpatialCurveDescriptorJson(ss, wireEdge, curve.FirstParameter(), curve.LastParameter(), orientedRange.first, orientedRange.last);
                ss << '}';
                firstSegment = false;
            }

            ss << "]}";
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

std::string OcctKernel::getOperationSchema() const {
    return "{"
        "\"schemaVersion\":1,"
        "\"operations\":{"
        "\"extrudeProfile\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"direction\":true,\"draft\":true,\"plane\":true,\"reverseDirection\":true,\"endConditions\":[\"blind\",\"upToNext\",\"throughAll\",\"upToSurface\",\"offsetFromSurface\"],\"surfaceTarget\":true,\"curvedSurfaceTarget\":true}},"
        "\"extrudeCutProfile\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"direction\":true,\"draft\":true,\"plane\":true,\"reverseDirection\":true,\"endConditions\":[\"blind\",\"upToNext\",\"throughAll\",\"upToSurface\",\"offsetFromSurface\"],\"surfaceTarget\":true,\"curvedSurfaceTarget\":true}},"
        "\"revolveProfile\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"plane\":true,\"axis\":true,\"reverseDirection\":true,\"signedAngle\":true,\"endConditions\":[\"angle\",\"upToSurface\",\"fromSurfaceToSurface\",\"throughAll\",\"upToSurfaceAtAngle\"],\"surfaceTarget\":true,\"curvedSurfaceTarget\":true,\"slidingEdges\":true}},"
        "\"revolveCutProfile\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"plane\":true,\"axis\":true,\"reverseDirection\":true,\"signedAngle\":true,\"endConditions\":[\"angle\",\"upToSurface\",\"fromSurfaceToSurface\",\"throughAll\",\"upToSurfaceAtAngle\"],\"surfaceTarget\":true,\"curvedSurfaceTarget\":true,\"slidingEdges\":true}},"
        "\"sweepProfile\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"cutBoolean\":true,\"plane\":true,\"spine\":true,\"trihedronModes\":[\"correctedFrenet\",\"frenet\",\"discrete\",\"fixedTrihedron\",\"fixedBinormal\",\"auxiliarySpine\"],\"sectionWithContact\":true,\"sectionWithCorrection\":true,\"solid\":true,\"forceApproxC1\":true,\"transitionModes\":[\"transformed\",\"rightCorner\",\"roundCorner\"],\"tolerances\":true,\"maxDegree\":true,\"maxSegments\":true}},"
        "\"loft\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"cutBoolean\":true,\"sectionKinds\":[\"profile\",\"wire\",\"point\"],\"solid\":true,\"ruled\":true,\"pres3d\":true,\"checkCompatibility\":true,\"smoothing\":true,\"parametrization\":[\"chordLength\",\"centripetal\",\"isoParametric\"],\"continuity\":[\"C0\",\"G1\",\"C1\",\"G2\",\"C2\",\"C3\",\"CN\"],\"criteriumWeight\":true,\"maxDegree\":true,\"mutableInput\":true}},"
        "\"filletEdges\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"constantRadius\":true,\"startEndRadius\":true,\"stationRadii\":true,\"lawRadius\":[\"constant\",\"linear\"],\"tangentPropagation\":true,\"partialEdges\":false,\"setbackCorners\":false,\"blendShape\":[\"rational\",\"quasiAngular\",\"polynomial\"],\"continuity\":[\"C0\",\"C1\",\"C2\"],\"overflowModes\":[\"fail\"]}},"
        "\"chamferEdges\":{\"schemaVersion\":1,\"nativeExact\":true,\"requiresMeshFallback\":false,\"supports\":{\"symmetric\":true,\"twoDistance\":true,\"distanceAngle\":true,\"referenceFace\":true,\"tangentPropagation\":true,\"partialEdges\":false,\"setbackCorners\":false,\"overflowModes\":[\"fail\"]}},"
        "\"getVersionInfo\":{\"schemaVersion\":1,\"nativeExact\":true,\"sessionScoped\":true},"
        "\"analyzeShape\":{\"schemaVersion\":1,\"nativeExact\":true,\"pointContainment\":true},"
        "\"classifyPointContainment\":{\"schemaVersion\":1,\"nativeExact\":true,\"states\":[\"in\",\"out\",\"on\",\"unknown\"]},"
        "\"intersectShapes\":{\"schemaVersion\":1,\"nativeExact\":true,\"returnsSectionShape\":true},"
        "\"findClosestPointOnShape\":{\"schemaVersion\":1,\"nativeExact\":true,\"supportKinds\":[\"vertex\",\"edge\",\"face\"]},"
        "\"measureShapeDistance\":{\"schemaVersion\":1,\"nativeExact\":true,\"multipleSolutions\":true,\"supportKinds\":[\"vertex\",\"edge\",\"face\"]},"
        "\"evaluateEdge\":{\"schemaVersion\":1,\"nativeExact\":true,\"parameterModes\":[\"normalized\",\"native\"]},"
        "\"sampleEdge\":{\"schemaVersion\":1,\"nativeExact\":true,\"parameterModes\":[\"normalized\",\"native\"]},"
        "\"getEdgeCurve\":{\"schemaVersion\":1,\"nativeExact\":true},"
        "\"evaluateFace\":{\"schemaVersion\":1,\"nativeExact\":true,\"parameterModes\":[\"normalized\",\"native\"]},"
        "\"getPlanarFaceWires\":{\"schemaVersion\":1,\"nativeExact\":true,\"planarFacesOnly\":true,\"returnsLocalAndWorldWires\":true}"
        "}"
        "}";
}

std::string OcctKernel::getCapabilities() const {
    return "{"
        "\"featureEdgesV1\":true,"
        "\"rawEdgeSegmentsV1\":true,"
        "\"featurePreviewV1\":true,"
        "\"tessellationOptionsV1\":true,"
        "\"triangleNormalsV1\":true,"
        "\"triangleFaceMappingV1\":true,"
        "\"topologySubshapesV1\":true,"
        "\"topologyHierarchyV1\":true,"
        "\"geometricStableHashesV1\":true,"
        "\"revisionInfoV1\":true,"
        "\"entityResolutionV1\":true,"
        "\"entityRemapV1\":true,"
        "\"revisionRetentionV1\":true,"
        "\"historyV1\":true,"
        "\"stableNamingV1\":true,"
        "\"checkpointV1\":true,"
        "\"versionInfoV1\":true,"
        "\"analysisV1\":true,"
        "\"sessionHandlesV1\":true,"
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
        "\"sweepProfile\":{"
        "\"schemaVersion\":1,"
        "\"nativeExact\":true,"
        "\"cutBoolean\":true,"
        "\"plane\":true,"
        "\"spine\":true,"
        "\"trihedronModes\":[\"correctedFrenet\",\"frenet\",\"discrete\",\"fixedTrihedron\",\"fixedBinormal\",\"auxiliarySpine\"],"
        "\"sectionWithContact\":true,"
        "\"sectionWithCorrection\":true,"
        "\"solid\":true,"
        "\"forceApproxC1\":true,"
        "\"transitionModes\":[\"transformed\",\"rightCorner\",\"roundCorner\"],"
        "\"tolerances\":true,"
        "\"maxDegree\":true,"
        "\"maxSegments\":true"
        "},"
        "\"loft\":{"
        "\"schemaVersion\":1,"
        "\"nativeExact\":true,"
        "\"cutBoolean\":true,"
        "\"sectionKinds\":[\"profile\",\"wire\",\"point\"],"
        "\"solid\":true,"
        "\"ruled\":true,"
        "\"pres3d\":true,"
        "\"checkCompatibility\":true,"
        "\"smoothing\":true,"
        "\"parametrization\":[\"chordLength\",\"centripetal\",\"isoParametric\"],"
        "\"continuity\":[\"C0\",\"G1\",\"C1\",\"G2\",\"C2\",\"C3\",\"CN\"],"
        "\"criteriumWeight\":true,"
        "\"maxDegree\":true,"
        "\"mutableInput\":true"
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
        "\"getPlanarFaceWires\":true,"
        "\"parameterModes\":[\"normalized\",\"native\"]"
        "},"
        "\"analysis\":{"
        "\"volume\":true,"
        "\"surfaceArea\":true,"
        "\"linearLength\":true,"
        "\"boundingBox\":true,"
        "\"centerOfMass\":true,"
        "\"shapeValidity\":true,"
        "\"pointContainment\":true,"
        "\"shapeIntersection\":true,"
        "\"closestPoint\":true,"
        "\"shapeDistance\":true"
        "},"
        "\"runtime\":{"
        "\"browser\":true,"
        "\"worker\":true,"
        "\"node\":true"
        "}"
        "}";
}

std::string OcctKernel::getKernelVersionInfo() const {
    return "{"
        "\"kernelVersion\":\"" OCC_VERSION_COMPLETE "\","
        "\"kernelVersionMajor\":" + std::to_string(OCC_VERSION_MAJOR) + ","
        "\"kernelVersionMinor\":" + std::to_string(OCC_VERSION_MINOR) + ","
        "\"kernelVersionMaintenance\":" + std::to_string(OCC_VERSION_MAINTENANCE) + ","
        "\"checkpointSchemaVersion\":1,"
        "\"operationSchemaVersion\":1"
        "}";
}

std::string OcctKernel::analyzeShape(uint32_t id) {
    try {
        const TopoDS_Shape& shape = requireShape(id);

        ShapeMap solids, shells, wires, faces, edges, vertices;
        TopExp::MapShapes(shape, TopAbs_SOLID, solids);
        TopExp::MapShapes(shape, TopAbs_SHELL, shells);
        TopExp::MapShapes(shape, TopAbs_WIRE, wires);
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        TopExp::MapShapes(shape, TopAbs_EDGE, edges);
        TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);

        GProp_GProps linearProps;
        GProp_GProps surfaceProps;
        GProp_GProps volumeProps;
        BRepGProp::LinearProperties(shape, linearProps, false, false);
        BRepGProp::SurfaceProperties(shape, surfaceProps, false, false);
        if (solids.Extent() > 0) {
            BRepGProp::VolumeProperties(shape, volumeProps, true, false, false);
        }

        const auto bounds = boundsOfShape(shape);
        BRepCheck_Analyzer analyzer(shape);

        std::string basis = "none";
        gp_Pnt center(0.0, 0.0, 0.0);
        bool hasCenter = false;
        if (solids.Extent() > 0 && std::abs(volumeProps.Mass()) > Precision::Confusion()) {
            basis = "volume";
            center = volumeProps.CentreOfMass();
            hasCenter = true;
        } else if (faces.Extent() > 0 && std::abs(surfaceProps.Mass()) > Precision::Confusion()) {
            basis = "surface";
            center = surfaceProps.CentreOfMass();
            hasCenter = true;
        } else if (edges.Extent() > 0 && std::abs(linearProps.Mass()) > Precision::Confusion()) {
            basis = "linear";
            center = linearProps.CentreOfMass();
            hasCenter = true;
        }

        std::ostringstream ss;
        ss << "{";
        ss << "\"shapeType\":"; appendJsonString(ss, shapeTypeName(shape.ShapeType())); ss << ",";
        ss << "\"solidCount\":" << solids.Extent() << ",";
        ss << "\"shellCount\":" << shells.Extent() << ",";
        ss << "\"wireCount\":" << wires.Extent() << ",";
        ss << "\"faceCount\":" << faces.Extent() << ",";
        ss << "\"edgeCount\":" << edges.Extent() << ",";
        ss << "\"vertexCount\":" << vertices.Extent() << ",";
        ss << "\"boundingBox\":{";
        ss << "\"xMin\":" << bounds[0] << ",";
        ss << "\"yMin\":" << bounds[1] << ",";
        ss << "\"zMin\":" << bounds[2] << ",";
        ss << "\"xMax\":" << bounds[3] << ",";
        ss << "\"yMax\":" << bounds[4] << ",";
        ss << "\"zMax\":" << bounds[5] << "},";
        ss << "\"isValid\":" << (analyzer.IsValid() ? "true" : "false") << ",";
        ss << "\"volume\":" << (solids.Extent() > 0 ? volumeProps.Mass() : 0.0) << ",";
        ss << "\"surfaceArea\":" << surfaceProps.Mass() << ",";
        ss << "\"linearLength\":" << linearProps.Mass() << ",";
        ss << "\"centerOfMass\":";
        if (hasCenter) {
            appendPointJson(ss, center);
        } else {
            ss << "null";
        }
        ss << ",\"centerOfMassBasis\":"; appendJsonString(ss, basis); ss << "}";
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return "{}";
}

std::string OcctKernel::classifyPointContainment(uint32_t id, const std::string& pointJson, double tolerance) {
    try {
        if (!(tolerance > 0.0) || !std::isfinite(tolerance)) {
            throwKernelError("INVALID_PARAMS", "tolerance must be a positive finite number");
        }

        const TopoDS_Shape& shape = requireShape(id);
        const gp_Pnt point = parsePoint3(mini_json::parse(pointJson), "point");

        ShapeMap solids;
        TopExp::MapShapes(shape, TopAbs_SOLID, solids);

        TopAbs_State finalState = TopAbs_UNKNOWN;
        if (solids.Extent() == 0) {
            finalState = TopAbs_UNKNOWN;
        } else {
            finalState = TopAbs_OUT;
            for (int index = 1; index <= solids.Extent(); ++index) {
                BRepClass3d_SolidClassifier classifier(TopoDS::Solid(solids(index)), point, tolerance);
                const TopAbs_State state = classifier.State();
                if (state == TopAbs_IN) {
                    finalState = TopAbs_IN;
                    break;
                }
                if (state == TopAbs_ON) {
                    finalState = TopAbs_ON;
                } else if (state == TopAbs_UNKNOWN && finalState != TopAbs_ON) {
                    finalState = TopAbs_UNKNOWN;
                }
            }
        }

        std::ostringstream ss;
        ss << "{";
        ss << "\"point\":"; appendPointJson(ss, point); ss << ",";
        ss << "\"tolerance\":" << tolerance << ",";
        ss << "\"state\":"; appendJsonString(ss, containmentStateName(finalState)); ss << ",";
        ss << "\"isInside\":" << ((finalState == TopAbs_IN || finalState == TopAbs_ON) ? "true" : "false");
        ss << "}";
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.what());
    }
    return "{}";
}

std::string OcctKernel::intersectShapes(uint32_t id1, uint32_t id2) {
    const std::string operation = "intersectShapes";
    try {
        auto sourceIt1 = _impl->records.find(id1);
        auto sourceIt2 = _impl->records.find(id2);
        if (sourceIt1 == _impl->records.end() || sourceIt2 == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "IntersectShapes requires valid resident handles");
        }

        BRepAlgoAPI_Section section(sourceIt1->second.shape, sourceIt2->second.shape, false);
        section.Build();
        if (!section.IsDone()) {
            throwStructuredOperationError(operation, "Section operation failed");
        }

        const TopoDS_Shape result = section.Shape();
        ShapeMap edges;
        ShapeMap vertices;
        if (!result.IsNull()) {
            TopExp::MapShapes(result, TopAbs_EDGE, edges);
            TopExp::MapShapes(result, TopAbs_VERTEX, vertices);
        }

        const bool hasIntersection = !result.IsNull() && (edges.Extent() > 0 || vertices.Extent() > 0);
        uint32_t sectionShapeId = 0;
        if (hasIntersection) {
            sectionShapeId = storeShapeWithMetadata(result,
                                                   "intersectShapes",
                                                   "shapeA=" + sourceIt1->second.revision.revisionId + ";shapeB=" + sourceIt2->second.revision.revisionId,
                                                   { sourceIt1->second.revision.revisionId, sourceIt2->second.revision.revisionId },
                                                   "generated",
                                                   "generated",
                                                   {});
        }

        std::ostringstream ss;
        ss << "{";
        ss << "\"hasIntersection\":" << (hasIntersection ? "true" : "false") << ",";
        ss << "\"edgeCount\":" << edges.Extent() << ",";
        ss << "\"vertexCount\":" << vertices.Extent();
        if (sectionShapeId != 0) {
            ss << ",\"sectionShapeId\":" << sectionShapeId;
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

std::string OcctKernel::findClosestPointOnShape(uint32_t id, const std::string& pointJson, double tolerance) {
    const std::string operation = "findClosestPointOnShape";
    try {
        if (!(tolerance > 0.0) || !std::isfinite(tolerance)) {
            throwKernelError("INVALID_PARAMS", "tolerance must be a positive finite number");
        }

        auto sourceIt = _impl->records.find(id);
        if (sourceIt == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
        }

        const gp_Pnt queryPoint = parsePoint3(mini_json::parse(pointJson), "point");
        const TopoDS_Vertex queryVertex = BRepBuilderAPI_MakeVertex(queryPoint);

        BRepExtrema_DistShapeShape extrema;
        extrema.SetDeflection(tolerance);
        extrema.LoadS1(queryVertex);
        extrema.LoadS2(sourceIt->second.shape);
        if (!extrema.Perform() || !extrema.IsDone() || extrema.NbSolution() < 1) {
            throwStructuredOperationError(operation, "Closest-point query failed");
        }

        std::ostringstream ss;
        ss << "{";
        ss << "\"queryPoint\":"; appendPointJson(ss, queryPoint); ss << ",";
        ss << "\"closestPoint\":"; appendPointJson(ss, extrema.PointOnShape2(1)); ss << ",";
        ss << "\"distance\":" << extrema.Value() << ",";
        ss << "\"solutionCount\":" << extrema.NbSolution() << ",";
        ss << "\"support\":";

        const BRepExtrema_SupportType supportType = extrema.SupportTypeShape2(1);
        switch (supportType) {
        case BRepExtrema_IsVertex:
            appendSupportRefJson(ss, sourceIt->second, extrema.SupportOnShape2(1), supportType);
            break;
        case BRepExtrema_IsOnEdge: {
            double parameter = 0.0;
            extrema.ParOnEdgeS2(1, parameter);
            appendSupportRefJson(ss, sourceIt->second, extrema.SupportOnShape2(1), supportType, true, parameter);
            break;
        }
        case BRepExtrema_IsInFace: {
            double u = 0.0;
            double v = 0.0;
            extrema.ParOnFaceS2(1, u, v);
            appendSupportRefJson(ss, sourceIt->second, extrema.SupportOnShape2(1), supportType, false, 0.0, true, u, v);
            break;
        }
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

std::string OcctKernel::measureShapeDistance(uint32_t id1, uint32_t id2, double tolerance) {
    const std::string operation = "measureShapeDistance";
    try {
        if (!(tolerance > 0.0) || !std::isfinite(tolerance)) {
            throwKernelError("INVALID_PARAMS", "tolerance must be a positive finite number");
        }

        auto sourceIt1 = _impl->records.find(id1);
        auto sourceIt2 = _impl->records.find(id2);
        if (sourceIt1 == _impl->records.end() || sourceIt2 == _impl->records.end()) {
            throwKernelError("INVALID_HANDLE", "MeasureShapeDistance requires valid resident handles");
        }

        BRepExtrema_DistShapeShape extrema;
        extrema.SetDeflection(tolerance);
        extrema.LoadS1(sourceIt1->second.shape);
        extrema.LoadS2(sourceIt2->second.shape);
        if (!extrema.Perform() || !extrema.IsDone()) {
            throwStructuredOperationError(operation, "Shape-distance query failed");
        }

        std::ostringstream ss;
        ss << "{";
        ss << "\"distance\":" << extrema.Value() << ",";
        ss << "\"clearance\":" << extrema.Value() << ",";
        ss << "\"innerSolution\":" << (extrema.InnerSolution() ? "true" : "false") << ",";
        ss << "\"isInContact\":" << (extrema.Value() <= tolerance ? "true" : "false") << ",";
        ss << "\"solutionCount\":" << extrema.NbSolution() << ",";
        ss << "\"solutions\":[";

        for (int index = 1; index <= extrema.NbSolution(); ++index) {
            if (index > 1) {
                ss << ",";
            }
            ss << "{";
            ss << "\"pointOnA\":"; appendPointJson(ss, extrema.PointOnShape1(index)); ss << ",";
            ss << "\"pointOnB\":"; appendPointJson(ss, extrema.PointOnShape2(index)); ss << ",";
            ss << "\"supportOnA\":";
            const BRepExtrema_SupportType supportTypeA = extrema.SupportTypeShape1(index);
            switch (supportTypeA) {
            case BRepExtrema_IsVertex:
                appendSupportRefJson(ss, sourceIt1->second, extrema.SupportOnShape1(index), supportTypeA);
                break;
            case BRepExtrema_IsOnEdge: {
                double parameter = 0.0;
                extrema.ParOnEdgeS1(index, parameter);
                appendSupportRefJson(ss, sourceIt1->second, extrema.SupportOnShape1(index), supportTypeA, true, parameter);
                break;
            }
            case BRepExtrema_IsInFace: {
                double u = 0.0;
                double v = 0.0;
                extrema.ParOnFaceS1(index, u, v);
                appendSupportRefJson(ss, sourceIt1->second, extrema.SupportOnShape1(index), supportTypeA, false, 0.0, true, u, v);
                break;
            }
            }
            ss << ",\"supportOnB\":";
            const BRepExtrema_SupportType supportTypeB = extrema.SupportTypeShape2(index);
            switch (supportTypeB) {
            case BRepExtrema_IsVertex:
                appendSupportRefJson(ss, sourceIt2->second, extrema.SupportOnShape2(index), supportTypeB);
                break;
            case BRepExtrema_IsOnEdge: {
                double parameter = 0.0;
                extrema.ParOnEdgeS2(index, parameter);
                appendSupportRefJson(ss, sourceIt2->second, extrema.SupportOnShape2(index), supportTypeB, true, parameter);
                break;
            }
            case BRepExtrema_IsInFace: {
                double u = 0.0;
                double v = 0.0;
                extrema.ParOnFaceS2(index, u, v);
                appendSupportRefJson(ss, sourceIt2->second, extrema.SupportOnShape2(index), supportTypeB, false, 0.0, true, u, v);
                break;
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

struct TessellationOptions {
    bool includeTriangleNormals = true;
    bool includeTriangleTopoFaceIds = true;
    bool includeTriangleFaceGroups = true;
    bool includeTriangleStableHashes = true;
    bool includeFeatureEdges = true;
    bool includeRawEdgeSegments = true;
    std::vector<int> faceTopoIds;
};

TessellationOptions parseTessellationOptions(const std::string& optionsJson,
                                             const TopoDS_Shape& shape,
                                             const RevisionMetadata* revision) {
    TessellationOptions options;
    if (optionsJson.empty()) {
        return options;
    }

    const mini_json::Value root = mini_json::requireObject(mini_json::parse(optionsJson), "tessellation options");
    const bool includeMetadata = optionalBoolMember(root, "includeMetadata", true);
    options.includeTriangleNormals = optionalBoolMember(root, "includeTriangleNormals", includeMetadata);
    options.includeTriangleTopoFaceIds = optionalBoolMember(root, "includeTriangleTopoFaceIds", includeMetadata);
    options.includeTriangleFaceGroups = optionalBoolMember(root, "includeTriangleFaceGroups", includeMetadata);
    options.includeTriangleStableHashes = optionalBoolMember(root, "includeTriangleStableHashes", includeMetadata);
    options.includeFeatureEdges = optionalBoolMember(root, "includeFeatureEdges", includeMetadata);
    options.includeRawEdgeSegments = optionalBoolMember(root, "includeRawEdgeSegments", includeMetadata);
    if (const mini_json::Value* facesValue = root.get("faces")) {
        const mini_json::Value& faces = mini_json::requireArray(*facesValue, "tessellation options.faces");
        if (faces.array.empty()) {
            throwStructuredValidation("tessellate", "faces", "At least one face reference is required when faces is provided");
        }
        for (std::size_t i = 0; i < faces.array.size(); ++i) {
            ResolvedFaceRef face = resolveFaceRef(shape,
                                                  revision,
                                                  faces.array[i],
                                                  "tessellate",
                                                  "faces[" + std::to_string(i) + "]");
            if (std::find(options.faceTopoIds.begin(), options.faceTopoIds.end(), face.topoId) == options.faceTopoIds.end()) {
                options.faceTopoIds.push_back(face.topoId);
            }
        }
    }
    return options;
}

std::string OcctKernel::tessellate(uint32_t id, double linearDeflection, double angularDeflection) {
    return tessellateWithOptions(id, linearDeflection, angularDeflection, "");
}

std::string OcctKernel::tessellateWithOptions(uint32_t id, double linearDeflection, double angularDeflection, const std::string& optionsJson) {
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
        const TessellationOptions options = parseTessellationOptions(optionsJson, shape, &revision);

        ShapeMap faceMap, edgeMap;
        TopExp::MapShapes(shape, TopAbs_FACE, faceMap);
        TopExp::MapShapes(shape, TopAbs_EDGE, edgeMap);
        if (options.faceTopoIds.empty()) {
            BRepMesh_IncrementalMesh mesh(shape, linearDeflection, false, angularDeflection);
            mesh.Perform();
            BRepLib::EnsureNormalConsistency(shape);
        } else {
            for (int faceTopoId : options.faceTopoIds) {
                const TopoDS_Face& face = TopoDS::Face(faceMap(faceTopoId));
                BRepMesh_IncrementalMesh mesh(face, linearDeflection, false, angularDeflection);
                mesh.Perform();
                BRepLib::EnsureNormalConsistency(face);
            }
        }
        const std::set<int> selectedFaceIds(options.faceTopoIds.begin(), options.faceTopoIds.end());

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
            if (!options.faceTopoIds.empty()
                && std::find(options.faceTopoIds.begin(), options.faceTopoIds.end(), faceIndex) == options.faceTopoIds.end()) {
                continue;
            }
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

                if (options.includeTriangleNormals) {
                    if (!firstTriangleNormal) triangle_normals_ss << ",";
                    firstTriangleNormal = false;
                    triangle_normals_ss << triangleNormal.X() << "," << triangleNormal.Y() << "," << triangleNormal.Z();
                }

                if (options.includeTriangleTopoFaceIds) {
                    if (!firstTriangleFaceId) triangle_face_ids_ss << ",";
                    firstTriangleFaceId = false;
                    triangle_face_ids_ss << faceIndex;
                }

                if (options.includeTriangleFaceGroups) {
                    if (!firstTriangleFaceGroup) triangle_face_groups_ss << ",";
                    firstTriangleFaceGroup = false;
                    triangle_face_groups_ss << faceIndex;
                }

                if (options.includeTriangleStableHashes) {
                    if (!firstTriangleHash) triangle_hashes_ss << ",";
                    firstTriangleHash = false;
                    appendJsonString(triangle_hashes_ss, faceHashes[static_cast<std::size_t>(faceIndex)]);
                }
            }

            globalIndex += (uint32_t)tri->NbNodes();
        }

        if (options.includeRawEdgeSegments) {
            for (int edgeIndex = 1; edgeIndex <= edgeMap.Extent(); ++edgeIndex) {
                const TopoDS_Edge& edge = TopoDS::Edge(edgeMap(edgeIndex));
                const std::vector<int> faceIds = faceIdsForEdge(edge, faceMap, edgeToFaces);
                if (!selectedFaceIds.empty()
                    && std::none_of(faceIds.begin(), faceIds.end(), [&](int faceId) { return selectedFaceIds.count(faceId) > 0; })) {
                    continue;
                }
                const std::vector<gp_Pnt> points = collectEdgePolyline(edge);
                for (const gp_Pnt& pt : points) {
                    if (!firstRawEdge) raw_edges_ss << ",";
                    firstRawEdge = false;
                    raw_edges_ss << pt.X() << "," << pt.Y() << "," << pt.Z();
                }
            }
        }

        std::set<std::string> emittedFeatureKeys;
        int chainId = 1;
        bool firstFeatureEdge = true;
        if (options.includeFeatureEdges) {
            for (int edgeIndex = 1; edgeIndex <= edgeMap.Extent(); ++edgeIndex) {
                const TopoDS_Edge& edge = TopoDS::Edge(edgeMap(edgeIndex));
                std::vector<int> faceIds = faceIdsForEdge(edge, faceMap, edgeToFaces);
                if (!selectedFaceIds.empty()
                    && std::none_of(faceIds.begin(), faceIds.end(), [&](int faceId) { return selectedFaceIds.count(faceId) > 0; })) {
                    continue;
                }
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
        result << "\"indices\":"      << indices_ss.str();
        if (options.includeTriangleNormals) {
            result << ",\"triangleNormals\":" << triangle_normals_ss.str();
        }
        if (options.includeTriangleTopoFaceIds) {
            result << ",\"triangleTopoFaceIds\":" << triangle_face_ids_ss.str();
        }
        if (options.includeTriangleFaceGroups) {
            result << ",\"triangleFaceGroups\":" << triangle_face_groups_ss.str();
        }
        if (options.includeTriangleStableHashes) {
            result << ",\"triangleStableHashes\":" << triangle_hashes_ss.str();
        }
        if (options.includeFeatureEdges) {
            result << ",\"featureEdges\":" << feature_edges_ss.str();
        }
        if (options.includeRawEdgeSegments) {
            result << ",\"rawEdgeSegments\":" << raw_edges_ss.str();
        }
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

std::string OcctKernel::importStepPackage(const std::string& content,
                                         bool heal,
                                         bool sew,
                                         bool fixSameParameter,
                                         bool fixSolid,
                                         double sewingTolerance,
                                         double linearDeflection,
                                         double angularDeflection) {
    StepImportOptions options;
    options.heal = heal;
    options.sew = sew;
    options.fixSameParameter = fixSameParameter;
    options.fixSolid = fixSolid;
    options.sewingTolerance = sewingTolerance > 0 ? sewingTolerance : 1.0e-6;

    StepImportRunResult result = runStepImport(content, options);

    std::ostringstream ss;
    ss << "{";
    ss << "\"readStatus\":\"" << escapeJson(result.readStatus) << "\",";
    ss << "\"transferStatus\":\"" << escapeJson(result.transferStatus) << "\",";
    ss << "\"healed\":" << (result.healed ? "true" : "false") << ",";
    ss << "\"isValid\":" << (result.isValid ? "true" : "false") << ",";

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

    if (result.hasShape) {
        uint32_t shapeId = storeShapeWithMetadata(result.shape,
                                                 "importStepPackage",
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

        ss << ",\"shapeId\":" << shapeId << ",";

        // Let's get the revision info from records
        auto recordIt = _impl->records.find(shapeId);
        if (recordIt != _impl->records.end()) {
            const ShapeRecord& record = recordIt->second;
            ss << "\"revision\":{";
            ss << "\"revisionId\":"; appendJsonString(ss, record.revision.revisionId); ss << ",";
            ss << "\"topologyHash\":"; appendJsonString(ss, record.revision.topologyHash);
            ss << "},";
        }

        // Physical properties and BRepCheck as in analyzeShape:
        ShapeMap solids, shells, wires, faces, edges, vertices;
        TopExp::MapShapes(result.shape, TopAbs_SOLID, solids);
        TopExp::MapShapes(result.shape, TopAbs_SHELL, shells);
        TopExp::MapShapes(result.shape, TopAbs_WIRE, wires);
        TopExp::MapShapes(result.shape, TopAbs_FACE, faces);
        TopExp::MapShapes(result.shape, TopAbs_EDGE, edges);
        TopExp::MapShapes(result.shape, TopAbs_VERTEX, vertices);

        GProp_GProps linearProps;
        GProp_GProps surfaceProps;
        GProp_GProps volumeProps;
        BRepGProp::LinearProperties(result.shape, linearProps, false, false);
        BRepGProp::SurfaceProperties(result.shape, surfaceProps, false, false);
        if (solids.Extent() > 0) {
            BRepGProp::VolumeProperties(result.shape, volumeProps, true, false, false);
        }

        const auto bounds = boundsOfShape(result.shape);
        BRepCheck_Analyzer analyzer(result.shape);

        std::string basis = "none";
        gp_Pnt center(0.0, 0.0, 0.0);
        bool hasCenter = false;
        if (solids.Extent() > 0 && std::abs(volumeProps.Mass()) > Precision::Confusion()) {
            basis = "volume";
            center = volumeProps.CentreOfMass();
            hasCenter = true;
        } else if (faces.Extent() > 0 && std::abs(surfaceProps.Mass()) > Precision::Confusion()) {
            basis = "surface";
            center = surfaceProps.CentreOfMass();
            hasCenter = true;
        } else if (edges.Extent() > 0 && std::abs(linearProps.Mass()) > Precision::Confusion()) {
            basis = "linear";
            center = linearProps.CentreOfMass();
            hasCenter = true;
        }

        ss << "\"properties\":{";
        ss << "\"boundingBox\":{";
        ss << "\"xMin\":" << bounds[0] << ",";
        ss << "\"yMin\":" << bounds[1] << ",";
        ss << "\"zMin\":" << bounds[2] << ",";
        ss << "\"xMax\":" << bounds[3] << ",";
        ss << "\"yMax\":" << bounds[4] << ",";
        ss << "\"zMax\":" << bounds[5] << "},";
        ss << "\"volume\":" << (solids.Extent() > 0 ? volumeProps.Mass() : 0.0) << ",";
        ss << "\"surfaceArea\":" << surfaceProps.Mass() << ",";
        ss << "\"linearLength\":" << linearProps.Mass() << ",";
        ss << "\"centerOfMass\":";
        if (hasCenter) {
            appendPointJson(ss, center);
        } else {
            ss << "null";
        }
        ss << ",\"centerOfMassBasis\":"; appendJsonString(ss, basis);
        ss << "},";

        // Topology:
        ss << "\"topology\":{";
        ss << "\"solidCount\":" << solids.Extent() << ",";
        ss << "\"shellCount\":" << shells.Extent() << ",";
        ss << "\"wireCount\":" << wires.Extent() << ",";
        ss << "\"faceCount\":" << faces.Extent() << ",";
        ss << "\"edgeCount\":" << edges.Extent() << ",";
        ss << "\"vertexCount\":" << vertices.Extent() << ",";
        ss << "\"isValid\":" << (analyzer.IsValid() ? "true" : "false");
        ss << "},";

        // Checkpoint:
        std::string checkpointJson = createCheckpoint(shapeId);
        ss << "\"checkpoint\":" << checkpointJson << ",";

        // Compute and append the mesh (tessellate):
        if (linearDeflection > 0 && angularDeflection > 0) {
            std::string meshJson = tessellate(shapeId, linearDeflection, angularDeflection);
            ss << "\"mesh\":" << meshJson;
        } else {
            ss << "\"mesh\":null";
        }
    } else {
        ss << ",\"shapeId\":null";
    }

    ss << "}";
    return ss.str();
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
    } catch (const std::runtime_error& error) {
        if (looksLikeKernelErrorJson(error.what())) {
            throw;
        }
        throwKernelError("INVALID_CHECKPOINT", error.what());
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
