import { KernelError } from './errors';
import { unwrapFactory, type WasmModuleFactory } from './default-module';
import { SketchToolkit, type SketchToolkitWasmModule } from './sketch-kernel';
import type { CreateSketchToolkitOptions } from './sketch-toolkit';

export * from './sketch-api';

declare global {
    var createSketchToolkitModule: WasmModuleFactory | undefined;
}

let browserFactoryPromise: Promise<WasmModuleFactory> | undefined;

function normalizeCreateSketchToolkitInput(input?: SketchToolkitWasmModule | CreateSketchToolkitOptions): CreateSketchToolkitOptions {
    if (!input) {
        return {};
    }
    if (typeof input === 'object' && 'wasmModule' in input) {
        return input as CreateSketchToolkitOptions;
    }
    return { wasmModule: input as SketchToolkitWasmModule };
}

function getGlobalFactory(): WasmModuleFactory | undefined {
    return unwrapFactory((globalThis as { createSketchToolkitModule?: unknown }).createSketchToolkitModule);
}

async function loadBrowserFactory(): Promise<WasmModuleFactory> {
    const existingFactory = getGlobalFactory();
    if (existingFactory) {
        return existingFactory;
    }

    if (browserFactoryPromise) {
        return browserFactoryPromise;
    }

    browserFactoryPromise = (async () => {
        const scriptUrl = new URL('./sketch-toolkit.wasm.js', import.meta.url).href;

        if (typeof importScripts === 'function') {
            importScripts(scriptUrl);
            const workerFactory = getGlobalFactory();
            if (!workerFactory) {
                throw new KernelError('NOT_INITIALIZED', 'Loaded sketch-toolkit.wasm.js in a worker, but no factory became available');
            }
            return workerFactory;
        }

        if (typeof document === 'undefined') {
            throw new KernelError('NOT_INITIALIZED', 'No browser loader is available in this runtime; pass a pre-loaded sketch toolkit wasmModule instead');
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
            throw new KernelError('NOT_INITIALIZED', 'Loaded sketch-toolkit.wasm.js, but no WASM module factory was registered on globalThis');
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

export async function createSketchToolkit(input?: SketchToolkitWasmModule | CreateSketchToolkitOptions): Promise<SketchToolkit> {
    const options = normalizeCreateSketchToolkitInput(input);
    if (options.wasmModule) {
        return new SketchToolkit(options.wasmModule);
    }

    const mod = await (await loadBrowserFactory())();
    return new SketchToolkit(mod as unknown as SketchToolkitWasmModule);
}
