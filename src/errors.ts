/**
 * Structured error types for occt-kernel-wasm.
 */

export type KernelErrorCode =
    | 'INVALID_HANDLE'
    | 'SESSION_MISMATCH'
    | 'INVALID_PARAMS'
    | 'INVALID_CHECKPOINT'
    | 'OPERATION_FAILED'
    | 'IMPORT_FAILED'
    | 'EXPORT_FAILED'
    | 'NOT_INITIALIZED'
    | 'UNKNOWN';

/** Structured error returned by all kernel operations. */
export class KernelError extends Error {
    public readonly code: KernelErrorCode;
    public readonly detail: string;

    constructor(code: KernelErrorCode, detail: string) {
        super(`[${code}] ${detail}`);
        this.name = 'KernelError';
        this.code = code;
        this.detail = detail;
    }
}

/** @internal Parse a JSON error descriptor returned by the C++ kernel. */
export function parseNativeError(raw: unknown): KernelError {
    if (typeof raw === 'string') {
        try {
            const parsed = JSON.parse(raw) as { code?: string; detail?: string };
            const code = (parsed.code ?? 'UNKNOWN') as KernelErrorCode;
            const detail = parsed.detail ?? raw;
            return new KernelError(code, detail);
        } catch {
            return new KernelError('UNKNOWN', raw);
        }
    }
    if (raw instanceof KernelError) return raw;
    if (raw instanceof Error) return new KernelError('UNKNOWN', raw.message);
    return new KernelError('UNKNOWN', String(raw));
}
