import { existsSync } from 'node:fs';
import { join } from 'node:path';

import type { OcctKernel } from '../../src/kernel';
import type { Profile } from '../../src/types';

jest.setTimeout(120000);

const distDir = join(__dirname, '..', '..', 'dist');
const hasBuiltKernel = ['index.js', 'occt-kernel.js', 'occt-kernel.wasm'].every((fileName) => existsSync(join(distDir, fileName)));
const describeIfBuilt = hasBuiltKernel ? describe : describe.skip;

const bsplineRectangleProfile: Profile = {
    segments: [
        {
            type: 'bspline',
            controlPoints: [[0, 0], [6, 4], [14, 4], [20, 0]],
            degree: 3,
            knots: [0, 1],
            multiplicities: [4, 4],
        },
        { type: 'line', start: [20, 0], end: [20, 10] },
        { type: 'line', start: [20, 10], end: [0, 10] },
        { type: 'line', start: [0, 10], end: [0, 0] },
    ],
};

function expectBlendFacesResolveToFinalTopology(
    blendFaces: ReadonlyArray<{
        stableHash: string | null;
        topoFaceId?: number;
        finalOutputFaceRef?: { stableHash?: string; topoFaceId?: number };
        finalOutputFaceRefs?: readonly { stableHash?: string; topoFaceId?: number }[];
    }>,
    faces: ReadonlyArray<{ id: number; stableHash?: string }> | undefined,
): string[] {
    expect(blendFaces.length).toBeGreaterThan(0);
    const finalFaces = faces ?? [];
    const stableHashes: string[] = [];
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
            if (typeof ref.stableHash === 'string') {
                stableHashes.push(ref.stableHash);
            }
        }
    }
    return stableHashes;
}

describeIfBuilt('Real WASM analysis queries', () => {
    let kernel: OcctKernel;
    let createKernel: () => Promise<OcctKernel>;

    beforeAll(async () => {
        const distIndex = require(join(distDir, 'index.js')) as { createKernel: () => Promise<OcctKernel> };
        createKernel = distIndex.createKernel;
        kernel = await createKernel();
    });

    it('returns session and kernel version metadata', () => {
        const info = kernel.getVersionInfo();

        expect(info.libraryVersion).toBe('1.1.0');
        expect(info.apiVersion).toBe(1);
        expect(info.kernelVersion).toBe('8.0.0');
        expect(info.sessionId).toMatch(/^session_/);
        expect(info.supportedRuntimes).toEqual(['browser', 'worker', 'node']);
    });

    it('returns exact box analysis and topology hierarchy', () => {
        const box = kernel.createBox({ dx: 2, dy: 3, dz: 4 });

        try {
            const analysis = kernel.analyzeShape({ shape: box });
            const topology = kernel.getTopology(box);

            expect(analysis.shapeType).toBe('solid');
            expect(analysis.solidCount).toBe(1);
            expect(analysis.shellCount).toBe(1);
            expect(analysis.faceCount).toBe(6);
            expect(analysis.volume).toBeCloseTo(24, 6);
            expect(analysis.surfaceArea).toBeCloseTo(52, 6);
            expect(analysis.centerOfMass?.[0]).toBeCloseTo(1, 6);
            expect(analysis.centerOfMass?.[1]).toBeCloseTo(1.5, 6);
            expect(analysis.centerOfMass?.[2]).toBeCloseTo(2, 6);

            expect(topology.shapeType).toBe('solid');
            expect(topology.solidCount).toBe(1);
            expect(topology.shellCount).toBe(1);
            expect(topology.wireCount).toBeGreaterThan(0);
            expect(topology.solids?.[0]?.shellIds).toEqual([1]);
            expect(topology.shells?.[0]?.faceIds?.length).toBe(6);
        } finally {
            kernel.disposeShape({ shape: box });
        }
    });

    it('can tessellate a resident shape without heavyweight metadata channels', () => {
        const box = kernel.createBox({ dx: 2, dy: 3, dz: 4 });

        try {
            const mesh = kernel.tessellate({ shape: box, includeMetadata: false });

            expect(mesh.positions.length).toBeGreaterThan(0);
            expect(mesh.normals.length).toBe(mesh.positions.length);
            expect(mesh.indices.length).toBeGreaterThan(0);
            expect(mesh.triangleNormals).toBeUndefined();
            expect(mesh.triangleTopoFaceIds).toBeUndefined();
            expect(mesh.triangleFaceGroups).toBeUndefined();
            expect(mesh.triangleStableHashes).toBeUndefined();
            expect(mesh.featureEdges).toBeUndefined();
            expect(mesh.rawEdgeSegments).toBeUndefined();
        } finally {
            kernel.disposeShape({ shape: box });
        }
    });

    it('can tessellate a selected topological face', () => {
        const box = kernel.createBox({ dx: 2, dy: 3, dz: 4 });

        try {
            const fullMesh = kernel.tessellate({ shape: box, includeMetadata: false });
            const mesh = kernel.tessellate({
                shape: box,
                faces: [{ topoId: 2 }],
                includeMetadata: false,
                includeTriangleTopoFaceIds: true,
            });

            expect(mesh.positions.length).toBeGreaterThan(0);
            expect(mesh.indices.length).toBeGreaterThan(0);
            expect(mesh.positions.length).toBeLessThan(fullMesh.positions.length);
            expect(mesh.indices.length).toBeLessThan(fullMesh.indices.length);
            expect(new Set(Array.from(mesh.triangleTopoFaceIds ?? []))).toEqual(new Set([2]));
            expect(mesh.featureEdges).toBeUndefined();
        } finally {
            kernel.disposeShape({ shape: box });
        }
    });

    it('classifies points against an exact resident solid', () => {
        const box = kernel.createBox({ dx: 2, dy: 3, dz: 4 });

        try {
            expect(kernel.classifyPointContainment({ shape: box, point: [1, 1, 1] }).state).toBe('in');
            expect(kernel.classifyPointContainment({ shape: box, point: [2, 1, 1] }).state).toBe('on');
            expect(kernel.classifyPointContainment({ shape: box, point: [3, 1, 1] }).state).toBe('out');
        } finally {
            kernel.disposeShape({ shape: box });
        }
    });

    it('retains a modified face stable hash across a real boolean cut revision', () => {
        const box = kernel.createBox({ dx: 10, dy: 10, dz: 10 });
        const cutter = kernel.createCylinder({ radius: 1.5, height: 12 });
        const movedCutter = kernel.transformShape({ shape: cutter, transform: { translation: [5, 5, -1] } });

        try {
            const sourceTopology = kernel.getTopology(box);
            const topFace = sourceTopology.faces?.find((face) => {
                const sample = kernel.evaluateFace({
                    shape: box,
                    face: { topoId: face.id },
                    u: 0.5,
                    v: 0.5,
                });
                return sample.normal !== null
                    && sample.normal[2] > 0.9
                    && Math.abs(sample.point[2] - 10) < 1e-6;
            });

            expect(topFace?.stableHash).toBeDefined();

            const cut = kernel.booleanSubtract({ base: box, tool: movedCutter });

            try {
                const cutTopology = kernel.getTopology(cut);
                const sourceRevision = kernel.getRevisionInfo(box);
                const cutRevision = kernel.getRevisionInfo(cut);
                const stableHash = topFace?.stableHash ?? '';

                expect(cutTopology.faceCount).toBeGreaterThan(sourceTopology.faceCount);

                expect(kernel.resolveStableEntity({ shape: cut, stableHash })).toMatchObject({
                    found: true,
                    kind: 'face',
                    stableHash,
                });

                expect(kernel.mapEntitiesAcrossRevisions({
                    fromRevisionId: sourceRevision.revisionId,
                    toRevisionId: cutRevision.revisionId,
                    stableHashes: [stableHash],
                }).mappings[0]).toMatchObject({
                    stableHash,
                    status: 'mapped',
                    mappedStableHash: stableHash,
                });
            } finally {
                kernel.disposeShape({ shape: cut });
            }
        } finally {
            kernel.disposeShape({ shape: movedCutter });
            kernel.disposeShape({ shape: cutter });
            kernel.disposeShape({ shape: box });
        }
    });

    it('resolves fillet blend faces against the final topology and tessellation hashes', () => {
        const box = kernel.createBox({ dx: 4, dy: 5, dz: 6 });
        let filletResult: ReturnType<typeof kernel.filletEdgesWithSpec> | undefined;

        try {
            const sourceTopology = kernel.getTopology(box);
            const sourceEdge = sourceTopology.edges?.[0];
            const sourceEdgeStableHash = sourceEdge?.stableHash;

            expect(sourceEdgeStableHash).toBeDefined();
            if (sourceEdgeStableHash === undefined) {
                throw new Error('Expected a source edge stable hash for fillet blend validation');
            }

            filletResult = kernel.filletEdgesWithSpec({
                shape: box,
                spec: {
                    schemaVersion: 1,
                    edges: [{ stableHash: sourceEdgeStableHash, radius: 0.4 }],
                },
            });

            const topo = kernel.getTopology(filletResult.shape);
            const stableHashes = expectBlendFacesResolveToFinalTopology(filletResult.blendFaces, topo.faces);
            const mesh = kernel.tessellate({ shape: filletResult.shape, linearDeflection: 0.4, angularDeflection: 0.35 });

            expect(mesh.triangleStableHashes).toBeDefined();
            for (const stableHash of stableHashes) {
                expect(mesh.triangleStableHashes ?? []).toContain(stableHash);
            }
        } finally {
            if (filletResult !== undefined) {
                kernel.disposeShape({ shape: filletResult.shape });
            }
            kernel.disposeShape({ shape: box });
        }
    });

    it('resolves chamfer blend faces against the final topology and tessellation hashes', () => {
        const box = kernel.createBox({ dx: 4, dy: 5, dz: 6 });
        let chamferResult: ReturnType<typeof kernel.chamferEdgesWithSpec> | undefined;

        try {
            const sourceTopology = kernel.getTopology(box);
            const sourceEdge = sourceTopology.edges?.[0];
            const sourceEdgeStableHash = sourceEdge?.stableHash;

            expect(sourceEdgeStableHash).toBeDefined();
            if (sourceEdgeStableHash === undefined) {
                throw new Error('Expected a source edge stable hash for chamfer blend validation');
            }

            chamferResult = kernel.chamferEdgesWithSpec({
                shape: box,
                spec: {
                    schemaVersion: 1,
                    edges: [{ stableHash: sourceEdgeStableHash, distance: 0.35 }],
                },
            });

            const topo = kernel.getTopology(chamferResult.shape);
            const stableHashes = expectBlendFacesResolveToFinalTopology(chamferResult.blendFaces, topo.faces);
            const mesh = kernel.tessellate({ shape: chamferResult.shape, linearDeflection: 0.4, angularDeflection: 0.35 });

            expect(mesh.triangleStableHashes).toBeDefined();
            for (const stableHash of stableHashes) {
                expect(mesh.triangleStableHashes ?? []).toContain(stableHash);
            }
        } finally {
            if (chamferResult !== undefined) {
                kernel.disposeShape({ shape: chamferResult.shape });
            }
            kernel.disposeShape({ shape: box });
        }
    });

    it('chamfers a BSpline edge with symmetric distance and an adjacent reference face', async () => {
        const bsplineKernel = await createKernel();
        const solid = bsplineKernel.extrudeProfile({ profile: bsplineRectangleProfile, height: 12 });
        let chamferResult: ReturnType<typeof kernel.chamferEdgesWithSpec> | undefined;

        try {
            const sourceTopology = bsplineKernel.getTopology(solid);
            const bsplineEdge = (sourceTopology.edges ?? []).find((edge) => {
                const curve = bsplineKernel.getEdgeCurve({ shape: solid, edge: { topoId: edge.id } });
                return curve.curveType === 'bsplineCurve' && (edge.topoFaceIds?.length ?? 0) > 0;
            });

            expect(bsplineEdge).toBeDefined();
            if (bsplineEdge === undefined) {
                throw new Error('Expected an extruded BSpline source edge');
            }
            expect(bsplineEdge.stableHash).toBeDefined();
            const referenceFaceId = (bsplineEdge.topoFaceIds ?? []).find((faceId) => {
                const sample = bsplineKernel.evaluateFace({ shape: solid, face: { topoId: faceId }, u: 0.5, v: 0.5 });
                return Math.abs(sample.normal?.[2] ?? 0) < 0.5;
            }) ?? bsplineEdge.topoFaceIds?.[0];
            expect(referenceFaceId).toBeDefined();
            if (bsplineEdge.stableHash === undefined || referenceFaceId === undefined) {
                throw new Error('Expected BSpline edge stable hash and adjacent face id');
            }

            chamferResult = bsplineKernel.chamferEdgesWithSpec({
                shape: solid,
                spec: {
                    schemaVersion: 1,
                    mode: 'symmetric',
                    distance: 0.2,
                    edges: [{
                        edge: { topoId: bsplineEdge.id, stableHash: bsplineEdge.stableHash },
                        mode: 'symmetric',
                        distance: 0.2,
                        referenceFace: { topoId: referenceFaceId },
                    }],
                },
            });

            expect(bsplineKernel.checkValidity(chamferResult.shape)).toBe(true);
            expect(chamferResult.blendFaces[0]).toMatchObject({
                kind: 'chamferFace',
                sourceEdge: { topoId: bsplineEdge.id, stableHash: bsplineEdge.stableHash },
            });
            expect(chamferResult.blendFaces[0].usedParameters.referenceFace).toMatchObject({ topoId: referenceFaceId });
            expectBlendFacesResolveToFinalTopology(chamferResult.blendFaces, bsplineKernel.getTopology(chamferResult.shape).faces);
        } finally {
            if (chamferResult !== undefined) {
                bsplineKernel.disposeShape({ shape: chamferResult.shape });
            }
            bsplineKernel.disposeShape({ shape: solid });
        }
    });

    it('returns structured diagnostics for a chamfer reference face that is not adjacent to the edge', () => {
        const box = kernel.createBox({ dx: 4, dy: 5, dz: 6 });

        try {
            const topology = kernel.getTopology(box);
            const edge = topology.edges?.[0];
            expect(edge).toBeDefined();
            if (edge === undefined) {
                throw new Error('Expected a box edge');
            }
            const adjacentFaceIds = new Set(edge.topoFaceIds ?? []);
            const nonAdjacentFace = topology.faces?.find((face) => !adjacentFaceIds.has(face.id));
            expect(nonAdjacentFace).toBeDefined();
            if (nonAdjacentFace === undefined) {
                throw new Error('Expected a non-adjacent face for the selected edge');
            }

            try {
                kernel.chamferEdgesWithSpec({
                    shape: box,
                    spec: {
                        schemaVersion: 1,
                        edges: [{ topoId: edge.id, distance: 0.25, referenceFace: { topoId: nonAdjacentFace.id } }],
                    },
                });
                fail('should have thrown');
            } catch (err) {
                expect(err).toMatchObject({ code: 'INVALID_PARAMS' });
                const detail = JSON.parse((err as { detail: string }).detail);
                expect(detail).toMatchObject({
                    operation: 'chamferEdges',
                    path: 'chamfer.edges[0].referenceFace',
                    edge: { topoId: edge.id },
                    referenceFace: { topoId: nonAdjacentFace.id },
                });
                expect(detail.adjacentFaceIds).toEqual(edge.topoFaceIds);
            }
        } finally {
            kernel.disposeShape({ shape: box });
        }
    });

    it('returns exact section, closest-point, and shape-distance queries', () => {
        const box = kernel.createBox({ dx: 2, dy: 2, dz: 2 });
        const farBox = kernel.transformShape({ shape: box, transform: { translation: [5, 0, 0] } });
        const overlappingBox = kernel.transformShape({ shape: box, transform: { translation: [1, 1, 0] } });
        let sectionShape: ReturnType<typeof kernel.createBox> | undefined;

        try {
            const closest = kernel.findClosestPointOnShape({ shape: box, point: [5, 1, 1] });
            expect(closest.closestPoint[0]).toBeCloseTo(2, 6);
            expect(closest.closestPoint[1]).toBeCloseTo(1, 6);
            expect(closest.closestPoint[2]).toBeCloseTo(1, 6);
            expect(closest.distance).toBeCloseTo(3, 6);
            expect(closest.support.kind).toBe('face');

            const distance = kernel.measureShapeDistance({ shapeA: box, shapeB: farBox });
            expect(distance.distance).toBeCloseTo(3, 6);
            expect(distance.clearance).toBeCloseTo(3, 6);
            expect(distance.isInContact).toBe(false);
            expect(distance.solutions.length).toBeGreaterThan(0);

            const section = kernel.intersectShapes({ shapeA: box, shapeB: overlappingBox });
            expect(section.hasIntersection).toBe(true);
            expect(section.edgeCount).toBeGreaterThan(0);
            expect(section.sectionShape).toBeDefined();

            sectionShape = section.sectionShape;
            if (sectionShape !== undefined) {
                const topology = kernel.getTopology(sectionShape);
                expect(topology.edgeCount).toBe(section.edgeCount);
            }
        } finally {
            if (sectionShape !== undefined) {
                kernel.disposeShape({ shape: sectionShape });
            }
            kernel.disposeShape({ shape: overlappingBox });
            kernel.disposeShape({ shape: farBox });
            kernel.disposeShape({ shape: box });
        }
    });

    it('can return a disposable live feature preview mesh and wireframe', () => {
        const box = kernel.createBox({ dx: 4, dy: 5, dz: 6 });

        try {
            const preview = kernel.previewFeature({
                operation: 'filletEdges',
                params: {
                    shape: box,
                    spec: { schemaVersion: 1, edges: [{ topoId: 1, radius: 0.3 }] },
                },
                includeWireframe: true,
                includeTopology: true,
                tessellation: { linearDeflection: 0.35, angularDeflection: 0.35 },
            });

            expect(preview.mesh?.positions.length).toBeGreaterThan(0);
            expect(preview.mesh?.triangleStableHashes).toBeUndefined();
            expect(preview.wireframe?.length).toBeGreaterThan(0);
            expect(preview.topology?.faceCount).toBeGreaterThan(0);
            expect(preview.blendFaces?.[0]).toMatchObject({ kind: 'filletFace' });
            expect(preview.previewShape).toBeUndefined();
            expect(kernel.checkValidity(box)).toBe(true);
        } finally {
            kernel.disposeShape({ shape: box });
        }
    });
});