/**
 * All public TypeScript types for occt-kernel-wasm.
 *
 * No OCCT types are exposed here. All shapes are referenced by opaque integer handles.
 */

// ---------------------------------------------------------------------------
// Shape handle
// ---------------------------------------------------------------------------

/** Opaque reference to a shape held inside the OCCT kernel. */
export interface ShapeHandle {
    readonly id: number;
}

// ---------------------------------------------------------------------------
// Primitive creation parameters
// ---------------------------------------------------------------------------

export interface BoxParams {
    /** Width  (X direction) in model units. */
    readonly dx: number;
    /** Depth  (Y direction) in model units. */
    readonly dy: number;
    /** Height (Z direction) in model units. */
    readonly dz: number;
}

export interface CylinderParams {
    /** Base-circle radius. */
    readonly radius: number;
    /** Cylinder height. */
    readonly height: number;
}

export interface SphereParams {
    /** Sphere radius. */
    readonly radius: number;
}

// ---------------------------------------------------------------------------
// 2-D sketch profile
// ---------------------------------------------------------------------------

export type ProfileSegmentLine = {
    readonly type: 'line';
    /** Start point [x, y]. */
    readonly start: readonly [number, number];
    /** End point   [x, y]. */
    readonly end: readonly [number, number];
};

export type ProfileSegmentArc = {
    readonly type: 'arc';
    /** Start point [x, y]. */
    readonly start: readonly [number, number];
    /** Mid-point   [x, y] – used to define the arc. */
    readonly mid: readonly [number, number];
    /** End point   [x, y]. */
    readonly end: readonly [number, number];
};

export type ProfileSegmentCircle = {
    readonly type: 'circle';
    /** Centre [x, y]. */
    readonly centre: readonly [number, number];
    /** Radius. */
    readonly radius: number;
};

export type ProfileSegment =
    | ProfileSegmentLine
    | ProfileSegmentArc
    | ProfileSegmentCircle;

/**
 * A closed 2-D profile defined in the XY plane.
 * The profile is built into an OCCT wire/face by the kernel.
 */
export interface Profile {
    readonly segments: readonly ProfileSegment[];
}

// ---------------------------------------------------------------------------
// Feature parameters
// ---------------------------------------------------------------------------

export interface ExtrudeParams {
    /** 2-D profile to extrude. */
    readonly profile: Profile;
    /** Distance to extrude (positive = +Z direction). */
    readonly height: number;
}

export interface RevolveParams {
    /** 2-D profile to revolve. */
    readonly profile: Profile;
    /**
     * Revolution angle in degrees.
     * Use 360 for a full revolution.
     */
    readonly angleDegrees: number;
}

// ---------------------------------------------------------------------------
// Boolean operation parameters
// ---------------------------------------------------------------------------

export interface BooleanParams {
    /** Base shape. */
    readonly base: ShapeHandle;
    /** Tool shape (subtracted from / intersected with / unified with the base). */
    readonly tool: ShapeHandle;
}

// ---------------------------------------------------------------------------
// Modifier parameters
// ---------------------------------------------------------------------------

export interface FilletParams {
    /** Shape to fillet. */
    readonly shape: ShapeHandle;
    /** Fillet radius. */
    readonly radius: number;
}

export interface ChamferParams {
    /** Shape to chamfer. */
    readonly shape: ShapeHandle;
    /** Chamfer distance. */
    readonly distance: number;
}

// ---------------------------------------------------------------------------
// Tessellation
// ---------------------------------------------------------------------------

export interface TessellateParams {
    /** Shape to tessellate. */
    readonly shape: ShapeHandle;
    /**
     * Linear deflection (chord height). Smaller = finer mesh.
     * Defaults to 0.1 model units.
     */
    readonly linearDeflection?: number;
    /**
     * Angular deflection in radians. Smaller = finer curves.
     * Defaults to 0.5 radians.
     */
    readonly angularDeflection?: number;
}

/**
 * Triangulated mesh output suitable for WebGL / Three.js rendering pipelines.
 *
 * All arrays are flat (interleaved):
 *   - positions: [x0, y0, z0, x1, y1, z1, …]
 *   - normals:   [nx0, ny0, nz0, nx1, ny1, nz1, …]
 *   - indices:   [i0, i1, i2, …] (triangles)
 */
export interface TessellationResult {
    readonly positions: Float32Array;
    readonly normals: Float32Array;
    readonly indices: Uint32Array;
    /** Optional: edge polyline segments (pairs of points). */
    readonly edgeSegments?: Float32Array;
}

// ---------------------------------------------------------------------------
// Topology query
// ---------------------------------------------------------------------------

export interface TopologyResult {
    readonly faceCount: number;
    readonly edgeCount: number;
    readonly vertexCount: number;
    /** Bounding box in model coordinates. */
    readonly boundingBox: BoundingBox;
    /** True if the shape is geometrically and topologically valid. */
    readonly isValid: boolean;
}

export interface BoundingBox {
    readonly xMin: number;
    readonly yMin: number;
    readonly zMin: number;
    readonly xMax: number;
    readonly yMax: number;
    readonly zMax: number;
}

// ---------------------------------------------------------------------------
// Import / export
// ---------------------------------------------------------------------------

export interface ImportStepParams {
    /** Raw content of a STEP file as a UTF-8 string. */
    readonly content: string;
}

export interface ExportStepParams {
    readonly shape: ShapeHandle;
}

// ---------------------------------------------------------------------------
// Dispose
// ---------------------------------------------------------------------------

export interface DisposeParams {
    readonly shape: ShapeHandle;
}
