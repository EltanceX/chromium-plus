# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Deterministic physics comparison with the original JavaScript oracle."""
import json
import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent / 'data/pyscript/mario'
REFERENCE = json.loads((ROOT / 'reference.json').read_text(encoding='utf-8'))


def core():
  scope = {}
  for name in ('state.py', 'physics.py'):
    exec(compile((ROOT / name).read_text(encoding='utf-8'), str(ROOT / name), 'exec'), scope)
  return scope


def capture(scope):
  s = scope['snapshot']()
  return dict(game=s['game'], player=s['player'],
              usedBlocks=[i for i, b in enumerate(s['blocks']) if b['used']],
              collectedCoins=[i for i, c in enumerate(s['coins']) if c['collected']],
              enemies=s['enemies'], particles=s['particles'][:2],
              particleCount=len(s['particles']), floatTexts=s['floatTexts'], rng=s['rng'])


def compare(expected, actual, location=''):
  if isinstance(expected, dict):
    assert expected.keys() == actual.keys(), location
    for key in expected:
      compare(expected[key], actual[key], location+'.'+key)
  elif isinstance(expected, list):
    assert len(expected) == len(actual), location
    for i, (a, b) in enumerate(zip(expected, actual)):
      compare(a, b, location+'['+str(i)+']')
  elif type(expected) in (int, float) and type(actual) in (int, float):
    assert abs(expected-actual) < 1e-7, (location, expected, actual)
  else:
    assert expected == actual, (location, expected, actual)


def run_scenario(scenario):
  scope = core()
  scope['startGame'](scenario.get('seed', 0x4D415249))
  vars(scope['player']).update(scenario.get('player', {}))
  vars(scope['game']).update(scenario.get('game', {}))
  checkpoints = {c['step']: c['state'] for c in scenario['expected']}
  for step in range(scenario['steps']):
    for at, key, down in scenario['actions']:
      if at == step:
        if key == 'death':
          scope['loseLife']('fixture')
        elif key == 'jump' and down:
          scope['pressJump']()
        else:
          setattr(scope['input'], key, down)
    scope['update'](scope['STEP'])
    if step+1 in checkpoints:
      compare(checkpoints[step+1], capture(scope), scenario['name']+':'+str(step+1))


class MarioCoreTest(unittest.TestCase):
  def test_fixed_clock_clamps_and_limits_steps(self):
    s = core()
    s['startGame']()
    clock = s['FixedClock']()
    self.assertEqual(clock.advance(100), (0, 0))
    self.assertEqual(clock.advance(10100), (10, 6))
    self.assertAlmostEqual(s['game'].elapsed, 0.05)
    self.assertAlmostEqual(clock.dropped, 9.95)
    clock.reset()
    self.assertEqual(clock.advance(999999), (0, 0))

  def test_pause_and_rate_independence(self):
    states = []
    for hz in (30, 60, 120):
      s = core()
      s['startGame']()
      s['input'].right = True
      clock = s['FixedClock']()
      for n in range(hz+1):
        clock.advance(n*1000/hz)
      states.append(capture(s))
    compare(states[0], states[1])
    compare(states[0], states[2])
    s['togglePause']()
    before = capture(s)
    clock.advance(10000)
    compare(before, capture(s))

  def test_physics_work_quota_drops_backlog(self):
    s = core()
    s['startGame']()
    counter = iter((0, 0.001, 0.002))
    clock = s['FixedClock'](work_quota_ms=2, work_clock=lambda: next(counter))
    clock.advance(0)
    self.assertEqual(clock.advance(50), (0.05, 2))
    self.assertAlmostEqual(s['game'].elapsed, 2/120)
    self.assertAlmostEqual(clock.dropped, 4/120)

  def test_random_vector_and_visual_pool(self):
    s = core()
    r = s['ReplayRandom'](1)
    self.assertEqual([int(r.random()*2**32) for _ in range(3)], [270369, 67634689, 2647435461])
    self.assertEqual(s['ReplayRandom'](0).state, s['DEFAULT_SEED'])
    s['burst'](0, 0, '#fff', 1000)
    self.assertEqual(len(s['particles']), 128)
    for _ in range(100):
      s['addText'](0, 0, '+1', '#fff')
    self.assertEqual(len(s['floatTexts']), 32)
    s['updateEffects'](2)
    self.assertEqual(s['particles'], [])
    self.assertEqual(s['floatTexts'], [])


def add_scenario(scenario):
  def test(self):
    run_scenario(scenario)
  setattr(MarioCoreTest, 'test_reference_'+scenario['name'].replace('-', '_'), test)


for scenario in REFERENCE['scenarios']:
  add_scenario(scenario)

if __name__ == '__main__':
  output = next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--output=')), None)
  sys.argv = [a for a in sys.argv if not a.startswith('--output=')]
  cases = []

  class Result(unittest.TextTestResult):
    def addSuccess(self, test):
      super().addSuccess(test)
      cases.append(dict(name=test.id(), passed=True))

    def addFailure(self, test, error):
      super().addFailure(test, error)
      cases.append(dict(name=test.id(), passed=False))

    def addError(self, test, error):
      super().addError(test, error)
      cases.append(dict(name=test.id(), passed=False))

  run = unittest.main(exit=False, testRunner=unittest.TextTestRunner(resultclass=Result))
  if output:
    Path(output).write_text(json.dumps(dict(passed=run.result.wasSuccessful(), cases=cases), indent=2)+'\n', encoding='utf-8')
  sys.exit(0 if run.result.wasSuccessful() else 1)
