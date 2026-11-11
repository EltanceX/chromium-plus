# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Deterministic physics comparison with the original JavaScript oracle."""
import ast
import json
import sys
from pathlib import Path
import unittest
from unittest.mock import patch
from types import SimpleNamespace

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
  def test_all_eight_lessons_are_valid_python(self):
    tree = ast.parse((ROOT / 'ui.py').read_text(encoding='utf-8'))
    assignment = next(n for n in tree.body if isinstance(n, ast.Assign)
                      and any(isinstance(t, ast.Name) and t.id == 'lessons'
                              for t in n.targets))
    lessons = ast.literal_eval(assignment.value)
    self.assertEqual(len(lessons), 8)
    for name, title, source, explanation, exercise in lessons:
      with self.subTest(lesson=name):
        self.assertTrue(title and explanation and exercise)
        compile(source, 'lesson:'+name, 'exec')

  def test_slow_motion_scales_fixed_physics_and_timer(self):
    s = core()
    s['startGame']()
    s['settings'].slow = True
    clock = s['FixedClock']()
    for n in range(121):
      clock.advance(n*1000/120)
    self.assertAlmostEqual(s['game'].elapsed, 0.45)
    self.assertAlmostEqual(s['game'].remaining, 179.55)

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


def platform_scope(stored=None, failure=False):
  s = core()
  values = {} if stored is None else {'marioLabBest': stored}
  calls, updates = [], []
  def get(key):
    if failure: raise PermissionError('blocked')
    return values.get(key)
  def set_value(key, value):
    if failure: raise PermissionError('blocked')
    values[key] = value
  browser = SimpleNamespace(storage_get=get, storage_set=set_value,
      audio_stop=lambda: calls.append('stop'), audio_state=lambda: 'running',
      audio_resume=lambda: 1, audio_suspend=lambda: 2, poll=lambda: None,
      play_tones=lambda tones: calls.append(tones), is_fullscreen=lambda: False)
  s.update(queueUI=lambda *args: updates.append(args))
  with patch.dict(sys.modules, {'browser': browser}):
    exec(compile((ROOT/'platform.py').read_text(encoding='utf-8'), 'platform.py', 'exec'), s)
  s['platformLastState'] = s['game'].state
  return s, browser, values, calls, updates


class MarioPlatformTest(unittest.TestCase):
  def test_load_best_missing_invalid_negative_and_bounded(self):
    for text, expected in ((None, 0), ('bad', 0), ('-1', 0), ('5170', 5170),
                           ('999999999999', 999999999)):
      self.assertEqual(platform_scope(text)[0]['game'].best, expected)

  def test_storage_denial_does_not_change_playability(self):
    s, _, _, _, _ = platform_scope(failure=True)
    self.assertFalse(s['storageAvailable'])
    s['startGame']()
    self.assertEqual(s['game'].state, 'playing')
    s['game'].score, s['game'].remaining = 1200, 100
    s['winGame']()
    s['audioNeedsStop'] = False
    s['platformTick'](0, s['time'].perf_counter())
    self.assertEqual(s['game'].best, 2200)
    self.assertFalse(s['storageAvailable'])

  def test_win_persists_and_lower_score_keeps_best(self):
    s, _, values, _, _ = platform_scope('5000')
    s['game'].score, s['game'].remaining = 1200, 100
    s['winGame']()
    s['audioNeedsStop'] = False
    s['platformTick'](0, s['time'].perf_counter())
    self.assertEqual(values['marioLabBest'], '5000')

  def test_eight_sound_patterns_match_original_notes(self):
    s, _, _, _, _ = platform_scope()
    self.assertEqual(set(s['SOUNDS']), {'jump','coin','bump','stomp','hurt','checkpoint','start','win'})
    self.assertEqual(s['SOUNDS']['jump'][0], ('square',220,620,.15,0,.035))
    self.assertEqual([t[1] for t in s['SOUNDS']['win']], [523,659,784,1046,784,1046])

  def test_sound_queue_bounded_reset_and_mute(self):
    s, _, _, _, _ = platform_scope()
    s['playSound']('jump')
    self.assertEqual(s['soundQueue'], [])
    s['soundEnabled'] = True
    for _ in range(100): s['playSound']('coin')
    self.assertEqual(len(s['soundQueue']), 8)
    s['resetSound']()
    self.assertEqual(s['soundQueue'], [])
    self.assertTrue(s['audioNeedsStop'])

  def test_audio_batch_is_bounded(self):
    s, _, _, calls, _ = platform_scope()
    s['soundEnabled'] = s['soundRunning'] = True
    for _ in range(8): s['playSound']('win')
    s['platformTick'](0, s['time'].perf_counter())
    self.assertEqual(len(calls[0]), 12)
    self.assertEqual(len(s['soundQueue']), 6)

  def test_delayed_resume_reconciles_hidden_page(self):
    s, browser, _, _, _ = platform_scope()
    s['soundEnabled'] = True
    s['platformPending'][1] = 'audio'
    s['platformPageChanged'](SimpleNamespace(hidden=True, has_focus=False))
    browser.poll = lambda: (1, True, 'completed')
    s['platformTick'](0, s['time'].perf_counter())
    s['platformTick'](1, s['time'].perf_counter())
    self.assertTrue(s['soundRunning'])
    s['audioNeedsStop'] = False
    s['platformTick'](1, s['time'].perf_counter())
    self.assertEqual(s['platformPending'], {2:'audio'})

  def test_failed_audio_completion_resets_toggle(self):
    s, browser, _, _, updates = platform_scope()
    s['soundEnabled'] = True
    s['platformPending'][1] = 'audio'
    browser.poll = lambda: (1, False, 'denied')
    s['platformTick'](0, s['time'].perf_counter())
    self.assertFalse(s['soundEnabled'])
    self.assertIn(('soundButton', 'pressed', False), updates)


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
