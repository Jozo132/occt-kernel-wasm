/**
 * @file kernel.cpp
 * @brief Implementation of OcctKernel.
 */

#include "kernel.h"
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
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>

// Primitives
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>

// Prism / Revolution
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>

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

// Topology iteration
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopExp.hxx>
#include <TopAbs_ShapeEnum.hxx>

// String / stream helpers
#include <OSD_File.hxx>
#include <OSD_Path.hxx>
#include <OSD_Protection.hxx>

#include <sstream>
#include <stdexcept>
#include <unordered_map>
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

uint32_t OcctKernel::extrudeProfile(const std::string& profileJson, double height) {
    if (height <= 0) {
        throwKernelError("INVALID_PARAMS", "Extrusion height must be > 0");
    }
    try {
        TopoDS_Face face = buildFaceFromProfile(profileJson);
        gp_Vec dir(0, 0, height);
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

uint32_t OcctKernel::revolveProfile(const std::string& profileJson, double angleDegrees) {
    if (angleDegrees <= 0 || angleDegrees > 360) {
        throwKernelError("INVALID_PARAMS", "angleDegrees must be in (0, 360]");
    }
    try {
        TopoDS_Face face = buildFaceFromProfile(profileJson);
        gp_Ax1 axis(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0)); // revolve about Y
        double angleRad = angleDegrees * M_PI / 180.0;
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
    if (content.empty()) {
        throwKernelError("IMPORT_FAILED", "STEP content is empty");
    }
    try {
        // Write content to a temporary in-memory file via OSD_File
        TCollection_AsciiString tmpPath("/tmp/occt_import_tmp.step");
        {
            std::ofstream ofs(tmpPath.ToCString());
            if (!ofs) throwKernelError("IMPORT_FAILED", "Cannot create temporary file for STEP import");
            ofs << content;
        }
        STEPControl_Reader reader;
        IFSelect_ReturnStatus status = reader.ReadFile(tmpPath.ToCString());
        if (status != IFSelect_RetDone) {
            throwKernelError("IMPORT_FAILED", "STEPControl_Reader::ReadFile failed");
        }
        reader.TransferRoots();
        if (reader.NbShapes() == 0) {
            throwKernelError("IMPORT_FAILED", "No shapes found in STEP file");
        }
        TopoDS_Shape shape = reader.OneShape();
        return storeShape(shape);
    } catch (const std::runtime_error&) {
        throw;
    } catch (const Standard_Failure& sf) {
        throwKernelError("IMPORT_FAILED", sf.GetMessageString());
    }
    return 0;
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
