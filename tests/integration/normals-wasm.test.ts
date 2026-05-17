import { existsSync } from 'node:fs';
import { join } from 'node:path';

import type { OcctKernel } from '../../src/kernel';

jest.setTimeout(120000);

const distDir = join(__dirname, '..', '..', 'dist');
const hasBuiltKernel = ['index.js', 'occt-kernel.js', 'occt-kernel.wasm'].every((fileName) => existsSync(join(distDir, fileName)));
const describeIfBuilt = hasBuiltKernel ? describe : describe.skip;

describeIfBuilt('Real WASM tessellation normals', () => {
    let kernel: OcctKernel;

    beforeAll(async () => {
        const distIndex = require(join(distDir, 'index.js')) as { createKernel: () => Promise<OcctKernel> };
        kernel = await distIndex.createKernel();
    });

    it('returns smoothed vertex normals for curved sphere surfaces', () => {
        const sphere = kernel.createSphere({ radius: 10 });
        const mesh = kernel.tessellate({ shape: sphere, linearDeflection: 0.4, angularDeflection: 15 });

        let comparedCount = 0;
        let accumulatedDot = 0;
        const roundedNormals = new Set<string>();

        for (let index = 0; index < mesh.positions.length; index += 3) {
            const px = mesh.positions[index];
            const py = mesh.positions[index + 1];
            const pz = mesh.positions[index + 2];
            const nx = mesh.normals[index];
            const ny = mesh.normals[index + 1];
            const nz = mesh.normals[index + 2];

            const pointLength = Math.hypot(px, py, pz);
            const normalLength = Math.hypot(nx, ny, nz);
            if (pointLength < 1e-6 || normalLength < 1e-6) {
                continue;
            }

            accumulatedDot += (px * nx + py * ny + pz * nz) / (pointLength * normalLength);
            comparedCount += 1;
            roundedNormals.add(`${nx.toFixed(2)},${ny.toFixed(2)},${nz.toFixed(2)}`);
        }

        expect(comparedCount).toBeGreaterThan(0);
        expect(accumulatedDot / comparedCount).toBeGreaterThan(0.97);
        expect(roundedNormals.size).toBeGreaterThan(16);

        kernel.disposeShape({ shape: sphere });
    });
});