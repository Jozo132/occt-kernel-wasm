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
    BoundingBox,
    BoxParams,
    ChamferParams,
    CylinderParams,
    DisposeParams,
    ExportStepParams,
    ExtrudeParams,
    FilletParams,
    ImportStepDetailedResult,
    ImportStepParams,
    PlaneFrame,
    Point2,
    Point3,
    Profile,
    ProfileSegment,
    ProfileWire,
    RevolveParams,
    RotationTransform,
    ShapeHandle,
    ShapeTransform,
    SphereParams,
    StepImportMessage,
    StepImportOptions,
    StepImportReadStatus,
    StepImportTransferStatus,
    TessellateParams,
    TessellationResult,
    TopologyResult,
    TransformParams,
    Vector3,
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
    revolveProfile(profileJson: string, optionsJson: string): number;
    booleanUnion(id1: number, id2: number): number;
    booleanSubtract(id1: number, id2: number): number;
    booleanIntersect(id1: number, id2: number): number;
    filletEdges(id: number, radius: number): number;
    chamferEdges(id: number, distance: number): number;
    transformShape(id: number, transformJson: string): number;
    getTopology(id: number): string;
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
    disposeShape(id: number): void;
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

function requirePoint2Array(values: readonly Point2[], name: string, minimumLength: number): void {
    if (!Array.isArray(values) || values.length < minimumLength) {
        throw new KernelError('INVALID_PARAMS', `${name} must contain at least ${minimumLength} points`);
    }
    values.forEach((value, index) => requirePoint2(value, `${name}[${index}]`));
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
    edgeSegments?: number[];
}

interface RawTopology {
    faceCount: number;
    edgeCount: number;
    vertexCount: number;
    boundingBox: BoundingBox;
    isValid: boolean;
}

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

interface ExtrudeOptionsPayload {
    height?: number;
    vector?: Vector3;
    plane?: PlaneFrame;
}

interface RevolveOptionsPayload {
    angleDegrees: number;
    axisOrigin?: Point3;
    axisDirection?: Vector3;
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

    /**
     * Revolve a closed 2-D profile about an arbitrary world-space axis.
     */
    revolveProfile(params: RevolveParams): ShapeHandle {
        const profileJson = JSON.stringify(normalizeProfile(params.profile));
        const optionsJson = JSON.stringify(normalizeRevolveOptions(params));
        return makeHandle(wrap(() => this._native.revolveProfile(profileJson, optionsJson)));
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

    /** Return face, edge, vertex counts, bounding box, and validity flag. */
    getTopology(shape: ShapeHandle): TopologyResult {
        const raw = wrap(() => this._native.getTopology(shape.id));
        return parseJson<RawTopology>(raw, 'topology');
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
            ...(data.edgeSegments !== undefined
                ? { edgeSegments: new Float32Array(data.edgeSegments) }
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
}
