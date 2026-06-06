/**
 * Node.js example: create a box, tessellate it, and export to STEP.
 *
 * This example uses the mock adapter since no WASM binary is required.
 * Replace `mockModule` with the real Emscripten module to use actual OCCT.
 *
 * Run:
 *   node examples/nodejs/demo.js
 */

'use strict';

// When running from source (without a built dist/):
// const { OcctKernel } = require('../../dist/index');
// When running with the mock for illustration:
const { OcctKernel } = require('../../src/kernel');
const { MockNativeKernel } = require('../../src/mock-adapter');

async function main() {
    console.log('occt-kernel-wasm Node.js demo');
    console.log('==============================\n');

    // In production:
    //   const factory = require('./dist/occt-kernel.js');
    //   const wasmModule = await factory();
    // For this demo we use the mock:
    const wasmModule = {
        OcctKernel: MockNativeKernel,
    };

    const kernel = new OcctKernel(wasmModule);

    // -----------------------------------------------------------------------
    // 1. Create a box
    // -----------------------------------------------------------------------
    console.log('Creating box (dx=100, dy=50, dz=25)...');
    const box = kernel.createBox({ dx: 100, dy: 50, dz: 25 });
    console.log(`  Handle id: ${box.id}`);

    // -----------------------------------------------------------------------
    // 2. Query topology
    // -----------------------------------------------------------------------
    const topo = kernel.getTopology(box);
    console.log('\nTopology:');
    console.log(`  Faces:    ${topo.faceCount}`);
    console.log(`  Edges:    ${topo.edgeCount}`);
    console.log(`  Vertices: ${topo.vertexCount}`);
    console.log(`  Valid:    ${topo.isValid}`);
    console.log(`  Bounding box: [${topo.boundingBox.xMin}, ${topo.boundingBox.yMin}, ${topo.boundingBox.zMin}] → [${topo.boundingBox.xMax}, ${topo.boundingBox.yMax}, ${topo.boundingBox.zMax}]`);

    // -----------------------------------------------------------------------
    // 3. Tessellate for rendering
    // -----------------------------------------------------------------------
    const mesh = kernel.tessellate({ shape: box, linearDeflection: 0.5 });
    console.log('\nTessellation:');
    console.log(`  Vertices: ${mesh.positions.length / 3}`);
    console.log(`  Triangles: ${mesh.indices.length / 3}`);

    // -----------------------------------------------------------------------
    // 4. Boolean subtraction: box - cylinder
    // -----------------------------------------------------------------------
    console.log('\nBoolean subtract: box - cylinder...');
    const cyl = kernel.createCylinder({ radius: 15, height: 30 });
    const result = kernel.booleanSubtract({ base: box, tool: cyl });
    console.log(`  Result handle id: ${result.id}`);

    // -----------------------------------------------------------------------
    // 5. Fillet the result
    // -----------------------------------------------------------------------
    console.log('\nApplying fillet (radius=2)...');
    const filleted = kernel.filletEdges({ shape: result, radius: 2 });
    console.log(`  Filleted handle id: ${filleted.id}`);

    // -----------------------------------------------------------------------
    // 6. Export to STEP
    // -----------------------------------------------------------------------
    console.log('\nExporting to STEP...');
    const step = kernel.exportStep({ shape: filleted });
    console.log(`  STEP content length: ${step.length} characters`);
    console.log(`  First line: ${step.split('\n')[0]}`);

    // -----------------------------------------------------------------------
    // 7. Dispose shapes
    // -----------------------------------------------------------------------
    kernel.disposeShape({ shape: box });
    kernel.disposeShape({ shape: cyl });
    kernel.disposeShape({ shape: result });
    kernel.disposeShape({ shape: filleted });

    console.log('\nAll shapes disposed. Done.\n');
}

main().catch((err) => {
    console.error('Error:', err?.message || String(err));
    process.exit(1);
});
