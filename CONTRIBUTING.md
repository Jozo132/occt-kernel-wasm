# Contributing to occt-kernel-wasm

Thank you for your interest in contributing! This document outlines the contribution
process, coding standards, and how to run the test suite.

---

## Code of Conduct

Be respectful. Contributions of all kinds are welcome — bug reports, documentation
improvements, new features, and tests.

---

## Before You Start

- For non-trivial changes, open an issue first to discuss the approach.
- Keep changes focused. One logical change per pull request.
- All public API changes must include corresponding TypeScript types and tests.

---

## Development Setup

### Prerequisites

| Tool          | Version     | Purpose                        |
|---------------|-------------|--------------------------------|
| Node.js       | ≥ 18        | TypeScript tooling and tests   |
| npm           | ≥ 9         | Package manager                |
| CMake         | ≥ 3.20      | Build system                   |
| Emscripten    | ≥ 3.1       | C++ → WASM compiler            |
| Python 3      | ≥ 3.8       | Emscripten dependency          |
| OCCT          | 7.8.x       | CAD kernel (see below)         |

### Install Node.js Dependencies

```bash
npm install
```

### Build OCCT for Emscripten

```bash
bash scripts/build-occt.sh
```

This downloads and builds the OCCT libraries needed for the WASM target. It only
needs to be run once (or when upgrading OCCT).

### Build the WASM Module

```bash
bash scripts/build-wasm.sh
```

Output is placed in `dist/`.

### Build Native (for local testing without Emscripten)

```bash
bash scripts/build-native.sh
```

---

## Running Tests

```bash
# All tests (TypeScript unit tests + geometry stubs)
npm test

# Watch mode
npm run test:watch

# Coverage
npm run test:coverage
```

The TypeScript tests run entirely in Node.js using the mock adapter — no WASM binary
required. Geometry regression tests that require the compiled WASM binary are skipped
if the binary is not present.

---

## Code Style

### TypeScript

- Strict TypeScript (`strict: true` in tsconfig.json).
- No `any` unless unavoidable and annotated with a comment.
- Prefer `readonly` properties.
- Use `interface` for public API shapes, `type` for unions and aliases.
- Error objects must extend or use `KernelError`.

### C++

- C++17.
- RAII for all resource management.
- Catch all OCCT exceptions and convert to string error descriptors.
- No raw owning pointers — use `Handle<>` and stack allocation.
- Format with `clang-format` using the project `.clang-format` file.

---

## API Design Rules

Any contribution that touches the public API must follow these rules:

1. **High-level and domain-oriented.** Expose CAD concepts, not OCCT internals.
2. **No OCCT types in TypeScript.** All shapes are opaque integer handles.
3. **JSON-serializable inputs and outputs.** No binary blobs in parameters.
4. **Explicit memory management.** Provide a `disposeShape` path for all shapes.
5. **Structured errors.** Never throw raw strings; always use `KernelError`.
6. **Deterministic.** Operations with the same inputs must return the same shape.

---

## Pull Request Checklist

- [ ] Tests pass (`npm test`)
- [ ] New code is covered by tests
- [ ] TypeScript builds without errors (`npm run build`)
- [ ] Public API changes are documented in README.md
- [ ] No OCCT types exposed in TypeScript
- [ ] `ARCHITECTURE.md` updated if architecture changed
- [ ] `THIRD_PARTY_LICENSES.md` updated if a new dependency was added

---

## Releasing

Releases are tagged `vMAJOR.MINOR.PATCH`. After merging to `main`:

1. Update `version` in `package.json`.
2. Add a changelog entry.
3. Push a version tag: `git tag v1.1.0 && git push origin v1.1.0`.
4. The GitHub Actions CI will publish to npm automatically.

---

## Questions

Open an issue on GitHub. Please include your OS, Node.js version, and a minimal
reproduction if applicable.
