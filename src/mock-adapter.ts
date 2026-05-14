/**
 * Mock adapter for the OCCT WASM module.
 *
 * This module is loaded instead of the real compiled WASM binary when running
 * TypeScript tests in Node.js without a built WASM binary. It provides enough
 * of the NativeKernel interface to allow the TypeScript wrapper to be tested.
 *
 * The mock does NOT perform real geometry. It is deliberately minimal and only
 * exercises the JS/TS layer logic (parameter validation, error propagation, etc.).
 *
 * Real geometry regression tests require the compiled WASM binary and are skipped
 * when it is absent.
 */

import { KernelError } from './errors';

// ---------------------------------------------------------------------------
// Internal shape store
// ---------------------------------------------------------------------------

interface MockShape {
    kind: string;
    params: Record<string, unknown>;
}

interface MockImportResult {
    readStatus: string;
    transferStatus: string;
    rootCount: number;
    transferredRootCount: number;
    messageList: Array<{
        phase: 'load' | 'transfer' | 'heal' | 'validation';
        severity: 'info' | 'warning' | 'fail';
        text: string;
        entityNumber?: number;
    }>;
    shapeId?: number;
    isValid: boolean;
    wasValidBeforeHealing: boolean;
    healed: boolean;
}

// ---------------------------------------------------------------------------
// Mock native kernel class
// ---------------------------------------------------------------------------

export class MockNativeKernel {
    private _shapes = new Map<number, MockShape>();
    private _nextId = 1;

    private _store(kind: string, params: Record<string, unknown>): number {
        const id = this._nextId++;
        this._shapes.set(id, { kind, params });
        return id;
    }

    private _require(id: number): MockShape {
        const shape = this._shapes.get(id);
        if (!shape) {
            throw new KernelError('INVALID_HANDLE', `No shape with handle ${id}`);
        }
        return shape;
    }

    // -- Primitives --

    createBox(dx: number, dy: number, dz: number): number {
        if (dx <= 0 || dy <= 0 || dz <= 0) {
            throw new KernelError('INVALID_PARAMS', 'Box dimensions must be > 0');
        }
        return this._store('box', { dx, dy, dz });
    }

    createCylinder(radius: number, height: number): number {
        if (radius <= 0 || height <= 0) {
            throw new KernelError('INVALID_PARAMS', 'Cylinder radius and height must be > 0');
        }
        return this._store('cylinder', { radius, height });
    }

    createSphere(radius: number): number {
        if (radius <= 0) {
            throw new KernelError('INVALID_PARAMS', 'Sphere radius must be > 0');
        }
        return this._store('sphere', { radius });
    }

    // -- Sketch-based features --

    extrudeProfile(profileJson: string, height: number): number {
        if (height <= 0) {
            throw new KernelError('INVALID_PARAMS', 'Extrusion height must be > 0');
        }
        JSON.parse(profileJson); // validate JSON
        return this._store('extrude', { profileJson, height });
    }

    revolveProfile(profileJson: string, angleDegrees: number): number {
        if (angleDegrees <= 0 || angleDegrees > 360) {
            throw new KernelError('INVALID_PARAMS', 'Revolution angle must be in (0, 360]');
        }
        JSON.parse(profileJson);
        return this._store('revolve', { profileJson, angleDegrees });
    }

    // -- Booleans --

    booleanUnion(id1: number, id2: number): number {
        this._require(id1);
        this._require(id2);
        return this._store('union', { id1, id2 });
    }

    booleanSubtract(id1: number, id2: number): number {
        this._require(id1);
        this._require(id2);
        return this._store('subtract', { id1, id2 });
    }

    booleanIntersect(id1: number, id2: number): number {
        this._require(id1);
        this._require(id2);
        return this._store('intersect', { id1, id2 });
    }

    // -- Modifiers --

    filletEdges(id: number, radius: number): number {
        this._require(id);
        if (radius <= 0) {
            throw new KernelError('INVALID_PARAMS', 'Fillet radius must be > 0');
        }
        return this._store('fillet', { id, radius });
    }

    chamferEdges(id: number, distance: number): number {
        this._require(id);
        if (distance <= 0) {
            throw new KernelError('INVALID_PARAMS', 'Chamfer distance must be > 0');
        }
        return this._store('chamfer', { id, distance });
    }

    // -- Queries --

    getTopology(id: number): string {
        const shape = this._require(id);
        return JSON.stringify({
            faceCount: 6,
            edgeCount: 12,
            vertexCount: 8,
            boundingBox: { xMin: 0, yMin: 0, zMin: 0, xMax: 1, yMax: 1, zMax: 1 },
            isValid: shape.params.invalid !== true,
        });
    }

    checkValidity(id: number): boolean {
        const shape = this._shapes.get(id);
        return shape !== undefined && shape.params.invalid !== true;
    }

    // -- Tessellation --

    tessellate(id: number, _linearDeflection: number, _angularDeflection: number): string {
        this._require(id);
        // Minimal triangle (not geometrically meaningful — mock only)
        return JSON.stringify({
            positions: [0, 0, 0, 1, 0, 0, 0, 1, 0],
            normals:   [0, 0, 1, 0, 0, 1, 0, 0, 1],
            indices:   [0, 1, 2],
        });
    }

    // -- Import / export --

    importStep(content: string): number {
        const result = JSON.parse(this.importStepDetailed(content, false, false, false, false, 1e-6)) as MockImportResult;
        if (result.shapeId === undefined) {
            throw new KernelError('IMPORT_FAILED', result.messageList[0]?.text ?? 'STEP import failed');
        }
        return result.shapeId;
    }

    importStepDetailed(
        content: string,
        heal: boolean,
        sew: boolean,
        fixSameParameter: boolean,
        fixSolid: boolean,
        _sewingTolerance: number,
    ): string {
        const trimmed = typeof content === 'string' ? content.trim() : '';
        if (trimmed.length === 0) {
            return JSON.stringify({
                readStatus: 'IFSelect_RetError',
                transferStatus: 'FAILED',
                rootCount: 0,
                transferredRootCount: 0,
                messageList: [{ phase: 'load', severity: 'fail', text: 'STEP content is empty' }],
                isValid: false,
                wasValidBeforeHealing: false,
                healed: false,
            } satisfies MockImportResult);
        }

        if (!content.includes('ISO-10303')) {
            return JSON.stringify({
                readStatus: 'IFSelect_RetFail',
                transferStatus: 'FAILED',
                rootCount: 1,
                transferredRootCount: 0,
                messageList: [{ phase: 'load', severity: 'fail', text: 'Content does not appear to be a valid STEP file' }],
                isValid: false,
                wasValidBeforeHealing: false,
                healed: false,
            } satisfies MockImportResult);
        }

        const wasValidBeforeHealing = !content.includes('MOCK_INVALID_SHAPE');
        const healedShape = !wasValidBeforeHealing && (heal || sew || fixSameParameter || fixSolid);
        const isValid = wasValidBeforeHealing || healedShape;
        const shapeId = this._store('imported', {
            content: content.slice(0, 64),
            invalid: !isValid,
        });

        return JSON.stringify({
            readStatus: 'IFSelect_RetDone',
            transferStatus: 'DONE',
            rootCount: 1,
            transferredRootCount: 1,
            messageList: [
                ...(!wasValidBeforeHealing
                    ? [{ phase: 'validation' as const, severity: isValid ? 'info' as const : 'warning' as const, text: isValid ? 'Healing produced a valid shape' : 'Imported shape is not valid according to BRepCheck_Analyzer' }]
                    : []),
            ],
            shapeId,
            isValid,
            wasValidBeforeHealing,
            healed: healedShape,
        } satisfies MockImportResult);
    }

    exportStep(id: number): string {
        this._require(id);
        return [
            'ISO-10303-21;',
            'HEADER;',
            "FILE_DESCRIPTION(('occt-kernel-wasm mock export'),'2;1');",
            "FILE_NAME('','',(''),(''),'','','');",
            "FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));",
            'ENDSEC;',
            'DATA;',
            'ENDSEC;',
            'END-ISO-10303-21;',
        ].join('\n');
    }

    // -- Memory --

    disposeShape(id: number): void {
        this._shapes.delete(id);
    }

    /** Returns the number of live shapes (for leak detection in tests). */
    liveShapeCount(): number {
        return this._shapes.size;
    }
}

// ---------------------------------------------------------------------------
// Module factory (mirrors the real Emscripten module factory signature)
// ---------------------------------------------------------------------------

export interface MockWasmModule {
    OcctKernel: new () => MockNativeKernel;
}

/** Creates a mock WASM module. Resolves immediately (no async IO needed). */
export default function createMockModule(): Promise<MockWasmModule> {
    return Promise.resolve({
        OcctKernel: MockNativeKernel,
    });
}
