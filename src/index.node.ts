import { KernelError } from './errors';
import { unwrapFactory, type WasmModuleFactory } from './default-module';
import { OcctKernel, type WasmModule } from './kernel';
import type { CreateKernelOptions, KernelVariant } from './index';

export * from './api';

function normalizeCreateKernelInput(input?: WasmModule | CreateKernelOptions): CreateKernelOptions {
    if (!input) {
        return {};
    }
    if (typeof input === 'object' && ('variant' in input || 'wasmModule' in input)) {
        return input as CreateKernelOptions;
    }
    return { wasmModule: input as WasmModule };
}

function normalizeKernelVariant(variant?: KernelVariant): KernelVariant {
    return variant === 'mt' ? 'mt' : 'st';
}

function kernelBasename(variant: KernelVariant): string {
    return variant === 'mt' ? 'occt-kernel.mt' : 'occt-kernel.st';
}

function loadNodeFactory(variant: KernelVariant): WasmModuleFactory {
    const { join } = require('node:path') as typeof import('node:path');

    let candidate: unknown;
    try {
        candidate = require(join(__dirname, `${kernelBasename(variant)}.js`));
    } catch (error) {
        const detail = error instanceof Error ? error.message : String(error);
        throw new KernelError('NOT_INITIALIZED', `Failed to load ${kernelBasename(variant)}.js from dist: ${detail}`);
    }

    const factory = unwrapFactory(candidate);
    if (!factory) {
        throw new KernelError('NOT_INITIALIZED', `dist/${kernelBasename(variant)}.js did not export a WASM module factory`);
    }

    return factory;
}

function loadNodeWasmBinary(variant: KernelVariant): Uint8Array {
    const { readFileSync } = require('node:fs') as typeof import('node:fs');
    const { join } = require('node:path') as typeof import('node:path');

    try {
        return readFileSync(join(__dirname, `${kernelBasename(variant)}.wasm`));
    } catch (error) {
        const detail = error instanceof Error ? error.message : String(error);
        throw new KernelError('NOT_INITIALIZED', `Failed to load ${kernelBasename(variant)}.wasm from dist: ${detail}`);
    }
}

export async function createKernel(wasmModule?: WasmModule): Promise<OcctKernel> {
    const options = normalizeCreateKernelInput(wasmModule);
    if (options.wasmModule) {
        return new OcctKernel(options.wasmModule);
    }

    const variant = normalizeKernelVariant(options.variant);
    const mod = await loadNodeFactory(variant)({ wasmBinary: loadNodeWasmBinary(variant) });
    return new OcctKernel(mod);
}