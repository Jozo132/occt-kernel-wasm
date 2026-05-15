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

export type Point2 = readonly [number, number];
export type Point3 = readonly [number, number, number];
export type Vector3 = readonly [number, number, number];

// ---------------------------------------------------------------------------
// 2-D sketch profile
// ---------------------------------------------------------------------------

export type ProfileSegmentLine = {
    readonly type: 'line';
    /** Start point [x, y]. */
    readonly start: Point2;
    /** End point   [x, y]. */
    readonly end: Point2;
};

export type ProfileSegmentArc = {
    readonly type: 'arc';
    /** Start point [x, y]. */
    readonly start: Point2;
    /** Mid-point   [x, y] – used to define the arc. */
    readonly mid: Point2;
    /** End point   [x, y]. */
    readonly end: Point2;
};

export type ProfileSegmentCircle = {
    readonly type: 'circle';
    /** Centre [x, y]. */
    readonly centre: Point2;
    /** Radius. */
    readonly radius: number;
};

export type ProfileSegment =
    | ProfileSegmentLine
    | ProfileSegmentArc
    | ProfileSegmentCircle;

export interface ProfileWire {
    readonly segments: readonly ProfileSegment[];
}

export interface PlaneFrame {
    /** World-space origin for the local sketch plane. */
    readonly origin: Point3;
    /** Plane normal, used as the local +Z direction. */
    readonly normal: Vector3;
    /** World-space direction for the local +X axis. */
    readonly xDirection: Vector3;
}

/**
 * A closed 2-D profile defined in the local XY plane.
 * The kernel accepts a legacy single-wire `segments` profile, an `outer` wire
 * plus optional `holes`, or a canonical `wires` array where the first wire is
 * treated as outer and any remaining wires are treated as holes.
 */
export interface Profile {
    readonly segments?: readonly ProfileSegment[];
    readonly outer?: ProfileWire;
    readonly holes?: readonly ProfileWire[];
    readonly wires?: readonly ProfileWire[];
}

// ---------------------------------------------------------------------------
// Feature parameters
// ---------------------------------------------------------------------------

export interface ExtrudeParams {
    /** 2-D profile to extrude. */
    readonly profile: Profile;
    /** Optional sketch-plane placement for the local 2-D profile. */
    readonly plane?: PlaneFrame;
    /** Distance to extrude along local +Z / plane normal. */
    readonly height?: number;
    /** Explicit world-space extrusion vector. Mutually exclusive with `height`. */
    readonly vector?: Vector3;
}

export interface RevolveParams {
    /** 2-D profile to revolve. */
    readonly profile: Profile;
    /**
     * Revolution angle in degrees.
     * Use 360 for a full revolution.
     */
    readonly angleDegrees: number;
    /** Optional world-space point on the revolve axis. Defaults to the origin. */
    readonly axisOrigin?: Point3;
    /** Optional world-space axis direction. Defaults to +Y. */
    readonly axisDirection?: Vector3;
}

export interface RotationTransform {
    readonly axisOrigin: Point3;
    readonly axisDirection: Vector3;
    readonly angleDegrees: number;
}

export interface ShapeTransform {
    readonly translation?: Vector3;
    readonly rotation?: RotationTransform;
}

export interface TransformParams {
    readonly shape: ShapeHandle;
    readonly transform: ShapeTransform;
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

export type StepImportReadStatus =
    | 'IFSelect_RetVoid'
    | 'IFSelect_RetDone'
    | 'IFSelect_RetError'
    | 'IFSelect_RetFail'
    | 'IFSelect_RetStop';

export type StepImportTransferStatus =
    | 'NOT_RUN'
    | 'DONE'
    | 'PARTIAL'
    | 'EMPTY'
    | 'FAILED';

export type StepImportMessagePhase = 'load' | 'transfer' | 'heal' | 'validation';
export type StepImportMessageSeverity = 'info' | 'warning' | 'fail';

export interface StepImportMessage {
    readonly phase: StepImportMessagePhase;
    readonly severity: StepImportMessageSeverity;
    readonly text: string;
    readonly entityNumber?: number;
}

export interface StepImportOptions {
    /** Enable the broad OCCT ShapeFix pass after translation. */
    readonly heal?: boolean;
    /** Sew imported shells before applying ShapeFix. */
    readonly sew?: boolean;
    /** Force SameParameter fixing on imported edges. */
    readonly fixSameParameter?: boolean;
    /** Run solid-fix logic on the imported result. */
    readonly fixSolid?: boolean;
    /** Sewing tolerance used when `sew` is enabled. */
    readonly sewingTolerance?: number;
}

export interface ImportStepDetailedResult {
    readonly readStatus: StepImportReadStatus;
    readonly transferStatus: StepImportTransferStatus;
    readonly rootCount: number;
    readonly transferredRootCount: number;
    readonly messageList: readonly StepImportMessage[];
    readonly shape?: ShapeHandle;
    readonly isValid: boolean;
    readonly wasValidBeforeHealing: boolean;
    readonly healed: boolean;
}

export interface ImportStepParams {
    /** Raw content of a STEP file as a UTF-8 string. */
    readonly content: string;
    /** Optional post-processing controls for healing imported geometry. */
    readonly options?: StepImportOptions;
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
