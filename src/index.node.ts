import { KernelError } from './errors';
import { unwrapFactory, type WasmModuleFactory } from './default-module';
import { OcctKernel, type WasmModule } from './kernel';

export * from './api';

function loadNodeFactory(): WasmModuleFactory {
    const { join } = require('node:path') as typeof import('node:path');

    let candidate: unknown;
    try {
        candidate = require(join(__dirname, 'occt-kernel.js'));
    } catch (error) {
        const detail = error instanceof Error ? error.message : String(error);
        throw new KernelError('NOT_INITIALIZED', `Failed to load occt-kernel.js from dist: ${detail}`);
    }

    const factory = unwrapFactory(candidate);
    if (!factory) {
        throw new KernelError('NOT_INITIALIZED', 'dist/occt-kernel.js did not export a WASM module factory');
    }

    return factory;
}

function loadNodeWasmBinary(): Uint8Array {
    const { readFileSync } = require('node:fs') as typeof import('node:fs');
    const { join } = require('node:path') as typeof import('node:path');

    try {
        return readFileSync(join(__dirname, 'occt-kernel.wasm'));
    } catch (error) {
        const detail = error instanceof Error ? error.message : String(error);
        throw new KernelError('NOT_INITIALIZED', `Failed to load occt-kernel.wasm from dist: ${detail}`);
    }
}

export async function createKernel(wasmModule?: WasmModule): Promise<OcctKernel> {
    if (wasmModule) {
        return new OcctKernel(wasmModule);
    }

    const mod = await loadNodeFactory()({ wasmBinary: loadNodeWasmBinary() });
    return new OcctKernel(mod);
}