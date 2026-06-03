/**
 * Unit tests for the OcctKernel TypeScript wrapper (using MockNativeKernel).
 *
 * These tests exercise parameter validation, error propagation, and the shape
 * of outputs – all without a compiled WASM binary.
 */

import { OcctKernel } from '../../src/kernel';
import { KernelError } from '../../src/errors';
import { MockNativeKernel } from '../../src/mock-adapter';
import type { WasmModule } from '../../src/kernel';
import type { ShapeHandle } from '../../src/types';

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function makeMockModule(): WasmModule {
    return {
        OcctKernel: MockNativeKernel as unknown as new () => InstanceType<typeof MockNativeKernel>,
    } as unknown as WasmModule;
}

function makeKernel(): OcctKernel {
    return new OcctKernel(makeMockModule());
}

function makeKernelWithNative(nativeCtor: new () => InstanceType<typeof MockNativeKernel>): OcctKernel {
    return new OcctKernel({ OcctKernel: nativeCtor } as unknown as WasmModule);
}

function invalidHandle(handle: ShapeHandle, id = 9999): ShapeHandle {
    return { ...handle, id };
}

function foreignHandle(handle: ShapeHandle): ShapeHandle {
    return { ...handle, sessionId: `${handle.sessionId}_other` };
}

// ---------------------------------------------------------------------------
// createBox
// ---------------------------------------------------------------------------

describe('createBox', () => {
    it('returns a shape handle with a positive integer id', () => {
        const k = makeKernel();
        const handle = k.createBox({ dx: 10, dy: 5, dz: 2 });
        expect(handle.id).toBeGreaterThan(0);
        expect(Number.isInteger(handle.id)).toBe(true);
        expect(handle.sessionId).toMatch(/^session_/);
    });

    it('each call returns a unique id', () => {
        const k = makeKernel();
        const a = k.createBox({ dx: 1, dy: 1, dz: 1 });
        const b = k.createBox({ dx: 2, dy: 2, dz: 2 });
        expect(a.id).not.toBe(b.id);
    });

    it('throws KernelError when dx <= 0', () => {
        const k = makeKernel();
        expect(() => k.createBox({ dx: 0, dy: 1, dz: 1 })).toThrow(KernelError);
        expect(() => k.createBox({ dx: -5, dy: 1, dz: 1 })).toThrow(KernelError);
    });

    it('throws KernelError with code INVALID_PARAMS when dy <= 0', () => {
        const k = makeKernel();
        try {
            k.createBox({ dx: 1, dy: 0, dz: 1 });
            fail('should have thrown');
        } catch (err) {
            expect(err).toBeInstanceOf(KernelError);
            expect((err as KernelError).code).toBe('INVALID_PARAMS');
        }
    });

    it('handle is frozen (immutable)', () => {
        const k = makeKernel();
        const handle = k.createBox({ dx: 1, dy: 1, dz: 1 });
        expect(Object.isFrozen(handle)).toBe(true);
    });
});

// ---------------------------------------------------------------------------
// createCylinder
// ---------------------------------------------------------------------------

describe('createCylinder', () => {
    it('returns a valid handle', () => {
        const k = makeKernel();
        const h = k.createCylinder({ radius: 5, height: 10 });
        expect(h.id).toBeGreaterThan(0);
    });

    it('throws KernelError when radius <= 0', () => {
        const k = makeKernel();
        expect(() => k.createCylinder({ radius: 0, height: 10 })).toThrow(KernelError);
        expect(() => k.createCylinder({ radius: -1, height: 10 })).toThrow(KernelError);
    });

    it('throws KernelError when height <= 0', () => {
        const k = makeKernel();
        expect(() => k.createCylinder({ radius: 5, height: 0 })).toThrow(KernelError);
    });
});

// ---------------------------------------------------------------------------
// createSphere
// ---------------------------------------------------------------------------

describe('createSphere', () => {
    it('returns a valid handle', () => {
        const k = makeKernel();
        const h = k.createSphere({ radius: 3 });
        expect(h.id).toBeGreaterThan(0);
    });

    it('throws KernelError when radius <= 0', () => {
        const k = makeKernel();
        expect(() => k.createSphere({ radius: 0 })).toThrow(KernelError);
        expect(() => k.createSphere({ radius: -10 })).toThrow(KernelError);
    });
});

// ---------------------------------------------------------------------------
// extrudeProfile
// ---------------------------------------------------------------------------

describe('extrudeProfile', () => {
    const squareProfile = {
        segments: [
            { type: 'line' as const, start: [0, 0] as [number,number], end: [10, 0]  as [number,number] },
            { type: 'line' as const, start: [10, 0] as [number,number], end: [10, 10] as [number,number] },
            { type: 'line' as const, start: [10, 10] as [number,number], end: [0, 10]  as [number,number] },
            { type: 'line' as const, start: [0, 10]  as [number,number], end: [0, 0]   as [number,number] },
        ],
    };

    const bezierProfile = {
        segments: [
            { type: 'bezier' as const, controlPoints: [[0, 0] as [number, number], [3, 4] as [number, number], [7, 4] as [number, number], [10, 0] as [number, number]] },
            { type: 'line' as const, start: [10, 0] as [number,number], end: [10, 10] as [number,number] },
            { type: 'line' as const, start: [10, 10] as [number,number], end: [0, 10] as [number,number] },
            { type: 'line' as const, start: [0, 10] as [number,number], end: [0, 0] as [number,number] },
        ],
    };

    const bsplineProfile = {
        segments: [
            {
                type: 'bspline' as const,
                controlPoints: [[0, 0] as [number, number], [3, 4] as [number, number], [7, 4] as [number, number], [10, 0] as [number, number]],
                degree: 3,
                knots: [0, 1] as number[],
                multiplicities: [4, 4] as number[],
            },
            { type: 'line' as const, start: [10, 0] as [number,number], end: [10, 10] as [number,number] },
            { type: 'line' as const, start: [10, 10] as [number,number], end: [0, 10] as [number,number] },
            { type: 'line' as const, start: [0, 10] as [number,number], end: [0, 0] as [number,number] },
        ],
    };

    const profileWithHole = {
        outer: squareProfile,
        holes: [{
            segments: [
                { type: 'line' as const, start: [3, 3] as [number,number], end: [7, 3] as [number,number] },
                { type: 'line' as const, start: [7, 3] as [number,number], end: [7, 7] as [number,number] },
                { type: 'line' as const, start: [7, 7] as [number,number], end: [3, 7] as [number,number] },
                { type: 'line' as const, start: [3, 7] as [number,number], end: [3, 3] as [number,number] },
            ],
        }],
    };

    it('returns a valid handle for a legacy single-wire profile', () => {
        const k = makeKernel();
        const h = k.extrudeProfile({ profile: squareProfile, height: 5 });
        expect(h.id).toBeGreaterThan(0);
    });

    it('supports multi-wire profiles with holes and an explicit sketch plane', () => {
        const k = makeKernel();
        const h = k.extrudeProfile({
            profile: profileWithHole,
            plane: {
                origin: [0, 0, 5],
                normal: [0, 1, 0],
                xDirection: [1, 0, 0],
            },
            height: 12,
        });
        expect(h.id).toBeGreaterThan(0);
    });

    it('supports explicit extrusion vectors', () => {
        const k = makeKernel();
        const h = k.extrudeProfile({ profile: squareProfile, vector: [0, 0, 8] });
        expect(h.id).toBeGreaterThan(0);
    });

    it('supports bezier wire segments', () => {
        const k = makeKernel();
        const h = k.extrudeProfile({ profile: bezierProfile, height: 5 });
        expect(h.id).toBeGreaterThan(0);
    });

    it('supports bspline wire segments', () => {
        const k = makeKernel();
        const h = k.extrudeProfile({ profile: bsplineProfile, height: 5 });
        expect(h.id).toBeGreaterThan(0);
    });

    it('throws KernelError when height and vector are both provided', () => {
        const k = makeKernel();
        expect(() => k.extrudeProfile({ profile: squareProfile, height: 5, vector: [0, 0, 5] })).toThrow(KernelError);
    });

    it('throws KernelError when plane.xDirection is parallel to plane.normal', () => {
        const k = makeKernel();
        expect(() => k.extrudeProfile({
            profile: squareProfile,
            plane: {
                origin: [0, 0, 0],
                normal: [0, 0, 1],
                xDirection: [0, 0, 2],
            },
            height: 5,
        })).toThrow(KernelError);
    });

    it('throws KernelError when profile has no wires', () => {
        const k = makeKernel();
        expect(() => k.extrudeProfile({ profile: { wires: [] }, height: 5 })).toThrow(KernelError);
    });

    it('throws KernelError for invalid bezier control points', () => {
        const k = makeKernel();
        expect(() => k.extrudeProfile({
            profile: { segments: [{ type: 'bezier', controlPoints: [[0, 0] as [number, number]] }] },
            height: 5,
        })).toThrow(KernelError);
    });

    it('throws KernelError for an invalid bspline knot contract', () => {
        const k = makeKernel();
        expect(() => k.extrudeProfile({
            profile: {
                segments: [{
                    type: 'bspline',
                    controlPoints: [[0, 0], [3, 4], [7, 4], [10, 0]],
                    degree: 3,
                    knots: [0, 1],
                    multiplicities: [3, 3],
                }],
            },
            height: 5,
        })).toThrow(KernelError);
    });
});

// ---------------------------------------------------------------------------
// revolveProfile
// ---------------------------------------------------------------------------

describe('revolveProfile', () => {
    const profile = {
        segments: [
            { type: 'line' as const, start: [2, 0] as [number,number], end: [5, 0]  as [number,number] },
            { type: 'line' as const, start: [5, 0] as [number,number], end: [5, 10] as [number,number] },
            { type: 'line' as const, start: [5, 10] as [number,number], end: [2, 10] as [number,number] },
            { type: 'line' as const, start: [2, 10] as [number,number], end: [2, 0]  as [number,number] },
        ],
    };

    it('returns a handle for a 360-degree revolution', () => {
        const k = makeKernel();
        const h = k.revolveProfile({ profile, angleDegrees: 360 });
        expect(h.id).toBeGreaterThan(0);
    });

    it('supports an arbitrary revolve axis', () => {
        const k = makeKernel();
        const h = k.revolveProfile({
            profile,
            angleDegrees: 90,
            axisOrigin: [10, 0, 0],
            axisDirection: [0, 0, 1],
        });
        expect(h.id).toBeGreaterThan(0);
    });

    it('throws KernelError when angleDegrees is out of range', () => {
        const k = makeKernel();
        expect(() => k.revolveProfile({ profile, angleDegrees: 0 })).toThrow(KernelError);
        expect(() => k.revolveProfile({ profile, angleDegrees: 361 })).toThrow(KernelError);
        expect(() => k.revolveProfile({ profile, angleDegrees: -90 })).toThrow(KernelError);
    });

    it('throws KernelError when axisDirection is the zero vector', () => {
        const k = makeKernel();
        expect(() => k.revolveProfile({ profile, angleDegrees: 180, axisDirection: [0, 0, 0] })).toThrow(KernelError);
    });

    it('throws KernelError when profile has no segments', () => {
        const k = makeKernel();
        expect(() => k.revolveProfile({ profile: { wires: [] }, angleDegrees: 360 })).toThrow(KernelError);
    });
});

// ---------------------------------------------------------------------------
// transformShape
// ---------------------------------------------------------------------------

describe('transformShape', () => {
    it('returns a new handle for a translation', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 2, dy: 2, dz: 2 });
        const moved = k.transformShape({ shape: box, transform: { translation: [5, 0, 0] } });
        expect(moved.id).toBeGreaterThan(0);
        expect(moved.id).not.toBe(box.id);
    });

    it('returns a new handle for rotation and translation', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 2, dy: 2, dz: 2 });
        const moved = k.transformShape({
            shape: box,
            transform: {
                rotation: {
                    axisOrigin: [0, 0, 0],
                    axisDirection: [0, 0, 1],
                    angleDegrees: 45,
                },
                translation: [1, 2, 3],
            },
        });
        expect(moved.id).toBeGreaterThan(0);
    });

    it('throws KernelError for an empty transform', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 2, dy: 2, dz: 2 });
        expect(() => k.transformShape({ shape: box, transform: {} })).toThrow(KernelError);
    });

    it('throws KernelError for an invalid shape handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 2, dy: 2, dz: 2 });
        expect(() => k.transformShape({ shape: invalidHandle(box), transform: { translation: [1, 0, 0] } })).toThrow(KernelError);
    });
});

// ---------------------------------------------------------------------------
// Boolean operations
// ---------------------------------------------------------------------------

describe('booleanUnion', () => {
    it('returns a new handle', () => {
        const k = makeKernel();
        const a = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const b = k.createBox({ dx: 5, dy: 5, dz: 5 });
        const result = k.booleanUnion({ base: a, tool: b });
        expect(result.id).toBeGreaterThan(0);
        expect(result.id).not.toBe(a.id);
        expect(result.id).not.toBe(b.id);
    });

    it('throws KernelError for an invalid base handle', () => {
        const k = makeKernel();
        const b = k.createBox({ dx: 5, dy: 5, dz: 5 });
        const a = k.createBox({ dx: 10, dy: 10, dz: 10 });
        expect(() => k.booleanUnion({ base: invalidHandle(a), tool: b })).toThrow(KernelError);
    });

    it('throws KernelError for an invalid tool handle', () => {
        const k = makeKernel();
        const a = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const b = k.createBox({ dx: 5, dy: 5, dz: 5 });
        expect(() => k.booleanUnion({ base: a, tool: invalidHandle(b) })).toThrow(KernelError);
    });
});

describe('booleanSubtract', () => {
    it('returns a new handle', () => {
        const k = makeKernel();
        const a = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const b = k.createBox({ dx: 5, dy: 5, dz: 15 });
        const result = k.booleanSubtract({ base: a, tool: b });
        expect(result.id).toBeGreaterThan(0);
    });
});

describe('booleanIntersect', () => {
    it('returns a new handle', () => {
        const k = makeKernel();
        const a = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const b = k.createBox({ dx: 5, dy: 5, dz: 20 });
        const result = k.booleanIntersect({ base: a, tool: b });
        expect(result.id).toBeGreaterThan(0);
    });
});

// ---------------------------------------------------------------------------
// filletEdges / chamferEdges
// ---------------------------------------------------------------------------

describe('filletEdges', () => {
    it('returns a new handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const filleted = k.filletEdges({ shape: box, radius: 1 });
        expect(filleted.id).toBeGreaterThan(0);
        expect(filleted.id).not.toBe(box.id);
    });

    it('throws KernelError when radius <= 0', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        expect(() => k.filletEdges({ shape: box, radius: 0 })).toThrow(KernelError);
        expect(() => k.filletEdges({ shape: box, radius: -1 })).toThrow(KernelError);
    });

    it('throws KernelError for invalid shape handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 2, dy: 2, dz: 2 });
        expect(() => k.filletEdges({ shape: invalidHandle(box), radius: 1 })).toThrow(KernelError);
    });
});

describe('chamferEdges', () => {
    it('returns a new handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const chamfered = k.chamferEdges({ shape: box, distance: 0.5 });
        expect(chamfered.id).toBeGreaterThan(0);
    });

    it('throws KernelError when distance <= 0', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        expect(() => k.chamferEdges({ shape: box, distance: 0 })).toThrow(KernelError);
    });
});

describe('structured blend operations', () => {
    const expectBlendFacesResolveToFinalTopology = (
        blendFaces: ReadonlyArray<{
            stableHash: string | null;
            topoFaceId?: number;
            finalOutputFaceRef?: { stableHash?: string; topoFaceId?: number };
            finalOutputFaceRefs?: readonly { stableHash?: string; topoFaceId?: number }[];
        }>,
        faces: ReadonlyArray<{ id: number; stableHash?: string }> | undefined,
    ) => {
        expect(blendFaces.length).toBeGreaterThan(0);
        const finalFaces = faces ?? [];
        for (const blendFace of blendFaces) {
            const refs: readonly { stableHash?: string | null; topoFaceId?: number }[] = blendFace.finalOutputFaceRefs
                ?? (blendFace.finalOutputFaceRef !== undefined ? [blendFace.finalOutputFaceRef] : [blendFace]);
            expect(refs.length).toBeGreaterThan(0);
            for (const ref of refs) {
                const resolves = finalFaces.some((face) =>
                    (typeof ref.stableHash === 'string' && face.stableHash === ref.stableHash)
                    || (Number.isInteger(ref.topoFaceId) && face.id === ref.topoFaceId),
                );
                expect(resolves).toBe(true);
            }
        }
    };

    it('returns exact fillet lineage and a resident shape handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const result = k.filletEdgesWithSpec({
            shape: box,
            spec: {
                schemaVersion: 1,
                edges: [{ topoId: 1, radius: 1.25 }],
                blendShape: 'rational',
                continuity: 'C1',
            },
        });

        expect(result.shape.id).toBe(result.shapeId);
        expect(result.shape.id).not.toBe(box.id);
        expect(result.status.isExact).toBe(true);
        expect(result.blendFaces[0]).toMatchObject({ kind: 'filletFace' });
        expect(result.topology.revisionId).toBe(result.revision.revisionId);
        expectBlendFacesResolveToFinalTopology(result.blendFaces, k.getTopology(result.shape).faces);
    });

    it('returns exact chamfer lineage with reference-face parameters', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const result = k.chamferEdgesWithSpec({
            shape: box,
            spec: {
                schemaVersion: 1,
                mode: 'twoDistance',
                edges: [{ topoId: 1, distance1: 0.5, distance2: 1.0, referenceFace: { topoId: 1 } }],
            },
        });

        expect(result.shape.id).toBeGreaterThan(0);
        expect(result.blendFaces[0]).toMatchObject({ kind: 'chamferFace' });
        expect(result.lineage.deleted[0]).toMatch(/^E:/);
        expectBlendFacesResolveToFinalTopology(result.blendFaces, k.getTopology(result.shape).faces);
    });

    it('rejects empty native blend results instead of returning an invalid shape handle', () => {
        class EmptyBlendNativeKernel extends MockNativeKernel {
            filletEdgesWithSpec(): string {
                return '{}';
            }

            chamferEdgesWithSpec(): string {
                return '{}';
            }
        }

        const k = makeKernelWithNative(EmptyBlendNativeKernel);
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });

        for (const run of [
            () => k.filletEdgesWithSpec({ shape: box, spec: { schemaVersion: 1, edges: [{ topoId: 1, radius: 1 }] } }),
            () => k.chamferEdgesWithSpec({ shape: box, spec: { schemaVersion: 1, edges: [{ topoId: 1, distance: 1 }] } }),
        ]) {
            try {
                run();
                fail('should have thrown');
            } catch (err) {
                expect(err).toBeInstanceOf(KernelError);
                expect((err as KernelError).code).toBe('OPERATION_FAILED');
                expect((err as KernelError).detail).toContain('valid shapeId');
            }
        }
    });

    it('decodes opaque native numeric exceptions through the native last-error bridge', () => {
        class OpaqueChamferFailureNativeKernel extends MockNativeKernel {
            private lastError = '';

            clearLastError(): void {
                this.lastError = '';
            }

            getLastError(): string {
                return this.lastError;
            }

            chamferEdgesWithSpec(): string {
                this.lastError = JSON.stringify({
                    code: 'OPERATION_FAILED',
                    detail: JSON.stringify({
                        operation: 'chamferEdges',
                        message: 'BRepFilletAPI_MakeChamfer failed',
                        requestedEdgeRefs: [{ topoId: 1, stableHash: 'E:test' }],
                    }),
                });
                throw 15690360;
            }
        }

        const k = makeKernelWithNative(OpaqueChamferFailureNativeKernel);
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });

        try {
            k.chamferEdgesWithSpec({ shape: box, spec: { schemaVersion: 1, edges: [{ topoId: 1, distance: 1 }] } });
            fail('should have thrown');
        } catch (err) {
            expect(err).toBeInstanceOf(KernelError);
            expect((err as KernelError).code).toBe('OPERATION_FAILED');
            expect(JSON.parse((err as KernelError).detail)).toMatchObject({
                operation: 'chamferEdges',
                message: 'BRepFilletAPI_MakeChamfer failed',
            });
        }
    });

    it('throws a structured unsupported error for partial-edge fillets', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });

        try {
            k.filletEdgesWithSpec({
                shape: box,
                spec: {
                    schemaVersion: 1,
                    edges: [{ topoId: 1, radius: 1, limits: { start: 0.2, end: 0.8 } }],
                },
            });
            fail('should have thrown');
        } catch (err) {
            expect(err).toBeInstanceOf(KernelError);
            expect((err as KernelError).code).toBe('INVALID_PARAMS');
            expect(JSON.parse((err as KernelError).detail)).toMatchObject({
                operation: 'filletEdges',
                unsupportedFeature: 'fillet.partialEdge',
            });
        }
    });
});

describe('feature preview', () => {
    it('returns a lightweight mesh and blend lineage without applying the feature to the source', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const preview = k.previewFeature({
            operation: 'filletEdges',
            params: {
                shape: box,
                spec: { schemaVersion: 1, edges: [{ topoId: 1, radius: 1 }] },
            },
            includeWireframe: true,
        });

        expect(preview.operation).toBe('filletEdges');
        expect(preview.mesh?.positions).toBeInstanceOf(Float32Array);
        expect(preview.mesh?.triangleStableHashes).toBeUndefined();
        expect(preview.wireframe?.length).toBeGreaterThan(0);
        expect(preview.blendFaces?.[0]).toMatchObject({ kind: 'filletFace' });
        expect(preview.previewShape).toBeUndefined();
        expect(k.checkValidity(box)).toBe(true);
    });

    it('can retain the temporary preview shape for explicit caller inspection', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const preview = k.previewFeature({
            operation: 'transformShape',
            params: { shape: box, transform: { translation: [1, 0, 0] } },
            retainPreviewShape: true,
            includeTopology: true,
        });

        expect(preview.previewShape?.id).toBeGreaterThan(0);
        expect(preview.previewShape?.id).not.toBe(box.id);
        expect(preview.topology?.faceCount).toBeGreaterThan(0);
        expect(k.checkValidity(preview.previewShape!)).toBe(true);

        k.disposeShape({ shape: preview.previewShape! });
    });

    it('can return wireframe-only feedback for realtime editing loops', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const preview = k.previewFeature({
            operation: 'chamferEdges',
            params: {
                shape: box,
                spec: { schemaVersion: 1, edges: [{ topoId: 1, distance: 0.5 }] },
            },
            includeMesh: false,
            includeWireframe: true,
        });

        expect(preview.mesh).toBeUndefined();
        expect(preview.wireframe?.length).toBeGreaterThan(0);
        expect(preview.status?.isExact).toBe(true);
    });
});

describe('structured profile extrusion operations', () => {
    const squareProfile = {
        segments: [
            { type: 'line' as const, start: [0, 0] as [number, number], end: [10, 0] as [number, number] },
            { type: 'line' as const, start: [10, 0] as [number, number], end: [10, 10] as [number, number] },
            { type: 'line' as const, start: [10, 10] as [number, number], end: [0, 10] as [number, number] },
            { type: 'line' as const, start: [0, 10] as [number, number], end: [0, 0] as [number, number] },
        ],
    };

    it('applies a drafted additive extrude feature from a versioned spec', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const result = k.extrudeProfileWithSpec({
            shape: box,
            profile: squareProfile,
            spec: {
                schemaVersion: 1,
                direction: [0, 0, 1],
                reverseDirection: true,
                draftAngleDegrees: -3,
                plane: {
                    origin: [0, 0, 5],
                    normal: [0, 0, 1],
                    xDirection: [1, 0, 0],
                },
                extent: {
                    type: 'blind',
                    distance: 6,
                },
            },
        });

        expect(result.id).toBeGreaterThan(0);
        expect(result.id).not.toBe(box.id);
    });

    it('cuts a base shape up to an offset from a referenced surface', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const target = k.createBox({ dx: 20, dy: 20, dz: 20 });
        const result = k.extrudeCutProfileWithSpec({
            shape: box,
            profile: squareProfile,
            spec: {
                schemaVersion: 1,
                plane: {
                    origin: [0, 0, 0],
                    normal: [0, 0, 1],
                    xDirection: [1, 0, 0],
                },
                extent: {
                    type: 'offsetFromSurface',
                    offset: 0.5,
                    surface: {
                        shape: target,
                        face: { topoId: 1 },
                    },
                },
            },
        });

        expect(result.id).toBeGreaterThan(0);
        expect(result.id).not.toBe(box.id);
    });

    it('throws when both draft angle unit fields are provided in a structured spec', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });

        expect(() => k.extrudeProfileWithSpec({
            shape: box,
            profile: squareProfile,
            spec: {
                schemaVersion: 1,
                draftAngleRadians: 0.1,
                draftAngleDegrees: 5,
                extent: {
                    type: 'throughAll',
                },
            },
        })).toThrow(KernelError);
    });
});

describe('structured profile revolve operations', () => {
    const profile = {
        segments: [
            { type: 'line' as const, start: [2, 0] as [number, number], end: [5, 0] as [number, number] },
            { type: 'line' as const, start: [5, 0] as [number, number], end: [5, 10] as [number, number] },
            { type: 'line' as const, start: [5, 10] as [number, number], end: [2, 10] as [number, number] },
            { type: 'line' as const, start: [2, 10] as [number, number], end: [2, 0] as [number, number] },
        ],
    };

    it('applies an additive revolve feature with signed angle and sliding edges', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const result = k.revolveProfileWithSpec({
            shape: box,
            profile,
            spec: {
                schemaVersion: 1,
                plane: {
                    origin: [0, 0, 0],
                    normal: [0, 0, 1],
                    xDirection: [1, 0, 0],
                },
                axisOrigin: [0, 0, 0],
                axisDirection: [0, 1, 0],
                slidingEdges: [{ profileEdgeIndex: 1, face: { topoId: 1 } }],
                extent: {
                    type: 'angle',
                    angleDegrees: -180,
                },
            },
        });

        expect(result.id).toBeGreaterThan(0);
        expect(result.id).not.toBe(box.id);
    });

    it('cuts a base shape up to a referenced surface at a limiting angle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const target = k.createBox({ dx: 20, dy: 20, dz: 20 });
        const result = k.revolveProfileWithSpec({
            shape: box,
            cut: true,
            profile,
            spec: {
                schemaVersion: 1,
                axisOrigin: [0, 0, 0],
                axisDirection: [0, 0, 1],
                extent: {
                    type: 'upToSurfaceAtAngle',
                    angleDegrees: 90,
                    surface: {
                        shape: target,
                        face: { topoId: 1 },
                    },
                },
            },
        });

        expect(result.id).toBeGreaterThan(0);
        expect(result.id).not.toBe(box.id);
        expect(k.getRevisionInfo(result).operationType).toBe('revolveCutFeature');
    });

    it('throws when both angle unit fields are provided in a structured revolve extent', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });

        expect(() => k.revolveProfileWithSpec({
            shape: box,
            profile,
            spec: {
                schemaVersion: 1,
                extent: {
                    type: 'angle',
                    angleRadians: Math.PI / 2,
                    angleDegrees: 90,
                },
            },
        })).toThrow(KernelError);
    });
});

describe('structured sweep operations', () => {
    const profile = {
        segments: [
            { type: 'line' as const, start: [0, 0] as [number, number], end: [8, 0] as [number, number] },
            { type: 'line' as const, start: [8, 0] as [number, number], end: [8, 8] as [number, number] },
            { type: 'line' as const, start: [8, 8] as [number, number], end: [0, 8] as [number, number] },
            { type: 'line' as const, start: [0, 8] as [number, number], end: [0, 0] as [number, number] },
        ],
    };

    const spine = {
        segments: [
            { type: 'line' as const, start: [0, 0, 0] as [number, number, number], end: [0, 0, 20] as [number, number, number] },
        ],
    };

    it('applies an additive sweep feature with trihedron controls', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const result = k.sweepProfileWithSpec({
            shape: box,
            profile,
            spec: {
                schemaVersion: 1,
                spine,
                trihedronMode: { type: 'discrete' },
                sectionWithCorrection: true,
                solid: true,
                maxDegree: 9,
                maxSegments: 16,
            },
        });

        expect(result.id).toBeGreaterThan(0);
        expect(result.id).not.toBe(box.id);
        expect(k.getRevisionInfo(result).operationType).toBe('sweepFeature');
    });

    it('cuts a base shape through the sweep cut flag', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const result = k.sweepProfileWithSpec({
            shape: box,
            cut: true,
            profile,
            spec: {
                schemaVersion: 1,
                spine,
                solid: true,
            },
        });

        expect(result.id).toBeGreaterThan(0);
        expect(k.getRevisionInfo(result).operationType).toBe('sweepCutFeature');
    });
});

describe('structured loft operations', () => {
    const baseProfile = {
        segments: [
            { type: 'line' as const, start: [0, 0] as [number, number], end: [6, 0] as [number, number] },
            { type: 'line' as const, start: [6, 0] as [number, number], end: [6, 6] as [number, number] },
            { type: 'line' as const, start: [6, 6] as [number, number], end: [0, 6] as [number, number] },
            { type: 'line' as const, start: [0, 6] as [number, number], end: [0, 0] as [number, number] },
        ],
    };

    const topProfile = {
        segments: [
            { type: 'line' as const, start: [1, 1] as [number, number], end: [5, 1] as [number, number] },
            { type: 'line' as const, start: [5, 1] as [number, number], end: [5, 5] as [number, number] },
            { type: 'line' as const, start: [5, 5] as [number, number], end: [1, 5] as [number, number] },
            { type: 'line' as const, start: [1, 5] as [number, number], end: [1, 1] as [number, number] },
        ],
    };

    it('applies an additive loft feature with mixed section kinds', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const result = k.loftWithSpec({
            shape: box,
            sections: [
                { type: 'profile', profile: baseProfile, plane: { origin: [0, 0, 0], normal: [0, 0, 1], xDirection: [1, 0, 0] } },
                { type: 'wire', wire: { segments: [{ type: 'line', start: [0, 0, 10], end: [6, 0, 10] }, { type: 'line', start: [6, 0, 10], end: [6, 6, 10] }, { type: 'line', start: [6, 6, 10], end: [0, 6, 10] }, { type: 'line', start: [0, 6, 10], end: [0, 0, 10] }] } },
                { type: 'point', point: [3, 3, 16] },
            ],
            spec: {
                schemaVersion: 1,
                solid: true,
                ruled: false,
                smoothing: true,
                parametrization: 'centripetal',
                continuity: 'C1',
                maxDegree: 8,
            },
        });

        expect(result.id).toBeGreaterThan(0);
        expect(k.getRevisionInfo(result).operationType).toBe('loftFeature');
    });

    it('cuts a base shape through the loft cut flag', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const result = k.loftWithSpec({
            shape: box,
            cut: true,
            sections: [
                { type: 'profile', profile: baseProfile, plane: { origin: [0, 0, 0], normal: [0, 0, 1], xDirection: [1, 0, 0] } },
                { type: 'profile', profile: topProfile, plane: { origin: [0, 0, 12], normal: [0, 0, 1], xDirection: [1, 0, 0] } },
            ],
            spec: {
                schemaVersion: 1,
                solid: true,
                ruled: true,
            },
        });

        expect(result.id).toBeGreaterThan(0);
        expect(k.getRevisionInfo(result).operationType).toBe('loftCutFeature');
    });
});

describe('exact subshape evaluation APIs', () => {
    it('evaluates, samples, and describes an edge without tessellation', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });

        const evaluated = k.evaluateEdge({ shape: box, edge: { topoId: 1 }, t: 0.5 });
        const samples = k.sampleEdge({ shape: box, edge: { topoId: 1 }, count: 3 });
        const curve = k.getEdgeCurve({ shape: box, edge: { topoId: 1 } });

        expect(evaluated.point).toEqual([0.5, 0, 0]);
        expect(samples.samples).toHaveLength(3);
        expect(curve.curveType).toBe('line');
        expect(curve.line?.direction).toEqual([1, 0, 0]);
    });

    it('evaluates a face point and normal without tessellation', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });

        const evaluated = k.evaluateFace({ shape: box, face: { topoId: 1 }, u: 0.25, v: 0.75 });

        expect(evaluated.surfaceType).toBe('plane');
        expect(evaluated.point).toEqual([0.25, 0.75, 0]);
        expect(evaluated.normal).toEqual([0, 0, 1]);
    });
});

// ---------------------------------------------------------------------------
// getTopology / checkValidity
// ---------------------------------------------------------------------------

describe('getTopology', () => {
    it('returns a topology result with required fields', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const topo = k.getTopology(box);

        expect(typeof topo.faceCount).toBe('number');
        expect(typeof topo.edgeCount).toBe('number');
        expect(typeof topo.vertexCount).toBe('number');
        expect(typeof topo.isValid).toBe('boolean');
        expect(topo.boundingBox).toBeDefined();
        expect(typeof topo.boundingBox.xMin).toBe('number');
        expect(typeof topo.boundingBox.xMax).toBe('number');
    });

    it('returns additive revision and subshape metadata when available', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const topo = k.getTopology(box);

        expect(topo.revisionId).toMatch(/^rev_/);
        expect(topo.topologyHash).toMatch(/^T:/);
        expect(topo.historySchemaVersion).toBe(1);
        expect(topo.faces).toHaveLength(topo.faceCount);
        expect(topo.edges).toHaveLength(topo.edgeCount);
        expect(topo.vertices).toHaveLength(topo.vertexCount);
        expect(topo.faces?.[0]?.stableHash).toMatch(/^F:/);
        expect(topo.edges?.[0]?.topoFaceIds?.length).toBeGreaterThan(0);
        expect(topo.shapeType).toBe('solid');
        expect(topo.solidCount).toBe(1);
        expect(topo.shellCount).toBe(1);
        expect(topo.wireCount).toBeGreaterThan(0);
        expect(topo.solids?.[0]?.shellIds).toEqual([1]);
        expect(topo.shells?.[0]?.faceIds?.length).toBe(topo.faceCount);
        expect(topo.deletedEntities).toEqual([]);
    });

    it('throws KernelError for invalid handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        expect(() => k.getTopology(invalidHandle(box))).toThrow(KernelError);
    });
});

describe('session ownership', () => {
    it('throws SESSION_MISMATCH when a handle crosses kernel instances', () => {
        const firstKernel = makeKernel();
        const secondKernel = makeKernel();
        const box = firstKernel.createBox({ dx: 1, dy: 1, dz: 1 });

        expect(() => secondKernel.getTopology(box)).toThrow(KernelError);
        try {
            secondKernel.getTopology(box);
            fail('should have thrown');
        } catch (error) {
            expect((error as KernelError).code).toBe('SESSION_MISMATCH');
        }
    });

    it('treats altered handles with a different session as a session mismatch', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 1, dy: 1, dz: 1 });
        expect(() => k.getTopology(foreignHandle(box))).toThrow(KernelError);
    });
});

describe('getCapabilities', () => {
    it('reports implemented feature-edge, history, and stable naming contracts', () => {
        const k = makeKernel();
        const capabilities = k.getCapabilities();

        expect(capabilities.featureEdgesV1).toBe(true);
        expect(capabilities.rawEdgeSegmentsV1).toBe(true);
        expect(capabilities.featurePreviewV1).toBe(true);
        expect(capabilities.tessellationOptionsV1).toBe(true);
        expect(capabilities.triangleNormalsV1).toBe(true);
        expect(capabilities.topologySubshapesV1).toBe(true);
        expect(capabilities.topologyHierarchyV1).toBe(true);
        expect(capabilities.revisionInfoV1).toBe(true);
        expect(capabilities.entityResolutionV1).toBe(true);
        expect(capabilities.entityRemapV1).toBe(true);
        expect(capabilities.revisionRetentionV1).toBe(true);
        expect(capabilities.historyV1).toBe(true);
        expect(capabilities.stableNamingV1).toBe(true);
        expect(capabilities.checkpointV1).toBe(true);
        expect(capabilities.versionInfoV1).toBe(true);
        expect(capabilities.analysisV1).toBe(true);
        expect(capabilities.analysis?.shapeIntersection).toBe(true);
        expect(capabilities.analysis?.closestPoint).toBe(true);
        expect(capabilities.analysis?.shapeDistance).toBe(true);
        expect(capabilities.sessionHandlesV1).toBe(true);
        expect(capabilities.operations?.nativeExactBlendOpsV1).toBe(true);
        expect(capabilities.extrudeProfile?.draft).toBe(true);
        expect(capabilities.extrudeProfile?.endConditions).toContain('upToSurface');
        expect(capabilities.extrudeCutProfile?.curvedSurfaceTarget).toBe(true);
        expect(capabilities.revolveProfile?.slidingEdges).toBe(true);
        expect(capabilities.revolveCutProfile?.endConditions).toContain('upToSurfaceAtAngle');
        expect(capabilities.sweepProfile?.cutBoolean).toBe(true);
        expect(capabilities.loft?.sectionKinds).toContain('point');
        expect(capabilities.fillet?.stationRadii).toBe(true);
        expect(capabilities.fillet?.partialEdges).toBe(false);
        expect(capabilities.chamfer?.distanceAngle).toBe(true);
        expect(capabilities.subshapeEvaluation?.evaluateEdge).toBe(true);
        expect(capabilities.analysis?.volume).toBe(true);
        expect(capabilities.analysis?.pointContainment).toBe(true);
        expect(capabilities.runtime?.worker).toBe(true);
    });

    it('returns a versioned operation schema', () => {
        const k = makeKernel();
        const schema = k.getOperationSchema();

        expect(schema.schemaVersion).toBe(1);
        expect(schema.operations.extrudeProfile).toBeDefined();
        expect(schema.operations.extrudeCutProfile).toBeDefined();
        expect(schema.operations.revolveProfile).toBeDefined();
        expect(schema.operations.revolveCutProfile).toBeDefined();
        expect(schema.operations.sweepProfile).toBeDefined();
        expect(schema.operations.loft).toBeDefined();
        expect(schema.operations.filletEdges).toBeDefined();
        expect(schema.operations.getVersionInfo).toBeDefined();
        expect(schema.operations.analyzeShape).toBeDefined();
        expect(schema.operations.classifyPointContainment).toBeDefined();
        expect(schema.operations.intersectShapes).toBeDefined();
        expect(schema.operations.findClosestPointOnShape).toBeDefined();
        expect(schema.operations.measureShapeDistance).toBeDefined();
        expect(schema.operations.evaluateFace).toBeDefined();
    });
});

describe('getVersionInfo', () => {
    it('returns wrapper and native version metadata for the current session', () => {
        const k = makeKernel();
        const info = k.getVersionInfo();

        expect(info.libraryVersion).toBe('1.1.0');
        expect(info.apiVersion).toBe(1);
        expect(info.kernelVersion).toBe('8.0.0');
        expect(info.sessionId).toMatch(/^session_/);
        expect(info.supportedRuntimes).toEqual(['browser', 'worker', 'node']);
    });
});

describe('analysis queries', () => {
    it('returns exact-ish mass properties and counts for boxes', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 2, dy: 3, dz: 4 });
        const analysis = k.analyzeShape({ shape: box });

        expect(analysis.shapeType).toBe('solid');
        expect(analysis.solidCount).toBe(1);
        expect(analysis.faceCount).toBe(6);
        expect(analysis.volume).toBeCloseTo(24);
        expect(analysis.surfaceArea).toBeCloseTo(52);
        expect(analysis.linearLength).toBeCloseTo(36);
        expect(analysis.centerOfMass).toEqual([1, 1.5, 2]);
        expect(analysis.centerOfMassBasis).toBe('volume');
    });

    it('classifies points against resident solids', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 2, dy: 3, dz: 4 });

        expect(k.classifyPointContainment({ shape: box, point: [1, 1, 1] }).state).toBe('in');
        expect(k.classifyPointContainment({ shape: box, point: [2, 1, 1] }).state).toBe('on');
        expect(k.classifyPointContainment({ shape: box, point: [3, 1, 1] }).state).toBe('out');
    });

    it('returns section, closest-point, and shape-distance query results', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 2, dy: 2, dz: 2 });
        const farBox = k.transformShape({ shape: box, transform: { translation: [5, 0, 0] } });
        const overlappingBox = k.transformShape({ shape: box, transform: { translation: [1, 1, 0] } });

        const closest = k.findClosestPointOnShape({ shape: box, point: [5, 1, 1] });
        expect(closest.closestPoint).toEqual([2, 1, 1]);
        expect(closest.distance).toBeCloseTo(3);

        const distance = k.measureShapeDistance({ shapeA: box, shapeB: farBox });
        expect(distance.distance).toBeCloseTo(3);
        expect(distance.clearance).toBeCloseTo(3);
        expect(distance.solutions).toHaveLength(1);

        const section = k.intersectShapes({ shapeA: box, shapeB: overlappingBox });
        expect(section.hasIntersection).toBe(true);
        expect(section.edgeCount).toBeGreaterThan(0);
        expect(section.sectionShape).toBeDefined();
    });
});

describe('revision identity APIs', () => {
    it('returns revision info for a resident handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const revision = k.getRevisionInfo(box);

        expect(revision.revisionId).toMatch(/^rev_/);
        expect(revision.operationType).toBe('box');
        expect(revision.historySchemaVersion).toBe(1);
        expect(revision.entityStatus).toBe('generated');
    });

    it('resolves stable topology entities without consulting mesh edges', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const topology = k.getTopology(box);
        const stableHash = topology.faces?.[0]?.stableHash;
        expect(stableHash).toBeDefined();

        const resolved = k.resolveStableEntity({ shape: box, stableHash: stableHash ?? '' });
        expect(resolved).toMatchObject({
            found: true,
            status: 'active',
            kind: 'face',
            id: 1,
        });
    });

    it('maps stable hashes across resident revisions', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const moved = k.transformShape({ shape: box, transform: { translation: [1, 2, 3] } });
        const sourceTopology = k.getTopology(box);
        const sourceRevision = k.getRevisionInfo(box);
        const targetRevision = k.getRevisionInfo(moved);
        const stableHash = sourceTopology.faces?.[0]?.stableHash ?? '';

        const mapping = k.mapEntitiesAcrossRevisions({
            fromRevisionId: sourceRevision.revisionId,
            toRevisionId: targetRevision.revisionId,
            stableHashes: [stableHash],
        });

        expect(mapping.mappings).toHaveLength(1);
        expect(mapping.mappings[0]).toMatchObject({
            stableHash,
            status: 'mapped',
            mappedStableHash: stableHash,
        });
    });

    it('retains stable hashes across transformed topology revisions', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const moved = k.transformShape({ shape: box, transform: { translation: [1, 2, 3] } });

        const sourceTopology = k.getTopology(box);
        const movedTopology = k.getTopology(moved);

        expect(movedTopology.faces?.[0]?.stableHash).toBe(sourceTopology.faces?.[0]?.stableHash);
        expect(movedTopology.edges?.[0]?.stableHash).toBe(sourceTopology.edges?.[0]?.stableHash);
    });

    it('round-trips checkpoint metadata and hydrates a new handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const checkpoint = k.createCheckpoint({ shape: box });
        const hydrated = k.hydrateCheckpoint({ checkpoint });
        const revision = k.getRevisionInfo(hydrated);

        expect(checkpoint.checkpointSchemaVersion).toBe(1);
        expect(checkpoint.brep.length).toBeGreaterThan(0);
        expect(revision.createdFromCheckpoint).toBe(true);
        expect(revision.revisionId).toBe(checkpoint.revision.revisionId);
    });

    it('throws INVALID_CHECKPOINT for malformed checkpoint payloads', () => {
        const k = makeKernel();

        expect(() => k.hydrateCheckpoint({ checkpoint: '{not-json' })).toThrow(KernelError);
        try {
            k.hydrateCheckpoint({ checkpoint: '{not-json' });
            fail('should have thrown');
        } catch (error) {
            expect((error as KernelError).code).toBe('INVALID_CHECKPOINT');
        }
    });

    it('retains and releases resident revisions by reference count', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });

        k.retainRevision({ shape: box });
        expect(k.releaseRevision({ shape: box })).toBe(false);
        expect(k.checkValidity(box)).toBe(true);
        expect(k.releaseRevision({ shape: box })).toBe(true);
        expect(k.checkValidity(box)).toBe(false);
    });
});

describe('checkValidity', () => {
    it('returns true for a valid shape', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 5, dy: 5, dz: 5 });
        expect(k.checkValidity(box)).toBe(true);
    });

    it('returns false for a disposed shape', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 5, dy: 5, dz: 5 });
        k.disposeShape({ shape: box });
        expect(k.checkValidity(box)).toBe(false);
    });
});

// ---------------------------------------------------------------------------
// tessellate
// ---------------------------------------------------------------------------

describe('tessellate', () => {
    it('returns positions, normals, indices as typed arrays', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const mesh = k.tessellate({ shape: box });

        expect(mesh.positions).toBeInstanceOf(Float32Array);
        expect(mesh.normals).toBeInstanceOf(Float32Array);
        expect(mesh.indices).toBeInstanceOf(Uint32Array);
        expect(mesh.positions.length).toBeGreaterThan(0);
        expect(mesh.normals.length).toBe(mesh.positions.length);
        expect(mesh.indices.length % 3).toBe(0); // triangles
    });

    it('returns triangle metadata and sanitized feature-edge chains separately from raw debug edges', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const mesh = k.tessellate({ shape: box });
        const triangleCount = mesh.indices.length / 3;

        expect(mesh.triangleNormals).toBeInstanceOf(Float32Array);
        expect(mesh.triangleNormals?.length).toBe(triangleCount * 3);
        expect(mesh.triangleTopoFaceIds).toBeInstanceOf(Uint32Array);
        expect(mesh.triangleTopoFaceIds?.length).toBe(triangleCount);
        expect(mesh.triangleFaceGroups?.length).toBe(triangleCount);
        expect(mesh.triangleStableHashes?.length).toBe(triangleCount);
        expect(mesh.featureEdges?.[0]).toMatchObject({
            isClosed: false,
            chainId: 1,
            isSharp: true,
        });
        expect(mesh.featureEdges?.[0]?.points.length).toBeGreaterThan(1);
        expect(mesh.rawEdgeSegments).toBeInstanceOf(Float32Array);
        expect((mesh as unknown as Record<string, unknown>).edgeSegments).toBeUndefined();
    });

    it('can omit tessellation metadata for lightweight previews', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const mesh = k.tessellate({ shape: box, includeMetadata: false });

        expect(mesh.positions.length).toBeGreaterThan(0);
        expect(mesh.normals.length).toBe(mesh.positions.length);
        expect(mesh.indices.length % 3).toBe(0);
        expect(mesh.triangleNormals).toBeUndefined();
        expect(mesh.triangleTopoFaceIds).toBeUndefined();
        expect(mesh.triangleFaceGroups).toBeUndefined();
        expect(mesh.triangleStableHashes).toBeUndefined();
        expect(mesh.featureEdges).toBeUndefined();
        expect(mesh.rawEdgeSegments).toBeUndefined();
    });

    it('can selectively include tessellation metadata channels', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const mesh = k.tessellate({
            shape: box,
            includeMetadata: false,
            includeTriangleTopoFaceIds: true,
            includeFeatureEdges: true,
        });

        expect(mesh.triangleTopoFaceIds).toBeInstanceOf(Uint32Array);
        expect(mesh.featureEdges?.length).toBeGreaterThan(0);
        expect(mesh.triangleStableHashes).toBeUndefined();
        expect(mesh.rawEdgeSegments).toBeUndefined();
    });

    it('accepts face subsets for partial tessellation', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const mesh = k.tessellate({
            shape: box,
            faces: [{ topoId: 4 }],
            includeMetadata: false,
            includeTriangleTopoFaceIds: true,
        });

        expect(mesh.positions.length).toBeGreaterThan(0);
        expect(Array.from(mesh.triangleTopoFaceIds ?? [])).toEqual([4]);
        expect(mesh.featureEdges).toBeUndefined();
    });

    it('does not silently ignore tessellation options on legacy native modules', () => {
        class LegacyTessellationNativeKernel extends MockNativeKernel {
            tessellateWithOptions = undefined as unknown as MockNativeKernel['tessellateWithOptions'];
        }

        const k = makeKernelWithNative(LegacyTessellationNativeKernel);
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });

        expect(() => k.tessellate({ shape: box, includeMetadata: false })).toThrow(KernelError);
    });

    it('applies default deflection values', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        // Should not throw even without explicit deflection parameters
        expect(() => k.tessellate({ shape: box })).not.toThrow();
    });

    it('throws KernelError when linearDeflection <= 0', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        expect(() => k.tessellate({ shape: box, linearDeflection: 0 })).toThrow(KernelError);
        expect(() => k.tessellate({ shape: box, linearDeflection: -0.1 })).toThrow(KernelError);
    });

    it('throws KernelError for invalid handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        expect(() => k.tessellate({ shape: invalidHandle(box) })).toThrow(KernelError);
    });
});

// ---------------------------------------------------------------------------
// importStep / exportStep
// ---------------------------------------------------------------------------

describe('importStepDetailed', () => {
    const validStep = [
        'ISO-10303-21;',
        'HEADER;',
        "FILE_DESCRIPTION(('test'),'2;1');",
        "FILE_NAME('','',(''),(''),'','','');",
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));",
        'ENDSEC;',
        'DATA;',
        'ENDSEC;',
        'END-ISO-10303-21;',
    ].join('\n');

    it('returns structured reader and transfer diagnostics for valid content', () => {
        const k = makeKernel();
        const result = k.importStepDetailed({ content: validStep });

        expect(result.readStatus).toBe('IFSelect_RetDone');
        expect(result.transferStatus).toBe('DONE');
        expect(result.rootCount).toBeGreaterThan(0);
        expect(result.transferredRootCount).toBeGreaterThan(0);
        expect(result.shape?.id).toBeGreaterThan(0);
        expect(result.isValid).toBe(true);
        expect(Array.isArray(result.messageList)).toBe(true);
    });

    it('returns structured failure details instead of throwing for invalid STEP content', () => {
        const k = makeKernel();
        const result = k.importStepDetailed({ content: 'not a step file at all' });

        expect(result.shape).toBeUndefined();
        expect(result.transferStatus).toBe('FAILED');
        expect(result.messageList.some((message) => message.severity === 'fail')).toBe(true);
    });

    it('applies healing options and reports improved validity', () => {
        const k = makeKernel();
        const result = k.importStepDetailed({
            content: `${validStep}\nMOCK_INVALID_SHAPE`,
            options: {
                heal: true,
                sew: true,
                fixSameParameter: true,
                fixSolid: true,
            },
        });

        expect(result.shape?.id).toBeGreaterThan(0);
        expect(result.wasValidBeforeHealing).toBe(false);
        expect(result.healed).toBe(true);
        expect(result.isValid).toBe(true);
    });
});

describe('importStep', () => {
    const validStep = [
        'ISO-10303-21;',
        'HEADER;',
        "FILE_DESCRIPTION(('test'),'2;1');",
        "FILE_NAME('','',(''),(''),'','','');",
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));",
        'ENDSEC;',
        'DATA;',
        'ENDSEC;',
        'END-ISO-10303-21;',
    ].join('\n');

    it('returns a valid handle for a STEP string', () => {
        const k = makeKernel();
        const h = k.importStep({ content: validStep });
        expect(h.id).toBeGreaterThan(0);
    });

    it('throws KernelError when content is empty', () => {
        const k = makeKernel();
        expect(() => k.importStep({ content: '' })).toThrow(KernelError);
        expect(() => k.importStep({ content: '   ' })).toThrow(KernelError);
    });

    it('throws KernelError with code IMPORT_FAILED for invalid content', () => {
        const k = makeKernel();
        try {
            k.importStep({ content: 'not a step file at all' });
            fail('should have thrown');
        } catch (err) {
            expect(err).toBeInstanceOf(KernelError);
            expect((err as KernelError).code).toBe('IMPORT_FAILED');
        }
    });
});

describe('importStepPackage', () => {
    const validStep = [
        'ISO-10303-21;',
        'HEADER;',
        "FILE_DESCRIPTION(('test'),'2;1');",
        "FILE_NAME('','',(''),(''),'','','');",
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));",
        'ENDSEC;',
        'DATA;',
        'ENDSEC;',
        'END-ISO-10303-21;',
    ].join('\n');

    it('returns a fully fused result package with mesh, properties, topology, and checkpoint', () => {
        const k = makeKernel();
        const result = k.importStepPackage({
            content: validStep,
            options: {
                linearDeflection: 0.1,
                angularDeflection: 0.5,
            }
        });

        expect(result.readStatus).toBe('IFSelect_RetDone');
        expect(result.transferStatus).toBe('DONE');
        expect(result.isValid).toBe(true);
        expect(result.healed).toBe(false);
        expect(result.messageList).toBeDefined();

        expect(result.shape).toBeDefined();
        expect(result.shape?.id).toBeGreaterThan(0);

        expect(result.revision).toBeDefined();
        expect(typeof result.revision?.revisionId).toBe('string');
        expect(typeof result.revision?.topologyHash).toBe('string');

        expect(result.topology).toBeDefined();
        expect(typeof result.topology?.faceCount).toBe('number');
        expect(typeof result.topology?.edgeCount).toBe('number');
        expect(typeof result.topology?.vertexCount).toBe('number');

        expect(result.properties).toBeDefined();
        expect(result.properties?.boundingBox).toBeDefined();
        expect(typeof result.properties?.volume).toBe('number');

        expect(result.checkpoint).toBeDefined();
        expect(result.checkpoint?.checkpointSchemaVersion).toBe(1);
        expect(typeof result.checkpoint?.brep).toBe('string');

        // Ensure we can hydrate the checkpoint back
        const hydrated = k.hydrateCheckpoint({ checkpoint: result.checkpoint! });
        expect(hydrated.id).toBeGreaterThan(0);

        // Ensure we have the mesh arrays populated correctly as TypedArrays
        expect(result.mesh).toBeDefined();
        expect(result.mesh?.positions).toBeInstanceOf(Float32Array);
        expect(result.mesh?.normals).toBeInstanceOf(Float32Array);
        expect(result.mesh?.indices).toBeInstanceOf(Uint32Array);
        expect(result.mesh?.triangleNormals).toBeInstanceOf(Float32Array);
        expect(result.mesh?.triangleTopoFaceIds).toBeInstanceOf(Uint32Array);
        expect(result.mesh?.triangleFaceGroups).toBeInstanceOf(Uint32Array);
        expect(Array.isArray(result.mesh?.triangleStableHashes)).toBe(true);
        expect(Array.isArray(result.mesh?.featureEdges)).toBe(true);
    });

    it('skips mesh computation if deflection options are <= 0', () => {
        const k = makeKernel();
        const result = k.importStepPackage({
            content: validStep,
            options: {
                linearDeflection: -1,
                angularDeflection: -1,
            }
        });

        expect(result.readStatus).toBe('IFSelect_RetDone');
        expect(result.shape).toBeDefined();
        expect(result.mesh).toBeUndefined();
    });

    it('returns empty shape and mesh fields on invalid STEP string without throwing', () => {
        const k = makeKernel();
        const result = k.importStepPackage({ content: 'invalid step file' });

        expect(result.readStatus).toBe('IFSelect_RetFail');
        expect(result.transferStatus).toBe('FAILED');
        expect(result.shape).toBeUndefined();
        expect(result.mesh).toBeUndefined();
        expect(result.properties).toBeUndefined();
        expect(result.checkpoint).toBeUndefined();
    });
});

describe('exportStep', () => {
    it('returns a non-empty string', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const step = k.exportStep({ shape: box });
        expect(typeof step).toBe('string');
        expect(step.length).toBeGreaterThan(0);
    });

    it('returned string contains STEP header', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        const step = k.exportStep({ shape: box });
        expect(step).toContain('ISO-10303-21');
    });

    it('throws KernelError for invalid handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        expect(() => k.exportStep({ shape: invalidHandle(box) })).toThrow(KernelError);
    });
});

// ---------------------------------------------------------------------------
// disposeShape
// ---------------------------------------------------------------------------

describe('disposeShape', () => {
    it('does not throw for a valid handle', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        expect(() => k.disposeShape({ shape: box })).not.toThrow();
    });

    it('is idempotent (disposing a missing handle is a no-op)', () => {
        const k = makeKernel();
        const box = k.createBox({ dx: 10, dy: 10, dz: 10 });
        k.disposeShape({ shape: box });
        // Second dispose of the same id should not throw (erasing a missing key is a no-op)
        expect(() => k.disposeShape({ shape: box })).not.toThrow();
    });
});

// ---------------------------------------------------------------------------
// KernelError
// ---------------------------------------------------------------------------

describe('KernelError', () => {
    it('has the correct name and code', () => {
        const err = new KernelError('INVALID_PARAMS', 'test detail');
        expect(err.name).toBe('KernelError');
        expect(err.code).toBe('INVALID_PARAMS');
        expect(err.detail).toBe('test detail');
    });

    it('is an instance of Error', () => {
        const err = new KernelError('UNKNOWN', 'test');
        expect(err).toBeInstanceOf(Error);
    });

    it('message includes code and detail', () => {
        const err = new KernelError('OPERATION_FAILED', 'something went wrong');
        expect(err.message).toContain('OPERATION_FAILED');
        expect(err.message).toContain('something went wrong');
    });
});
