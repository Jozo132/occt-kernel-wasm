import { build } from 'esbuild';
import { execFile } from 'node:child_process';
import { readdir, rm } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
const repoRoot = fileURLToPath(new URL('..', import.meta.url));
const distDir = path.join(repoRoot, 'dist');

async function cleanDistArtifacts() {
    const entries = await readdir(distDir, { withFileTypes: true });

    await Promise.all(
        entries.map(async (entry) => {
            if (!entry.isFile()) {
                return;
            }

            const { name } = entry;
            if (name === 'occt-kernel.js' || name === 'occt-kernel.wasm') {
                return;
            }

            if (
                name.endsWith('.js')
                || name.endsWith('.mjs')
                || name.endsWith('.map')
                || name.endsWith('.d.ts')
            ) {
                await rm(path.join(distDir, name), { force: true });
            }
        }),
    );
}

async function buildDeclarations() {
    const tscPath = path.join(repoRoot, 'node_modules', 'typescript', 'lib', 'tsc.js');
    await execFileAsync(process.execPath, [tscPath, '--project', 'tsconfig.declarations.json'], {
        cwd: repoRoot,
    });
}

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