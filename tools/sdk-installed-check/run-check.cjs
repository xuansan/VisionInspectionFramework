const {spawnSync} = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const env = {};
for (const [k,v] of Object.entries(process.env)) env[k.toUpperCase()] = v;
const root = path.resolve(__dirname, '..', '..');
const build = path.join(root, 'out', 'sdk-installed-check');
const log = [];
function run(exe,args) {
  const r = spawnSync(exe,args,{env,encoding:'utf8',windowsHide:true,timeout:120000});
  log.push(r.stdout || '',r.stderr || '');
  console.log((r.stdout || '').split(/\r?\n/).slice(-12).join('\n'));
  if(r.stderr) console.error(r.stderr);
  if(r.error) throw r.error;
  if(r.status !== 0) throw new Error(`Command failed: ${r.status}`);
  return r.stdout.trim();
}
try {
  const vs=run(path.join(env['PROGRAMFILES(X86)'],'Microsoft Visual Studio','Installer','vswhere.exe'),
    ['-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
      '-property','installationPath']);
  const bin=path.join(vs,'Common7','IDE','CommonExtensions','Microsoft','CMake','CMake','bin');
  run(path.join(bin,'cmake.exe'),['-S',__dirname,'-B',build,'-G','Visual Studio 17 2022','-A','x64','-T','v143']);
  run(path.join(bin,'cmake.exe'),['--build',build,'--config','Release','--parallel','2']);
  run(path.join(bin,'ctest.exe'),['--test-dir',build,'-C','Release','--output-on-failure','-V']);
} finally {
  const dir=path.join(root,'开发环境','验证记录');
  fs.mkdirSync(dir,{recursive:true});
  fs.writeFileSync(path.join(dir,'installed-sdk-consumer.log'),log.join('\n'));
}
