# occt-kernel-wasm

Reusable OCCT-based CAD kernel for WebAssembly with a small TypeScript API for exact solid modeling.

The repository also contains an in-progress `sketch-toolkit` WASM target for the next
generation 2D sketch solver. It builds separately from the OCCT target so sketch work
does not require a full OCCT rebuild.

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
| Sketch sweep / loft            | ✅     |
| Boolean union / subtract / intersect | ✅ |
| Fillet / chamfer               | ✅     |
| Structured native blend specs  | ✅     |
| Structured sketch feature specs | ✅    |
| Exact edge/face interrogation  | ✅     |
| Exact analysis + point containment | ✅ |
| Topology query (solids/shells/wires/faces/edges/vertices/bbox) | ✅ |
| Validity check                | ✅     |
| Tessellation for WebGL        | ✅     |
| STEP import                   | ✅     |
| STEP export                   | ✅     |
| Version + session metadata    | ✅     |
| Structured error objects      | ✅     |
| Browser + Node.js support     | ✅     |

---

## Install

```bash
npm install occt-kernel-wasm
```

> **Note:** The npm package ships with a pre-compiled WASM binary. If you need to rebuild against a different OCCT version, see [Building from source](#building-from-source).

## Additional WASM targets

- `npm run build:sketch` builds the in-progress `dist/sketch-toolkit.wasm.js` and
    `dist/sketch-toolkit.wasm` artifacts.
- `npm run build:wasm` still builds the OCCT-backed exact modelling target.

---

## API

```ts
import { createKernel } from 'occt-kernel-wasm';

const kernel = await createKernel();
```

All shapes are represented by opaque `ShapeHandle` objects (`{ id: number, sessionId: string }`). Resident handles are valid only inside the kernel session that created them; passing a handle to a different kernel instance throws `SESSION_MISMATCH`. No OCCT types are ever exposed.

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

### Structured sketch feature specs

The last three feature commits added versioned CAD-style feature APIs on top of the legacy sketch helpers. Use these methods when you need exact OCCT feature behavior, richer end conditions, native feature lineage, and machine-readable capability discovery through `getCapabilities()` / `getOperationSchema()`.

Structured profile extrude and extrude cut:

```ts
const base = kernel.createBox({ dx: 80, dy: 60, dz: 20 });

const bossProfile = {
    segments: [
        { type: 'line', start: [0, 0], end: [24, 0] },
        { type: 'line', start: [24, 0], end: [24, 18] },
        { type: 'line', start: [24, 18], end: [0, 18] },
        { type: 'line', start: [0, 18], end: [0, 0] },
    ],
};

const boss = kernel.extrudeProfileWithSpec({
    shape: base,
    profile: bossProfile,
    spec: {
        schemaVersion: 1,
        plane: {
            origin: [20, 20, 20],
            normal: [0, 0, 1],
            xDirection: [1, 0, 0],
        },
        draftAngleDegrees: 2,
        extent: {
            type: 'blind',
            distance: 16,
        },
    },
});

const topFace = kernel.getTopology(boss).faces?.[0];

const pocket = kernel.extrudeCutProfileWithSpec({
    shape: boss,
    profile: {
        segments: [
            { type: 'line', start: [4, 4], end: [20, 4] },
            { type: 'line', start: [20, 4], end: [20, 14] },
            { type: 'line', start: [20, 14], end: [4, 14] },
            { type: 'line', start: [4, 14], end: [4, 4] },
        ],
    },
    spec: {
        schemaVersion: 1,
        plane: {
            origin: [20, 20, 20],
            normal: [0, 0, 1],
            xDirection: [1, 0, 0],
        },
        extent: {
            type: 'offsetFromSurface',
            surface: { face: { topoId: topFace?.id ?? 1 } },
            offset: 3,
        },
    },
});
```

`extrudeProfileWithSpec` supports draft plus blind, `upToNext`, `throughAll`, `upToSurface`, and `offsetFromSurface` end conditions. The subtractive path is exposed as `extrudeCutProfileWithSpec`.

Structured profile revolve and revolve cut:

```ts
const revolveResult = kernel.revolveProfileWithSpec({
    shape: base,
    profile: {
        segments: [
            { type: 'line', start: [0, 0], end: [8, 0] },
            { type: 'line', start: [8, 0], end: [8, 14] },
            { type: 'line', start: [8, 14], end: [0, 14] },
            { type: 'line', start: [0, 14], end: [0, 0] },
        ],
    },
    spec: {
        schemaVersion: 1,
        plane: {
            origin: [0, 30, 0],
            normal: [0, -1, 0],
            xDirection: [1, 0, 0],
        },
        axisOrigin: [0, 30, 0],
        axisDirection: [0, 0, 1],
        extent: {
            type: 'angle',
            angleDegrees: 225,
        },
    },
});

const revolveCut = kernel.revolveProfileWithSpec({
    shape: revolveResult,
    cut: true,
    profile: {
        segments: [
            { type: 'line', start: [0, 2], end: [5, 2] },
            { type: 'line', start: [5, 2], end: [5, 10] },
            { type: 'line', start: [5, 10], end: [0, 10] },
            { type: 'line', start: [0, 10], end: [0, 2] },
        ],
    },
    spec: {
        schemaVersion: 1,
        plane: {
            origin: [0, 30, 0],
            normal: [0, -1, 0],
            xDirection: [1, 0, 0],
        },
        axisOrigin: [0, 30, 0],
        axisDirection: [0, 0, 1],
        extent: {
            type: 'throughAll',
        },
    },
});
```

`revolveProfileWithSpec` supports additive and subtractive revolve through `cut?: boolean`, plus angle, `upToSurface`, `fromSurfaceToSurface`, `throughAll`, and `upToSurfaceAtAngle` extents. If you prefer separate calls, `revolveCutProfileWithSpec` remains available too.

Structured sweep and loft:

```ts
const path = {
    segments: [
        { type: 'line', start: [40, 30, 20], end: [40, 30, 50] },
    ],
};

const sweep = kernel.sweepProfileWithSpec({
    shape: pocket,
    profile: {
        segments: [
            { type: 'line', start: [-4, -4], end: [4, -4] },
            { type: 'line', start: [4, -4], end: [4, 4] },
            { type: 'line', start: [4, 4], end: [-4, 4] },
            { type: 'line', start: [-4, 4], end: [-4, -4] },
        ],
    },
    spec: {
        schemaVersion: 1,
        plane: {
            origin: [40, 30, 20],
            normal: [0, 0, 1],
            xDirection: [1, 0, 0],
        },
        spine: path,
        trihedronMode: { type: 'discrete' },
        sectionWithCorrection: true,
        solid: true,
        transitionMode: 'roundCorner',
    },
});

const loft = kernel.loftWithSpec({
    shape: sweep,
    sections: [
        {
            type: 'profile',
            profile: {
                segments: [
                    { type: 'line', start: [-6, -6], end: [6, -6] },
                    { type: 'line', start: [6, -6], end: [6, 6] },
                    { type: 'line', start: [6, 6], end: [-6, 6] },
                    { type: 'line', start: [-6, 6], end: [-6, -6] },
                ],
            },
            plane: {
                origin: [60, 20, 20],
                normal: [0, 0, 1],
                xDirection: [1, 0, 0],
            },
        },
        {
            type: 'wire',
            wire: {
                segments: [
                    { type: 'line', start: [56, 16, 34], end: [64, 16, 34] },
                    { type: 'line', start: [64, 16, 34], end: [64, 24, 34] },
                    { type: 'line', start: [64, 24, 34], end: [56, 24, 34] },
                    { type: 'line', start: [56, 24, 34], end: [56, 16, 34] },
                ],
            },
        },
        {
            type: 'point',
            point: [60, 20, 46],
        },
    ],
    spec: {
        schemaVersion: 1,
        solid: true,
        smoothing: true,
        parametrization: 'centripetal',
        continuity: 'C1',
    },
});

const loftCut = kernel.loftWithSpec({
    shape: loft,
    cut: true,
    sections: [
        {
            type: 'profile',
            profile: {
                segments: [
                    { type: 'line', start: [-5, -5], end: [5, -5] },
                    { type: 'line', start: [5, -5], end: [5, 5] },
                    { type: 'line', start: [5, 5], end: [-5, 5] },
                    { type: 'line', start: [-5, 5], end: [-5, -5] },
                ],
            },
            plane: {
                origin: [15, 15, -1],
                normal: [0, 0, 1],
                xDirection: [1, 0, 0],
            },
        },
        {
            type: 'profile',
            profile: {
                segments: [
                    { type: 'line', start: [-3, -3], end: [3, -3] },
                    { type: 'line', start: [3, -3], end: [3, 3] },
                    { type: 'line', start: [3, 3], end: [-3, 3] },
                    { type: 'line', start: [-3, 3], end: [-3, -3] },
                ],
            },
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
```

`sweepProfileWithSpec` uses `cut?: boolean` for additive vs subtractive sweep and supports spine wires, trihedron modes, solid output, transition modes, tolerances, and continuity controls. `loftWithSpec` accepts `profile`, `wire`, and `point` sections plus `cut?: boolean`; profile sections must be single-wire closed loops, matching the native OCCT builder restriction enforced by the wrapper.

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

Simple modifiers keep their original all-edge behavior. For feature-level CAD workflows, use the structured native blend APIs so OCCT owns exact blend construction and the JS layer receives lineage metadata instead of rebuilding blends from mesh edges.

```ts
const topo = kernel.getTopology(box);
const edge = topo.edges?.[0];
const referenceFace = edge?.topoFaceIds?.[0];

const filletResult = kernel.filletEdgesWithSpec({
    shape: box,
    spec: {
        schemaVersion: 1,
        edges: [{ topoId: edge?.id ?? 1, radius: 2 }],
        blendShape: 'rational',
        continuity: 'C1',
        overflowMode: 'fail',
    },
});

const chamferResult = kernel.chamferEdgesWithSpec({
    shape: box,
    spec: {
        schemaVersion: 1,
        mode: 'twoDistance',
        edges: [{
            topoId: edge?.id ?? 1,
            distance1: 1,
            distance2: 2,
            referenceFace: { topoId: referenceFace ?? 1 },
        }],
    },
});

const nextShape = filletResult.shape;
const generatedBlendFaces = filletResult.blendFaces;
```

Fillets support constant radius, start/end radius, station radii, constant and linear radius laws, OCCT fillet shape modes (`rational`, `quasiAngular`, `polynomial`), and C0/C1/C2 continuity. Chamfers support symmetric, two-distance, and distance-angle modes with reference-face side selection. Tangent propagation is native and enabled. Partial-edge blends, disabled tangent propagation, and setback-style corner handling are reported as structured unsupported `INVALID_PARAMS` errors in this OCCT-backed build.

### Exact subshape evaluation

```ts
const edgeEval = kernel.evaluateEdge({
    shape: box,
    edge: { topoId: 1 },
    t: 0.5,
});

const edgeSamples = kernel.sampleEdge({
    shape: box,
    edge: { topoId: 1 },
    count: 16,
});

const edgeCurve = kernel.getEdgeCurve({ shape: box, edge: { topoId: 1 } });

const faceEval = kernel.evaluateFace({
    shape: box,
    face: { topoId: 1 },
    u: 0.5,
    v: 0.5,
});

const planarTrim = kernel.getPlanarFaceWires({
    shape: box,
    face: { topoId: 1 },
});
```

Edge and face references accept a runtime `topoId` or a `stableHash`. Edge and face evaluation use normalized parameters by default; pass `parameterMode: 'native'` on the reference to use the OCCT curve or surface parameter domain directly. `getEdgeCurve` returns analytic line/circle metadata where available and Bezier/B-spline poles, weights, knots, and multiplicities for exact curve types. `getPlanarFaceWires` is planar-face-only and returns the exact ordered trimmed loops twice: once in local plane coordinates and once as exact world-space edge curves. That makes it suitable for CAM boundaries, sketch/profile recovery, and other exact face-outline workflows without consulting tessellation.

### Topology query

```ts
const topo = kernel.getTopology(box);
// {
//   revisionId: 'rev_...', topologyHash: 'T:...', historySchemaVersion: 1,
//   operationId: 'op_...', operationType: 'createBox', operandRevisionIds: [],
//   identityStatus: 'generated', historyWarnings: [],
//   shapeType: 'solid', solidCount: 1, shellCount: 1, wireCount: 6,
//   faceCount: 6, edgeCount: 12, vertexCount: 8,
//   boundingBox: { xMin: 0, yMin: 0, zMin: 0, xMax: 100, yMax: 50, zMax: 25 },
//   isValid: true,
//   solids: [{ id: 1, shellIds: [1], status: 'generated' }],
//   shells: [{ id: 1, solidIds: [1], faceIds: [1, 2, 3, 4, 5, 6], status: 'generated' }],
//   wires: [{ id: 1, edgeIds: [1, 2, 3, 4], topoFaceIds: [1], status: 'generated' }],
//   faces: [{ id: 1, stableHash: 'F:...', status: 'generated' }],
//   edges: [{ id: 1, stableHash: 'E:...', topoFaceIds: [1, 2], status: 'generated' }],
//   vertices: [{ id: 1, stableHash: 'V:...', status: 'generated' }],
//   deletedEntities: []
// }

const capabilities = kernel.getCapabilities();
// featureEdgesV1, featurePreviewV1, tessellationOptionsV1, topologyHierarchyV1, versionInfoV1, analysisV1,
// sessionHandlesV1, triangleNormalsV1, topologySubshapesV1, historyV1,
// entityRemapV1, revisionRetentionV1, checkpointV1, and native exact blend
// operations are available. capabilities.analysis and capabilities.runtime
// describe exact mass/containment support and browser/worker/node coverage.
// capabilities.fillet / capabilities.chamfer describe supported modes and
// explicitly false unsupported modes.
// stableNamingV1 is true: semantic face/edge/vertex ids are materialized per
// revision and propagated through exact transform, boolean, and blend history
// instead of falling back to geometry-derived hashes.

const schema = kernel.getOperationSchema();
// Versioned machine-readable operation contracts for structured extrude,
// revolve, sweep, loft, fillet, chamfer, getVersionInfo, analyzeShape,
// classifyPointContainment, intersectShapes, findClosestPointOnShape,
// measureShapeDistance, evaluateEdge, sampleEdge, getEdgeCurve,
// evaluateFace, and getPlanarFaceWires.
```

### Version metadata and exact analysis

```ts
const version = kernel.getVersionInfo();
// {
//   libraryVersion: '1.2.0',
//   apiVersion: 1,
//   kernelVersion: '8.0.0',
//   checkpointSchemaVersion: 1,
//   operationSchemaVersion: 1,
//   sessionId: 'session_...',
//   supportedRuntimes: ['browser', 'worker', 'node']
// }

const analysis = kernel.analyzeShape({ shape: box });
// {
//   shapeType: 'solid',
//   solidCount: 1,
//   shellCount: 1,
//   wireCount: 6,
//   faceCount: 6,
//   edgeCount: 12,
//   vertexCount: 8,
//   boundingBox: { xMin: 0, yMin: 0, zMin: 0, xMax: 100, yMax: 50, zMax: 25 },
//   isValid: true,
//   volume: 125000,
//   surfaceArea: 25000,
//   linearLength: 700,
//   centerOfMass: [50, 25, 12.5],
//   centerOfMassBasis: 'volume'
// }

const inside = kernel.classifyPointContainment({
    shape: box,
    point: [50, 25, 12.5],
});
// { point: [50, 25, 12.5], tolerance: 1e-7, state: 'in', isInside: true }

const closest = kernel.findClosestPointOnShape({
    shape: box,
    point: [140, 25, 12.5],
});
// {
//   queryPoint: [140, 25, 12.5],
//   closestPoint: [100, 25, 12.5],
//   distance: 40,
//   support: { kind: 'face', topoId: 2, stableHash: 'F:...' }
// }

const moved = kernel.transformShape({
    shape: box,
    transform: { translation: [140, 0, 0] },
});

const clearance = kernel.measureShapeDistance({
    shapeA: box,
    shapeB: moved,
});
// {
//   distance: 40,
//   clearance: 40,
//   isInContact: false,
//   solutions: [{ pointOnA: [100, 25, 12.5], pointOnB: [140, 25, 12.5], ... }]
// }

const overlap = kernel.transformShape({
    shape: box,
    transform: { translation: [50, 10, 0] },
});

const section = kernel.intersectShapes({ shapeA: box, shapeB: overlap });
// { hasIntersection: true, edgeCount: 4, vertexCount: 4, sectionShape: { id: ..., sessionId: ... } }
```

### Revision history, remap, and checkpoints

```ts
const revision = kernel.getRevisionInfo(box);

const moved = kernel.transformShape({
    shape: box,
    transform: { translation: [25, 0, 0] },
});

const firstFaceHash = topo.faces?.[0]?.stableHash ?? '';
const resolved = kernel.resolveStableEntity({ shape: moved, stableHash: firstFaceHash });
const remap = kernel.mapEntitiesAcrossRevisions({
    fromRevisionId: revision.revisionId,
    toRevisionId: kernel.getRevisionInfo(moved).revisionId,
    stableHashes: [firstFaceHash],
});

const checkpoint = kernel.createCheckpoint({ shape: moved });
const restored = kernel.hydrateCheckpoint({ checkpoint });

kernel.retainRevision({ shape: restored });
kernel.releaseRevision({ shape: restored });
```

### Tessellation

```ts
const mesh = kernel.tessellate({ shape: box, linearDeflection: 0.1 });
// {
//   positions: Float32Array,  // [x0,y0,z0, x1,y1,z1, ...]
//   normals:   Float32Array,  // [nx0,ny0,nz0, ...]
//   indices:   Uint32Array,   // [i0,i1,i2, ...] triangles
//   triangleNormals?: Float32Array,
//   triangleTopoFaceIds?: Uint32Array,
//   triangleFaceGroups?: Uint32Array,
//   triangleStableHashes?: string[],
//   featureEdges?: [{
//     points: [[x, y, z], [x, y, z]],
//     isClosed: false,
//     chainId: 1,
//     faceIndices: [1, 2],
//     topoFaceIds: [1, 2],
//     isBoundary: false,
//     isSharp: true,
//     isSeam: false,
//     stableHash: 'E:...'
//   }],
//   rawEdgeSegments?: Float32Array // debug only; do not use for selection
// }
```

For interactive previews, omit heavyweight topology metadata and edge extraction:

```ts
const previewMesh = kernel.tessellate({
    shape: liveBlend,
    linearDeflection: 0.25,
    includeMetadata: false,
});
```

You can also tessellate only changed exact faces and opt individual metadata channels back in:

```ts
const facePatch = kernel.tessellate({
    shape: liveBlend,
    faces: [{ topoId: 12 }],
    includeMetadata: false,
    includeTriangleTopoFaceIds: true,
});
```

### Live feature previews

Use `previewFeature` while the user edits feature parameters. It runs the same exact feature implementation against the current resident shape, returns lightweight render feedback, and disposes the temporary exact result by default so the model is not committed.

```ts
const preview = kernel.previewFeature({
    operation: 'filletEdges',
    params: {
        shape: box,
        spec: { schemaVersion: 1, edges: [{ topoId: 1, radius: liveRadius }] },
    },
    includeWireframe: true,
    tessellation: { linearDeflection: 0.35, angularDeflection: 0.35 },
});

sceneMesh.geometry.setAttribute('position', new THREE.BufferAttribute(preview.mesh!.positions, 3));
wireOverlay.update(preview.wireframe ?? []);
```

The generic preview surface supports `extrudeProfile`, `extrudeCutProfile`, `revolveProfile`, `revolveCutProfile`, `sweepProfile`, `loft`, `filletEdges`, `chamferEdges`, `transformShape`, `booleanUnion`, `booleanSubtract`, and `booleanIntersect`. Pass `retainPreviewShape: true` only when the UI needs to inspect the temporary exact shape; otherwise apply the feature later with the normal exact operation once the user commits.

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

Tagged releases publish the same prebuilt `dist/` payload to npm and attach it to the GitHub release, so the package works through `npm install`, jsDelivr, and unpkg without requiring a local WASM rebuild.

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
| OCCT       | 8.0.x   | CAD kernel             |

### Steps

```bash
# 1. Install Node.js dependencies
npm install

# 2. Build OCCT for Emscripten (dispatches to PowerShell on Windows)
npm run build:occt

# Optional: build the pthread OCCT cache only when you actually need it
npm run build:occt:mt

# 3. Build the WASM module
npm run build:wasm

# Optional: build both exported kernel variants once the mt OCCT cache exists
npm run build:wasm:all

# 3b. Build the mandatory sketch toolkit runtime used by modeller
npm run build:sketch

# 4. Build the publishable dist/
npm run build

# Or build every OCCT + WASM + sketch artifact in one go
npm run build:all
```

On Windows, `npm run build:occt`, `npm run build:wasm`, and `npm run build:sketch` use the PowerShell wrappers in `scripts/` so the local Emscripten `.bat` entrypoints are used directly.

The OCCT build scripts pin `V8_0_0` and keep the V8 source, build, and install trees in a versioned local cache outside the workspace by default. On Windows that defaults to `%LOCALAPPDATA%\occt-kernel-wasm\V8_0_0`; on Unix-like systems it defaults to `$XDG_CACHE_HOME/occt-kernel-wasm/V8_0_0` or `~/.cache/occt-kernel-wasm/V8_0_0`. Set `OCCT_WASM_CACHE_ROOT` to override that location.

If the existing `third-party/occt-src` checkout has local changes, the build preserves it and uses the cached V8 checkout instead of overwriting that tree.

For faster local iteration, use `npm run build:wasm:fast`. That builds only the single-threaded kernel with a separate `Fast` CMake build, low optimization, and no debug source maps, and the wrapper scripts reuse the existing CMake configure unless you explicitly request a reconfigure.

`npm run build:wasm` now stays on the single-threaded `st` artifact by default so normal modelling work does not trigger an unnecessary threaded rebuild. Use `npm run build:wasm:all` when you explicitly need both exported kernel variants.

`npm run build:sketch` emits the mandatory `dist/sketch-toolkit.wasm.js` and `dist/sketch-toolkit.wasm.wasm` browser runtime files used by `modeller`.

`npm run build:all` produces the OCCT `st` and `mt` caches, both exported kernel variants, the sketch toolkit runtime, and the publishable TypeScript entrypoints in one pass. The legacy `dist/occt-kernel.js` and `dist/occt-kernel.wasm` paths remain as compatibility aliases for the `st` build, and the release workflow uses that full build before publishing to npm and creating the GitHub release assets.

The `mt` kernel must link against a separate OCCT cache built with pthread atomics, so its OCCT install lives at `.../V8_0_0/i-mt` instead of sharing the single-threaded `.../V8_0_0/i` archive set.

The OCCT build scripts are cache-aware per variant. Once `st` or `mt` has been built successfully, rerunning the same command reuses the existing OCCT install instead of recompiling the whole dependency stack. Use `--reconfigure` only when you intentionally want to invalidate that cached OCCT variant.

Output: `dist/occt-kernel.st.js`, `dist/occt-kernel.st.wasm`, `dist/occt-kernel.mt.js`, `dist/occt-kernel.mt.wasm`, `dist/occt-kernel.js`, `dist/occt-kernel.wasm`, `dist/index.js`, `dist/index.mjs`, `dist/index.d.ts`

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
