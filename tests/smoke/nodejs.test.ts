/**
 * Node.js smoke test for occt-kernel-wasm.
 *
 * This test is intentionally simple: it verifies that the mock adapter
 * can be imported and used via the full createKernel() factory path.
 *
 * To test the real WASM binary, pass a real WasmModule to createKernel().
 */

import { OcctKernel } from '../../src/kernel';
import { MockNativeKernel } from '../../src/mock-adapter';
import type { WasmModule } from '../../src/kernel';

function makeMockModule(): WasmModule {
    return {
        OcctKernel: MockNativeKernel as unknown as new () => InstanceType<typeof MockNativeKernel>,
    } as unknown as WasmModule;
}

describe('Node.js smoke test', () => {
    it('creates a kernel and performs the box→tessellate→exportStep flow', () => {
        const kernel = new OcctKernel(makeMockModule());

        // Create
        const box = kernel.createBox({ dx: 100, dy: 50, dz: 25 });
        expect(box.id).toBeGreaterThan(0);

        // Tessellate
        const mesh = kernel.tessellate({ shape: box, linearDeflection: 0.5 });
        expect(mesh.positions.length).toBeGreaterThan(0);
        expect(mesh.normals.length).toBe(mesh.positions.length);
        expect(mesh.indices.length % 3).toBe(0);

        // Export
        const step = kernel.exportStep({ shape: box });
        expect(step).toContain('ISO-10303-21');

        // Dispose
        expect(() => kernel.disposeShape({ shape: box })).not.toThrow();
    });

    it('performs the cylinder → boolean subtract → tessellate flow', () => {
        const kernel = new OcctKernel(makeMockModule());

        const box = kernel.createBox({ dx: 50, dy: 50, dz: 50 });
        const cyl = kernel.createCylinder({ radius: 15, height: 60 });
        const result = kernel.booleanSubtract({ base: box, tool: cyl });
        const mesh = kernel.tessellate({ shape: result });

        expect(mesh.positions.length).toBeGreaterThan(0);

        kernel.disposeShape({ shape: box });
        kernel.disposeShape({ shape: cyl });
        kernel.disposeShape({ shape: result });
    });
});
