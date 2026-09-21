const { spawnSync } = require('node:child_process');
const path = require('node:path');
const fs = require('node:fs');
const env = {};
for (const [key, value] of Object.entries(process.env)) env[key.toUpperCase()] = value;
const root = path.resolve(__dirname, '..', '..');
const sdk = path.join(root, '开发环境', 'SDK');
const build = path.join(root, 'out', 'sdk-check');
const logs = path.join(root, '开发环境', '验证记录',
  new Date().toISOString().replace(/[:.]/g, '-'));
fs.mkdirSync(logs, { recursive: true });
function run(command, args, name, timeout = 240000) {
  const result = spawnSync(command, args, { env, encoding: 'utf8', windowsHide: true, timeout,
    maxBuffer: 16 * 1024 * 1024 });
  const output = (result.stdout || '') + (result.stderr || '');
  fs.writeFileSync(path.join(logs, `${name}.log`), output);
  console.log(output.split(/\r?\n/).slice(-24).join('\n'));
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${name} failed (${result.status}); see 验证记录/${name}.log`);
  return (result.stdout || '').trim();
}
const vs = run(path.join(env['PROGRAMFILES(X86)'], 'Microsoft Visual Studio', 'Installer',
  'vswhere.exe'), ['-latest', '-products', '*', '-requires',
  'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], 'vswhere');
const bin = path.join(vs, 'Common7', 'IDE', 'CommonExtensions', 'Microsoft', 'CMake', 'CMake', 'bin');
env.PATH = [
  path.join(sdk, 'Qt', '6.8.3', 'msvc2022_64', 'bin'),
  path.join(sdk, 'OpenCV', 'opencv', 'build', 'x64', 'vc16', 'bin'),
  path.join(sdk, 'ONNXRuntime', 'onnxruntime-win-x64-1.30.0', 'lib'),
  env.PATH,
].join(path.delimiter);
env.QT_PLUGIN_PATH = path.join(sdk, 'Qt', '6.8.3', 'msvc2022_64', 'plugins');
env.VISION_TEST_MODEL = path.join(root, '开发环境', '测试数据', 'mul_1.onnx');
// Avoid old Anaconda flags affecting the explicitly chosen SDKs.
delete env.CFLAGS; delete env.CXXFLAGS; delete env.LDFLAGS;
run(path.join(bin, 'cmake.exe'), ['--fresh', '-S', __dirname, '-B', build,
  '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'v143',
  `-DCMAKE_INSTALL_PREFIX=${path.join(sdk, 'native')}`], 'sdk-configure');
run(path.join(bin, 'cmake.exe'), ['--build', build, '--config', 'Release', '--parallel', '4'],
  'sdk-build', 600000);
run(path.join(bin, 'ctest.exe'), ['--test-dir', build, '-C', 'Release',
  '--output-on-failure', '-V'], 'sdk-test');
run(path.join(bin, 'cmake.exe'), ['--install', build, '--config', 'Release'], 'sdk-install');
