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
#include <vector>

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
     * Extrude a 2-D profile using JSON options for placement and direction.
     * @param profileJson  JSON serialisation of a profile struct.
     * @param optionsJson  JSON serialisation of the extrusion options.
     */
    uint32_t extrudeProfile(const std::string& profileJson, const std::string& optionsJson);

    /// Apply a structured additive profile extrude feature to a resident shape.
    uint32_t extrudeProfileWithSpec(uint32_t id, const std::string& profileJson, const std::string& specJson);

    /// Apply a structured subtractive profile extrude feature to a resident shape.
    uint32_t extrudeCutProfileWithSpec(uint32_t id, const std::string& profileJson, const std::string& specJson);

    /**
     * Revolve a 2-D profile about an arbitrary world-space axis.
     * @param profileJson    JSON serialisation of a profile struct.
     * @param optionsJson    JSON serialisation of the revolve options.
     */
    uint32_t revolveProfile(const std::string& profileJson, const std::string& optionsJson);

    /// Apply a structured additive profile revolve feature to a resident shape.
    uint32_t revolveProfileWithSpec(uint32_t id, const std::string& profileJson, const std::string& specJson);

    /// Apply a structured subtractive profile revolve feature to a resident shape.
    uint32_t revolveCutProfileWithSpec(uint32_t id, const std::string& profileJson, const std::string& specJson);

    /// Apply a structured sweep feature with optional subtractive composition to a resident shape.
    uint32_t sweepProfileWithSpec(uint32_t id, const std::string& profileJson, const std::string& specJson);

    /// Apply a structured loft feature with optional subtractive composition to a resident shape.
    uint32_t loftWithSpec(uint32_t id, const std::string& sectionsJson, const std::string& specJson);

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

    /// Apply a versioned structured fillet spec and return blend lineage JSON.
    std::string filletEdgesWithSpec(uint32_t id, const std::string& specJson);

    /// Apply a versioned structured chamfer spec and return blend lineage JSON.
    std::string chamferEdgesWithSpec(uint32_t id, const std::string& specJson);

    /// Apply a world-space transform to an existing resident shape.
    uint32_t transformShape(uint32_t id, const std::string& transformJson);

    // -----------------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------------

    /**
        * Return a JSON string containing summary counts plus additive revision and
        * subshape metadata for faces, edges, and vertices.
     */
    std::string getTopology(uint32_t id);

    /// Return metadata for the immutable exact revision represented by a handle.
    std::string getRevisionInfo(uint32_t id);

    /// Resolve a stable face/edge/vertex hash in the current exact topology.
    std::string resolveStableEntity(uint32_t id, const std::string& stableHash);

    /// Map stable hashes from one resident revision id to another.
    std::string mapEntitiesAcrossRevisions(const std::string& fromRevisionId,
                                           const std::string& toRevisionId,
                                           const std::string& stableHashesJson);

    /// Evaluate an exact edge at a normalized or native curve parameter.
    std::string evaluateEdge(uint32_t id, const std::string& edgeRefJson, double t);

    /// Sample an exact edge using a JSON options object.
    std::string sampleEdge(uint32_t id, const std::string& edgeRefJson, const std::string& optionsJson);

    /// Return exact curve metadata for an edge reference.
    std::string getEdgeCurve(uint32_t id, const std::string& edgeRefJson);

    /// Evaluate an exact face at a normalized or native UV parameter.
    std::string evaluateFace(uint32_t id, const std::string& faceRefJson, double u, double v);

    /// Return the native operation schema/capability contract.
    std::string getOperationSchema() const;

    /// Return capability flags for additive API contracts exposed by this build.
    std::string getCapabilities() const;

    /// Return native kernel and schema version metadata.
    std::string getKernelVersionInfo() const;

    /// Return exact OCCT analysis properties for a resident shape.
    std::string analyzeShape(uint32_t id);

    /// Classify a world-space point against the exact resident solid model.
    std::string classifyPointContainment(uint32_t id, const std::string& pointJson, double tolerance);

    /// Build the exact section curves and vertices between two resident shapes.
    std::string intersectShapes(uint32_t id1, uint32_t id2);

    /// Return the closest point on a resident shape to a world-space query point.
    std::string findClosestPointOnShape(uint32_t id, const std::string& pointJson, double tolerance);

    /// Return the exact minimum distance / clearance between two resident shapes.
    std::string measureShapeDistance(uint32_t id1, uint32_t id2, double tolerance);

    bool checkValidity(uint32_t id);

    // -----------------------------------------------------------------------
    // Tessellation
    // -----------------------------------------------------------------------

    /**
     * Triangulate the shape and return a JSON string containing:
        *   positions[], normals[], indices[], triangle metadata, featureEdges[], rawEdgeSegments[]
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

    /// Parse a STEP file and return a fully fused result package as JSON.
    std::string importStepPackage(const std::string& content,
                                  bool heal,
                                  bool sew,
                                  bool fixSameParameter,
                                  bool fixSolid,
                                  double sewingTolerance,
                                  double linearDeflection,
                                  double angularDeflection);

    /// Write the shape to STEP format and return the file content as a string.
    std::string exportStep(uint32_t id);

    /// Serialize CBREP plus revision/history metadata to a JSON checkpoint.
    std::string createCheckpoint(uint32_t id);

    /// Hydrate a checkpoint JSON payload and return a resident handle.
    uint32_t hydrateCheckpoint(const std::string& checkpointJson);

    // -----------------------------------------------------------------------
    // Memory management
    // -----------------------------------------------------------------------

    /// Release the native memory for the given handle.
    void disposeShape(uint32_t id);

    /// Increment the resident revision reference count.
    void retainRevision(uint32_t id);

    /// Decrement the resident revision reference count; returns true if evicted.
    bool releaseRevision(uint32_t id);

private:
    struct Impl;
    Impl* _impl;

    uint32_t storeShape(const TopoDS_Shape& shape);
    uint32_t storeShapeWithMetadata(const TopoDS_Shape& shape,
                                    const std::string& operationType,
                                    const std::string& parameterSignature,
                                    const std::vector<std::string>& operandRevisionIds,
                                    const std::string& entityStatus,
                                    const std::string& identityStatus,
                                    const std::vector<std::string>& warnings);
    uint32_t performStructuredExtrudeFeature(uint32_t id,
                                             const std::string& profileJson,
                                             const std::string& specJson,
                                             const std::string& operationType,
                                             int fuseMode);
    uint32_t performStructuredRevolveFeature(uint32_t id,
                                             const std::string& profileJson,
                                             const std::string& specJson,
                                             const std::string& operationType,
                                             int fuseMode);
    uint32_t performStructuredSweepFeature(uint32_t id,
                                           const std::string& profileJson,
                                           const std::string& specJson,
                                           const std::string& operationType);
    uint32_t performStructuredLoftFeature(uint32_t id,
                                          const std::string& sectionsJson,
                                          const std::string& specJson,
                                          const std::string& operationType);
    const TopoDS_Shape& requireShape(uint32_t id) const;
    std::string requireRevisionId(uint32_t id) const;
};

} // namespace occt_kernel
