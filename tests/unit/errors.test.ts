/**
 * Unit tests for KernelError and parseNativeError helper.
 */

import { KernelError, parseNativeError } from '../../src/errors';

describe('parseNativeError', () => {
    it('parses a valid JSON error string', () => {
        const raw = JSON.stringify({ code: 'INVALID_PARAMS', detail: 'dx must be > 0' });
        const err = parseNativeError(raw);
        expect(err).toBeInstanceOf(KernelError);
        expect(err.code).toBe('INVALID_PARAMS');
        expect(err.detail).toBe('dx must be > 0');
    });

    it('falls back to UNKNOWN when JSON is invalid', () => {
        const err = parseNativeError('not json at all');
        expect(err).toBeInstanceOf(KernelError);
        expect(err.code).toBe('UNKNOWN');
    });

    it('returns the same KernelError when passed a KernelError', () => {
        const original = new KernelError('EXPORT_FAILED', 'fail');
        const result = parseNativeError(original);
        expect(result).toBe(original);
    });

    it('wraps a plain Error', () => {
        const err = parseNativeError(new Error('native failure'));
        expect(err).toBeInstanceOf(KernelError);
        expect(err.code).toBe('UNKNOWN');
        expect(err.detail).toContain('native failure');
    });

    it('wraps an unknown thrown value', () => {
        const err = parseNativeError(42);
        expect(err).toBeInstanceOf(KernelError);
        expect(err.code).toBe('UNKNOWN');
    });
});
