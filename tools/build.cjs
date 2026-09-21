// Convenience runner for this Windows workstation. CMake remains the build interface.
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const preset = process.argv[2] || 'windows-core';
if (preset === 'windows-all') {
  // Keep each preset's original logs and collect an additional whole-run index.
  // Continue independent suites after a failure, but fail the overall command.
  const root = path.resolve(__dirname, '..');
  const directory = path.join(root, 'out', 'validation',
    `full-${new Date().toISOString().replace(/[:.]/g, '-')}`);
  fs.mkdirSync(directory, { recursive: true });
  const cli = path.join(root, 'out', 'build', 'windows-core', 'apps', 'vision-cli', 'Release', 'vision-cli.exe');
  const results = [];
  const jobs = ['windows-core', 'windows-gui', 'windows-ipc'].map(name => ({
    name, exe: process.execPath, args: [__filename, name]
  }));
  for (const script of ['check_schemas.py', 'check_ipc_schema.py', 'check_capture_schema.py'])
    jobs.push({name: script, exe: process.env.VISION_PYTHON || 'python',
      args: [path.join(root, 'tools', script), '--cli', cli]});
  jobs.push({name:'storage-rustfs',exe:process.execPath,args:[path.join(root,'tools','test-storage-rustfs.cjs')]});
  for (const job of jobs) {
    console.log(`Full verification: ${job.name}`);
    const result = spawnSync(job.exe, job.args, {
      cwd: root, encoding: 'utf8', windowsHide: true, timeout: 600000, maxBuffer: 16 * 1024 * 1024
    });
    const output = (result.stdout || '') + (result.stderr || '') + (result.error ? String(result.error) : '');
    fs.writeFileSync(path.join(directory, `${job.name}.log`), output);
    const passed = !result.error && result.status === 0;
    results.push({name: job.name, passed, exitCode: result.status,
      evidence: output.match(/Verification logs: (.+)/)?.[1] || null});
    console.log(`${job.name}: ${passed ? 'PASS' : 'FAIL'}; log: ${directory}`);
    if (!passed) console.error(output.slice(-8000));
  }
  fs.writeFileSync(path.join(directory, 'summary.json'), JSON.stringify(results, null, 2));
  console.log(`Full verification summary: ${directory}`);
  process.exit(results.every(result => result.passed) ? 0 : 1);
}
if (!['windows-core', 'windows-gui', 'windows-ipc'].includes(preset)) throw new Error('Use windows-core, windows-gui or windows-ipc');
const root = path.resolve(__dirname, '..');
const env = {};
for (const [key, value] of Object.entries(process.env)) env[key.toUpperCase()] = value;
// Avoid inherited Python/Qt5 build and plugin settings. This changes child processes only.
for (const key of ['CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'QT_PLUGIN_PATH',
  'QT_QPA_PLATFORM_PLUGIN_PATH', 'CMAKE_PREFIX_PATH', 'QTDIR']) delete env[key];
const logs = path.join(root, 'out', 'validation', new Date().toISOString().replace(/[:.]/g, '-'));
fs.mkdirSync(logs, { recursive: true });
function run(exe, args, name) {
  const result = spawnSync(exe, args, {
    cwd: root, env, encoding: 'utf8', windowsHide: true, timeout: 180000,
    maxBuffer: 8 * 1024 * 1024
  });
  const output = (result.stdout || '') + (result.stderr || '');
  fs.writeFileSync(path.join(logs, `${name}.log`), output);
  console.log(output);
  if(result.error) throw result.error;
  if(result.status !== 0) throw new Error(`${name} failed: ${result.status}. Logs: ${logs}`);
  return result.stdout.trim();
}
const vs = run(path.join(env['PROGRAMFILES(X86)'], 'Microsoft Visual Studio', 'Installer', 'vswhere.exe'),
  ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '-property', 'installationPath'], 'vswhere');
const bin = path.join(vs, 'Common7', 'IDE', 'CommonExtensions', 'Microsoft', 'CMake', 'CMake', 'bin');
run(path.join(bin, 'cmake.exe'), ['--preset', preset], 'configure');
run(path.join(bin, 'cmake.exe'), ['--build', '--preset', preset, '--parallel', '4'], 'build');
run(path.join(bin, 'ctest.exe'), ['--preset', preset], 'test');
console.log(`Verification logs: ${logs}`);
