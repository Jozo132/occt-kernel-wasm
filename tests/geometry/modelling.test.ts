/**
 * Geometry regression tests.
 *
 * These tests verify modelling flows using the mock adapter, ensuring the
 * TypeScript layer correctly sequences operations and handles results.
 *
 * When the compiled WASM binary is available, the same tests are executed
 * against the real kernel (see tests/smoke/ for WASM smoke tests).
 */

import { OcctKernel } from '../../src/kernel';
import { MockNativeKernel } from '../../src/mock-adapter';
import type { WasmModule } from '../../src/kernel';

function makeMockModule(): WasmModule {
    return {
        OcctKernel: MockNativeKernel as unknown as new () => InstanceType<typeof MockNativeKernel>,
    } as unknown as WasmModule;
}

function makeKernel(): OcctKernel {
    return new OcctKernel(makeMockModule());
}

// ---------------------------------------------------------------------------
// End-to-end flow: create box → tessellate → export STEP
// ---------------------------------------------------------------------------

describe('box → tessellate → exportStep', () => {
    it('completes without error', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const mesh = k.tessellate({ shape: box, linearDeflection: 0.1 });
        const step = k.exportStep({ shape: box });

        expect(mesh.positions.length).toBeGreaterThan(0);
        expect(step).toContain('ISO-10303-21');
    });

    it('frees the shape after export', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        k.tessellate({ shape: box });
        k.exportStep({ shape: box });
        expect(() => k.disposeShape({ shape: box })).not.toThrow();
    });
});

// ---------------------------------------------------------------------------
// Boolean subtraction flow: box - cylinder
// ---------------------------------------------------------------------------

describe('box - cylinder boolean subtract', () => {
    it('produces a shape handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 20, dy: 20, dz: 20 });
        const cyl = k.createCylinder({ radius: 5, height: 25 });
        const result = k.booleanSubtract({ base: box, tool: cyl });
        expect(result.id).toBeGreaterThan(0);
    });

    it('tessellates the result', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 20, dy: 20, dz: 20 });
        const cyl = k.createCylinder({ radius: 5, height: 25 });
        const result = k.booleanSubtract({ base: box, tool: cyl });
        const mesh = k.tessellate({ shape: result });
        expect(mesh.positions.length).toBeGreaterThan(0);
    });
});

// ---------------------------------------------------------------------------
// Fillet flow: box → fillet → tessellate
// ---------------------------------------------------------------------------

describe('box → fillet → tessellate', () => {
    it('produces a mesh from a filleted box', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const filleted = k.filletEdges({ shape: box, radius: 1 });
        const mesh = k.tessellate({ shape: filleted });
        expect(mesh.positions.length).toBeGreaterThan(0);
        expect(mesh.indices.length % 3).toBe(0);
    });
});

// ---------------------------------------------------------------------------
// STEP round-trip
// ---------------------------------------------------------------------------

describe('STEP round-trip (export → import)', () => {
    const validStep = [
        'ISO-10303-21;',
        'HEADER;',
        "FILE_DESCRIPTION(('round-trip test'),'2;1');",
        "FILE_NAME('','',(''),(''),'','','');",
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));",
        'ENDSEC;',
        'DATA;',
        'ENDSEC;',
        'END-ISO-10303-21;',
    ].join('\n');

    it('imported shape can be tessellated', () => {
        const k = makeKernel();
        const imported = k.importStep({ content: validStep });
        const mesh = k.tessellate({ shape: imported });
        expect(mesh.positions.length).toBeGreaterThan(0);
    });

    it('export then re-import produces a valid handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 5, dy: 5, dz: 5 });
        const step = k.exportStep({ shape: box });
        // The mock export always returns valid STEP – re-import it
        const imported = k.importStep({ content: step });
        expect(imported.id).toBeGreaterThan(0);
    });
});

// ---------------------------------------------------------------------------
// Extrusion flow
// ---------------------------------------------------------------------------

describe('extrudeProfile flow', () => {
    const rectangleProfile = {
        segments: [
            { type: 'line' as const, start: [0, 0] as [number,number],   end: [20, 0]  as [number,number] },
            { type: 'line' as const, start: [20, 0] as [number,number],  end: [20, 10] as [number,number] },
            { type: 'line' as const, start: [20, 10] as [number,number], end: [0, 10]  as [number,number] },
            { type: 'line' as const, start: [0, 10]  as [number,number], end: [0, 0]   as [number,number] },
        ],
    };

    const bezierRectangleProfile = {
        segments: [
            { type: 'bezier' as const, controlPoints: [[0, 0] as [number, number], [6, 4] as [number, number], [14, 4] as [number, number], [20, 0] as [number, number]] },
            { type: 'line' as const, start: [20, 0] as [number,number], end: [20, 10] as [number,number] },
            { type: 'line' as const, start: [20, 10] as [number,number], end: [0, 10] as [number,number] },
            { type: 'line' as const, start: [0, 10] as [number,number], end: [0, 0] as [number,number] },
        ],
    };

    const bsplineRectangleProfile = {
        segments: [
            {
                type: 'bspline' as const,
                controlPoints: [[0, 0] as [number, number], [6, 4] as [number, number], [14, 4] as [number, number], [20, 0] as [number, number]],
                degree: 3,
                knots: [0, 1] as number[],
                multiplicities: [4, 4] as number[],
            },
            { type: 'line' as const, start: [20, 0] as [number,number], end: [20, 10] as [number,number] },
            { type: 'line' as const, start: [20, 10] as [number,number], end: [0, 10] as [number,number] },
            { type: 'line' as const, start: [0, 10] as [number,number], end: [0, 0] as [number,number] },
        ],
    };

    it('extrudes a rectangle profile', () => {
        const k = makeKernel();
        const solid = k.extrudeProfile({ profile: rectangleProfile, height: 15 });
        expect(solid.id).toBeGreaterThan(0);
    });

    it('extrudes a profile with holes in a custom plane', () => {
        const k = makeKernel();
        const solid = k.extrudeProfile({
            profile: {
                outer: rectangleProfile,
                holes: [{
                    segments: [
                        { type: 'line' as const, start: [5, 2] as [number,number], end: [15, 2] as [number,number] },
                        { type: 'line' as const, start: [15, 2] as [number,number], end: [15, 8] as [number,number] },
                        { type: 'line' as const, start: [15, 8] as [number,number], end: [5, 8] as [number,number] },
                        { type: 'line' as const, start: [5, 8] as [number,number], end: [5, 2] as [number,number] },
                    ],
                }],
            },
            plane: {
                origin: [0, 0, 10],
                normal: [0, 1, 0],
                xDirection: [1, 0, 0],
            },
            height: 15,
        });
        expect(solid.id).toBeGreaterThan(0);
    });

    it('tessellates an extruded profile', () => {
        const k = makeKernel();
        const solid = k.extrudeProfile({ profile: rectangleProfile, height: 15 });
        const mesh = k.tessellate({ shape: solid });
        expect(mesh.positions.length).toBeGreaterThan(0);
    });

    it('extrudes and tessellates a bezier-edged profile', () => {
        const k = makeKernel();
        const solid = k.extrudeProfile({ profile: bezierRectangleProfile, height: 15 });
        const mesh = k.tessellate({ shape: solid });
        expect(mesh.positions.length).toBeGreaterThan(0);
    });

    it('extrudes and tessellates a bspline-edged profile', () => {
        const k = makeKernel();
        const solid = k.extrudeProfile({ profile: bsplineRectangleProfile, height: 15 });
        const mesh = k.tessellate({ shape: solid });
        expect(mesh.positions.length).toBeGreaterThan(0);
    });

    it('can perform boolean subtract on extruded profile', () => {
        const k = makeKernel();
        const outer = k.extrudeProfile({ profile: rectangleProfile, height: 15 });
        const cyl = k.createCylinder({ radius: 4, height: 20 });
        const result = k.booleanSubtract({ base: outer, tool: cyl });
        expect(result.id).toBeGreaterThan(0);
    });

    it('can transform an extruded profile before tessellation', () => {
        const k = makeKernel();
        const solid = k.extrudeProfile({ profile: rectangleProfile, vector: [0, 0, 12] });
        const moved = k.transformShape({
            shape: solid,
            transform: {
                rotation: {
                    axisOrigin: [0, 0, 0],
                    axisDirection: [0, 0, 1],
                    angleDegrees: 30,
                },
                translation: [5, 0, 0],
            },
        });
        const mesh = k.tessellate({ shape: moved });
        expect(mesh.positions.length).toBeGreaterThan(0);
    });
});

// ---------------------------------------------------------------------------
// Memory management
// ---------------------------------------------------------------------------

describe('memory management', () => {
    it('disposes shapes without error', () => {
        const k = makeKernel();
        const shapes = [
            k.createBox({ dx: 1, dy: 1, dz: 1 }),
            k.createCylinder({ radius: 1, height: 1 }),
            k.createSphere({ radius: 1 }),
        ];
        for (const shape of shapes) {
            expect(() => k.disposeShape({ shape })).not.toThrow();
        }
    });

    it('boolean result can be disposed independently of inputs', () => {
        const k = makeKernel();
        const a = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const b = k.createBox({ dx: 5, dy: 5, dz: 5 });
        const result = k.booleanUnion({ base: a, tool: b });

        // Dispose inputs first
        k.disposeShape({ shape: a });
        k.disposeShape({ shape: b });

        // Result should still be tessellatable (in mock – real kernel depends on shape copy semantics)
        expect(() => k.tessellate({ shape: result })).not.toThrow();
        k.disposeShape({ shape: result });
    });
});
