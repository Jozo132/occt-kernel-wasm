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
#include <TopoDS.hxx>
#include <BRep_Builder.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>

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
#include <TColgp_Array1OfPnt.hxx>
#include <Poly_Array1OfTriangle.hxx>
#include <TShort_Array1OfShortReal.hxx>
#include <gp_Vec.hxx>

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
    std::unordered_map<uint32_t, TopoDS_Shape> shapes;
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
    if (shape.IsNull()) {
        throwKernelError("OPERATION_FAILED", "Produced a null shape");
    }
    uint32_t id = _impl->nextId++;
    _impl->shapes[id] = shape;
    return id;
}

const TopoDS_Shape& OcctKernel::requireShape(uint32_t id) const {
    auto it = _impl->shapes.find(id);
    if (it == _impl->shapes.end()) {
        throwKernelError("INVALID_HANDLE", "No shape with handle " + std::to_string(id));
    }
    return it->second;
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
        return storeShape(mkBox.Shape());
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
        return storeShape(mkCyl.Shape());
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
        return storeShape(mkSph.Shape());
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
        return storeShape(mkPrism.Shape());
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
        return storeShape(mkRevol.Shape());
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
        return storeShape(op.Shape());
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
        return storeShape(op.Shape());
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
        return storeShape(op.Shape());
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
        return storeShape(mkFillet.Shape());
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
        return storeShape(mkChamfer.Shape());
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

        return storeShape(shape);
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
        const TopoDS_Shape& shape = requireShape(id);

        TopTools_IndexedMapOfShape faces, edges, vertices;
        TopExp::MapShapes(shape, TopAbs_FACE,   faces);
        TopExp::MapShapes(shape, TopAbs_EDGE,   edges);
        TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);

        Bnd_Box bbox;
        BRepBndLib::Add(shape, bbox);
        double xMin, yMin, zMin, xMax, yMax, zMax;
        bbox.Get(xMin, yMin, zMin, xMax, yMax, zMax);

        BRepCheck_Analyzer analyzer(shape);
        bool isValid = analyzer.IsValid();

        std::ostringstream ss;
        ss << "{";
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
        ss << "\"isValid\":" << (isValid ? "true" : "false");
        ss << "}";
        return ss.str();
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("OPERATION_FAILED", sf.GetMessageString());
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

std::string OcctKernel::tessellate(uint32_t id, double linearDeflection, double angularDeflection) {
    if (linearDeflection  <= 0) throwKernelError("INVALID_PARAMS", "linearDeflection must be > 0");
    if (angularDeflection <= 0) throwKernelError("INVALID_PARAMS", "angularDeflection must be > 0");

    try {
        const TopoDS_Shape& shape = requireShape(id);

        BRepMesh_IncrementalMesh mesh(shape, linearDeflection, Standard_False, angularDeflection);
        mesh.Perform();

        std::ostringstream positions_ss, normals_ss, indices_ss, edges_ss;
        positions_ss << "[";
        normals_ss   << "[";
        indices_ss   << "[";
        edges_ss     << "[";

        uint32_t globalIndex = 0;
        bool firstPos = true, firstIdx = true, firstEdge = true;

        for (TopExp_Explorer faceEx(shape, TopAbs_FACE); faceEx.More(); faceEx.Next()) {
            const TopoDS_Face& face = TopoDS::Face(faceEx.Current());
            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull()) continue;

            gp_Trsf trsf;
            if (!loc.IsIdentity()) trsf = loc.Transformation();
            bool reversed = (face.Orientation() == TopAbs_REVERSED);

            // Nodes
            for (int n = 1; n <= tri->NbNodes(); ++n) {
                gp_Pnt pt = tri->Node(n).Transformed(trsf);
                if (!firstPos) { positions_ss << ","; normals_ss << ","; }
                firstPos = false;
                positions_ss << pt.X() << "," << pt.Y() << "," << pt.Z();
                // Compute normal from triangulation if available, otherwise use 0,0,1
                if (tri->HasNormals()) {
                    gp_Dir nDir = tri->Normal(n);
                    gp_Vec nVec(nDir.X(), nDir.Y(), nDir.Z());
                    nVec.Transform(trsf);
                    double len = nVec.Magnitude();
                    if (len > 1e-9) nVec /= len;
                    normals_ss << nVec.X() << "," << nVec.Y() << "," << nVec.Z();
                } else {
                    normals_ss << "0,0,1";
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
            }

            globalIndex += (uint32_t)tri->NbNodes();
        }

        // Edge polylines
        for (TopExp_Explorer edgeEx(shape, TopAbs_EDGE); edgeEx.More(); edgeEx.Next()) {
            const TopoDS_Edge& edge = TopoDS::Edge(edgeEx.Current());
            TopLoc_Location loc;
            Handle(Poly_PolygonOnTriangulation) poly;
            Handle(Poly_Triangulation) tri;
            BRep_Tool::PolygonOnTriangulation(edge, poly, tri, loc);
            if (!poly.IsNull() && !tri.IsNull()) {
                gp_Trsf trsf;
                if (!loc.IsIdentity()) trsf = loc.Transformation();
                for (int n = 1; n <= poly->NbNodes(); ++n) {
                    gp_Pnt pt = tri->Node(poly->Nodes()(n)).Transformed(trsf);
                    if (!firstEdge) edges_ss << ",";
                    firstEdge = false;
                    edges_ss << pt.X() << "," << pt.Y() << "," << pt.Z();
                }
            }
        }

        positions_ss << "]";
        normals_ss   << "]";
        indices_ss   << "]";
        edges_ss     << "]";

        std::ostringstream result;
        result << "{";
        result << "\"positions\":"    << positions_ss.str() << ",";
        result << "\"normals\":"      << normals_ss.str()   << ",";
        result << "\"indices\":"      << indices_ss.str()   << ",";
        result << "\"edgeSegments\":" << edges_ss.str();
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

    return storeShape(result.shape);
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
        shapeId = storeShape(result.shape);
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

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

void OcctKernel::disposeShape(uint32_t id) {
    _impl->shapes.erase(id);
}

} // namespace occt_kernel
