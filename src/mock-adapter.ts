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
        this._require(id);
        return JSON.stringify({
            faceCount: 6,
            edgeCount: 12,
            vertexCount: 8,
            boundingBox: { xMin: 0, yMin: 0, zMin: 0, xMax: 1, yMax: 1, zMax: 1 },
            isValid: true,
        });
    }

    checkValidity(id: number): boolean {
        return this._shapes.has(id);
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
        if (!content || content.trim().length === 0) {
            throw new KernelError('IMPORT_FAILED', 'STEP content is empty');
        }
        if (!content.includes('ISO-10303')) {
            throw new KernelError('IMPORT_FAILED', 'Content does not appear to be a valid STEP file');
        }
        return this._store('imported', { content: content.slice(0, 64) });
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
