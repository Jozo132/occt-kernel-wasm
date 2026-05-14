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

export * from './api';

import type { OcctKernel, WasmModule } from './kernel';

/**
 * Create and initialise an {@link OcctKernel} instance.
 *
 * The published package provides runtime-specific implementations for Node.js
 * and browser/CDN consumers. This declaration-only entry keeps the shared type
 * surface at `dist/index.d.ts`.
 */
export declare function createKernel(wasmModule?: WasmModule): Promise<OcctKernel>;
