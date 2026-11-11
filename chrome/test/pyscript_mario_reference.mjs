// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Read the original project without modifying it. Regenerate the checked-in
// oracle only when intentionally reviewing the original implementation.
import fs from 'node:fs';
import vm from 'node:vm';
import crypto from 'node:crypto';
import assert from 'node:assert/strict';

const option = (name, fallback) => process.argv.find(x => x.startsWith(`--${name}=`))?.slice(name.length+3) ?? fallback;
const original = option('reference', 'C:/VD/Projects/mario/index.html');
const destination = option('output', 'chrome/test/data/pyscript/mario/reference.json');
const bytes = fs.readFileSync(original);
const sha256 = crypto.createHash('sha256').update(bytes).digest('hex');
assert.equal(sha256, '53b556316bbc25a5c2c80b046be836dbaaea8aa5c25e85a2cf5f29a4b4bc9ef1');
const html = bytes.toString('utf8');
function section(begin, end) {
  const start = html.indexOf(begin);
  const finish = html.indexOf(end, start+begin.length);
  assert.ok(start >= 0 && finish > start, begin);
  return html.slice(start, finish);
}
const source = [
  section('let WIDTH = 1120;', '// 小屏幕显示'),
  section('const groundSegments =', '// ── 3.'),
  section('function clearInput()', 'document.addEventListener("keydown"'),
  section('function togglePause()', 'window.addEventListener("blur"'),
  section('function overlaps(a, b)', '// ── 6.'),
].join('\n');
const scenarios = [
  {name: 'acceleration-brake', steps: 160, actions: [[0,'right',true],[90,'right',false]], checkpoints: [1,60,90,160]},
  {name: 'long-jump', steps: 140, actions: [[0,'jump',true],[100,'jump',false]], checkpoints: [30,60,100,140]},
  {name: 'short-jump', steps: 140, actions: [[0,'jump',true],[9,'jump',false]], checkpoints: [30,60,140]},
  {name: 'question-once', player: {x: 490, y: 425}, steps: 160, actions: [[0,'jump',true],[70,'jump',false],[100,'jump',true]], checkpoints: [20,80,160]},
  {name: 'collect-coin', player: {x: 642, y: 412}, steps: 2, actions: [], checkpoints: [1,2]},
  {name: 'stomp', player: {x: 755, y: 428, vy: 240, onGround: false}, steps: 12, actions: [], checkpoints: [4,12]},
  {name: 'side-hit-respawn', player: {x: 746, y: 460}, steps: 160, actions: [], checkpoints: [1,100,160]},
  {name: 'pit-respawn', player: {x: 1690, y: 660, onGround: false}, steps: 170, actions: [], checkpoints: [12,160,170]},
  {name: 'checkpoint-respawn', player: {x: 3361, y: 460}, steps: 170,
    actions: [[2,'death',true]], checkpoints: [1,3,170]},
  {name: 'last-life', player: {x: 1690, y: 690}, game: {lives: 1}, steps: 150, actions: [], checkpoints: [1,150]},
  {name: 'timeout-reset', game: {remaining: 0.001}, steps: 140, actions: [], checkpoints: [1,140]},
  {name: 'finish', player: {x: 6550, y: 200}, steps: 180, actions: [], checkpoints: [1,90,180]},
  {name: 'coyote', player: {x: 1634, y: 460, onGround: false, coyote: 0.09}, steps: 50,
    actions: [[0,'jump',true]], checkpoints: [1,30,50]},
  {name: 'jump-buffer', player: {x: 100, y: 455, vy: 120, onGround: false}, steps: 30,
    actions: [[0,'jump',true]], checkpoints: [1,10,30]},
];
const route = JSON.parse(fs.readFileSync('chrome/test/data/pyscript/mario/route.json'));
route.checkpoints = [1, 600, 1200, 1800, 2400, route.steps];
scenarios.push(route);
const capture = `JSON.parse(JSON.stringify({game, player,
  usedBlocks: blocks.map((b,i) => b.used ? i : -1).filter(i => i >= 0),
  collectedCoins: coins.map((c,i) => c.collected ? i : -1).filter(i => i >= 0),
  enemies, particles: particles.slice(0,2), particleCount: particles.length,
  floatTexts, rng: randomState}))`;
function run(scenario) {
  const context = vm.createContext({});
  vm.runInContext(`
    let randomState = ${scenario.seed ?? 0x4D415249};
    const Math = Object.create(globalThis.Math);
    Math.random = () => {let x=randomState; x^=x<<13; x^=x>>>17; x^=x<<5;
      randomState=x>>>0; return randomState/4294967296;};
    const localStorage = {setItem() {}};
    const window = {matchMedia: () => ({matches: false})};
    function updateOverlay() {} function logEvent() {} function showToast() {}
    function playSound() {}
    ${source}
    startGame();
    Object.assign(player, ${JSON.stringify(scenario.player ?? {})});
    Object.assign(game, ${JSON.stringify(scenario.game ?? {})});
  `, context);
  const expected = [];
  for (let step = 0; step < scenario.steps; step++) {
    for (const [at, key, down] of scenario.actions) if (at === step) {
      vm.runInContext(key === 'death' ? 'loseLife("fixture")' :
          down && key === 'jump' ? 'pressJump()' : `input[${JSON.stringify(key)}]=${down}`, context);
    }
    vm.runInContext('update(STEP)', context);
    if (scenario.checkpoints.includes(step+1)) expected.push({step: step+1, state: vm.runInContext(capture, context)});
  }
  return {...scenario, expected};
}
const result = {original, originalSha256: sha256, step: 1/120,
  random: 'xorshift32 shifts 13,17,5; unsigned32 / 2^32; seed 0x4D415249',
  scenarios: scenarios.map(run)};
fs.writeFileSync(destination, JSON.stringify(result, null, 2)+'\n');
console.log(`Saved ${result.scenarios.length} original-JavaScript scenarios to ${destination}`);
