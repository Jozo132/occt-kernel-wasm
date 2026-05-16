/**
 * OcctKernel – the primary public class of occt-kernel-wasm.
 *
 * Wraps the native OCCT WASM module behind a clean, high-level TypeScript API.
 * All shapes are referenced by opaque {@link ShapeHandle} objects. No OCCT types
 * are ever exposed to the caller.
 *
 * Usage:
 * ```ts
 * import { createKernel } from 'occt-kernel-wasm';
 *
 * const kernel = await createKernel();
 * const box = kernel.createBox({ dx: 10, dy: 10, dz: 10 });
 * const mesh = kernel.tessellate({ shape: box });
 * const step = kernel.exportStep({ shape: box });
 * kernel.disposeShape({ shape: box });
 * ```
 */

import { KernelError, parseNativeError } from './errors';
import type {
    BooleanParams,
    BlendOperationResult,
    BoundingBox,
    BoxParams,
    ChamferFeatureParams,
    ChamferParams,
    CreateCheckpointParams,
    CylinderParams,
    DisposeParams,
    EdgeCurveResult,
    EdgeEvaluationParams,
    EdgeEvaluationResult,
    EdgeRef,
    EdgeSampleResult,
    EntityRevisionMapResult,
    ExportStepParams,
    ExtrudeCutProfileFeatureParams,
    ExtrudeParams,
    ExtrudeProfileFeatureParams,
    ExtrudeProfileSpec,
    FaceEvaluationParams,
    FaceEvaluationResult,
    FaceRef,
    FeatureEdgeChain,
    FilletFeatureParams,
    FilletParams,
    ImportStepDetailedResult,
    ImportStepParams,
    KernelCapabilities,
    HydrateCheckpointParams,
    LoftFeatureParams,
    LoftSection,
    LoftSpec,
    MapEntitiesAcrossRevisionsParams,
    PlaneFrame,
    Point2,
    Point3,
    Profile,
    ProfileSegment,
    ProfileWire,
    ReleaseRevisionParams,
    ResolveStableEntityParams,
    RetainRevisionParams,
    RevolveCutProfileFeatureParams,
    RevolveParams,
    RevolveProfileFeatureParams,
    RevolveProfileSpec,
    RevisionCheckpoint,
    RevisionInfo,
    RotationTransform,
    SampleEdgeParams,
    ShapeHandle,
    ShapeTransform,
    SphereParams,
    SpatialCurveSegment,
    SpatialWire,
    StableEntityResolution,
    StepImportMessage,
    StepImportOptions,
    StepImportReadStatus,
    StepImportTransferStatus,
    SweepProfileFeatureParams,
    SweepProfileSpec,
    TessellateParams,
    TessellationResult,
    TopologyResult,
    TransformParams,
    Vector3,
    OperationSchema,
} from './types';

// ---------------------------------------------------------------------------
// Native module interface
// ---------------------------------------------------------------------------

/**
 * Interface of the native OcctKernel C++ class as exposed via Emscripten embind.
 * @internal
 */
export interface NativeKernel {
    createBox(dx: number, dy: number, dz: number): number;
    createCylinder(radius: number, height: number): number;
    createSphere(radius: number): number;
    extrudeProfile(profileJson: string, optionsJson: string): number;
    extrudeProfileWithSpec?: (id: number, profileJson: string, specJson: string) => number;
    extrudeCutProfileWithSpec?: (id: number, profileJson: string, specJson: string) => number;
    revolveProfile(profileJson: string, optionsJson: string): number;
    revolveProfileWithSpec?: (id: number, profileJson: string, specJson: string) => number;
    revolveCutProfileWithSpec?: (id: number, profileJson: string, specJson: string) => number;
    sweepProfileWithSpec?: (id: number, profileJson: string, specJson: string) => number;
    loftWithSpec?: (id: number, sectionsJson: string, specJson: string) => number;
    booleanUnion(id1: number, id2: number): number;
    booleanSubtract(id1: number, id2: number): number;
    booleanIntersect(id1: number, id2: number): number;
    filletEdges(id: number, radius: number): number;
    chamferEdges(id: number, distance: number): number;
    filletEdgesWithSpec?: (id: number, specJson: string) => string;
    chamferEdgesWithSpec?: (id: number, specJson: string) => string;
    transformShape(id: number, transformJson: string): number;
    getTopology(id: number): string;
    getRevisionInfo?: (id: number) => string;
    resolveStableEntity?: (id: number, stableHash: string) => string;
    mapEntitiesAcrossRevisions?: (fromRevisionId: string, toRevisionId: string, stableHashesJson: string) => string;
    evaluateEdge?: (id: number, edgeRefJson: string, t: number) => string;
    sampleEdge?: (id: number, edgeRefJson: string, optionsJson: string) => string;
    getEdgeCurve?: (id: number, edgeRefJson: string) => string;
    evaluateFace?: (id: number, faceRefJson: string, u: number, v: number) => string;
    getOperationSchema?: () => string;
    getCapabilities?: () => string;
    checkValidity(id: number): boolean;
    tessellate(id: number, linearDeflection: number, angularDeflection: number): string;
    importStep(content: string): number;
    importStepDetailed(
        content: string,
        heal: boolean,
        sew: boolean,
        fixSameParameter: boolean,
        fixSolid: boolean,
        sewingTolerance: number,
    ): string;
    exportStep(id: number): string;
    createCheckpoint?: (id: number) => string;
    hydrateCheckpoint?: (checkpointJson: string) => number;
    disposeShape(id: number): void;
    retainRevision?: (id: number) => void;
    releaseRevision?: (id: number) => boolean;
}

export interface WasmModule {
    OcctKernel: new () => NativeKernel;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Make a {@link ShapeHandle} from a raw integer. */
function makeHandle(id: number): ShapeHandle {
    return Object.freeze({ id });
}

/**
 * Wrap a native call, converting any thrown value (native C++ exception
 * or KernelError from the mock) to a {@link KernelError}.
 */
function wrap<T>(fn: () => T): T {
    try {
        return fn();
    } catch (err) {
        throw parseNativeError(err);
    }
}

/**
 * Parse a JSON string returned by the native layer.
 * Throws {@link KernelError} with code `OPERATION_FAILED` on failure.
 */
function parseJson<T>(raw: string, context: string): T {
    try {
        return JSON.parse(raw) as T;
    } catch {
        throw new KernelError('OPERATION_FAILED', `Failed to parse ${context} result: ${raw}`);
    }
}

/** Assert that a value is a positive finite number. */
function requirePositive(value: number, name: string): void {
    if (!Number.isFinite(value) || value <= 0) {
        throw new KernelError('INVALID_PARAMS', `${name} must be a positive finite number`);
    }
}

function requireFinite(value: number, name: string): void {
    if (!Number.isFinite(value)) {
        throw new KernelError('INVALID_PARAMS', `${name} must be a finite number`);
    }
}

function requirePoint2(value: Point2, name: string): void {
    if (!Array.isArray(value) || value.length !== 2) {
        throw new KernelError('INVALID_PARAMS', `${name} must be a 2-element array`);
    }
    requireFinite(value[0], `${name}[0]`);
    requireFinite(value[1], `${name}[1]`);
}

function requirePoint3(value: Point3, name: string): void {
    if (!Array.isArray(value) || value.length !== 3) {
        throw new KernelError('INVALID_PARAMS', `${name} must be a 3-element array`);
    }
    requireFinite(value[0], `${name}[0]`);
    requireFinite(value[1], `${name}[1]`);
    requireFinite(value[2], `${name}[2]`);
}

function requirePositiveInteger(value: number, name: string): void {
    if (!Number.isInteger(value) || value <= 0) {
        throw new KernelError('INVALID_PARAMS', `${name} must be a positive integer`);
    }
}

function requireSchemaVersion(value: { readonly schemaVersion?: number }, name: string): void {
    if (value.schemaVersion !== 1) {
        throw new KernelError('INVALID_PARAMS', `${name}.schemaVersion must be 1`);
    }
}

function throwStructuredInvalid(operation: string, path: string, reason: string, unsupportedFeature?: string): never {
    throw new KernelError('INVALID_PARAMS', JSON.stringify({
        phase: 'validation',
        operation,
        path,
        reason,
        ...(unsupportedFeature !== undefined ? { unsupportedFeature } : {}),
    }));
}

function requireSupportedBlendControls(
    operation: 'filletEdges' | 'chamferEdges',
    path: string,
    value: {
        readonly limits?: unknown;
        readonly tangentPropagation?: boolean;
        readonly cornerMode?: string;
        readonly overflowMode?: string;
    },
): void {
    const family = operation === 'filletEdges' ? 'fillet' : 'chamfer';
    if (value.limits !== undefined) {
        throwStructuredInvalid(operation, `${path}.limits`, 'Partial-edge blends are not exposed by this OCCT build', `${family}.partialEdge`);
    }
    if (value.tangentPropagation === false) {
        throwStructuredInvalid(operation, `${path}.tangentPropagation`, 'Disabling tangent propagation is not exposed by this OCCT build', `${family}.nonPropagatingEdges`);
    }
    if (value.cornerMode !== undefined && value.cornerMode !== 'rollingBall') {
        throwStructuredInvalid(operation, `${path}.cornerMode`, 'Only rollingBall corner handling is supported by this OCCT build', `${family}.cornerModes`);
    }
    if (value.overflowMode !== undefined && value.overflowMode !== 'fail') {
        throwStructuredInvalid(operation, `${path}.overflowMode`, 'Only fail-fast overflow handling is supported by this OCCT build', `${family}.overflowModes`);
    }
}

function requireEdgeRef(value: EdgeRef, name: string): void {
    if (value.topoId === undefined && value.stableHash === undefined) {
        throw new KernelError('INVALID_PARAMS', `${name} must include topoId or stableHash`);
    }
    if (value.topoId !== undefined) {
        requirePositiveInteger(value.topoId, `${name}.topoId`);
    }
    if (value.stableHash !== undefined && value.stableHash.length === 0) {
        throw new KernelError('INVALID_PARAMS', `${name}.stableHash must be non-empty`);
    }
}

function requireFaceRef(value: FaceRef, name: string): void {
    requireEdgeRef(value, name);
}

function withShapeHandle(result: Omit<BlendOperationResult, 'shape'>): BlendOperationResult {
    return {
        ...result,
        shape: makeHandle(result.shapeId),
    };
}

function requirePoint2Array(values: readonly Point2[], name: string, minimumLength: number): void {
    if (!Array.isArray(values) || values.length < minimumLength) {
        throw new KernelError('INVALID_PARAMS', `${name} must contain at least ${minimumLength} points`);
    }
    values.forEach((value, index) => requirePoint2(value, `${name}[${index}]`));
}

function requirePoint3Array(values: readonly Point3[], name: string, minimumLength: number): void {
    if (!Array.isArray(values) || values.length < minimumLength) {
        throw new KernelError('INVALID_PARAMS', `${name} must contain at least ${minimumLength} points`);
    }
    values.forEach((value, index) => requirePoint3(value, `${name}[${index}]`));
}

function requireFiniteNumberArray(values: readonly number[], name: string, minimumLength: number): void {
    if (!Array.isArray(values) || values.length < minimumLength) {
        throw new KernelError('INVALID_PARAMS', `${name} must contain at least ${minimumLength} values`);
    }
    values.forEach((value, index) => requireFinite(value, `${name}[${index}]`));
}

function requireStrictlyIncreasing(values: readonly number[], name: string): void {
    for (let index = 1; index < values.length; index += 1) {
        if (values[index] <= values[index - 1]) {
            throw new KernelError('INVALID_PARAMS', `${name} must be strictly increasing`);
        }
    }
}

function requireVector3(value: Vector3, name: string, allowZero = false): void {
    requirePoint3(value, name);
    if (!allowZero && value[0] === 0 && value[1] === 0 && value[2] === 0) {
        throw new KernelError('INVALID_PARAMS', `${name} must not be the zero vector`);
    }
}

function requireNonParallelVectors(a: Vector3, b: Vector3, aName: string, bName: string): void {
    const crossX = a[1] * b[2] - a[2] * b[1];
    const crossY = a[2] * b[0] - a[0] * b[2];
    const crossZ = a[0] * b[1] - a[1] * b[0];
    if (crossX === 0 && crossY === 0 && crossZ === 0) {
        throw new KernelError('INVALID_PARAMS', `${aName} must not be parallel to ${bName}`);
    }
}

// ---------------------------------------------------------------------------
// Raw tessellation JSON from native layer
// ---------------------------------------------------------------------------

interface RawTessellation {
    positions: number[];
    normals: number[];
    indices: number[];
    triangleNormals?: number[];
    triangleTopoFaceIds?: number[];
    triangleFaceGroups?: number[];
    triangleStableHashes?: string[];
    featureEdges?: FeatureEdgeChain[];
    rawEdgeSegments?: number[];
}

interface RawTopology {
    revisionId?: string;
    operationId?: string | null;
    sourceFeatureId?: string | null;
    operationType?: string;
    operandRevisionIds?: string[];
    parameterHash?: string | null;
    topologyHash?: string;
    historySchemaVersion?: number;
    createdFromCheckpoint?: boolean;
    faceCount: number;
    edgeCount: number;
    vertexCount: number;
    boundingBox: BoundingBox;
    isValid: boolean;
}

const fallbackCapabilities: KernelCapabilities = {
    featureEdgesV1: false,
    rawEdgeSegmentsV1: false,
    triangleNormalsV1: false,
    triangleFaceMappingV1: false,
    topologySubshapesV1: false,
    geometricStableHashesV1: false,
    revisionInfoV1: false,
    entityResolutionV1: false,
    entityRemapV1: false,
    revisionRetentionV1: false,
    historyV1: false,
    stableNamingV1: false,
    checkpointV1: false,
};

interface RawStepImportMessage extends StepImportMessage {}

interface RawStepImportDetailedResult {
    readStatus: StepImportReadStatus;
    transferStatus: StepImportTransferStatus;
    rootCount: number;
    transferredRootCount: number;
    messageList: RawStepImportMessage[];
    shapeId?: number;
    isValid: boolean;
    wasValidBeforeHealing: boolean;
    healed: boolean;
}

interface NormalizedStepImportOptions {
    heal: boolean;
    sew: boolean;
    fixSameParameter: boolean;
    fixSolid: boolean;
    sewingTolerance: number;
}

function normalizeImportOptions(options?: StepImportOptions): NormalizedStepImportOptions {
    const sewingTolerance = options?.sewingTolerance ?? 1e-6;
    requirePositive(sewingTolerance, 'sewingTolerance');

    return {
        heal: options?.heal ?? false,
        sew: options?.sew ?? false,
        fixSameParameter: options?.fixSameParameter ?? false,
        fixSolid: options?.fixSolid ?? false,
        sewingTolerance,
    };
}

function formatImportFailure(result: ImportStepDetailedResult): string {
    const firstFailure = result.messageList.find((message) => message.severity === 'fail');
    if (firstFailure) {
        return firstFailure.text;
    }

    const firstWarning = result.messageList.find((message) => message.severity === 'warning');
    if (firstWarning) {
        return firstWarning.text;
    }

    return `STEP import failed (${result.readStatus}/${result.transferStatus})`;
}

function toImportStepDetailedResult(raw: RawStepImportDetailedResult): ImportStepDetailedResult {
    return {
        readStatus: raw.readStatus,
        transferStatus: raw.transferStatus,
        rootCount: raw.rootCount,
        transferredRootCount: raw.transferredRootCount,
        messageList: raw.messageList,
        ...(raw.shapeId !== undefined ? { shape: makeHandle(raw.shapeId) } : {}),
        isValid: raw.isValid,
        wasValidBeforeHealing: raw.wasValidBeforeHealing,
        healed: raw.healed,
    };
}

interface CanonicalProfileWire {
    segments: ProfileSegment[];
}

interface CanonicalProfile {
    wires: CanonicalProfileWire[];
}

interface CanonicalSpatialWire {
    segments: SpatialCurveSegment[];
}

interface ExtrudeOptionsPayload {
    height?: number;
    vector?: Vector3;
    plane?: PlaneFrame;
}

interface ExtrudeProfileSpecPayload {
    schemaVersion: 1;
    allowUnknownFields?: boolean;
    unit?: { length?: 'model'; angle?: 'radians' | 'degrees' };
    plane?: PlaneFrame;
    direction?: Vector3;
    reverseDirection?: boolean;
    draftAngleRadians?: number;
    extent: { type: 'blind'; distance: number }
        | { type: 'upToNext' }
        | { type: 'throughAll' }
        | { type: 'upToSurface'; surface: { shapeId?: number; face: FaceRef } }
        | { type: 'offsetFromSurface'; surface: { shapeId?: number; face: FaceRef }; offset: number };
    metadata?: Record<string, unknown>;
}

interface RevolveOptionsPayload {
    angleDegrees: number;
    axisOrigin?: Point3;
    axisDirection?: Vector3;
}

interface RevolveProfileSpecPayload {
    schemaVersion: 1;
    allowUnknownFields?: boolean;
    unit?: { length?: 'model'; angle?: 'radians' | 'degrees' };
    plane?: PlaneFrame;
    axisOrigin?: Point3;
    axisDirection?: Vector3;
    reverseDirection?: boolean;
    slidingEdges?: Array<{ profileEdgeIndex: number; face: FaceRef }>;
    extent:
        | { type: 'angle'; angleRadians: number }
        | { type: 'upToSurface'; surface: { shapeId?: number; face: FaceRef } }
        | { type: 'fromSurfaceToSurface'; fromSurface: { shapeId?: number; face: FaceRef }; untilSurface: { shapeId?: number; face: FaceRef } }
        | { type: 'throughAll' }
        | { type: 'upToSurfaceAtAngle'; surface: { shapeId?: number; face: FaceRef }; angleRadians: number };
    metadata?: Record<string, unknown>;
}

interface SweepTrihedronModePayload {
    type: 'correctedFrenet' | 'frenet' | 'discrete' | 'fixedTrihedron' | 'fixedBinormal' | 'auxiliarySpine';
    frame?: PlaneFrame;
    binormal?: Vector3;
    spineJson?: string;
    curvilinearEquivalence?: boolean;
    contact?: 'none' | 'contact' | 'contactOnBorder';
}

interface SweepProfileSpecPayload {
    schemaVersion: 1;
    allowUnknownFields?: boolean;
    unit?: { length?: 'model'; angle?: 'radians' | 'degrees' };
    plane?: PlaneFrame;
    spineJson: string;
    trihedronMode?: SweepTrihedronModePayload;
    sectionWithContact?: boolean;
    sectionWithCorrection?: boolean;
    solid?: boolean;
    forceApproxC1?: boolean;
    transitionMode?: 'transformed' | 'rightCorner' | 'roundCorner';
    tolerance?: { tol3d?: number; boundTol?: number; angularTol?: number };
    maxDegree?: number;
    maxSegments?: number;
    cut?: boolean;
    metadata?: Record<string, unknown>;
}

type LoftSectionPayload =
    | { type: 'profile'; profileJson: string; plane?: PlaneFrame }
    | { type: 'wire'; wireJson: string }
    | { type: 'point'; point: Point3 };

interface LoftSpecPayload {
    schemaVersion: 1;
    allowUnknownFields?: boolean;
    solid?: boolean;
    ruled?: boolean;
    pres3d?: number;
    checkCompatibility?: boolean;
    smoothing?: boolean;
    parametrization?: 'chordLength' | 'centripetal' | 'isoParametric';
    continuity?: 'C0' | 'G1' | 'C1' | 'G2' | 'C2' | 'C3' | 'CN';
    criteriumWeight?: { w1: number; w2: number; w3: number };
    maxDegree?: number;
    mutableInput?: boolean;
    cut?: boolean;
    metadata?: Record<string, unknown>;
}

function validateProfileSegment(segment: ProfileSegment, indexLabel: string): void {
    switch (segment.type) {
        case 'line':
            requirePoint2(segment.start, `${indexLabel}.start`);
            requirePoint2(segment.end, `${indexLabel}.end`);
            break;
        case 'arc':
            requirePoint2(segment.start, `${indexLabel}.start`);
            requirePoint2(segment.mid, `${indexLabel}.mid`);
            requirePoint2(segment.end, `${indexLabel}.end`);
            break;
        case 'circle':
            requirePoint2(segment.centre, `${indexLabel}.centre`);
            requirePositive(segment.radius, `${indexLabel}.radius`);
            break;
        case 'bezier':
            requirePoint2Array(segment.controlPoints, `${indexLabel}.controlPoints`, 2);
            break;
        case 'bspline': {
            requirePoint2Array(segment.controlPoints, `${indexLabel}.controlPoints`, 2);
            requirePositiveInteger(segment.degree, `${indexLabel}.degree`);
            requireFiniteNumberArray(segment.knots, `${indexLabel}.knots`, 2);
            requireStrictlyIncreasing(segment.knots, `${indexLabel}.knots`);
            if (!Array.isArray(segment.multiplicities) || segment.multiplicities.length !== segment.knots.length) {
                throw new KernelError('INVALID_PARAMS', `${indexLabel}.multiplicities must match ${indexLabel}.knots in length`);
            }
            segment.multiplicities.forEach((value, index) => requirePositiveInteger(value, `${indexLabel}.multiplicities[${index}]`));

            const multiplicitySum = segment.multiplicities.reduce((sum, value) => sum + value, 0);
            if (multiplicitySum - segment.degree - 1 !== segment.controlPoints.length) {
                throw new KernelError('INVALID_PARAMS', `${indexLabel} has inconsistent controlPoints, degree, and multiplicities`);
            }
            break;
        }
        default:
            throw new KernelError('INVALID_PARAMS', `${indexLabel}.type is not supported`);
    }
}

function validateSpatialSegment(segment: SpatialCurveSegment, indexLabel: string): void {
    switch (segment.type) {
        case 'line':
            requirePoint3(segment.start, `${indexLabel}.start`);
            requirePoint3(segment.end, `${indexLabel}.end`);
            break;
        case 'arc':
            requirePoint3(segment.start, `${indexLabel}.start`);
            requirePoint3(segment.mid, `${indexLabel}.mid`);
            requirePoint3(segment.end, `${indexLabel}.end`);
            break;
        case 'circle':
            requirePoint3(segment.center, `${indexLabel}.center`);
            requireVector3(segment.normal, `${indexLabel}.normal`);
            requirePositive(segment.radius, `${indexLabel}.radius`);
            if (segment.xDirection !== undefined) {
                requireVector3(segment.xDirection, `${indexLabel}.xDirection`);
                requireNonParallelVectors(segment.normal, segment.xDirection, `${indexLabel}.normal`, `${indexLabel}.xDirection`);
            }
            break;
        case 'bezier':
            requirePoint3Array(segment.controlPoints, `${indexLabel}.controlPoints`, 2);
            break;
        case 'bspline': {
            requirePoint3Array(segment.controlPoints, `${indexLabel}.controlPoints`, 2);
            requirePositiveInteger(segment.degree, `${indexLabel}.degree`);
            requireFiniteNumberArray(segment.knots, `${indexLabel}.knots`, 2);
            requireStrictlyIncreasing(segment.knots, `${indexLabel}.knots`);
            if (!Array.isArray(segment.multiplicities) || segment.multiplicities.length !== segment.knots.length) {
                throw new KernelError('INVALID_PARAMS', `${indexLabel}.multiplicities must match ${indexLabel}.knots in length`);
            }
            segment.multiplicities.forEach((value, index) => requirePositiveInteger(value, `${indexLabel}.multiplicities[${index}]`));

            const multiplicitySum = segment.multiplicities.reduce((sum, value) => sum + value, 0);
            if (multiplicitySum - segment.degree - 1 !== segment.controlPoints.length) {
                throw new KernelError('INVALID_PARAMS', `${indexLabel} has inconsistent controlPoints, degree, and multiplicities`);
            }
            break;
        }
        default:
            throw new KernelError('INVALID_PARAMS', `${indexLabel}.type is not supported`);
    }
}

function normalizeWire(wire: ProfileWire, index: number): CanonicalProfileWire {
    if (!wire || !Array.isArray(wire.segments) || wire.segments.length === 0) {
        throw new KernelError('INVALID_PARAMS', `profile wire ${index} must have at least one segment`);
    }
    wire.segments.forEach((segment, segmentIndex) => validateProfileSegment(segment, `profile.wires[${index}].segments[${segmentIndex}]`));
    return { segments: [...wire.segments] };
}

function normalizeProfile(profile: Profile): CanonicalProfile {
    const wires = profile.wires
        ?? (profile.outer ? [profile.outer, ...(profile.holes ?? [])] : undefined)
        ?? (profile.segments ? [{ segments: profile.segments }] : undefined);

    if (!wires || wires.length === 0) {
        throw new KernelError('INVALID_PARAMS', "Profile must include 'segments', 'outer', or 'wires'");
    }

    return {
        wires: wires.map((wire, index) => normalizeWire(wire, index)),
    };
}

function normalizeSpatialWire(wire: SpatialWire, path: string): CanonicalSpatialWire {
    if (!wire || !Array.isArray(wire.segments) || wire.segments.length === 0) {
        throw new KernelError('INVALID_PARAMS', `${path} must have at least one segment`);
    }
    wire.segments.forEach((segment, segmentIndex) => validateSpatialSegment(segment, `${path}.segments[${segmentIndex}]`));
    return {
        segments: [...wire.segments],
    };
}

function requireSingleWireProfile(profile: CanonicalProfile, path: string): CanonicalProfile {
    if (profile.wires.length !== 1) {
        throw new KernelError('INVALID_PARAMS', `${path} must contain exactly one closed wire for this operation`);
    }
    return profile;
}

function normalizePlaneFrame(plane?: PlaneFrame): PlaneFrame | undefined {
    if (!plane) {
        return undefined;
    }

    requirePoint3(plane.origin, 'plane.origin');
    requireVector3(plane.normal, 'plane.normal');
    requireVector3(plane.xDirection, 'plane.xDirection');
    requireNonParallelVectors(plane.normal, plane.xDirection, 'plane.normal', 'plane.xDirection');

    return {
        origin: [...plane.origin] as Point3,
        normal: [...plane.normal] as Vector3,
        xDirection: [...plane.xDirection] as Vector3,
    };
}

function normalizeExtrudeOptions(params: ExtrudeParams): ExtrudeOptionsPayload {
    const hasHeight = params.height !== undefined;
    const hasVector = params.vector !== undefined;

    if (hasHeight === hasVector) {
        throw new KernelError('INVALID_PARAMS', "Extrude params must specify exactly one of 'height' or 'vector'");
    }

    const plane = normalizePlaneFrame(params.plane);
    if (hasHeight) {
        requirePositive(params.height as number, 'height');
        return {
            height: params.height,
            ...(plane ? { plane } : {}),
        };
    }

    requireVector3(params.vector as Vector3, 'vector');
    return {
        vector: [...(params.vector as Vector3)] as Vector3,
        ...(plane ? { plane } : {}),
    };
}

function normalizeExtrudeProfileSpec(spec: ExtrudeProfileSpec): ExtrudeProfileSpecPayload {
    requireSchemaVersion(spec, 'extrude spec');

    const unit = spec.unit !== undefined
        ? (() => {
            if (spec.unit.length !== undefined && spec.unit.length !== 'model') {
                throw new KernelError('INVALID_PARAMS', 'extrude spec.unit.length must be model');
            }
            if (spec.unit.angle !== undefined && spec.unit.angle !== 'radians' && spec.unit.angle !== 'degrees') {
                throw new KernelError('INVALID_PARAMS', 'extrude spec.unit.angle must be radians or degrees');
            }
            return {
                ...(spec.unit.length !== undefined ? { length: spec.unit.length } : {}),
                ...(spec.unit.angle !== undefined ? { angle: spec.unit.angle } : {}),
            } as { length?: 'model'; angle?: 'radians' | 'degrees' };
        })()
        : undefined;

    const plane = normalizePlaneFrame(spec.plane);
    const direction = spec.direction !== undefined
        ? (() => {
            requireVector3(spec.direction, 'spec.direction');
            return [...spec.direction] as Vector3;
        })()
        : undefined;

    const hasDraftAngleRadians = spec.draftAngleRadians !== undefined;
    const hasDraftAngleDegrees = spec.draftAngleDegrees !== undefined;
    if (hasDraftAngleRadians && hasDraftAngleDegrees) {
        throw new KernelError('INVALID_PARAMS', 'extrude spec must not specify both draftAngleRadians and draftAngleDegrees');
    }

    let draftAngleRadians: number | undefined;
    if (hasDraftAngleRadians) {
        requireFinite(spec.draftAngleRadians as number, 'spec.draftAngleRadians');
        if (Math.abs(spec.draftAngleRadians as number) >= Math.PI / 2) {
            throw new KernelError('INVALID_PARAMS', 'spec.draftAngleRadians must be in (-pi/2, pi/2)');
        }
        draftAngleRadians = spec.draftAngleRadians;
    } else if (hasDraftAngleDegrees) {
        requireFinite(spec.draftAngleDegrees as number, 'spec.draftAngleDegrees');
        if (Math.abs(spec.draftAngleDegrees as number) >= 90) {
            throw new KernelError('INVALID_PARAMS', 'spec.draftAngleDegrees must be in (-90, 90)');
        }
        draftAngleRadians = (spec.draftAngleDegrees as number) * Math.PI / 180;
    }

    const normalizeSurface = (name: string, surface: { readonly shape?: ShapeHandle; readonly face: FaceRef }) => ({
        ...(surface.shape !== undefined ? { shapeId: surface.shape.id } : {}),
        face: (() => {
            requireFaceRef(surface.face, `${name}.face`);
            return { ...surface.face };
        })(),
    });

    const extent = (() => {
        if (!spec.extent || typeof spec.extent !== 'object' || typeof spec.extent.type !== 'string') {
            throw new KernelError('INVALID_PARAMS', 'spec.extent must be a structured extent descriptor');
        }

        switch (spec.extent.type) {
            case 'blind':
                requirePositive(spec.extent.distance, 'spec.extent.distance');
                return { type: 'blind' as const, distance: spec.extent.distance };
            case 'upToNext':
                return { type: 'upToNext' as const };
            case 'throughAll':
                return { type: 'throughAll' as const };
            case 'upToSurface':
                return {
                    type: 'upToSurface' as const,
                    surface: normalizeSurface('spec.extent.surface', spec.extent.surface),
                };
            case 'offsetFromSurface':
                requirePositive(spec.extent.offset, 'spec.extent.offset');
                return {
                    type: 'offsetFromSurface' as const,
                    surface: normalizeSurface('spec.extent.surface', spec.extent.surface),
                    offset: spec.extent.offset,
                };
            default:
                throw new KernelError('INVALID_PARAMS', `spec.extent.type '${(spec.extent as { type: string }).type}' is not supported`);
        }
    })();

    return {
        schemaVersion: 1,
        ...(spec.allowUnknownFields !== undefined ? { allowUnknownFields: spec.allowUnknownFields } : {}),
        ...(unit ? { unit } : {}),
        ...(plane ? { plane } : {}),
        ...(direction ? { direction } : {}),
        ...(spec.reverseDirection !== undefined ? { reverseDirection: spec.reverseDirection } : {}),
        ...(draftAngleRadians !== undefined ? { draftAngleRadians } : {}),
        extent,
        ...(spec.metadata !== undefined ? { metadata: spec.metadata } : {}),
    };
}

function normalizeSurfaceTarget(
    name: string,
    surface: { readonly shape?: ShapeHandle; readonly face: FaceRef },
): { shapeId?: number; face: FaceRef } {
    return {
        ...(surface.shape !== undefined ? { shapeId: surface.shape.id } : {}),
        face: (() => {
            requireFaceRef(surface.face, `${name}.face`);
            return { ...surface.face };
        })(),
    };
}

function normalizeRevolveAngleRadians(
    path: string,
    angleRadians?: number,
    angleDegrees?: number,
): number {
    const hasRadians = angleRadians !== undefined;
    const hasDegrees = angleDegrees !== undefined;
    if (hasRadians && hasDegrees) {
        throw new KernelError('INVALID_PARAMS', `${path} must not specify both angleRadians and angleDegrees`);
    }
    if (!hasRadians && !hasDegrees) {
        throw new KernelError('INVALID_PARAMS', `${path} must specify angleRadians or angleDegrees`);
    }

    if (hasRadians) {
        requireFinite(angleRadians as number, `${path}.angleRadians`);
        if ((angleRadians as number) === 0 || Math.abs(angleRadians as number) > Math.PI * 2) {
            throw new KernelError('INVALID_PARAMS', `${path}.angleRadians must be in [-2pi, 2pi] excluding 0`);
        }
        return angleRadians as number;
    }

    requireFinite(angleDegrees as number, `${path}.angleDegrees`);
    if ((angleDegrees as number) === 0 || Math.abs(angleDegrees as number) > 360) {
        throw new KernelError('INVALID_PARAMS', `${path}.angleDegrees must be in [-360, 360] excluding 0`);
    }
    return (angleDegrees as number) * Math.PI / 180;
}

function normalizeRevolveProfileSpec(spec: RevolveProfileSpec): RevolveProfileSpecPayload {
    requireSchemaVersion(spec, 'revolve spec');

    const unit = spec.unit !== undefined
        ? (() => {
            if (spec.unit.length !== undefined && spec.unit.length !== 'model') {
                throw new KernelError('INVALID_PARAMS', 'revolve spec.unit.length must be model');
            }
            if (spec.unit.angle !== undefined && spec.unit.angle !== 'radians' && spec.unit.angle !== 'degrees') {
                throw new KernelError('INVALID_PARAMS', 'revolve spec.unit.angle must be radians or degrees');
            }
            return {
                ...(spec.unit.length !== undefined ? { length: spec.unit.length } : {}),
                ...(spec.unit.angle !== undefined ? { angle: spec.unit.angle } : {}),
            } as { length?: 'model'; angle?: 'radians' | 'degrees' };
        })()
        : undefined;

    const plane = normalizePlaneFrame(spec.plane);
    const axisOrigin = spec.axisOrigin !== undefined
        ? (() => {
            requirePoint3(spec.axisOrigin, 'spec.axisOrigin');
            return [...spec.axisOrigin] as Point3;
        })()
        : undefined;
    const axisDirection = spec.axisDirection !== undefined
        ? (() => {
            requireVector3(spec.axisDirection, 'spec.axisDirection');
            return [...spec.axisDirection] as Vector3;
        })()
        : undefined;

    const slidingEdges = spec.slidingEdges !== undefined
        ? spec.slidingEdges.map((entry, index) => {
            requirePositiveInteger(entry.profileEdgeIndex, `spec.slidingEdges[${index}].profileEdgeIndex`);
            requireFaceRef(entry.face, `spec.slidingEdges[${index}].face`);
            return {
                profileEdgeIndex: entry.profileEdgeIndex,
                face: { ...entry.face },
            };
        })
        : undefined;

    const extent = (() => {
        if (!spec.extent || typeof spec.extent !== 'object' || typeof spec.extent.type !== 'string') {
            throw new KernelError('INVALID_PARAMS', 'spec.extent must be a structured extent descriptor');
        }

        switch (spec.extent.type) {
            case 'angle':
                return {
                    type: 'angle' as const,
                    angleRadians: normalizeRevolveAngleRadians('spec.extent', spec.extent.angleRadians, spec.extent.angleDegrees),
                };
            case 'upToSurface':
                return {
                    type: 'upToSurface' as const,
                    surface: normalizeSurfaceTarget('spec.extent.surface', spec.extent.surface),
                };
            case 'fromSurfaceToSurface':
                return {
                    type: 'fromSurfaceToSurface' as const,
                    fromSurface: normalizeSurfaceTarget('spec.extent.fromSurface', spec.extent.fromSurface),
                    untilSurface: normalizeSurfaceTarget('spec.extent.untilSurface', spec.extent.untilSurface),
                };
            case 'throughAll':
                return { type: 'throughAll' as const };
            case 'upToSurfaceAtAngle':
                return {
                    type: 'upToSurfaceAtAngle' as const,
                    surface: normalizeSurfaceTarget('spec.extent.surface', spec.extent.surface),
                    angleRadians: normalizeRevolveAngleRadians('spec.extent', spec.extent.angleRadians, spec.extent.angleDegrees),
                };
            default:
                throw new KernelError('INVALID_PARAMS', `spec.extent.type '${(spec.extent as { type: string }).type}' is not supported`);
        }
    })();

    return {
        schemaVersion: 1,
        ...(spec.allowUnknownFields !== undefined ? { allowUnknownFields: spec.allowUnknownFields } : {}),
        ...(unit ? { unit } : {}),
        ...(plane ? { plane } : {}),
        ...(axisOrigin ? { axisOrigin } : {}),
        ...(axisDirection ? { axisDirection } : {}),
        ...(spec.reverseDirection !== undefined ? { reverseDirection: spec.reverseDirection } : {}),
        ...(slidingEdges !== undefined ? { slidingEdges } : {}),
        extent,
        ...(spec.metadata !== undefined ? { metadata: spec.metadata } : {}),
    };
}

function normalizeSweepProfileSpec(spec: SweepProfileSpec, cut: boolean): SweepProfileSpecPayload {
    requireSchemaVersion(spec, 'sweep spec');

    const unit = spec.unit !== undefined
        ? (() => {
            if (spec.unit.length !== undefined && spec.unit.length !== 'model') {
                throw new KernelError('INVALID_PARAMS', 'sweep spec.unit.length must be model');
            }
            if (spec.unit.angle !== undefined && spec.unit.angle !== 'radians' && spec.unit.angle !== 'degrees') {
                throw new KernelError('INVALID_PARAMS', 'sweep spec.unit.angle must be radians or degrees');
            }
            return {
                ...(spec.unit.length !== undefined ? { length: spec.unit.length } : {}),
                ...(spec.unit.angle !== undefined ? { angle: spec.unit.angle } : {}),
            } as { length?: 'model'; angle?: 'radians' | 'degrees' };
        })()
        : undefined;

    const plane = normalizePlaneFrame(spec.plane);
    const spineJson = JSON.stringify(normalizeSpatialWire(spec.spine, 'spec.spine'));
    const solid = spec.solid ?? true;
    if (cut && !solid) {
        throw new KernelError('INVALID_PARAMS', 'sweep cut operations require spec.solid !== false');
    }

    const trihedronMode = spec.trihedronMode !== undefined
        ? (() => {
            switch (spec.trihedronMode.type) {
                case 'correctedFrenet':
                case 'frenet':
                case 'discrete':
                    return { type: spec.trihedronMode.type } as SweepTrihedronModePayload;
                case 'fixedTrihedron':
                    return {
                        type: 'fixedTrihedron' as const,
                        frame: normalizePlaneFrame(spec.trihedronMode.frame) as PlaneFrame,
                    };
                case 'fixedBinormal':
                    requireVector3(spec.trihedronMode.binormal, 'spec.trihedronMode.binormal');
                    return {
                        type: 'fixedBinormal' as const,
                        binormal: [...spec.trihedronMode.binormal] as Vector3,
                    };
                case 'auxiliarySpine':
                    return {
                        type: 'auxiliarySpine' as const,
                        spineJson: JSON.stringify(normalizeSpatialWire(spec.trihedronMode.spine, 'spec.trihedronMode.spine')),
                        ...(spec.trihedronMode.curvilinearEquivalence !== undefined ? { curvilinearEquivalence: spec.trihedronMode.curvilinearEquivalence } : {}),
                        ...(spec.trihedronMode.contact !== undefined ? { contact: spec.trihedronMode.contact } : {}),
                    };
                default:
                    throw new KernelError('INVALID_PARAMS', 'spec.trihedronMode.type is not supported');
            }
        })()
        : undefined;

    const tolerance = spec.tolerance !== undefined
        ? (() => {
            if (spec.tolerance.tol3d !== undefined) {
                requirePositive(spec.tolerance.tol3d, 'spec.tolerance.tol3d');
            }
            if (spec.tolerance.boundTol !== undefined) {
                requirePositive(spec.tolerance.boundTol, 'spec.tolerance.boundTol');
            }
            if (spec.tolerance.angularTol !== undefined) {
                requirePositive(spec.tolerance.angularTol, 'spec.tolerance.angularTol');
            }
            return {
                ...(spec.tolerance.tol3d !== undefined ? { tol3d: spec.tolerance.tol3d } : {}),
                ...(spec.tolerance.boundTol !== undefined ? { boundTol: spec.tolerance.boundTol } : {}),
                ...(spec.tolerance.angularTol !== undefined ? { angularTol: spec.tolerance.angularTol } : {}),
            };
        })()
        : undefined;

    if (spec.maxDegree !== undefined) {
        requirePositiveInteger(spec.maxDegree, 'spec.maxDegree');
    }
    if (spec.maxSegments !== undefined) {
        requirePositiveInteger(spec.maxSegments, 'spec.maxSegments');
    }

    return {
        schemaVersion: 1,
        ...(spec.allowUnknownFields !== undefined ? { allowUnknownFields: spec.allowUnknownFields } : {}),
        ...(unit ? { unit } : {}),
        ...(plane ? { plane } : {}),
        spineJson,
        ...(trihedronMode ? { trihedronMode } : {}),
        ...(spec.sectionWithContact !== undefined ? { sectionWithContact: spec.sectionWithContact } : {}),
        ...(spec.sectionWithCorrection !== undefined ? { sectionWithCorrection: spec.sectionWithCorrection } : {}),
        ...(spec.solid !== undefined ? { solid: spec.solid } : {}),
        ...(spec.forceApproxC1 !== undefined ? { forceApproxC1: spec.forceApproxC1 } : {}),
        ...(spec.transitionMode !== undefined ? { transitionMode: spec.transitionMode } : {}),
        ...(tolerance ? { tolerance } : {}),
        ...(spec.maxDegree !== undefined ? { maxDegree: spec.maxDegree } : {}),
        ...(spec.maxSegments !== undefined ? { maxSegments: spec.maxSegments } : {}),
        ...(cut ? { cut: true } : {}),
        ...(spec.metadata !== undefined ? { metadata: spec.metadata } : {}),
    };
}

function normalizeLoftSections(sections: readonly LoftSection[]): LoftSectionPayload[] {
    if (!Array.isArray(sections) || sections.length < 2) {
        throw new KernelError('INVALID_PARAMS', 'loft sections must contain at least two entries');
    }

    return sections.map((section, index) => {
        switch (section.type) {
            case 'profile':
                return (() => {
                    const profile = requireSingleWireProfile(normalizeProfile(section.profile), `sections[${index}].profile`);
                    return {
                        type: 'profile' as const,
                        profileJson: JSON.stringify(profile),
                        ...(section.plane !== undefined ? { plane: normalizePlaneFrame(section.plane) as PlaneFrame } : {}),
                    };
                })();
            case 'wire':
                return {
                    type: 'wire' as const,
                    wireJson: JSON.stringify(normalizeSpatialWire(section.wire, `sections[${index}].wire`)),
                };
            case 'point':
                requirePoint3(section.point, `sections[${index}].point`);
                return {
                    type: 'point' as const,
                    point: [section.point[0], section.point[1], section.point[2]] as Point3,
                };
            default:
                throw new KernelError('INVALID_PARAMS', `sections[${index}].type is not supported`);
        }
    });
}

function normalizeLoftSpec(spec: LoftSpec, cut: boolean): LoftSpecPayload {
    requireSchemaVersion(spec, 'loft spec');

    const solid = spec.solid ?? true;
    if (cut && !solid) {
        throw new KernelError('INVALID_PARAMS', 'loft cut operations require spec.solid !== false');
    }

    if (spec.pres3d !== undefined) {
        requirePositive(spec.pres3d, 'spec.pres3d');
    }
    if (spec.maxDegree !== undefined) {
        requirePositiveInteger(spec.maxDegree, 'spec.maxDegree');
    }

    const criteriumWeight = spec.criteriumWeight !== undefined
        ? (() => {
            requirePositive(spec.criteriumWeight.w1, 'spec.criteriumWeight.w1');
            requirePositive(spec.criteriumWeight.w2, 'spec.criteriumWeight.w2');
            requirePositive(spec.criteriumWeight.w3, 'spec.criteriumWeight.w3');
            return {
                w1: spec.criteriumWeight.w1,
                w2: spec.criteriumWeight.w2,
                w3: spec.criteriumWeight.w3,
            };
        })()
        : undefined;

    return {
        schemaVersion: 1,
        ...(spec.allowUnknownFields !== undefined ? { allowUnknownFields: spec.allowUnknownFields } : {}),
        ...(spec.solid !== undefined ? { solid: spec.solid } : {}),
        ...(spec.ruled !== undefined ? { ruled: spec.ruled } : {}),
        ...(spec.pres3d !== undefined ? { pres3d: spec.pres3d } : {}),
        ...(spec.checkCompatibility !== undefined ? { checkCompatibility: spec.checkCompatibility } : {}),
        ...(spec.smoothing !== undefined ? { smoothing: spec.smoothing } : {}),
        ...(spec.parametrization !== undefined ? { parametrization: spec.parametrization } : {}),
        ...(spec.continuity !== undefined ? { continuity: spec.continuity } : {}),
        ...(criteriumWeight ? { criteriumWeight } : {}),
        ...(spec.maxDegree !== undefined ? { maxDegree: spec.maxDegree } : {}),
        ...(spec.mutableInput !== undefined ? { mutableInput: spec.mutableInput } : {}),
        ...(cut ? { cut: true } : {}),
        ...(spec.metadata !== undefined ? { metadata: spec.metadata } : {}),
    };
}

function normalizeRevolveOptions(params: RevolveParams): RevolveOptionsPayload {
    if (!Number.isFinite(params.angleDegrees) || params.angleDegrees <= 0 || params.angleDegrees > 360) {
        throw new KernelError('INVALID_PARAMS', 'angleDegrees must be in the range (0, 360]');
    }

    if (params.axisOrigin) {
        requirePoint3(params.axisOrigin, 'axisOrigin');
    }
    if (params.axisDirection) {
        requireVector3(params.axisDirection, 'axisDirection');
    }

    return {
        angleDegrees: params.angleDegrees,
        ...(params.axisOrigin ? { axisOrigin: [...params.axisOrigin] as Point3 } : {}),
        ...(params.axisDirection ? { axisDirection: [...params.axisDirection] as Vector3 } : {}),
    };
}

function normalizeRotationTransform(rotation: RotationTransform): RotationTransform {
    requirePoint3(rotation.axisOrigin, 'transform.rotation.axisOrigin');
    requireVector3(rotation.axisDirection, 'transform.rotation.axisDirection');
    requireFinite(rotation.angleDegrees, 'transform.rotation.angleDegrees');

    return {
        axisOrigin: [...rotation.axisOrigin] as Point3,
        axisDirection: [...rotation.axisDirection] as Vector3,
        angleDegrees: rotation.angleDegrees,
    };
}

function normalizeShapeTransform(transform: ShapeTransform): ShapeTransform {
    if (!transform.translation && !transform.rotation) {
        throw new KernelError('INVALID_PARAMS', 'Transform must specify translation and/or rotation');
    }

    const translation = transform.translation
        ? (() => {
            requireVector3(transform.translation as Vector3, 'transform.translation', true);
            return [...transform.translation] as Vector3;
        })()
        : undefined;

    const rotation = transform.rotation
        ? normalizeRotationTransform(transform.rotation)
        : undefined;

    return {
        ...(translation ? { translation } : {}),
        ...(rotation ? { rotation } : {}),
    };
}

// ---------------------------------------------------------------------------
// OcctKernel
// ---------------------------------------------------------------------------

/**
 * High-level CAD kernel wrapper around the OCCT WASM module.
 *
 * Obtain an instance via {@link createKernel}.
 */
export class OcctKernel {
    private readonly _native: NativeKernel;

    /** @internal – use {@link createKernel} instead. */
    constructor(wasmModule: WasmModule) {
        this._native = new wasmModule.OcctKernel();
    }

    // -----------------------------------------------------------------------
    // Primitives
    // -----------------------------------------------------------------------

    /** Create a solid box aligned with the world axes, with one corner at the origin. */
    createBox(params: BoxParams): ShapeHandle {
        requirePositive(params.dx, 'dx');
        requirePositive(params.dy, 'dy');
        requirePositive(params.dz, 'dz');
        return makeHandle(wrap(() => this._native.createBox(params.dx, params.dy, params.dz)));
    }

    /** Create a solid cylinder with its axis along +Z, base centred at the origin. */
    createCylinder(params: CylinderParams): ShapeHandle {
        requirePositive(params.radius, 'radius');
        requirePositive(params.height, 'height');
        return makeHandle(wrap(() => this._native.createCylinder(params.radius, params.height)));
    }

    /** Create a solid sphere centred at the origin. */
    createSphere(params: SphereParams): ShapeHandle {
        requirePositive(params.radius, 'radius');
        return makeHandle(wrap(() => this._native.createSphere(params.radius)));
    }

    // -----------------------------------------------------------------------
    // Sketch-based features
    // -----------------------------------------------------------------------

    /**
     * Extrude a closed 2-D profile using either a local-plane height or an explicit vector.
     */
    extrudeProfile(params: ExtrudeParams): ShapeHandle {
        const profileJson = JSON.stringify(normalizeProfile(params.profile));
        const optionsJson = JSON.stringify(normalizeExtrudeOptions(params));
        return makeHandle(wrap(() => this._native.extrudeProfile(profileJson, optionsJson)));
    }

    /** Apply a versioned additive profile extrusion feature spec to a resident shape. */
    extrudeProfileWithSpec(params: ExtrudeProfileFeatureParams): ShapeHandle {
        if (typeof this._native.extrudeProfileWithSpec !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support extrudeProfileWithSpec');
        }
        const profileJson = JSON.stringify(normalizeProfile(params.profile));
        const specJson = JSON.stringify(normalizeExtrudeProfileSpec(params.spec));
        return makeHandle(wrap(() => this._native.extrudeProfileWithSpec?.(params.shape.id, profileJson, specJson) ?? 0));
    }

    /** Apply a versioned subtractive profile extrusion feature spec to a resident shape. */
    extrudeCutProfileWithSpec(params: ExtrudeCutProfileFeatureParams): ShapeHandle {
        if (typeof this._native.extrudeCutProfileWithSpec !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support extrudeCutProfileWithSpec');
        }
        const profileJson = JSON.stringify(normalizeProfile(params.profile));
        const specJson = JSON.stringify(normalizeExtrudeProfileSpec(params.spec));
        return makeHandle(wrap(() => this._native.extrudeCutProfileWithSpec?.(params.shape.id, profileJson, specJson) ?? 0));
    }

    /**
     * Revolve a closed 2-D profile about an arbitrary world-space axis.
     */
    revolveProfile(params: RevolveParams): ShapeHandle {
        const profileJson = JSON.stringify(normalizeProfile(params.profile));
        const optionsJson = JSON.stringify(normalizeRevolveOptions(params));
        return makeHandle(wrap(() => this._native.revolveProfile(profileJson, optionsJson)));
    }

    /** Apply a versioned additive profile revolve feature spec to a resident shape. */
    revolveProfileWithSpec(params: RevolveProfileFeatureParams): ShapeHandle {
        if (params.cut === true) {
            if (typeof this._native.revolveCutProfileWithSpec !== 'function') {
                throw new KernelError('UNKNOWN', 'Native module does not support revolveCutProfileWithSpec');
            }
            const profileJson = JSON.stringify(normalizeProfile(params.profile));
            const specJson = JSON.stringify(normalizeRevolveProfileSpec(params.spec));
            return makeHandle(wrap(() => this._native.revolveCutProfileWithSpec?.(params.shape.id, profileJson, specJson) ?? 0));
        }

        if (typeof this._native.revolveProfileWithSpec !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support revolveProfileWithSpec');
        }
        const profileJson = JSON.stringify(normalizeProfile(params.profile));
        const specJson = JSON.stringify(normalizeRevolveProfileSpec(params.spec));
        return makeHandle(wrap(() => this._native.revolveProfileWithSpec?.(params.shape.id, profileJson, specJson) ?? 0));
    }

    /** Apply a versioned subtractive profile revolve feature spec to a resident shape. */
    revolveCutProfileWithSpec(params: RevolveCutProfileFeatureParams): ShapeHandle {
        if (typeof this._native.revolveCutProfileWithSpec !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support revolveCutProfileWithSpec');
        }
        const profileJson = JSON.stringify(normalizeProfile(params.profile));
        const specJson = JSON.stringify(normalizeRevolveProfileSpec(params.spec));
        return makeHandle(wrap(() => this._native.revolveCutProfileWithSpec?.(params.shape.id, profileJson, specJson) ?? 0));
    }

    /** Apply a versioned sweep feature spec to a resident shape. */
    sweepProfileWithSpec(params: SweepProfileFeatureParams): ShapeHandle {
        if (typeof this._native.sweepProfileWithSpec !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support sweepProfileWithSpec');
        }
        const profileJson = JSON.stringify(requireSingleWireProfile(normalizeProfile(params.profile), 'profile'));
        const specJson = JSON.stringify(normalizeSweepProfileSpec(params.spec, params.cut === true));
        return makeHandle(wrap(() => this._native.sweepProfileWithSpec?.(params.shape.id, profileJson, specJson) ?? 0));
    }

    /** Apply a versioned loft feature spec to a resident shape. */
    loftWithSpec(params: LoftFeatureParams): ShapeHandle {
        if (typeof this._native.loftWithSpec !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support loftWithSpec');
        }
        const sectionsJson = JSON.stringify(normalizeLoftSections(params.sections));
        const specJson = JSON.stringify(normalizeLoftSpec(params.spec, params.cut === true));
        return makeHandle(wrap(() => this._native.loftWithSpec?.(params.shape.id, sectionsJson, specJson) ?? 0));
    }

    // -----------------------------------------------------------------------
    // Boolean operations
    // -----------------------------------------------------------------------

    /** Compute the union of two shapes. Returns a new shape handle. */
    booleanUnion(params: BooleanParams): ShapeHandle {
        return makeHandle(wrap(() => this._native.booleanUnion(params.base.id, params.tool.id)));
    }

    /** Subtract `tool` from `base`. Returns a new shape handle. */
    booleanSubtract(params: BooleanParams): ShapeHandle {
        return makeHandle(wrap(() => this._native.booleanSubtract(params.base.id, params.tool.id)));
    }

    /** Compute the intersection of two shapes. Returns a new shape handle. */
    booleanIntersect(params: BooleanParams): ShapeHandle {
        return makeHandle(wrap(() => this._native.booleanIntersect(params.base.id, params.tool.id)));
    }

    // -----------------------------------------------------------------------
    // Modifiers
    // -----------------------------------------------------------------------

    /**
     * Apply a constant-radius fillet to all edges of a shape.
     * Returns a new shape handle (original is unchanged).
     */
    filletEdges(params: FilletParams): ShapeHandle {
        requirePositive(params.radius, 'radius');
        return makeHandle(wrap(() => this._native.filletEdges(params.shape.id, params.radius)));
    }

    /**
     * Apply a constant-distance chamfer to all edges of a shape.
     * Returns a new shape handle (original is unchanged).
     */
    chamferEdges(params: ChamferParams): ShapeHandle {
        requirePositive(params.distance, 'distance');
        return makeHandle(wrap(() => this._native.chamferEdges(params.shape.id, params.distance)));
    }

    /** Apply a versioned native OCCT fillet spec and return exact blend lineage. */
    filletEdgesWithSpec(params: FilletFeatureParams): BlendOperationResult {
        if (typeof this._native.filletEdgesWithSpec !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support filletEdgesWithSpec');
        }
        requireSchemaVersion(params.spec, 'fillet spec');
        requireSupportedBlendControls('filletEdges', 'fillet', params.spec);
        if (params.spec.edges !== undefined) {
            params.spec.edges.forEach((entry, index) => {
                const edge = entry.edge ?? entry.edgeRef ?? entry;
                requireEdgeRef(edge, `fillet spec.edges[${index}]`);
                requireSupportedBlendControls('filletEdges', `fillet.edges[${index}]`, entry);
            });
        }
        const raw = wrap(() => this._native.filletEdgesWithSpec?.(params.shape.id, JSON.stringify(params.spec)) ?? '{}');
        return withShapeHandle(parseJson<Omit<BlendOperationResult, 'shape'>>(raw, 'fillet result'));
    }

    /** Apply a versioned native OCCT chamfer spec and return exact blend lineage. */
    chamferEdgesWithSpec(params: ChamferFeatureParams): BlendOperationResult {
        if (typeof this._native.chamferEdgesWithSpec !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support chamferEdgesWithSpec');
        }
        requireSchemaVersion(params.spec, 'chamfer spec');
        requireSupportedBlendControls('chamferEdges', 'chamfer', params.spec);
        if (params.spec.referenceFace !== undefined) {
            requireFaceRef(params.spec.referenceFace, 'chamfer spec.referenceFace');
        }
        if (params.spec.edges !== undefined) {
            params.spec.edges.forEach((entry, index) => {
                const edge = entry.edge ?? entry.edgeRef ?? entry;
                requireEdgeRef(edge, `chamfer spec.edges[${index}]`);
                requireSupportedBlendControls('chamferEdges', `chamfer.edges[${index}]`, entry);
                if (entry.referenceFace !== undefined) {
                    requireFaceRef(entry.referenceFace, `chamfer spec.edges[${index}].referenceFace`);
                }
            });
        }
        const raw = wrap(() => this._native.chamferEdgesWithSpec?.(params.shape.id, JSON.stringify(params.spec)) ?? '{}');
        return withShapeHandle(parseJson<Omit<BlendOperationResult, 'shape'>>(raw, 'chamfer result'));
    }

    /**
     * Apply a world-space transform to a resident shape and return a new handle.
     * When both are provided, rotation is applied before translation.
     */
    transformShape(params: TransformParams): ShapeHandle {
        const transformJson = JSON.stringify(normalizeShapeTransform(params.transform));
        return makeHandle(wrap(() => this._native.transformShape(params.shape.id, transformJson)));
    }

    // -----------------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------------

    /** Return additive contract flags supported by the loaded native module. */
    getCapabilities(): KernelCapabilities {
        if (typeof this._native.getCapabilities !== 'function') {
            return fallbackCapabilities;
        }
        return {
            ...fallbackCapabilities,
            ...parseJson<Partial<KernelCapabilities>>(wrap(() => this._native.getCapabilities?.() ?? '{}'), 'capabilities'),
        };
    }

    /** Return face, edge, vertex counts, bounding box, and validity flag. */
    getTopology(shape: ShapeHandle): TopologyResult {
        const raw = wrap(() => this._native.getTopology(shape.id));
        return parseJson<RawTopology>(raw, 'topology');
    }

    /** Return immutable revision metadata for a resident shape handle. */
    getRevisionInfo(shape: ShapeHandle): RevisionInfo {
        if (typeof this._native.getRevisionInfo !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support getRevisionInfo');
        }
        return parseJson<RevisionInfo>(wrap(() => this._native.getRevisionInfo?.(shape.id) ?? '{}'), 'revision info');
    }

    /** Resolve a stable face/edge/vertex hash in the current resident revision. */
    resolveStableEntity(params: ResolveStableEntityParams): StableEntityResolution {
        if (typeof this._native.resolveStableEntity !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support resolveStableEntity');
        }
        const raw = wrap(() => this._native.resolveStableEntity?.(params.shape.id, params.stableHash) ?? '{}');
        return parseJson<StableEntityResolution>(raw, 'stable entity resolution');
    }

    /** Map stable hashes between two resident revisions without JS mesh inference. */
    mapEntitiesAcrossRevisions(params: MapEntitiesAcrossRevisionsParams): EntityRevisionMapResult {
        if (typeof this._native.mapEntitiesAcrossRevisions !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support mapEntitiesAcrossRevisions');
        }
        const raw = wrap(() => this._native.mapEntitiesAcrossRevisions?.(
            params.fromRevisionId,
            params.toRevisionId,
            JSON.stringify(params.stableHashes),
        ) ?? '{}');
        return parseJson<EntityRevisionMapResult>(raw, 'entity revision mapping');
    }

    /** Evaluate an exact edge point/tangent without tessellation. */
    evaluateEdge(params: EdgeEvaluationParams): EdgeEvaluationResult {
        if (typeof this._native.evaluateEdge !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support evaluateEdge');
        }
        requireEdgeRef(params.edge, 'edge');
        requireFinite(params.t, 't');
        const raw = wrap(() => this._native.evaluateEdge?.(params.shape.id, JSON.stringify(params.edge), params.t) ?? '{}');
        return parseJson<EdgeEvaluationResult>(raw, 'edge evaluation');
    }

    /** Sample an exact edge curve without tessellation. */
    sampleEdge(params: SampleEdgeParams): EdgeSampleResult {
        if (typeof this._native.sampleEdge !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support sampleEdge');
        }
        requireEdgeRef(params.edge, 'edge');
        if (params.count !== undefined) requirePositiveInteger(params.count, 'count');
        const options = {
            count: params.count,
            start: params.start,
            end: params.end,
            normalized: params.normalized,
            includeTangents: params.includeTangents,
        };
        const raw = wrap(() => this._native.sampleEdge?.(params.shape.id, JSON.stringify(params.edge), JSON.stringify(options)) ?? '{}');
        return parseJson<EdgeSampleResult>(raw, 'edge samples');
    }

    /** Return exact curve metadata for an edge. */
    getEdgeCurve(params: { readonly shape: ShapeHandle; readonly edge: EdgeRef }): EdgeCurveResult {
        if (typeof this._native.getEdgeCurve !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support getEdgeCurve');
        }
        requireEdgeRef(params.edge, 'edge');
        const raw = wrap(() => this._native.getEdgeCurve?.(params.shape.id, JSON.stringify(params.edge)) ?? '{}');
        return parseJson<EdgeCurveResult>(raw, 'edge curve');
    }

    /** Evaluate an exact face point/normal without tessellation. */
    evaluateFace(params: FaceEvaluationParams): FaceEvaluationResult {
        if (typeof this._native.evaluateFace !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support evaluateFace');
        }
        requireFaceRef(params.face, 'face');
        requireFinite(params.u, 'u');
        requireFinite(params.v, 'v');
        const raw = wrap(() => this._native.evaluateFace?.(params.shape.id, JSON.stringify(params.face), params.u, params.v) ?? '{}');
        return parseJson<FaceEvaluationResult>(raw, 'face evaluation');
    }

    /** Return the versioned operation schema advertised by the native kernel. */
    getOperationSchema(): OperationSchema {
        if (typeof this._native.getOperationSchema !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support getOperationSchema');
        }
        return parseJson<OperationSchema>(wrap(() => this._native.getOperationSchema?.() ?? '{}'), 'operation schema');
    }

    /** Return true when the shape is geometrically and topologically valid. */
    checkValidity(shape: ShapeHandle): boolean {
        return wrap(() => this._native.checkValidity(shape.id));
    }

    // -----------------------------------------------------------------------
    // Tessellation
    // -----------------------------------------------------------------------

    /**
     * Triangulate a shape for WebGL / Three.js rendering.
     *
     * @param params.linearDeflection  – chord-height tolerance (default 0.1).
     * @param params.angularDeflection – angular tolerance in radians (default 0.5).
     */
    tessellate(params: TessellateParams): TessellationResult {
        const linearDeflection = params.linearDeflection ?? 0.1;
        const angularDeflection = params.angularDeflection ?? 0.5;
        requirePositive(linearDeflection, 'linearDeflection');
        requirePositive(angularDeflection, 'angularDeflection');

        const raw = wrap(() => this._native.tessellate(params.shape.id, linearDeflection, angularDeflection));
        const data = parseJson<RawTessellation>(raw, 'tessellation');

        return {
            positions: new Float32Array(data.positions),
            normals:   new Float32Array(data.normals),
            indices:   new Uint32Array(data.indices),
            ...(data.triangleNormals !== undefined
                ? { triangleNormals: new Float32Array(data.triangleNormals) }
                : {}),
            ...(data.triangleTopoFaceIds !== undefined
                ? { triangleTopoFaceIds: new Uint32Array(data.triangleTopoFaceIds) }
                : {}),
            ...(data.triangleFaceGroups !== undefined
                ? { triangleFaceGroups: new Uint32Array(data.triangleFaceGroups) }
                : {}),
            ...(data.triangleStableHashes !== undefined
                ? { triangleStableHashes: data.triangleStableHashes }
                : {}),
            ...(data.featureEdges !== undefined
                ? { featureEdges: data.featureEdges }
                : {}),
            ...(data.rawEdgeSegments !== undefined
                ? { rawEdgeSegments: new Float32Array(data.rawEdgeSegments) }
                : {}),
        };
    }

    // -----------------------------------------------------------------------
    // Import / export
    // -----------------------------------------------------------------------

    /**
     * Import a STEP file from a UTF-8 string. Returns a handle to the imported shape.
     * Throws {@link KernelError} with code `IMPORT_FAILED` on parse errors.
     */
    importStep(params: ImportStepParams): ShapeHandle {
        if (typeof params.content !== 'string') {
            throw new KernelError('INVALID_PARAMS', 'STEP content must be a string');
        }

        const result = this.importStepDetailed(params);
        if (!result.shape) {
            throw new KernelError('IMPORT_FAILED', formatImportFailure(result));
        }
        return result.shape;
    }

    /**
     * Import a STEP file and return reader, transfer, and validity diagnostics.
     *
     * Unlike {@link importStep}, this method does not throw on STEP parse or
     * transfer failures; those are returned in the structured result.
     */
    importStepDetailed(params: ImportStepParams): ImportStepDetailedResult {
        if (typeof params.content !== 'string') {
            throw new KernelError('INVALID_PARAMS', 'STEP content must be a string');
        }

        const options = normalizeImportOptions(params.options);
        const raw = wrap(() => this._native.importStepDetailed(
            params.content,
            options.heal,
            options.sew,
            options.fixSameParameter,
            options.fixSolid,
            options.sewingTolerance,
        ));

        return toImportStepDetailedResult(parseJson<RawStepImportDetailedResult>(raw, 'STEP import'));
    }

    /**
     * Export a shape to STEP format. Returns the STEP file content as a UTF-8 string.
     * Throws {@link KernelError} with code `EXPORT_FAILED` on failure.
     */
    exportStep(params: ExportStepParams): string {
        return wrap(() => this._native.exportStep(params.shape.id));
    }

    /** Create a JSON checkpoint containing CBREP plus revision/history metadata. */
    createCheckpoint(params: CreateCheckpointParams): RevisionCheckpoint {
        if (typeof this._native.createCheckpoint !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support createCheckpoint');
        }
        return parseJson<RevisionCheckpoint>(wrap(() => this._native.createCheckpoint?.(params.shape.id) ?? '{}'), 'checkpoint');
    }

    /** Hydrate a checkpoint created by {@link createCheckpoint}. */
    hydrateCheckpoint(params: HydrateCheckpointParams): ShapeHandle {
        if (typeof this._native.hydrateCheckpoint !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support hydrateCheckpoint');
        }
        const checkpointJson = typeof params.checkpoint === 'string'
            ? params.checkpoint
            : JSON.stringify(params.checkpoint);
        return makeHandle(wrap(() => this._native.hydrateCheckpoint?.(checkpointJson) ?? 0));
    }

    // -----------------------------------------------------------------------
    // Memory management
    // -----------------------------------------------------------------------

    /**
     * Release the native memory held by the given shape handle.
     *
     * After calling this method the handle must not be used again.
     */
    disposeShape(params: DisposeParams): void {
        wrap(() => this._native.disposeShape(params.shape.id));
    }

    /** Increment the native reference count for a resident immutable revision. */
    retainRevision(params: RetainRevisionParams): void {
        if (typeof this._native.retainRevision !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support retainRevision');
        }
        wrap(() => this._native.retainRevision?.(params.shape.id));
    }

    /** Decrement the native reference count and return true when the revision was evicted. */
    releaseRevision(params: ReleaseRevisionParams): boolean {
        if (typeof this._native.releaseRevision !== 'function') {
            throw new KernelError('UNKNOWN', 'Native module does not support releaseRevision');
        }
        return wrap(() => this._native.releaseRevision?.(params.shape.id) ?? false);
    }
}
