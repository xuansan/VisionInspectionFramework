// Disposable local-only service probe; no startup service or permanent credentials.
const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const crypto = require('node:crypto');
const { setTimeout: delay } = require('node:timers/promises');
const root = path.resolve(__dirname, '..', '..');
const envRoot = path.join(root, '开发环境');
const run = crypto.randomBytes(6).toString('hex');
const data = path.join(root, 'out', 'rustfs-check', run);
fs.mkdirSync(data, { recursive: true });
const logs = path.join(envRoot, '验证记录');
fs.mkdirSync(logs, { recursive: true });
const log = path.join(logs, `rustfs-${run}.log`);
const access = `probe${crypto.randomBytes(8).toString('hex')}`;
const secret = crypto.randomBytes(24).toString('hex');
const env = {};
for (const [key, value] of Object.entries(process.env)) env[key.toUpperCase()] = value;
env.RUSTFS_ACCESS_KEY = access;
env.RUSTFS_SECRET_KEY = secret;
env.RUSTFS_CONSOLE_ENABLE = 'false';
env.RUSTFS_REGION = 'us-east-1';
env.RUST_LOG = 'warn';

function request(port, method, suffix, body) {
  const curl = path.join(envRoot, 'SDK', 'native', 'bin', 'curl.exe');
  const config = `user = "${access}:${secret}"\n`;
  const args = ['--config', '-', '--silent', '--show-error', '--fail-with-body',
    '--connect-timeout', '2', '--max-time', '8', '--aws-sigv4', 'aws:amz:us-east-1:s3',
    '--request', method, `http://127.0.0.1:${port}/${suffix}`];
  if (body !== undefined) args.push('--data-binary', body);
  const result = spawnSync(curl, args, { env, input: config, encoding: 'utf8',
    windowsHide: true, timeout: 12000, maxBuffer: 1024 * 1024 });
  if (result.error) throw result.error;
  return result;
}

(async () => {
  const reservation = net.createServer();
  await new Promise((resolve, reject) => {
    reservation.once('error', reject);
    reservation.listen(0, '127.0.0.1', resolve);
  });
  const port = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  const child = spawn(path.join(envRoot, 'SDK', 'RustFS', 'rustfs.exe'),
    ['server', '--address', `127.0.0.1:${port}`, data],
    { env, cwd: data, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  let childError;
  child.on('error', error => { childError = error; });
  const append = chunk => fs.appendFileSync(log,
    chunk.toString().replaceAll(secret, '[REDACTED]').replaceAll(access, '[REDACTED]'));
  child.stdout.on('data', append);
  child.stderr.on('data', append);
  try {
    let ready = false;
    const deadline = Date.now() + 45000;
    while (Date.now() < deadline) {
      if (childError) throw childError;
      if (child.exitCode !== null) throw new Error(`RustFS exited: ${child.exitCode}; see ${log}`);
      const result = request(port, 'GET', '');
      if (result.status === 0 && result.stdout.includes('ListAllMyBucketsResult')) {
        ready = true;
        break;
      }
      await delay(300);
    }
    if (!ready) throw new Error(`RustFS readiness timed out; see ${log}`);
    const bucket = `sdk-check-${run}`;
    const body = JSON.stringify({ inspection_id: 'probe-only', quality: 'Unknown' });
    for (const [method, suffix, value] of [
      ['PUT', bucket], ['PUT', `${bucket}/smoke.json`, body],
      ['GET', `${bucket}/smoke.json`],
      ['DELETE', `${bucket}/smoke.json`], ['DELETE', bucket],
    ]) {
      const result = request(port, method, suffix, value);
      if (result.status !== 0) throw new Error(`S3 ${method} failed: ${result.stderr} ${result.stdout}`);
      if (method === 'GET' && result.stdout !== body) throw new Error('S3 object content mismatch');
      console.log(`PASS: S3 ${method} ${suffix}`);
    }
    fs.writeFileSync(path.join(logs, 'rustfs-result.json'), JSON.stringify({
      status: 'passed', version: '1.0.0', bind: '127.0.0.1', port,
      transport: 'project-built libcurl with AWS SigV4',
      checks: ['authenticated-list', 'create-bucket', 'put-object', 'get-object',
        'verify-content', 'delete-object', 'delete-bucket'],
      serviceAutoStart: false, temporaryData: data, log,
      recordedAt: new Date().toISOString(),
    }, null, 2));
  } finally {
    if (child.exitCode === null && !childError) {
      const ended = new Promise(resolve => child.once('exit', resolve));
      child.kill();
      await ended;
    }
    console.log('Test RustFS process stopped; no background service retained.');
  }
})().catch(error => { console.error(error.message); process.exitCode = 1; });
