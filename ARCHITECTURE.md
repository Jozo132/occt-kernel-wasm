# Architecture

## Overview

`occt-kernel-wasm` is a narrow, high-level WebAssembly adapter for Open CASCADE
Technology (OCCT). It exposes a stable, domain-oriented API for exact solid modelling
in browser and Node.js environments, without leaking any OCCT types into JavaScript.

---

## Layer Diagram

```
┌─────────────────────────────────────────────────────────┐
│  Consumer application (TypeScript / JavaScript)          │
├─────────────────────────────────────────────────────────┤
│  TypeScript wrapper  (src/)                              │
│  - OcctKernel class                                      │
│  - Shape handles (opaque integer IDs)                    │
│  - Structured error objects                              │
│  - JSON-serializable inputs and outputs                  │
├─────────────────────────────────────────────────────────┤
│  Emscripten JS glue  (dist/occt-kernel.js)               │
│  Generated automatically by emcc from bindings/         │
├─────────────────────────────────────────────────────────┤
│  Embind boundary  (bindings/bindings.cpp)                │
│  - Exposes OcctKernel C++ class via Emscripten embind   │
│  - No OCCT types cross this boundary                    │
├─────────────────────────────────────────────────────────┤
│  C++ adapter  (cpp/)                                     │
│  - kernel.h / kernel.cpp                                │
│  - profile_builder.h / profile_builder.cpp              │
│  - Manages shape handle map (uint32_t → TopoDS_Shape)   │
│  - All OCCT operations implemented here                 │
├─────────────────────────────────────────────────────────┤
│  OCCT 7.8  (third-party, linked statically into WASM)   │
└─────────────────────────────────────────────────────────┘
```

---

## Directory Structure

```
occt-kernel-wasm/
├── .github/
│   └── workflows/
│       └── ci.yml             # GitHub Actions CI
├── bindings/
│   └── bindings.cpp           # Emscripten embind boundary
├── cpp/
│   ├── kernel.h               # OcctKernel class declaration
│   ├── kernel.cpp             # OcctKernel implementation
│   ├── profile_builder.h      # 2-D profile → OCCT wire/face helpers
│   └── profile_builder.cpp
├── dist/                      # Build output (git-ignored)
│   ├── occt-kernel.js
│   ├── occt-kernel.wasm
│   └── occt-kernel.d.ts       # Generated type declarations
├── examples/
│   ├── browser/               # Browser usage demo (HTML + JS)
│   └── nodejs/                # Node.js usage demo
├── scripts/
│   ├── build-occt.sh          # Download and build OCCT for Emscripten
│   ├── build-wasm.sh          # Build the WASM module
│   └── build-native.sh        # Build native (for local dev / testing)
├── src/
│   ├── index.ts               # Public entry point
│   ├── types.ts               # All public TypeScript types
│   ├── errors.ts              # Structured error types
│   ├── kernel.ts              # OcctKernel wrapper class
│   └── mock-adapter.ts        # In-process stub used by tests without WASM
├── tests/
│   ├── unit/                  # Pure TypeScript unit tests
│   ├── geometry/              # Geometry / modelling regression tests
│   └── smoke/                 # Browser and Node.js smoke tests
├── ARCHITECTURE.md            # This file
├── CONTRIBUTING.md
├── LICENSE                    # MIT
├── NOTICE
├── THIRD_PARTY_LICENSES.md
├── CMakeLists.txt             # Root CMake build
├── package.json
└── tsconfig.json
```

---

## Key Design Decisions

### 1. Opaque Integer Handles

All shapes are represented by `uint32_t` integer IDs on both the C++ and TypeScript
sides. The C++ kernel keeps an `std::unordered_map<uint32_t, TopoDS_Shape>` and
returns IDs to the JS layer. This prevents OCCT types from ever crossing the WASM
boundary and makes serialisation trivial.

### 2. JSON-Only Boundary

Inputs that are too complex to pass as scalar arguments (profiles, tessellation
results, topology metadata) are passed as JSON strings. This keeps the embind surface
minimal and avoids complex binding of OCCT data structures.

### 3. Explicit Memory Management

Shapes are explicitly disposed of by calling `disposeShape(id)`. The kernel makes no
assumption about when handles should be collected. Consumers are expected to track and
dispose shapes when they are no longer needed.

### 4. Separation of Kernel State

The `OcctKernel` C++ class is the only stateful object. A new instance is created via
`createKernel()`. Multiple independent kernel instances are allowed. There is no
hidden global state.

### 5. Error Reporting

C++ errors are caught, serialised to a JSON error descriptor, and returned to the JS
side rather than thrown directly. The TypeScript wrapper converts these to typed
`KernelError` objects. Emscripten exception handling (`-fwasm-exceptions`) is enabled
to catch C++ exceptions across the WASM boundary.

---

## Build System

Two build configurations are supported:

| Target         | Toolchain          | Output                         |
|----------------|--------------------|--------------------------------|
| Native         | CMake + host GCC   | `build-native/occt-kernel`     |
| WebAssembly    | CMake + Emscripten | `dist/occt-kernel.{js,wasm}`   |

The native build is used during local development and for running geometry regression
tests without a browser. The WASM build is the publishable artefact.

### Building OCCT for Emscripten

OCCT must be compiled for the Emscripten target before the WASM build. The script
`scripts/build-occt.sh` automates this. It downloads the OCCT source, applies any
necessary patches, and builds the required subset of OCCT libraries statically.

Only the following OCCT toolkit groups are required:

- **FoundationClasses** – basic types, NCollection
- **ModelingData** – BRep topology, geometry
- **ModelingAlgorithms** – boolean operations, fillets, chamfers, primitives
- **DataExchange** – STEP reader/writer
- **Visualization** – BRepMesh for tessellation

---

## Data Flows

### Create Box → Tessellate

```
JS: kernel.createBox({ dx: 10, dy: 10, dz: 10 })
  → C++: OcctKernel::createBox(10, 10, 10)
      → BRepPrimAPI_MakeBox(10, 10, 10).Shape()
      → storeShape(shape) → returns handle id
  ← JS receives: { id: 1 }

JS: kernel.tessellate({ shape: { id: 1 }, linearDeflection: 0.1 })
  → C++: OcctKernel::tessellate(1, 0.1, 0.5)
      → BRepMesh_IncrementalMesh(shape, 0.1)
      → iterate faces, extract triangles → serialize JSON
  ← JS receives: { positions: Float32Array, normals: Float32Array, indices: Uint32Array }
```

### STEP Export

```
JS: kernel.exportStep({ shape: { id: 1 } })
  → C++: OcctKernel::exportStep(1)
      → STEPControl_Writer → write to memory stream → return string
  ← JS receives: string (STEP file content)
```

---

## Extending the API

To add a new operation:

1. Add a method to `OcctKernel` in `cpp/kernel.h` and implement it in `cpp/kernel.cpp`.
2. Expose it via embind in `bindings/bindings.cpp`.
3. Add a corresponding method to the `OcctKernel` TypeScript class in `src/kernel.ts`.
4. Add type definitions in `src/types.ts` as needed.
5. Write a test in `tests/geometry/` or `tests/unit/`.

---

## Upgrading OCCT

See `scripts/build-occt.sh` for the pinned version and download URL. To upgrade:

1. Update `OCCT_VERSION` in `scripts/build-occt.sh`.
2. Re-run `scripts/build-occt.sh`.
3. Re-run `scripts/build-wasm.sh`.
4. Run the full test suite to verify no regressions.
5. Update `THIRD_PARTY_LICENSES.md` if the OCCT license text changed.
