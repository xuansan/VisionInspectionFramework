const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const cp = require('node:child_process');
const crypto = require('node:crypto');
const root = path.resolve(__dirname, '..');
const kind = process.argv[2];
if (!['mysql', 'postgresql'].includes(kind)) throw new Error('Expected mysql or postgresql');
const dir = fs.mkdtempSync(path.join(require('node:os').tmpdir(), `vision-database-${kind}-`));
fs.mkdirSync(dir, { recursive: true });
const password = crypto.randomBytes(18).toString('hex');
const sdk = kind === 'mysql' ? 'MySQL/mysql-8.4.4-winx64' : 'PostgreSQL/pgsql';
const portable = path.join(require('node:os').tmpdir(), `vision-test-sdk-${kind}`);
if (!fs.existsSync(path.join(portable, '.database-sdk-v2'))) {
  for (const part of ['bin', 'share', 'lib']) {
    const copied = cp.spawnSync('robocopy.exe', [path.join(root, '开发环境', 'SDK', sdk, part), path.join(portable, part),
      '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/R:0', '/W:0'], { windowsHide: true, encoding: 'utf8' });
    if (copied.error || copied.status === null || copied.status > 7) throw new Error('Unable to prepare ASCII test SDK directory');
  }
  fs.writeFileSync(path.join(portable, '.database-sdk-v2'), sdk);
}
const bin = path.join(portable, 'bin');
const env = { ...process.env, VISION_DB_USER: 'vision_writer', VISION_DB_ADMIN: kind === 'mysql' ? 'root' : 'postgres',
  VISION_DB_READONLY: 'vision_reader', VISION_DB_PASSWORD: password, PGPASSWORD: password };
function run(exe, args, input, extra = {}) {
  const r = cp.spawnSync(exe, args, { cwd: root, env: { ...env, ...extra }, input, encoding: 'utf8', timeout: 60000, windowsHide: true });
  if (r.status !== 0) throw new Error(`${path.basename(exe)} failed: ${(r.stderr || r.stdout || String(r.error)).replaceAll(password, '[redacted]')}`);
  return r.stdout;
}
function freePort() {
  return new Promise((resolve, reject) => { const s = net.createServer(); s.on('error', reject); s.listen(0, '127.0.0.1', () => {
    const p = s.address().port; s.close(() => resolve(p)); }); });
}
function delay(ms) { return new Promise(resolve => setTimeout(resolve, ms)); }
let server;
async function main() {
  const port = await freePort();
  const data = path.join(dir, 'data');
  const log = fs.openSync(path.join(dir, 'server.log'), 'a');
  const mysqlDefaults = path.join(dir, 'client.ini');
  const openssl = process.env.VISION_TEST_OPENSSL || 'C:/Program Files/Git/usr/bin/openssl.exe';
  const cert = path.join(dir, 'server.crt'), key = path.join(dir, 'server.key'), wrong = path.join(dir, 'wrong.crt');
  for (const [name, subject, extensions] of [['server', '/CN=vision-test', 'subjectAltName=IP:127.0.0.1'], ['wrong', '/CN=wrong-ca', 'basicConstraints=critical,CA:TRUE']])
    run(openssl, ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', subject, '-addext', extensions,
      '-keyout', path.join(dir, `${name}.key`), '-out', path.join(dir, `${name}.crt`)]);
  let sql;
  if (kind === 'mysql') {
    run(path.join(bin, 'mysqld.exe'), ['--no-defaults', '--initialize-insecure', `--basedir=${path.dirname(bin)}`, `--datadir=${data}`]);
    server = cp.spawn(path.join(bin, 'mysqld.exe'), ['--no-defaults', `--basedir=${path.dirname(bin)}`, `--datadir=${data}`,
      '--bind-address=127.0.0.1', `--port=${port}`, '--mysqlx=OFF', `--ssl-cert=${cert}`, `--ssl-key=${key}`, `--ssl-ca=${cert}`],
      { cwd: portable, windowsHide: true, stdio: ['ignore', log, log] });
    fs.writeFileSync(mysqlDefaults, `[client]\nhost=127.0.0.1\nport=${port}\nuser=root\nprotocol=tcp\n`);
    sql = text => run(path.join(bin, 'mysql.exe'), [`--defaults-file=${mysqlDefaults}`, '--batch', '--default-character-set=utf8mb4'], text);
  } else {
    const pwfile = path.join(dir, 'bootstrap-password');
    fs.writeFileSync(pwfile, password);
    run(path.join(bin, 'initdb.exe'), ['-D', data, '-U', 'postgres', '--encoding=UTF8', '--locale=C', '--auth=scram-sha-256', `--pwfile=${pwfile}`]);
    fs.unlinkSync(pwfile);
    server = cp.spawn(path.join(bin, 'postgres.exe'), ['-D', data, '-h', '127.0.0.1', '-p', String(port),
      '-c', 'ssl=on', '-c', `ssl_cert_file=${cert}`, '-c', `ssl_key_file=${key}`], { cwd: portable, windowsHide: true, stdio: ['ignore', log, log] });
    sql = text => run(path.join(bin, 'psql.exe'), ['-h', '127.0.0.1', '-p', String(port), '-U', 'postgres', '-d', 'postgres', '-v', 'ON_ERROR_STOP=1'], text);
  }
  try {
    let ready = false;
    for (let n = 0; n < 60; n++) { try { sql('SELECT 1;'); ready = true; break; } catch { await delay(250); } }
    if (!ready) throw new Error('Database startup failed');
    if (kind === 'mysql') {
      sql(`ALTER USER 'root'@'localhost' IDENTIFIED BY '${password}'; CREATE USER 'vision_writer'@'127.0.0.1' IDENTIFIED BY '${password}'; CREATE USER 'vision_reader'@'127.0.0.1' IDENTIFIED BY '${password}';`);
      fs.appendFileSync(mysqlDefaults, `password=${password}\n`);
    } else sql(`CREATE ROLE vision_writer LOGIN PASSWORD '${password}'; CREATE ROLE vision_reader LOGIN PASSWORD '${password}';`);
    const scenarios = ['success', 'repeat', 'conflict', 'identity-conflict', 'wrong-version', 'missing-table', 'permission', 'rollback',
      'lock-timeout', 'images', 'concurrent', 'process', 'durable', 'recovery', 'tls-valid', 'tls-ca', 'tls-host'];
    const migration = fs.readFileSync(path.join(root, 'plugins/database', `${kind}-output/migrations/001-initial.sql`), 'utf8');
    const config = { host: '127.0.0.1', port, database: 'vision_test', username_env: 'VISION_DB_USER', password_env: 'VISION_DB_PASSWORD',
      timeout_ms: 1500, tls: 'disable-loopback', ca_file: '', archive_root: '' };
    for (const scenario of scenarios) {
      sql('DROP DATABASE IF EXISTS vision_test; CREATE DATABASE vision_test;');
      if (kind === 'mysql') sql(`USE vision_test;\n${migration}\nGRANT SELECT ON vision_test.vision_schema_version TO 'vision_writer'@'127.0.0.1';\n${
        ['vision_results', 'vision_defects', 'vision_images'].map(table =>
          `GRANT SELECT,INSERT,UPDATE ON vision_test.${table} TO 'vision_writer'@'127.0.0.1';`).join('\n')
      }\nGRANT SELECT ON vision_test.* TO 'vision_reader'@'127.0.0.1';`);
      else run(path.join(bin, 'psql.exe'), ['-h', '127.0.0.1', '-p', String(port), '-U', 'postgres', '-d', 'vision_test', '-v', 'ON_ERROR_STOP=1'],
        `${migration}\nGRANT USAGE ON SCHEMA public TO vision_writer,vision_reader; GRANT SELECT ON vision_schema_version TO vision_writer; GRANT SELECT,INSERT,UPDATE ON vision_results,vision_defects,vision_images TO vision_writer; GRANT SELECT ON ALL TABLES IN SCHEMA public TO vision_reader;`);
      const output = run(path.join(root, 'out/build/windows-ipc/tests/Release', `vision-${kind}-database-tests.exe`),
        [path.join(root, 'out/build/windows-ipc/plugins/database', `${kind}-output/Release/${kind}-output.json`), scenario], undefined,
        { VISION_DB_TEST_CONFIG: JSON.stringify(scenario.startsWith('tls-') ? { ...config, tls: 'verify-full', ca_file: cert } : config),
          VISION_DB_WRONG_CA: wrong });
      console.log(`${kind}: ${output.trim() || scenario}`);
    }
    const evidence = path.join(root, 'out', 'validation', `database-${kind}-${Date.now()}`);
    fs.mkdirSync(evidence, { recursive: true });
    fs.writeFileSync(path.join(evidence, 'summary.json'), JSON.stringify({ kind, scenarios, passed: scenarios.length, time: new Date().toISOString() }, null, 2));
  } finally {
    if (server && server.exitCode === null) {
      try {
        if (kind === 'postgresql') run(path.join(bin, 'pg_ctl.exe'), ['-D', data, '-m', 'immediate', '-w', 'stop']);
        else run(path.join(bin, 'mysqladmin.exe'), [`--defaults-file=${mysqlDefaults}`, 'shutdown']);
      } catch { server.kill(); }
      await new Promise(resolve => { if (server.exitCode !== null) resolve(); else { server.once('exit', resolve); server.kill(); } });
    }
    fs.closeSync(log);
    if (fs.existsSync(mysqlDefaults)) fs.unlinkSync(mysqlDefaults);
  }
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
