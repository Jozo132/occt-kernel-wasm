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
    /** Resident handles are scoped to one kernel session. */
    readonly sessionId: string;
}

export interface KernelVersionInfo {
    readonly libraryVersion: string;
    readonly apiVersion: number;
    readonly kernelVersion: string;
    readonly kernelVersionMajor: number;
    readonly kernelVersionMinor: number;
    readonly kernelVersionMaintenance: number;
    readonly checkpointSchemaVersion: number;
    readonly operationSchemaVersion: number;
    readonly sessionId: string;
    readonly supportedRuntimes: readonly ('browser' | 'worker' | 'node')[];
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

export type ProfileSegmentBezier = {
    readonly type: 'bezier';
    /** Bezier control points. The first and last points are the segment endpoints. */
    readonly controlPoints: readonly Point2[];
};

export type ProfileSegmentBspline = {
    readonly type: 'bspline';
    /** Non-rational B-spline control points (poles). */
    readonly controlPoints: readonly Point2[];
    /** Polynomial degree of the curve. */
    readonly degree: number;
    /** Strictly increasing knot values. */
    readonly knots: readonly number[];
    /** Knot multiplicities matching the `knots` array. */
    readonly multiplicities: readonly number[];
};

export type ProfileSegment =
    | ProfileSegmentLine
    | ProfileSegmentArc
    | ProfileSegmentCircle
    | ProfileSegmentBezier
    | ProfileSegmentBspline;

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

export type SpatialCurveSegmentLine = {
    readonly type: 'line';
    readonly start: Point3;
    readonly end: Point3;
};

export type SpatialCurveSegmentArc = {
    readonly type: 'arc';
    readonly start: Point3;
    readonly mid: Point3;
    readonly end: Point3;
};

export type SpatialCurveSegmentCircle = {
    readonly type: 'circle';
    readonly center: Point3;
    readonly normal: Vector3;
    readonly radius: number;
    readonly xDirection?: Vector3;
};

export type SpatialCurveSegmentBezier = {
    readonly type: 'bezier';
    readonly controlPoints: readonly Point3[];
};

export type SpatialCurveSegmentBspline = {
    readonly type: 'bspline';
    readonly controlPoints: readonly Point3[];
    readonly degree: number;
    readonly knots: readonly number[];
    readonly multiplicities: readonly number[];
};

export type SpatialCurveSegment =
    | SpatialCurveSegmentLine
    | SpatialCurveSegmentArc
    | SpatialCurveSegmentCircle
    | SpatialCurveSegmentBezier
    | SpatialCurveSegmentBspline;

export interface SpatialWire {
    readonly segments: readonly SpatialCurveSegment[];
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

export interface ExtrudeSurfaceTarget {
    /** Optional external shape containing the limiting face. Defaults to `shape`. */
    readonly shape?: ShapeHandle;
    readonly face: FaceRef;
}

export type ExtrudeProfileExtent =
    | { readonly type: 'blind'; readonly distance: number }
    | { readonly type: 'upToNext' }
    | { readonly type: 'throughAll' }
    | { readonly type: 'upToSurface'; readonly surface: ExtrudeSurfaceTarget }
    | { readonly type: 'offsetFromSurface'; readonly surface: ExtrudeSurfaceTarget; readonly offset: number };

export interface ExtrudeProfileSpec {
    readonly schemaVersion: 1;
    readonly allowUnknownFields?: boolean;
    readonly unit?: { readonly length?: 'model'; readonly angle?: 'radians' | 'degrees' };
    /** Optional sketch-plane placement for the local 2-D profile. */
    readonly plane?: PlaneFrame;
    /** Explicit extrusion direction. Defaults to plane normal / +Z. */
    readonly direction?: Vector3;
    /** Flip the resolved extrusion direction before applying the feature. */
    readonly reverseDirection?: boolean;
    /** Optional draft angle in radians. Positive or negative values are allowed. */
    readonly draftAngleRadians?: number;
    /** Optional draft angle in degrees. Positive or negative values are allowed. */
    readonly draftAngleDegrees?: number;
    readonly extent: ExtrudeProfileExtent;
    readonly metadata?: Record<string, unknown>;
}

export interface ExtrudeProfileFeatureParams {
    readonly shape: ShapeHandle;
    readonly profile: Profile;
    readonly spec: ExtrudeProfileSpec;
}

export interface ExtrudeCutProfileFeatureParams {
    readonly shape: ShapeHandle;
    readonly profile: Profile;
    readonly spec: ExtrudeProfileSpec;
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

export interface RevolveSurfaceTarget {
    /** Optional external shape containing the limiting face. Defaults to `shape`. */
    readonly shape?: ShapeHandle;
    readonly face: FaceRef;
}

export interface RevolveSlidingEdgeSpec {
    /** 1-based edge index in the placed sketch profile topology. */
    readonly profileEdgeIndex: number;
    /** Base-shape face that the revolved sketch edge should slide on. */
    readonly face: FaceRef;
}

export type RevolveProfileExtent =
    | { readonly type: 'angle'; readonly angleRadians?: number; readonly angleDegrees?: number }
    | { readonly type: 'upToSurface'; readonly surface: RevolveSurfaceTarget }
    | { readonly type: 'fromSurfaceToSurface'; readonly fromSurface: RevolveSurfaceTarget; readonly untilSurface: RevolveSurfaceTarget }
    | { readonly type: 'throughAll' }
    | { readonly type: 'upToSurfaceAtAngle'; readonly surface: RevolveSurfaceTarget; readonly angleRadians?: number; readonly angleDegrees?: number };

export interface RevolveProfileSpec {
    readonly schemaVersion: 1;
    readonly allowUnknownFields?: boolean;
    readonly unit?: { readonly length?: 'model'; readonly angle?: 'radians' | 'degrees' };
    /** Optional sketch-plane placement for the local 2-D profile. */
    readonly plane?: PlaneFrame;
    /** Optional world-space point on the revolve axis. Defaults to the origin. */
    readonly axisOrigin?: Point3;
    /** Optional world-space axis direction. Defaults to +Y. */
    readonly axisDirection?: Vector3;
    /** Flip the resolved revolve axis direction before applying the feature. */
    readonly reverseDirection?: boolean;
    /** Optional profile-edge sliding constraints for local gluing behavior. */
    readonly slidingEdges?: readonly RevolveSlidingEdgeSpec[];
    readonly extent: RevolveProfileExtent;
    readonly metadata?: Record<string, unknown>;
}

export interface RevolveProfileFeatureParams {
    readonly shape: ShapeHandle;
    readonly profile: Profile;
    readonly spec: RevolveProfileSpec;
    /** When true, dispatches to the subtractive revolve feature path. */
    readonly cut?: boolean;
}

export interface RevolveCutProfileFeatureParams {
    readonly shape: ShapeHandle;
    readonly profile: Profile;
    readonly spec: RevolveProfileSpec;
}

export type SweepTrihedronMode =
    | { readonly type: 'correctedFrenet' }
    | { readonly type: 'frenet' }
    | { readonly type: 'discrete' }
    | { readonly type: 'fixedTrihedron'; readonly frame: PlaneFrame }
    | { readonly type: 'fixedBinormal'; readonly binormal: Vector3 }
    | {
        readonly type: 'auxiliarySpine';
        readonly spine: SpatialWire;
        readonly curvilinearEquivalence?: boolean;
        readonly contact?: 'none' | 'contact' | 'contactOnBorder';
    };

export interface SweepProfileSpec {
    readonly schemaVersion: 1;
    readonly allowUnknownFields?: boolean;
    readonly unit?: { readonly length?: 'model'; readonly angle?: 'radians' | 'degrees' };
    readonly plane?: PlaneFrame;
    readonly spine: SpatialWire;
    readonly trihedronMode?: SweepTrihedronMode;
    readonly sectionWithContact?: boolean;
    readonly sectionWithCorrection?: boolean;
    readonly solid?: boolean;
    readonly forceApproxC1?: boolean;
    readonly transitionMode?: 'transformed' | 'rightCorner' | 'roundCorner';
    readonly tolerance?: {
        readonly tol3d?: number;
        readonly boundTol?: number;
        readonly angularTol?: number;
    };
    readonly maxDegree?: number;
    readonly maxSegments?: number;
    readonly metadata?: Record<string, unknown>;
}

export interface SweepProfileFeatureParams {
    readonly shape: ShapeHandle;
    readonly profile: Profile;
    readonly spec: SweepProfileSpec;
    readonly cut?: boolean;
}

export type LoftSection =
    | { readonly type: 'profile'; readonly profile: Profile; readonly plane?: PlaneFrame }
    | { readonly type: 'wire'; readonly wire: SpatialWire }
    | { readonly type: 'point'; readonly point: Point3 };

export interface LoftSpec {
    readonly schemaVersion: 1;
    readonly allowUnknownFields?: boolean;
    readonly solid?: boolean;
    readonly ruled?: boolean;
    readonly pres3d?: number;
    readonly checkCompatibility?: boolean;
    readonly smoothing?: boolean;
    readonly parametrization?: 'chordLength' | 'centripetal' | 'isoParametric';
    readonly continuity?: 'C0' | 'G1' | 'C1' | 'G2' | 'C2' | 'C3' | 'CN';
    readonly criteriumWeight?: {
        readonly w1: number;
        readonly w2: number;
        readonly w3: number;
    };
    readonly maxDegree?: number;
    readonly mutableInput?: boolean;
    readonly metadata?: Record<string, unknown>;
}

export interface LoftFeatureParams {
    readonly shape: ShapeHandle;
    readonly sections: readonly LoftSection[];
    readonly spec: LoftSpec;
    readonly cut?: boolean;
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

export interface EdgeRef {
    readonly topoId?: number;
    readonly stableHash?: string;
    /** Defaults to `normalized` for evaluation APIs. */
    readonly parameterMode?: 'normalized' | 'native';
    readonly normalized?: boolean;
}

export interface FaceRef {
    readonly topoId?: number;
    readonly stableHash?: string;
    /** Defaults to `normalized` for evaluation APIs. */
    readonly parameterMode?: 'normalized' | 'native';
    readonly normalized?: boolean;
}

export interface RadiusStation {
    readonly t: number;
    readonly radius: number;
}

export type FilletRadiusLaw =
    | { readonly type: 'constant'; readonly radius: number }
    | { readonly type: 'linear'; readonly startRadius: number; readonly endRadius: number };

export interface FilletEdgeSpec {
    readonly edge?: EdgeRef;
    readonly edgeRef?: EdgeRef;
    readonly topoId?: number;
    readonly stableHash?: string;
    readonly radiusMode?: 'constant' | 'variable' | 'startEnd' | 'law';
    readonly radius?: number;
    readonly startRadius?: number;
    readonly endRadius?: number;
    readonly stations?: readonly RadiusStation[];
    readonly law?: FilletRadiusLaw;
    readonly tangentPropagation?: boolean;
    readonly limits?: { readonly start: number; readonly end: number; readonly normalized?: boolean };
    readonly cornerMode?: 'rollingBall' | 'setback';
    readonly overflowMode?: 'fail' | 'clamp' | 'heal';
}

export interface FilletSpec {
    readonly schemaVersion: 1;
    readonly allowUnknownFields?: boolean;
    readonly unit?: { readonly length?: 'model'; readonly angle?: 'radians' | 'degrees' };
    readonly edges?: readonly FilletEdgeSpec[];
    readonly radiusMode?: 'constant' | 'variable' | 'startEnd' | 'law';
    readonly radius?: number;
    readonly startRadius?: number;
    readonly endRadius?: number;
    readonly stations?: readonly RadiusStation[];
    readonly law?: FilletRadiusLaw;
    readonly tangentPropagation?: boolean;
    readonly limits?: { readonly start: number; readonly end: number; readonly normalized?: boolean };
    readonly cornerMode?: 'rollingBall' | 'setback';
    readonly blendShape?: 'rational' | 'quasiAngular' | 'polynomial';
    readonly continuity?: 'C0' | 'C1' | 'C2' | 'G0' | 'G1' | 'G2';
    readonly angularTolerance?: number;
    readonly overflowMode?: 'fail' | 'clamp' | 'heal';
    readonly metadata?: Record<string, unknown>;
}

export interface ChamferEdgeSpec {
    readonly edge?: EdgeRef;
    readonly edgeRef?: EdgeRef;
    readonly topoId?: number;
    readonly stableHash?: string;
    readonly mode?: 'symmetric' | 'twoDistance' | 'distanceAngle';
    readonly distance?: number;
    readonly distance1?: number;
    readonly distance2?: number;
    readonly angleRadians?: number;
    readonly angleDegrees?: number;
    readonly referenceFace?: FaceRef;
    readonly tangentPropagation?: boolean;
    readonly limits?: { readonly start: number; readonly end: number; readonly normalized?: boolean };
    readonly cornerMode?: 'rollingBall' | 'setback';
    readonly overflowMode?: 'fail' | 'clamp' | 'heal';
}

export interface ChamferSpec {
    readonly schemaVersion: 1;
    readonly allowUnknownFields?: boolean;
    readonly unit?: { readonly length?: 'model'; readonly angle?: 'radians' | 'degrees' };
    readonly edges?: readonly ChamferEdgeSpec[];
    readonly mode?: 'symmetric' | 'twoDistance' | 'distanceAngle';
    readonly distance?: number;
    readonly distance1?: number;
    readonly distance2?: number;
    readonly angleRadians?: number;
    readonly angleDegrees?: number;
    readonly referenceFace?: FaceRef;
    readonly tangentPropagation?: boolean;
    readonly limits?: { readonly start: number; readonly end: number; readonly normalized?: boolean };
    readonly cornerMode?: 'rollingBall' | 'setback';
    readonly overflowMode?: 'fail' | 'clamp' | 'heal';
    readonly metadata?: Record<string, unknown>;
}

export interface FilletFeatureParams {
    readonly shape: ShapeHandle;
    readonly spec: FilletSpec;
}

export interface ChamferFeatureParams {
    readonly shape: ShapeHandle;
    readonly spec: ChamferSpec;
}

export interface BlendOutputFaceRef {
    readonly stableHash?: string;
    readonly topoFaceId?: number;
}

export interface BlendFaceResult {
    readonly kind: 'filletFace' | 'chamferFace';
    readonly stableHash: string | null;
    readonly topoFaceId?: number;
    readonly finalOutputFaceRef?: BlendOutputFaceRef;
    readonly finalOutputFaceRefs?: readonly BlendOutputFaceRef[];
    readonly sourceEdge: EdgeRef;
    readonly tangentChainEdgeRefs: readonly EdgeRef[];
    readonly usedParameters: Record<string, unknown>;
    readonly supportingFaceIds: readonly number[];
    readonly terminalCapIds: readonly number[];
    readonly terminalCondition: 'unresolved' | 'open' | 'closed' | 'capped';
}

export interface BlendOperationResult {
    readonly shape: ShapeHandle;
    readonly shapeId: number;
    readonly revision: RevisionInfo;
    readonly topology: TopologyResult;
    readonly lineage: {
        readonly generated: readonly BlendFaceResult[];
        readonly modified: readonly unknown[];
        readonly retained: readonly unknown[];
        readonly deleted: readonly string[];
    };
    readonly blendFaces: readonly BlendFaceResult[];
    readonly status: {
        readonly isPartial: boolean;
        readonly isClipped: boolean;
        readonly isHealed: boolean;
        readonly isExact: boolean;
    };
}

export interface EdgeEvaluationParams {
    readonly shape: ShapeHandle;
    readonly edge: EdgeRef;
    /** Normalized [0, 1] unless `edge.parameterMode` is `native`. */
    readonly t: number;
}

export interface EdgeEvaluationResult {
    readonly edge: EdgeRef;
    readonly curveType: string;
    readonly parameter: number;
    readonly normalizedParameter: number;
    readonly domain: { readonly first: number; readonly last: number };
    readonly point: Point3;
    readonly tangent: Vector3 | null;
}

export interface SampleEdgeParams {
    readonly shape: ShapeHandle;
    readonly edge: EdgeRef;
    readonly count?: number;
    readonly start?: number;
    readonly end?: number;
    readonly normalized?: boolean;
    readonly includeTangents?: boolean;
}

export interface EdgeSample {
    readonly parameter: number;
    readonly normalizedParameter: number;
    readonly point: Point3;
    readonly tangent?: Vector3 | null;
}

export interface EdgeSampleResult {
    readonly edge: EdgeRef;
    readonly curveType: string;
    readonly domain: { readonly first: number; readonly last: number };
    readonly samples: readonly EdgeSample[];
}

export interface EdgeCurveResult {
    readonly edge: EdgeRef;
    readonly curveType: string;
    readonly domain: { readonly first: number; readonly last: number };
    readonly startPoint: Point3;
    readonly endPoint: Point3;
    readonly line?: { readonly origin: Point3; readonly direction: Vector3 };
    readonly circle?: { readonly center: Point3; readonly radius: number; readonly normal: Vector3 };
    readonly bspline?: {
        readonly degree: number;
        readonly periodic: boolean;
        readonly poles: readonly Point3[];
        readonly weights: readonly number[];
        readonly knots: readonly number[];
        readonly multiplicities: readonly number[];
    };
    readonly bezier?: {
        readonly degree: number;
        readonly poles: readonly Point3[];
        readonly weights: readonly number[];
    };
}

export interface FaceEvaluationParams {
    readonly shape: ShapeHandle;
    readonly face: FaceRef;
    /** Normalized [0, 1] unless `face.parameterMode` is `native`. */
    readonly u: number;
    /** Normalized [0, 1] unless `face.parameterMode` is `native`. */
    readonly v: number;
}

export interface FaceEvaluationResult {
    readonly face: FaceRef;
    readonly surfaceType: string;
    readonly uv: readonly [number, number];
    readonly normalizedUv: readonly [number, number];
    readonly domain: { readonly u: readonly [number, number]; readonly v: readonly [number, number] };
    readonly point: Point3;
    readonly dU: Vector3;
    readonly dV: Vector3;
    readonly normal: Vector3 | null;
}

export interface OperationSchema {
    readonly schemaVersion: number;
    readonly operations: Record<string, unknown>;
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
    /** One explicit geometric normal per triangle, flat [nx, ny, nz, ...]. */
    readonly triangleNormals?: Float32Array;
    /** Runtime topological face id for each triangle. */
    readonly triangleTopoFaceIds?: Uint32Array;
    /** Face grouping id for each triangle. Currently mirrors `triangleTopoFaceIds`. */
    readonly triangleFaceGroups?: Uint32Array;
    /** Face stable hash for each triangle. */
    readonly triangleStableHashes?: readonly string[];
    /** Sanitized selectable CAD feature-edge chains. */
    readonly featureEdges?: readonly FeatureEdgeChain[];
    /** Debug-only raw tessellation/topological edge point dump. Do not use for selection. */
    readonly rawEdgeSegments?: Float32Array;
}

export interface FeatureEdgeChain {
    readonly points: readonly Point3[];
    readonly isClosed: boolean;
    readonly chainId: number;
    readonly faceIndices: readonly number[];
    readonly topoFaceIds: readonly number[];
    readonly isBoundary: boolean;
    readonly isSharp: boolean;
    readonly isSeam?: boolean;
    readonly stableHash?: string;
}

// ---------------------------------------------------------------------------
// Topology query
// ---------------------------------------------------------------------------

export interface TopologyResult {
    readonly revisionId?: string;
    readonly operationId?: string | null;
    readonly sourceFeatureId?: string | null;
    readonly operationType?: string;
    readonly operandRevisionIds?: readonly string[];
    readonly parameterHash?: string | null;
    readonly topologyHash?: string;
    readonly historySchemaVersion?: number;
    readonly createdFromCheckpoint?: boolean;
    readonly shapeType?: string;
    readonly solidCount?: number;
    readonly shellCount?: number;
    readonly wireCount?: number;
    readonly faceCount: number;
    readonly edgeCount: number;
    readonly vertexCount: number;
    /** Bounding box in model coordinates. */
    readonly boundingBox: BoundingBox;
    /** True if the shape is geometrically and topologically valid. */
    readonly isValid: boolean;
    readonly solids?: readonly TopologySolid[];
    readonly shells?: readonly TopologyShell[];
    readonly wires?: readonly TopologyWire[];
    readonly faces?: readonly TopologyFace[];
    readonly edges?: readonly TopologyEdge[];
    readonly vertices?: readonly TopologyVertex[];
    readonly deletedEntities?: readonly TopologyDeletedEntity[];
}

export interface TopologySolid {
    readonly id: number;
    readonly shellIds?: readonly number[];
    readonly status?: TopologyEntityStatus;
}

export interface TopologyShell {
    readonly id: number;
    readonly solidIds?: readonly number[];
    readonly faceIds?: readonly number[];
    readonly status?: TopologyEntityStatus;
}

export interface TopologyWire {
    readonly id: number;
    readonly edgeIds?: readonly number[];
    readonly topoFaceIds?: readonly number[];
    readonly status?: TopologyEntityStatus;
}

export type TopologyEntityStatus = 'generated' | 'modified' | 'retained' | 'deleted' | 'unresolved';
export type RevisionIdentityStatus = 'generated' | 'retained' | 'resolved' | 'unresolved';

export interface TopologyFace {
    readonly id: number;
    readonly stableHash?: string;
    readonly role?: string;
    readonly sourceFeatureId?: string | null;
    readonly generatedFrom?: readonly (string | number)[];
    readonly modifiedFrom?: readonly (string | number)[];
    readonly retainedFrom?: readonly (string | number)[];
    readonly status?: TopologyEntityStatus;
    readonly shared?: {
        readonly sourceFeatureId?: string;
    };
}

export interface TopologyEdge {
    readonly id: number;
    readonly stableHash?: string;
    readonly topoFaceIds?: readonly number[];
    readonly generatedFrom?: readonly (string | number)[];
    readonly modifiedFrom?: readonly (string | number)[];
    readonly retainedFrom?: readonly (string | number)[];
    readonly status?: TopologyEntityStatus;
}

export interface TopologyVertex {
    readonly id: number;
    readonly stableHash?: string;
    readonly status?: TopologyEntityStatus;
}

export interface TopologyDeletedEntity {
    readonly kind: 'face' | 'edge' | 'vertex';
    readonly stableHash: string;
    readonly deletedBy?: string;
    readonly status?: TopologyEntityStatus;
}

export interface RevisionInfo {
    readonly revisionId: string;
    readonly operationId: string | null;
    readonly sourceFeatureId: string | null;
    readonly operationType: string;
    readonly operandRevisionIds: readonly string[];
    readonly parameterHash: string | null;
    readonly topologyHash: string;
    readonly historySchemaVersion: number;
    readonly createdFromCheckpoint: boolean;
    readonly entityStatus: TopologyEntityStatus;
    readonly identityStatus: RevisionIdentityStatus;
    readonly historyWarnings: readonly string[];
    readonly deletedEntities: readonly TopologyDeletedEntity[];
    readonly faceStableHashes?: readonly string[];
    readonly edgeStableHashes?: readonly string[];
    readonly vertexStableHashes?: readonly string[];
}

export interface ResolveStableEntityParams {
    readonly shape: ShapeHandle;
    readonly stableHash: string;
}

export interface StableEntityResolution {
    readonly found: boolean;
    readonly status: 'active' | 'deleted' | 'unresolved';
    readonly kind?: 'face' | 'edge' | 'vertex';
    readonly id?: number;
    readonly stableHash: string;
    readonly revisionId: string;
    readonly deletedBy?: string;
    readonly message?: string;
}

export interface MapEntitiesAcrossRevisionsParams {
    readonly fromRevisionId: string;
    readonly toRevisionId: string;
    readonly stableHashes: readonly string[];
}

export interface EntityRevisionMapping {
    readonly stableHash: string;
    readonly status: 'mapped' | 'deleted' | 'unresolved' | 'missing';
    readonly mappedStableHash: string | null;
    readonly message?: string;
}

export interface EntityRevisionMapResult {
    readonly fromRevisionId: string;
    readonly toRevisionId: string;
    readonly mappings: readonly EntityRevisionMapping[];
}

export interface CreateCheckpointParams {
    readonly shape: ShapeHandle;
}

export interface RevisionCheckpoint {
    readonly checkpointSchemaVersion: number;
    readonly brep: string;
    readonly revision: RevisionInfo;
}

export interface HydrateCheckpointParams {
    readonly checkpoint: RevisionCheckpoint | string;
}

export interface RetainRevisionParams {
    readonly shape: ShapeHandle;
}

export interface ReleaseRevisionParams {
    readonly shape: ShapeHandle;
}

export interface BoundingBox {
    readonly xMin: number;
    readonly yMin: number;
    readonly zMin: number;
    readonly xMax: number;
    readonly yMax: number;
    readonly zMax: number;
}

export interface KernelCapabilities {
    readonly featureEdgesV1: boolean;
    readonly rawEdgeSegmentsV1: boolean;
    readonly triangleNormalsV1: boolean;
    readonly triangleFaceMappingV1: boolean;
    readonly topologySubshapesV1: boolean;
    readonly topologyHierarchyV1?: boolean;
    readonly geometricStableHashesV1: boolean;
    readonly revisionInfoV1: boolean;
    readonly entityResolutionV1: boolean;
    readonly entityRemapV1: boolean;
    readonly revisionRetentionV1: boolean;
    readonly historyV1: boolean;
    readonly stableNamingV1: boolean;
    readonly checkpointV1: boolean;
    readonly versionInfoV1?: boolean;
    readonly analysisV1?: boolean;
    readonly sessionHandlesV1?: boolean;
    readonly operations?: {
        readonly structuredSpecsV1?: boolean;
        readonly operationSchemaV1?: boolean;
        readonly nativeExactBlendOpsV1?: boolean;
        readonly exactSubshapeEvaluationV1?: boolean;
    };
    readonly extrudeProfile?: {
        readonly schemaVersion: number;
        readonly nativeExact: boolean;
        readonly direction: boolean;
        readonly draft: boolean;
        readonly plane: boolean;
        readonly reverseDirection: boolean;
        readonly endConditions: readonly string[];
        readonly surfaceTarget: boolean;
        readonly curvedSurfaceTarget: boolean;
    };
    readonly extrudeCutProfile?: {
        readonly schemaVersion: number;
        readonly nativeExact: boolean;
        readonly direction: boolean;
        readonly draft: boolean;
        readonly plane: boolean;
        readonly reverseDirection: boolean;
        readonly endConditions: readonly string[];
        readonly surfaceTarget: boolean;
        readonly curvedSurfaceTarget: boolean;
    };
    readonly revolveProfile?: {
        readonly schemaVersion: number;
        readonly nativeExact: boolean;
        readonly plane: boolean;
        readonly axis: boolean;
        readonly reverseDirection: boolean;
        readonly signedAngle: boolean;
        readonly endConditions: readonly string[];
        readonly surfaceTarget: boolean;
        readonly curvedSurfaceTarget: boolean;
        readonly slidingEdges: boolean;
    };
    readonly revolveCutProfile?: {
        readonly schemaVersion: number;
        readonly nativeExact: boolean;
        readonly plane: boolean;
        readonly axis: boolean;
        readonly reverseDirection: boolean;
        readonly signedAngle: boolean;
        readonly endConditions: readonly string[];
        readonly surfaceTarget: boolean;
        readonly curvedSurfaceTarget: boolean;
        readonly slidingEdges: boolean;
    };
    readonly sweepProfile?: {
        readonly schemaVersion: number;
        readonly nativeExact: boolean;
        readonly cutBoolean: boolean;
        readonly plane: boolean;
        readonly spine: boolean;
        readonly trihedronModes: readonly string[];
        readonly sectionWithContact: boolean;
        readonly sectionWithCorrection: boolean;
        readonly solid: boolean;
        readonly forceApproxC1: boolean;
        readonly transitionModes: readonly string[];
        readonly tolerances: boolean;
        readonly maxDegree: boolean;
        readonly maxSegments: boolean;
    };
    readonly loft?: {
        readonly schemaVersion: number;
        readonly nativeExact: boolean;
        readonly cutBoolean: boolean;
        readonly sectionKinds: readonly string[];
        readonly solid: boolean;
        readonly ruled: boolean;
        readonly pres3d: boolean;
        readonly checkCompatibility: boolean;
        readonly smoothing: boolean;
        readonly parametrization: readonly string[];
        readonly continuity: readonly string[];
        readonly criteriumWeight: boolean;
        readonly maxDegree: boolean;
        readonly mutableInput: boolean;
    };
    readonly fillet?: {
        readonly schemaVersion: number;
        readonly nativeExact: boolean;
        readonly constantRadius: boolean;
        readonly startEndRadius: boolean;
        readonly stationRadii: boolean;
        readonly lawRadius: readonly string[];
        readonly tangentPropagation: boolean;
        readonly partialEdges: boolean;
        readonly setbackCorners: boolean;
        readonly blendShape: readonly string[];
        readonly continuity: readonly string[];
        readonly overflowModes: readonly string[];
    };
    readonly chamfer?: {
        readonly schemaVersion: number;
        readonly nativeExact: boolean;
        readonly symmetric: boolean;
        readonly twoDistance: boolean;
        readonly distanceAngle: boolean;
        readonly referenceFace: boolean;
        readonly tangentPropagation: boolean;
        readonly partialEdges: boolean;
        readonly setbackCorners: boolean;
        readonly overflowModes: readonly string[];
    };
    readonly subshapeEvaluation?: {
        readonly evaluateEdge: boolean;
        readonly sampleEdge: boolean;
        readonly getEdgeCurve: boolean;
        readonly evaluateFace: boolean;
        readonly parameterModes: readonly string[];
    };
    readonly analysis?: {
        readonly volume: boolean;
        readonly surfaceArea: boolean;
        readonly linearLength: boolean;
        readonly boundingBox: boolean;
        readonly centerOfMass: boolean;
        readonly shapeValidity: boolean;
        readonly pointContainment: boolean;
        readonly shapeIntersection?: boolean;
        readonly closestPoint?: boolean;
        readonly shapeDistance?: boolean;
    };
    readonly runtime?: {
        readonly browser: boolean;
        readonly worker: boolean;
        readonly node: boolean;
    };
}

export interface ShapeAnalysisParams {
    readonly shape: ShapeHandle;
}

export interface ShapeAnalysisResult {
    readonly shapeType: string;
    readonly solidCount: number;
    readonly shellCount: number;
    readonly wireCount: number;
    readonly faceCount: number;
    readonly edgeCount: number;
    readonly vertexCount: number;
    readonly boundingBox: BoundingBox;
    readonly isValid: boolean;
    readonly volume: number;
    readonly surfaceArea: number;
    readonly linearLength: number;
    readonly centerOfMass: Point3 | null;
    readonly centerOfMassBasis: 'volume' | 'surface' | 'linear' | 'none';
}

export interface PointContainmentParams {
    readonly shape: ShapeHandle;
    readonly point: Point3;
    readonly tolerance?: number;
}

export interface PointContainmentResult {
    readonly point: Point3;
    readonly tolerance: number;
    readonly state: 'in' | 'out' | 'on' | 'unknown';
    readonly isInside: boolean;
}

export interface ShapeIntersectionParams {
    readonly shapeA: ShapeHandle;
    readonly shapeB: ShapeHandle;
}

export interface ShapeIntersectionResult {
    readonly hasIntersection: boolean;
    readonly edgeCount: number;
    readonly vertexCount: number;
    readonly sectionShape?: ShapeHandle;
}

export interface ShapeSupportRef {
    readonly kind: 'vertex' | 'edge' | 'face';
    readonly topoId?: number;
    readonly stableHash?: string;
    readonly parameter?: number;
    readonly uv?: Point2;
}

export interface ClosestPointOnShapeParams {
    readonly shape: ShapeHandle;
    readonly point: Point3;
    readonly tolerance?: number;
}

export interface ClosestPointOnShapeResult {
    readonly queryPoint: Point3;
    readonly closestPoint: Point3;
    readonly distance: number;
    readonly solutionCount: number;
    readonly support: ShapeSupportRef;
}

export interface ShapeDistanceParams {
    readonly shapeA: ShapeHandle;
    readonly shapeB: ShapeHandle;
    readonly tolerance?: number;
}

export interface ShapeDistanceSolution {
    readonly pointOnA: Point3;
    readonly pointOnB: Point3;
    readonly supportOnA: ShapeSupportRef;
    readonly supportOnB: ShapeSupportRef;
}

export interface ShapeDistanceResult {
    readonly distance: number;
    readonly clearance: number;
    readonly innerSolution: boolean;
    readonly isInContact: boolean;
    readonly solutionCount: number;
    readonly solutions: readonly ShapeDistanceSolution[];
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

export interface StepImportPackageOptions extends StepImportOptions {
    readonly linearDeflection?: number;
    readonly angularDeflection?: number;
}

export interface ImportStepPackageParams {
    /** Raw content of a STEP file as a UTF-8 string. */
    readonly content: string;
    /** Optional post-processing and tessellation controls. */
    readonly options?: StepImportPackageOptions;
}

export interface ImportStepPackageResult {
    readonly readStatus: StepImportReadStatus;
    readonly transferStatus: StepImportTransferStatus;
    readonly healed: boolean;
    readonly isValid: boolean;
    readonly messageList: readonly StepImportMessage[];
    readonly shape?: ShapeHandle;
    readonly revision?: {
        readonly revisionId: string;
        readonly topologyHash: string;
    };
    readonly topology?: {
        readonly solidCount: number;
        readonly shellCount: number;
        readonly wireCount: number;
        readonly faceCount: number;
        readonly edgeCount: number;
        readonly vertexCount: number;
        readonly isValid: boolean;
    };
    readonly properties?: {
        readonly boundingBox: BoundingBox;
        readonly volume: number;
        readonly surfaceArea: number;
        readonly linearLength: number;
        readonly centerOfMass: Point3 | null;
        readonly centerOfMassBasis: 'volume' | 'surface' | 'linear' | 'none';
    };
    readonly mesh?: TessellationResult;
    readonly checkpoint?: RevisionCheckpoint;
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
