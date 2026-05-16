# occt-kernel-wasm

Reusable OCCT-based CAD kernel for WebAssembly with a small TypeScript API for exact solid modeling.

[![CI](https://github.com/Jozo132/occt-kernel-wasm/actions/workflows/ci.yml/badge.svg)](https://github.com/Jozo132/occt-kernel-wasm/actions/workflows/ci.yml)

---

## Purpose

`occt-kernel-wasm` is a narrow, high-level WebAssembly adapter for [Open CASCADE Technology (OCCT)](https://dev.opencascade.org/).
It exposes a stable, domain-oriented TypeScript API for exact solid modelling in browser and Node.js environments — without leaking any OCCT types into JavaScript.

It is designed to be consumed by modelling applications focused on SolidWorks-like sketching and feature workflows.

---

## Scope (v1)

| Feature                        | Status |
|-------------------------------|--------|
| Box / Cylinder / Sphere       | ✅     |
| Sketch extrusion               | ✅     |
| Sketch revolution              | ✅     |
| Boolean union / subtract / intersect | ✅ |
| Fillet / chamfer               | ✅     |
| Topology query (faces/edges/vertices/bbox) | ✅ |
| Validity check                | ✅     |
| Tessellation for WebGL        | ✅     |
| STEP import                   | ✅     |
| STEP export                   | ✅     |
| Structured error objects      | ✅     |
| Browser + Node.js support     | ✅     |

---

## Install

```bash
npm install occt-kernel-wasm
```

> **Note:** The npm package ships with a pre-compiled WASM binary. If you need to rebuild against a different OCCT version, see [Building from source](#building-from-source).

---

## API

```ts
import { createKernel } from 'occt-kernel-wasm';

const kernel = await createKernel();
```

All shapes are represented by opaque `ShapeHandle` objects (`{ id: number }`). No OCCT types are ever exposed.

### Primitives

```ts
const box = kernel.createBox({ dx: 100, dy: 50, dz: 25 });
const cyl = kernel.createCylinder({ radius: 15, height: 40 });
const sph = kernel.createSphere({ radius: 20 });
```

### Sketch-based features

```ts
const profile = {
    outer: {
        segments: [
            { type: 'line', start: [0, 0], end: [20, 0] },
            { type: 'line', start: [20, 0], end: [20, 10] },
            { type: 'line', start: [20, 10], end: [0, 10] },
            { type: 'line', start: [0, 10], end: [0, 0] },
        ],
    },
    holes: [{
        segments: [
            { type: 'line', start: [6, 3], end: [14, 3] },
            { type: 'line', start: [14, 3], end: [14, 7] },
            { type: 'line', start: [14, 7], end: [6, 7] },
            { type: 'line', start: [6, 7], end: [6, 3] },
        ],
    }],
};

const extruded = kernel.extrudeProfile({
    profile,
    plane: {
        origin: [0, 0, 10],
        normal: [0, 1, 0],
        xDirection: [1, 0, 0],
    },
    height: 15,
});

const revolved = kernel.revolveProfile({
    profile,
    axisOrigin: [0, 0, 0],
    axisDirection: [0, 0, 1],
    angleDegrees: 360,
});

const moved = kernel.transformShape({
    shape: extruded,
    transform: {
        rotation: {
            axisOrigin: [0, 0, 0],
            axisDirection: [0, 0, 1],
            angleDegrees: 30,
        },
        translation: [25, 0, 0],
    },
});
```

Legacy single-wire profiles using `{ segments: [...] }` are still supported. `extrudeProfile` accepts either `height` or an explicit world-space `vector`.

Arc segments:
```ts
{ type: 'arc', start: [10, 0], mid: [15, 5], end: [10, 10] }
```

Circle segments:
```ts
{ type: 'circle', centre: [0, 0], radius: 5 }
```

Bezier segments:
```ts
{ type: 'bezier', controlPoints: [[0, 0], [4, 6], [8, 6], [12, 0]] }
```

B-spline segments (non-rational, non-periodic):
```ts
{
    type: 'bspline',
    controlPoints: [[0, 0], [3, 4], [7, 4], [10, 0]],
    degree: 3,
    knots: [0, 1],
    multiplicities: [4, 4],
}
```

### Boolean operations

```ts
const united    = kernel.booleanUnion({ base: box, tool: cyl });
const cut       = kernel.booleanSubtract({ base: box, tool: cyl });
const intersect = kernel.booleanIntersect({ base: box, tool: cyl });
```

### Modifiers

```ts
const filleted = kernel.filletEdges({ shape: box, radius: 2 });
const chamfered = kernel.chamferEdges({ shape: box, distance: 1.5 });
```

### Topology query

```ts
const topo = kernel.getTopology(box);
// {
//   faceCount: 6, edgeCount: 12, vertexCount: 8,
//   boundingBox: { xMin: 0, yMin: 0, zMin: 0, xMax: 100, yMax: 50, zMax: 25 },
//   isValid: true
// }
```

### Tessellation

```ts
const mesh = kernel.tessellate({ shape: box, linearDeflection: 0.1 });
// {
//   positions: Float32Array,  // [x0,y0,z0, x1,y1,z1, ...]
//   normals:   Float32Array,  // [nx0,ny0,nz0, ...]
//   indices:   Uint32Array,   // [i0,i1,i2, ...] triangles
//   edgeSegments?: Float32Array
// }
```

Pass directly to Three.js `BufferGeometry`:
```ts
const geometry = new THREE.BufferGeometry();
geometry.setAttribute('position', new THREE.BufferAttribute(mesh.positions, 3));
geometry.setAttribute('normal',   new THREE.BufferAttribute(mesh.normals, 3));
geometry.setIndex(new THREE.BufferAttribute(mesh.indices, 1));
```

### Import / export (STEP)

```ts
// Export
const stepContent = kernel.exportStep({ shape: box });

// Import
const imported = kernel.importStep({ content: stepContent });

// Structured import diagnostics
const detailedImport = kernel.importStepDetailed({
    content: stepContent,
    options: {
        heal: true,
        sew: true,
        fixSameParameter: true,
    },
});

if (!detailedImport.shape) {
    console.error(detailedImport.readStatus, detailedImport.transferStatus);
    console.table(detailedImport.messageList);
}
```

### Memory management

Always dispose shapes when they are no longer needed:

```ts
kernel.disposeShape({ shape: box });
```

### Error handling

All operations throw a `KernelError` on failure:

```ts
import { KernelError } from 'occt-kernel-wasm';

try {
    const box = kernel.createBox({ dx: -1, dy: 1, dz: 1 });
} catch (err) {
    if (err instanceof KernelError) {
        console.error(err.code);   // 'INVALID_PARAMS'
        console.error(err.detail); // 'dx must be > 0'
    }
}
```

Error codes: `INVALID_HANDLE` | `INVALID_PARAMS` | `OPERATION_FAILED` | `IMPORT_FAILED` | `EXPORT_FAILED` | `NOT_INITIALIZED` | `UNKNOWN`

---

## Browser Usage

```html
<script type="module">
    import { createKernel } from './dist/index.mjs';

    const kernel = await createKernel();

    const box = kernel.createBox({ dx: 10, dy: 10, dz: 10 });
    const mesh = kernel.tessellate({ shape: box });
    // Use mesh.positions, mesh.normals, mesh.indices with WebGL/Three.js
    kernel.disposeShape({ shape: box });
</script>
```

CDN example:

```html
<script type="module">
    import { createKernel } from 'https://cdn.jsdelivr.net/npm/occt-kernel-wasm@VERSION/dist/index.mjs';

    const kernel = await createKernel();
    const box = kernel.createBox({ dx: 10, dy: 10, dz: 10 });
    console.log(kernel.getTopology(box));
    kernel.disposeShape({ shape: box });
</script>
```

See [`examples/browser/index.html`](examples/browser/index.html) for a complete demo.

---

## Node.js Usage

```js
const { createKernel } = require('occt-kernel-wasm');

async function main() {
    const kernel = await createKernel();

    const box = kernel.createBox({ dx: 10, dy: 10, dz: 10 });
    const step = kernel.exportStep({ shape: box });
    kernel.disposeShape({ shape: box });
    console.log(step);
}
main();
```

See [`examples/nodejs/demo.js`](examples/nodejs/demo.js) for a complete demo.

---

## Building from source

### Prerequisites

| Tool       | Version | Purpose                |
|------------|---------|------------------------|
| Node.js    | ≥ 18    | TypeScript / tests     |
| CMake      | ≥ 3.20  | Build system           |
| Emscripten | ≥ 3.1   | C++ → WASM             |
| OCCT       | 7.8.x   | CAD kernel             |

### Steps

```bash
# 1. Install Node.js dependencies
npm install

# 2. Build OCCT for Emscripten (dispatches to PowerShell on Windows)
npm run build:occt

# 3. Build the WASM module
npm run build:wasm

# 4. Build the publishable dist/
npm run build
```

On Windows, `npm run build:occt` and `npm run build:wasm` use the PowerShell wrappers in `scripts/` so the local Emscripten `.bat` entrypoints are used directly.

For faster local iteration, use `npm run build:wasm:fast`. That uses a separate `Fast` CMake build with low optimization and no debug source maps, and the wrapper scripts reuse the existing CMake configure unless you explicitly request a reconfigure.

Output: `dist/occt-kernel.js`, `dist/occt-kernel.wasm`, `dist/index.js`, `dist/index.mjs`, `dist/index.d.ts`

---

## Testing

```bash
npm test            # all tests
npm run test:watch  # watch mode
npm run test:coverage
```

Tests run entirely in Node.js using the mock adapter — no WASM binary required.

---

## Limitations

- Full raw OCCT API bindings are intentionally not exposed.
- Sketch constraint solving is not included.
- Parametric feature trees are not included.
- Assembly modelling is not in scope for v1.
- STEP import requires the WASM binary (not available in the mock adapter for production use).

---

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full layer diagram, design decisions, and extension guide.

---

## License

MIT — see [LICENSE](LICENSE).

OCCT is distributed under LGPL-2.1 with an exception. See [NOTICE](NOTICE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for details.
