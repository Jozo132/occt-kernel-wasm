/**
 * occt-kernel-wasm public API entry point.
 *
 * @module occt-kernel-wasm
 *
 * @example Browser (ESM)
 * ```ts
 * import { createKernel } from 'occt-kernel-wasm';
 * const kernel = await createKernel();
 * ```
 *
 * @example Node.js (CommonJS)
 * ```js
 * const { createKernel } = require('occt-kernel-wasm');
 * const kernel = await createKernel();
 * ```
 */

export type {
    BooleanParams,
    BoundingBox,
    BoxParams,
    ChamferParams,
    CylinderParams,
    DisposeParams,
    ExportStepParams,
    ExtrudeParams,
    FilletParams,
    ImportStepParams,
    Profile,
    ProfileSegment,
    ProfileSegmentArc,
    ProfileSegmentCircle,
    ProfileSegmentLine,
    RevolveParams,
    ShapeHandle,
    SphereParams,
    TessellateParams,
    TessellationResult,
    TopologyResult,
} from './types';

export { KernelError } from './errors';
export type { KernelErrorCode } from './errors';
export { OcctKernel } from './kernel';
export type { NativeKernel, WasmModule } from './kernel';

import { OcctKernel, type WasmModule } from './kernel';

/**
 * Create and initialise an {@link OcctKernel} instance.
 *
 * In browser environments the function dynamically imports the compiled WASM
 * module from `../dist/occt-kernel.js` (relative to the package root). In
 * Node.js the same file is loaded via `require`.
 *
 * Supply a custom `wasmModule` to override the default loader – useful in tests
 * or when hosting the WASM binary at a custom URL.
 *
 * @param wasmModule – Optional pre-loaded WASM module factory (for testing / custom hosting).
 */
export async function createKernel(wasmModule?: WasmModule): Promise<OcctKernel> {
    if (wasmModule) {
        return new OcctKernel(wasmModule);
    }

    // Dynamic import so bundlers can handle the WASM load correctly.
    //
    // The WASM module is expected at `dist/occt-kernel.js` relative to the
    // package root — i.e. next to the compiled TypeScript output in `dist/`.
    // This path matches the output of `scripts/build-wasm.sh`.
    //
    // For custom hosting (e.g. a CDN URL or a different directory), pass
    // a pre-loaded `wasmModule` to this function instead of relying on this
    // default loader. The wasmModule parameter is the recommended approach for
    // browser applications that bundle the WASM separately.
    //
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const factory = (await import('../dist/occt-kernel.js' as string)) as {
        default: (opts?: Record<string, unknown>) => Promise<WasmModule>;
    };
    const mod = await factory.default();
    return new OcctKernel(mod);
}
