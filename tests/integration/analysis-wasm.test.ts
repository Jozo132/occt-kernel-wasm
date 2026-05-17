import { existsSync } from 'node:fs';
import { join } from 'node:path';

import type { OcctKernel } from '../../src/kernel';

jest.setTimeout(120000);

const distDir = join(__dirname, '..', '..', 'dist');
const hasBuiltKernel = ['index.js', 'occt-kernel.js', 'occt-kernel.wasm'].every((fileName) => existsSync(join(distDir, fileName)));
const describeIfBuilt = hasBuiltKernel ? describe : describe.skip;

describeIfBuilt('Real WASM analysis queries', () => {
    let kernel: OcctKernel;

    beforeAll(async () => {
        const distIndex = require(join(distDir, 'index.js')) as { createKernel: () => Promise<OcctKernel> };
        kernel = await distIndex.createKernel();
    });

    it('returns session and kernel version metadata', () => {
        const info = kernel.getVersionInfo();

        expect(info.libraryVersion).toBe('1.0.0');
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
});