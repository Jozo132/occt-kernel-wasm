import { build } from 'esbuild';
import { execFile } from 'node:child_process';
import { readdir, rm } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
const repoRoot = fileURLToPath(new URL('..', import.meta.url));
const distDir = path.join(repoRoot, 'dist');
const startedAt = process.hrtime.bigint();

function formatElapsed(secondsTotal) {
    const hours = Math.floor(secondsTotal / 3600);
    const minutes = Math.floor((secondsTotal % 3600) / 60);
    const seconds = secondsTotal % 60;

    if (hours > 0) {
        return `${hours}h ${minutes}m ${seconds}s`;
    }
    if (minutes > 0) {
        return `${minutes}m ${seconds}s`;
    }
    return `${seconds}s`;
}

function printElapsedTime() {
    const elapsedSeconds = Number((process.hrtime.bigint() - startedAt) / 1000000000n);
    console.log(`[build] Finished in ${formatElapsed(elapsedSeconds)}`);
}

async function cleanDistArtifacts() {
    const entries = await readdir(distDir, { withFileTypes: true });
    const keepFiles = new Set(['occt-kernel.js', 'occt-kernel.wasm']);

    await Promise.all(
        entries.map(async (entry) => {
            if (!entry.isFile()) {
                return;
            }

            const { name } = entry;
            if (keepFiles.has(name)) {
                return;
            }

            await rm(path.join(distDir, name), { force: true });
        }),
    );
}

async function buildDeclarations() {
    const tscPath = path.join(repoRoot, 'node_modules', 'typescript', 'lib', 'tsc.js');
    await execFileAsync(process.execPath, [tscPath, '--project', 'tsconfig.declarations.json'], {
        cwd: repoRoot,
    });
}

try {
    await cleanDistArtifacts();
    await buildDeclarations();

    await Promise.all([
        build({
            entryPoints: [path.join(repoRoot, 'src', 'index.node.ts')],
            bundle: true,
            format: 'cjs',
            outfile: path.join(distDir, 'index.js'),
            platform: 'node',
            sourcemap: true,
            target: ['node18'],
        }),
        build({
            entryPoints: [path.join(repoRoot, 'src', 'index.browser.ts')],
            bundle: true,
            format: 'esm',
            outfile: path.join(distDir, 'index.mjs'),
            platform: 'browser',
            sourcemap: true,
            target: ['es2020'],
        }),
    ]);
} finally {
    printElapsedTime();
}