import { existsSync } from 'node:fs';
import { join } from 'node:path';

import type { OcctKernel } from '../../src/kernel';
import type { Profile } from '../../src/types';

jest.setTimeout(120000);

const distDir = join(__dirname, '..', '..', 'dist');
const hasBuiltKernel = ['index.js', 'occt-kernel.js', 'occt-kernel.wasm'].every((fileName) => existsSync(join(distDir, fileName)));
const describeIfBuilt = hasBuiltKernel ? describe : describe.skip;

describeIfBuilt('Real WASM revolve integration', () => {
    let kernel: OcctKernel;

    const frontFacePlane = {
        origin: [0, 0, 0] as [number, number, number],
        normal: [0, -1, 0] as [number, number, number],
        xDirection: [1, 0, 0] as [number, number, number],
    };

    const additiveProfile: Profile = {
        segments: [
            { type: 'line', start: [-6, 4], end: [0, 4] },
            { type: 'line', start: [0, 4], end: [0, 16] },
            { type: 'line', start: [0, 16], end: [-6, 16] },
            { type: 'line', start: [-6, 16], end: [-6, 4] },
        ],
    };

    const cutProfile: Profile = {
        segments: [
            { type: 'line', start: [0, 4], end: [6, 4] },
            { type: 'line', start: [6, 4], end: [6, 16] },
            { type: 'line', start: [6, 16], end: [0, 16] },
            { type: 'line', start: [0, 16], end: [0, 4] },
        ],
    };

    beforeAll(async () => {
        const distIndex = require(join(distDir, 'index.js')) as { createKernel: () => Promise<OcctKernel> };
        kernel = await distIndex.createKernel();
    });

    it('creates a real additive revolve feature against the compiled WASM kernel', () => {
        const box = kernel.createBox({ dx: 40, dy: 40, dz: 20 });
        const result = kernel.revolveProfileWithSpec({
            shape: box,
            profile: additiveProfile,
            spec: {
                schemaVersion: 1,
                plane: frontFacePlane,
                axisOrigin: [0, 0, 0],
                axisDirection: [0, 0, 1],
                extent: {
                    type: 'angle',
                    angleDegrees: 90,
                },
            },
        });

        const baseTopology = kernel.getTopology(box);
        const topology = kernel.getTopology(result);

        expect(kernel.checkValidity(result)).toBe(true);
        expect(topology.faceCount).toBeGreaterThan(baseTopology.faceCount);
        expect(topology.boundingBox.yMin).toBeLessThan(0);

        kernel.disposeShape({ shape: box });
        kernel.disposeShape({ shape: result });
    });

    it('creates a real subtractive revolve feature through the cut flag', () => {
        const box = kernel.createBox({ dx: 40, dy: 40, dz: 20 });
        const result = kernel.revolveProfileWithSpec({
            shape: box,
            cut: true,
            profile: cutProfile,
            spec: {
                schemaVersion: 1,
                plane: frontFacePlane,
                axisOrigin: [0, 0, 0],
                axisDirection: [0, 0, 1],
                extent: {
                    type: 'angle',
                    angleDegrees: 90,
                },
            },
        });

        const baseTopology = kernel.getTopology(box);
        const topology = kernel.getTopology(result);
        const revision = kernel.getRevisionInfo(result);

        expect(kernel.checkValidity(result)).toBe(true);
        expect(topology.faceCount).toBeGreaterThan(baseTopology.faceCount);
        expect(topology.boundingBox.xMin).toBe(baseTopology.boundingBox.xMin);
        expect(topology.boundingBox.yMin).toBe(baseTopology.boundingBox.yMin);
        expect(revision.operationType).toBe('revolveCutProfile');

        kernel.disposeShape({ shape: box });
        kernel.disposeShape({ shape: result });
    });
});