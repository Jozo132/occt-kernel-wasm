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
    RevolveParams,
    ShapeHandle,
    SphereParams,
    StepImportMessage,
    StepImportOptions,
    StepImportReadStatus,
    StepImportTransferStatus,
    TessellateParams,
    TessellationResult,
    TopologyResult,
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
    extrudeProfile(profileJson: string, height: number): number;
    revolveProfile(profileJson: string, angleDegrees: number): number;
    booleanUnion(id1: number, id2: number): number;
    booleanSubtract(id1: number, id2: number): number;
    booleanIntersect(id1: number, id2: number): number;
    filletEdges(id: number, radius: number): number;
    chamferEdges(id: number, distance: number): number;
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
     * Extrude a closed 2-D profile in the XY plane along +Z by `height` units.
     *
     * The profile must form a single closed wire.
     */
    extrudeProfile(params: ExtrudeParams): ShapeHandle {
        requirePositive(params.height, 'height');
        if (!params.profile.segments || params.profile.segments.length === 0) {
            throw new KernelError('INVALID_PARAMS', 'Profile must have at least one segment');
        }
        const profileJson = JSON.stringify(params.profile);
        return makeHandle(wrap(() => this._native.extrudeProfile(profileJson, params.height)));
    }

    /**
     * Revolve a closed 2-D profile in the XY plane about the Y axis.
     *
     * @param params.angleDegrees – revolution angle in degrees (0 < angle ≤ 360).
     */
    revolveProfile(params: RevolveParams): ShapeHandle {
        if (!Number.isFinite(params.angleDegrees) || params.angleDegrees <= 0 || params.angleDegrees > 360) {
            throw new KernelError('INVALID_PARAMS', 'angleDegrees must be in the range (0, 360]');
        }
        if (!params.profile.segments || params.profile.segments.length === 0) {
            throw new KernelError('INVALID_PARAMS', 'Profile must have at least one segment');
        }
        const profileJson = JSON.stringify(params.profile);
        return makeHandle(wrap(() => this._native.revolveProfile(profileJson, params.angleDegrees)));
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
