// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Phase one runs only a fixed native diagnostic after sandbox engagement.
// No webpage Python execution or DOM bridge is expected yet.
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

const option = (name, fallback) => process.argv.slice(2)
    .find(arg => arg.startsWith(`--${name}=`))?.slice(name.length + 3) ?? fallback;
const binary = path.resolve(option('chrome-binary', 'out/Debug/chrome.exe'));
const output = path.resolve(option('output', 'out/Debug/python-runtime-smoke.json'));
fs.mkdirSync(path.dirname(output), {recursive: true});
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'python-runtime-smoke-'));
const poison = path.join(root, 'poison');
const marker = path.join(root, 'sitecustomize-ran');
fs.mkdirSync(poison);
fs.writeFileSync(path.join(poison, 'sitecustomize.py'),
    `open(${JSON.stringify(marker.replaceAll('\\', '/'))}, 'w').write('unexpected')\n` +
    `raise RuntimeError('POISONED_PYTHON_ENVIRONMENT')\n`);
const html = `<!doctype html><meta charset="utf-8"><title>Python phase one</title>
<p id="result">网页正常</p>
<pyscript>print("PAGE_PYTHON_SHOULD_BE_INERT")</pyscript>
<script>
document.body.dataset.chromiumPlus = globalThis.chromiumPlus;
document.body.dataset.pythonElement = document.querySelector('pyscript').constructor.name;
</script>`;
const server = http.createServer((request, response) => {
  response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8'});
  response.end(html);
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const url = `http://127.0.0.1:${server.address().port}/`;
const results = {binary, sandboxDisabled: false, cases: []};

async function run(enabled) {
  const name = enabled ? 'enabled' : 'default-off';
  const args = ['--headless', '--disable-gpu', '--no-first-run',
    '--no-default-browser-check', '--disable-background-networking',
    '--no-proxy-server', '--enable-logging=stderr', '--dump-dom',
    `--user-data-dir=${path.join(root, name)}`];
  if (enabled) args.push('--enable-features=NativePythonRuntimeSelfTest');
  args.push(url);
  const child = spawn(binary, args, {windowsHide: true, env: {...process.env,
    PYTHONHOME: path.join(poison, 'nonexistent-home'), PYTHONPATH: poison,
    PYTHONUTF8: '0', PYTHONIOENCODING: 'ascii', PYTHONWARNINGS: 'error'}});
  let stdout = '', stderr = '';
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', data => { stdout += data; });
  child.stderr.on('data', data => { stderr += data; });
  let timedOut = false;
  const timer = setTimeout(() => {
    timedOut = true;
    if (process.platform === 'win32') {
      spawnSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], {windowsHide: true});
    } else {
      child.kill('SIGKILL');
    }
  }, 60000);
  let exitCode;
  try {
    exitCode = await new Promise((resolve, reject) => {
      child.once('error', reject);
      child.once('close', resolve);
    });
  } finally {
    clearTimeout(timer);
    fs.writeFileSync(`${output}.${name}.chrome.log`, stderr);
    fs.writeFileSync(`${output}.${name}.html`, stdout);
  }
  assert.equal(timedOut, false, `${name} browser timed out`);
  assert.equal(exitCode, 0, stderr.slice(-4000));
  assert.match(stdout, /data-chromium-plus="0\.0\.1"/);
  assert.match(stdout, /data-python-element="HTMLUnknownElement"/);
  assert.ok(stdout.includes('网页正常'));
  assert.equal(fs.existsSync(marker), false, 'sitecustomize unexpectedly executed');
  const reports = [...stderr.matchAll(/CPYTHON_RUNTIME_SELF_TEST (\{[^\r\n]+\})/g)]
      .map(match => JSON.parse(match[1]));
  if (!enabled) {
    assert.equal(reports.length, 0, 'default-off runtime ran diagnostic');
  } else {
    assert.ok(reports.length > 0, `No sandboxed renderer report: ${stderr.slice(-5000)}`);
    for (const report of reports) {
      assert.equal(report.passed, true, JSON.stringify(report));
      assert.equal(report.sandboxed, true);
      assert.equal(report.version, '3.13.18');
      assert.equal(report.stdout, 'Python 世界 42\n');
      assert.equal(report.isolated_contexts, true);
      assert.equal(report.context_cleanup, true);
      assert.equal(report.output_truncated, true);
      assert.match(report.traceback, /ValueError: phase-one/);
    }
  }
  results.cases.push({name, passed: true, pageLoaded: true,
    chromiumPlus: '0.0.1', pythonElement: 'HTMLUnknownElement',
    poisonedEnvironmentIgnored: true, reports});
}

try {
  await run(false);
  await run(true);
  results.passed = true;
} catch (error) {
  results.passed = false;
  results.error = String(error.stack ?? error);
  process.exitCode = 1;
} finally {
  await new Promise(resolve => server.close(resolve));
  fs.writeFileSync(output, JSON.stringify(results, null, 2) + '\n');
  // The only recursively removed directory is the test's mkdtemp result.
  fs.rmSync(root, {recursive: true, force: true, maxRetries: 8, retryDelay: 250});
}
console.log(JSON.stringify(results, null, 2));
