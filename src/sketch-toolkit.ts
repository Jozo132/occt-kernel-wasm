/**
 * sketch-toolkit public API entry point.
 *
 * @module occt-kernel-wasm/sketch-toolkit
 */

export * from './sketch-api';

import type { SketchToolkit, SketchToolkitWasmModule } from './sketch-kernel';

export interface CreateSketchToolkitOptions {
    wasmModule?: SketchToolkitWasmModule;
}

export declare function createSketchToolkit(wasmModule?: SketchToolkitWasmModule): Promise<SketchToolkit>;
export declare function createSketchToolkit(options?: CreateSketchToolkitOptions): Promise<SketchToolkit>;
