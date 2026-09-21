// Local diagnostic only; does not modify user or machine environment variables.
const { spawnSync } = require('node:child_process');
const path = require('node:path');

const env = {};
for (const [key, value] of Object.entries(process.env)) {
  env[key.toUpperCase()] = value;
}

function run(command, args) {
  const result = spawnSync(command, args, {
    env, encoding: 'utf8', windowsHide: true, timeout: 120000,
  });
  process.stdout.write(result.stdout || '');
  process.stderr.write(result.stderr || '');
  if (result.error) throw result.error;
  if (result.status !== 0) {
    throw new Error(`Command failed with exit code ${result.status}: ${command}`);
  }
  return result.stdout.trim();
}

const vswhere = path.join(env['PROGRAMFILES(X86)'], 'Microsoft Visual Studio',
  'Installer', 'vswhere.exe');
const vs = run(vswhere, ['-latest', '-products', '*', '-requires',
  'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath']);
if (!vs) throw new Error('No Visual Studio C++ installation found.');
const bin = path.join(vs, 'Common7', 'IDE', 'CommonExtensions', 'Microsoft',
  'CMake', 'CMake', 'bin');
const root = path.resolve(__dirname, '..', '..');
const build = path.join(root, 'out', 'environment-check-normalized');

run(path.join(bin, 'cmake.exe'), ['-S', __dirname, '-B', build,
  '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'v143']);
run(path.join(bin, 'cmake.exe'), ['--build', build, '--config', 'Release', '--parallel', '2']);
run(path.join(bin, 'ctest.exe'), ['--test-dir', build, '-C', 'Release', '--output-on-failure']);
