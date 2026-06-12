import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));

const targets = {
    occt: {
        windows: 'build-occt-wasm.ps1',
        other: 'build-occt.sh',
    },
    sketch: {
        windows: 'build-sketch-toolkit.ps1',
        other: 'build-sketch-toolkit.sh',
    },
    wasm: {
        windows: 'build-wasm.ps1',
        other: 'build-wasm.sh',
    },
};

const [target, ...forwardedArgs] = process.argv.slice(2);
if (!target || !(target in targets)) {
    console.error("Usage: node scripts/run-platform-build.mjs <occt|wasm|sketch> [...args]");
    process.exit(1);
}

const selectedScript = process.platform === 'win32'
    ? targets[target].windows
    : targets[target].other;

const scriptPath = path.join(scriptDirectory, selectedScript);
const command = process.platform === 'win32' ? 'powershell.exe' : 'bash';
const commandArgs = process.platform === 'win32'
    ? ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', scriptPath, ...forwardedArgs]
    : [scriptPath, ...forwardedArgs];

const result = spawnSync(command, commandArgs, {
    stdio: 'inherit',
});

if (result.error) {
    console.error(result.error.message);
    process.exit(1);
}

process.exit(result.status ?? 1);