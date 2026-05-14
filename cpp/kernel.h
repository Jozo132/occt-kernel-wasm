/**
 * @file kernel.h
 * @brief OcctKernel – the central C++ adapter between Emscripten/JS and OCCT.
 *
 * Design rules:
 *  - No OCCT types cross the public interface.
 *  - Shapes are stored by opaque uint32_t handles.
 *  - All errors are reported as C++ exceptions carrying a JSON descriptor string.
 *  - The class is instantiated once per logical kernel session.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

// Forward-declare so that headers that only include kernel.h do not need to
// pull in OCCT. The OCCT headers are included in kernel.cpp.
class TopoDS_Shape;

namespace occt_kernel {

/**
 * All OCCT adapter operations are methods of this class.
 *
 * Instance lifetime is managed by the JS/TS layer via the OcctKernel Emscripten
 * binding.  Memory is released when the JS object is garbage-collected or when
 * `delete instance` is called explicitly on the Emscripten binding.
 */
class OcctKernel {
public:
    OcctKernel();
    ~OcctKernel();

    // Prevent copying – the shape map must not be duplicated accidentally.
    OcctKernel(const OcctKernel&) = delete;
    OcctKernel& operator=(const OcctKernel&) = delete;

    // -----------------------------------------------------------------------
    // Primitive creation
    // -----------------------------------------------------------------------

    /// Create a solid box.  Corner at origin, dimensions dx × dy × dz.
    uint32_t createBox(double dx, double dy, double dz);

    /// Create a solid cylinder.  Axis along +Z, base centre at origin.
    uint32_t createCylinder(double radius, double height);

    /// Create a solid sphere centred at the origin.
    uint32_t createSphere(double radius);

    // -----------------------------------------------------------------------
    // Sketch-based features
    // -----------------------------------------------------------------------

    /**
     * Extrude a 2-D profile by `height` units along +Z.
     * @param profileJson  JSON serialisation of a ::Profile struct.
     * @param height       Extrusion length (must be > 0).
     */
    uint32_t extrudeProfile(const std::string& profileJson, double height);

    /**
     * Revolve a 2-D profile about the Y axis.
     * @param profileJson    JSON serialisation of a ::Profile struct.
     * @param angleDegrees   Revolution angle in degrees (0 < angle ≤ 360).
     */
    uint32_t revolveProfile(const std::string& profileJson, double angleDegrees);

    // -----------------------------------------------------------------------
    // Boolean operations
    // -----------------------------------------------------------------------

    uint32_t booleanUnion(uint32_t id1, uint32_t id2);
    uint32_t booleanSubtract(uint32_t id1, uint32_t id2);
    uint32_t booleanIntersect(uint32_t id1, uint32_t id2);

    // -----------------------------------------------------------------------
    // Modifiers
    // -----------------------------------------------------------------------

    /// Fillet all edges of the shape with a constant radius.
    uint32_t filletEdges(uint32_t id, double radius);

    /// Chamfer all edges of the shape with a constant distance.
    uint32_t chamferEdges(uint32_t id, double distance);

    // -----------------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------------

    /**
     * Return a JSON string containing:
     *   faceCount, edgeCount, vertexCount, boundingBox, isValid
     */
    std::string getTopology(uint32_t id);

    bool checkValidity(uint32_t id);

    // -----------------------------------------------------------------------
    // Tessellation
    // -----------------------------------------------------------------------

    /**
     * Triangulate the shape and return a JSON string containing:
     *   positions[], normals[], indices[], edgeSegments[]
     */
    std::string tessellate(uint32_t id, double linearDeflection, double angularDeflection);

    // -----------------------------------------------------------------------
    // Import / export
    // -----------------------------------------------------------------------

    /// Parse a STEP file supplied as a UTF-8 string.  Returns a new handle.
    uint32_t importStep(const std::string& content);

    /// Parse a STEP file and return structured reader / transfer diagnostics as JSON.
    std::string importStepDetailed(const std::string& content,
                                   bool heal,
                                   bool sew,
                                   bool fixSameParameter,
                                   bool fixSolid,
                                   double sewingTolerance);

    /// Write the shape to STEP format and return the file content as a string.
    std::string exportStep(uint32_t id);

    // -----------------------------------------------------------------------
    // Memory management
    // -----------------------------------------------------------------------

    /// Release the native memory for the given handle.
    void disposeShape(uint32_t id);

private:
    struct Impl;
    Impl* _impl;

    uint32_t storeShape(const TopoDS_Shape& shape);
    const TopoDS_Shape& requireShape(uint32_t id) const;
};

} // namespace occt_kernel
