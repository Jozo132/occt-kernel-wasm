import { existsSync } from 'node:fs';
import { join } from 'node:path';

import type { OcctKernel } from '../../src/kernel';
import type { Profile, SpatialWire } from '../../src/types';

jest.setTimeout(120000);

const distDir = join(__dirname, '..', '..', 'dist');
const hasBuiltKernel = ['index.js', 'occt-kernel.js', 'occt-kernel.wasm'].every((fileName) => existsSync(join(distDir, fileName)));
const describeIfBuilt = hasBuiltKernel ? describe : describe.skip;
const boundingBoxTolerance = 1e-4;

function squareProfile(halfSize: number): Profile {
    return {
        segments: [
            { type: 'line', start: [-halfSize, -halfSize], end: [halfSize, -halfSize] },
            { type: 'line', start: [halfSize, -halfSize], end: [halfSize, halfSize] },
            { type: 'line', start: [halfSize, halfSize], end: [-halfSize, halfSize] },
            { type: 'line', start: [-halfSize, halfSize], end: [-halfSize, -halfSize] },
        ],
    };
}

function lineSpine(start: readonly [number, number, number], end: readonly [number, number, number]): SpatialWire {
    return {
        segments: [
            { type: 'line', start, end },
        ],
    };
}

describeIfBuilt('Real WASM sweep and loft integration', () => {
    let kernel: OcctKernel;

    beforeAll(async () => {
        const distIndex = require(join(distDir, 'index.js')) as { createKernel: () => Promise<OcctKernel> };
        kernel = await distIndex.createKernel();
    });

    it('creates a real additive sweep feature against the compiled WASM kernel', () => {
        const box = kernel.createBox({ dx: 30, dy: 30, dz: 10 });
        const result = kernel.sweepProfileWithSpec({
            shape: box,
            profile: squareProfile(4),
            spec: {
                schemaVersion: 1,
                plane: {
                    origin: [15, 15, 8],
                    normal: [0, 0, 1],
                    xDirection: [1, 0, 0],
                },
                spine: lineSpine([15, 15, 8], [15, 15, 24]),
                trihedronMode: { type: 'discrete' },
                sectionWithCorrection: true,
                solid: true,
                maxDegree: 7,
                maxSegments: 12,
            },
        });

        const baseTopology = kernel.getTopology(box);
        const topology = kernel.getTopology(result);
        const revision = kernel.getRevisionInfo(result);

        expect(kernel.checkValidity(result)).toBe(true);
        expect(topology.faceCount).toBeGreaterThan(baseTopology.faceCount);
        expect(topology.boundingBox.zMax).toBeGreaterThan(baseTopology.boundingBox.zMax);
        expect(revision.operationType).toBe('sweepProfile');

        kernel.disposeShape({ shape: box });
        kernel.disposeShape({ shape: result });
    });

    it('creates a real subtractive sweep feature through the cut flag', () => {
        const box = kernel.createBox({ dx: 30, dy: 30, dz: 20 });
        const result = kernel.sweepProfileWithSpec({
            shape: box,
            cut: true,
            profile: squareProfile(3),
            spec: {
                schemaVersion: 1,
                plane: {
                    origin: [15, 15, -1],
                    normal: [0, 0, 1],
                    xDirection: [1, 0, 0],
                },
                spine: lineSpine([15, 15, -1], [15, 15, 21]),
                solid: true,
            },
        });

        const baseTopology = kernel.getTopology(box);
        const topology = kernel.getTopology(result);
        const revision = kernel.getRevisionInfo(result);

        expect(kernel.checkValidity(result)).toBe(true);
        expect(topology.faceCount).toBeGreaterThan(baseTopology.faceCount);
        expect(Math.abs(topology.boundingBox.xMin - baseTopology.boundingBox.xMin)).toBeLessThan(boundingBoxTolerance);
        expect(Math.abs(topology.boundingBox.zMax - baseTopology.boundingBox.zMax)).toBeLessThan(boundingBoxTolerance);
        expect(revision.operationType).toBe('sweepCutProfile');

        kernel.disposeShape({ shape: box });
        kernel.disposeShape({ shape: result });
    });

    it('creates a real additive loft feature against the compiled WASM kernel', () => {
        const box = kernel.createBox({ dx: 30, dy: 30, dz: 10 });
        const result = kernel.loftWithSpec({
            shape: box,
            sections: [
                {
                    type: 'profile',
                    profile: squareProfile(6),
                    plane: {
                        origin: [15, 15, 6],
                        normal: [0, 0, 1],
                        xDirection: [1, 0, 0],
                    },
                },
                {
                    type: 'profile',
                    profile: squareProfile(3),
                    plane: {
                        origin: [15, 15, 22],
                        normal: [0, 0, 1],
                        xDirection: [1, 0, 0],
                    },
                },
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

        const baseTopology = kernel.getTopology(box);
        const topology = kernel.getTopology(result);
        const revision = kernel.getRevisionInfo(result);

        expect(kernel.checkValidity(result)).toBe(true);
        expect(topology.faceCount).toBeGreaterThan(baseTopology.faceCount);
        expect(topology.boundingBox.zMax).toBeGreaterThan(baseTopology.boundingBox.zMax);
        expect(revision.operationType).toBe('loft');

        kernel.disposeShape({ shape: box });
        kernel.disposeShape({ shape: result });
    });

    it('creates a real subtractive loft feature through the cut flag', () => {
        const box = kernel.createBox({ dx: 30, dy: 30, dz: 20 });
        const result = kernel.loftWithSpec({
            shape: box,
            cut: true,
            sections: [
                {
                    type: 'profile',
                    profile: squareProfile(5),
                    plane: {
                        origin: [15, 15, -1],
                        normal: [0, 0, 1],
                        xDirection: [1, 0, 0],
                    },
                },
                {
                    type: 'profile',
                    profile: squareProfile(3),
                    plane: {
                        origin: [15, 15, 21],
                        normal: [0, 0, 1],
                        xDirection: [1, 0, 0],
                    },
                },
            ],
            spec: {
                schemaVersion: 1,
                solid: true,
                ruled: true,
            },
        });

        const baseTopology = kernel.getTopology(box);
        const topology = kernel.getTopology(result);
        const revision = kernel.getRevisionInfo(result);

        expect(kernel.checkValidity(result)).toBe(true);
        expect(topology.faceCount).toBeGreaterThan(baseTopology.faceCount);
        expect(Math.abs(topology.boundingBox.yMin - baseTopology.boundingBox.yMin)).toBeLessThan(boundingBoxTolerance);
        expect(Math.abs(topology.boundingBox.zMax - baseTopology.boundingBox.zMax)).toBeLessThan(boundingBoxTolerance);
        expect(revision.operationType).toBe('loftCut');

        kernel.disposeShape({ shape: box });
        kernel.disposeShape({ shape: result });
    });
});