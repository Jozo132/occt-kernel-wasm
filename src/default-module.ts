import type { WasmModule } from './kernel';

export type WasmModuleFactory = (opts?: Record<string, unknown>) => Promise<WasmModule>;

export function unwrapFactory(candidate: unknown): WasmModuleFactory | undefined {
    if (typeof candidate === 'function') {
        return candidate as WasmModuleFactory;
    }

    if (typeof candidate === 'object' && candidate !== null) {
        const maybeDefault = (candidate as { default?: unknown }).default;
        if (typeof maybeDefault === 'function') {
            return maybeDefault as WasmModuleFactory;
        }
    }

    return undefined;
}