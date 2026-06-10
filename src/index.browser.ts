import { KernelError } from './errors';
import { unwrapFactory, type WasmModuleFactory } from './default-module';
import { OcctKernel, type WasmModule } from './kernel';
import type { CreateKernelOptions, KernelVariant } from './index';

export * from './api';

declare global {
    var createOcctKernelModule: WasmModuleFactory | undefined;
}

let browserFactoryPromise: Promise<WasmModuleFactory> | undefined;
let browserFactoryVariant: KernelVariant | undefined;

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

function getGlobalFactory(): WasmModuleFactory | undefined {
    return unwrapFactory((globalThis as { createOcctKernelModule?: unknown }).createOcctKernelModule);
}

async function loadBrowserFactory(variant: KernelVariant): Promise<WasmModuleFactory> {
    const existingFactory = getGlobalFactory();
    if (existingFactory && browserFactoryVariant === variant) {
        return existingFactory;
    }

    if (browserFactoryPromise && browserFactoryVariant === variant) {
        return browserFactoryPromise;
    }

    browserFactoryVariant = variant;
    browserFactoryPromise = (async () => {
        const scriptUrl = new URL(`./${kernelBasename(variant)}.js`, import.meta.url).href;

        if (typeof importScripts === 'function') {
            importScripts(scriptUrl);
            const workerFactory = getGlobalFactory();
            if (!workerFactory) {
                throw new KernelError('NOT_INITIALIZED', 'Loaded occt-kernel.js in a worker, but no factory became available');
            }
            return workerFactory;
        }

        if (typeof document === 'undefined') {
            throw new KernelError('NOT_INITIALIZED', 'No browser loader is available in this runtime; pass a pre-loaded wasmModule instead');
        }

        await new Promise<void>((resolve, reject) => {
            const existingScript = Array.from(document.getElementsByTagName('script')).find((script) => script.src === scriptUrl);

            const handleLoad = () => resolve();
            const handleError = () => reject(new KernelError('NOT_INITIALIZED', `Failed to load ${scriptUrl}`));

            if (existingScript) {
                if (getGlobalFactory()) {
                    resolve();
                    return;
                }

                existingScript.addEventListener('load', handleLoad, { once: true });
                existingScript.addEventListener('error', handleError, { once: true });
                return;
            }

            const script = document.createElement('script');
            script.src = scriptUrl;
            script.async = true;
            script.addEventListener('load', handleLoad, { once: true });
            script.addEventListener('error', handleError, { once: true });
            (document.head ?? document.body ?? document.documentElement).appendChild(script);
        });

        const loadedFactory = getGlobalFactory();
        if (!loadedFactory) {
            throw new KernelError('NOT_INITIALIZED', 'Loaded occt-kernel.js, but no WASM module factory was registered on globalThis');
        }

        return loadedFactory;
    })();

    try {
        return await browserFactoryPromise;
    } catch (error) {
        browserFactoryPromise = undefined;
        throw error;
    }
}

export async function createKernel(wasmModule?: WasmModule): Promise<OcctKernel> {
    const options = normalizeCreateKernelInput(wasmModule);
    if (options.wasmModule) {
        return new OcctKernel(options.wasmModule);
    }

    const mod = await (await loadBrowserFactory(normalizeKernelVariant(options.variant)))();
    return new OcctKernel(mod);
}