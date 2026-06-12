import { KernelError } from './errors';
import { unwrapFactory, type WasmModuleFactory } from './default-module';
import { SketchToolkit, type SketchToolkitWasmModule } from './sketch-kernel';
import type { CreateSketchToolkitOptions } from './sketch-toolkit';

export * from './sketch-api';

function normalizeCreateSketchToolkitInput(input?: SketchToolkitWasmModule | CreateSketchToolkitOptions): CreateSketchToolkitOptions {
    if (!input) {
        return {};
    }
    if (typeof input === 'object' && 'wasmModule' in input) {
        return input as CreateSketchToolkitOptions;
    }
    return { wasmModule: input as SketchToolkitWasmModule };
}

function loadNodeFactory(): WasmModuleFactory {
    const { join } = require('node:path') as typeof import('node:path');

    let candidate: unknown;
    try {
        candidate = require(join(__dirname, 'sketch-toolkit.wasm.js'));
    } catch (error) {
        const detail = error instanceof Error ? error.message : String(error);
        throw new KernelError('NOT_INITIALIZED', `Failed to load sketch-toolkit.wasm.js from dist: ${detail}`);
    }

    const factory = unwrapFactory(candidate);
    if (!factory) {
        throw new KernelError('NOT_INITIALIZED', 'dist/sketch-toolkit.wasm.js did not export a WASM module factory');
    }
    return factory;
}

function loadNodeWasmBinary(): Uint8Array {
    const { readFileSync } = require('node:fs') as typeof import('node:fs');
    const { join } = require('node:path') as typeof import('node:path');

    try {
        return readFileSync(join(__dirname, 'sketch-toolkit.wasm.wasm'));
    } catch (error) {
        const detail = error instanceof Error ? error.message : String(error);
        throw new KernelError('NOT_INITIALIZED', `Failed to load sketch-toolkit.wasm.wasm from dist: ${detail}`);
    }
}

export async function createSketchToolkit(input?: SketchToolkitWasmModule | CreateSketchToolkitOptions): Promise<SketchToolkit> {
    const options = normalizeCreateSketchToolkitInput(input);
    if (options.wasmModule) {
        return new SketchToolkit(options.wasmModule);
    }

    const mod = await loadNodeFactory()({ wasmBinary: loadNodeWasmBinary() });
    return new SketchToolkit(mod as unknown as SketchToolkitWasmModule);
}
