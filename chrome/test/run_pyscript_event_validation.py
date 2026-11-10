# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Build and verify native Python interaction without disabling the sandbox.

Run from any directory. Results go into a new directory under out/Debug by
default; an existing output directory is never overwritten.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import traceback

TARGETS = [
    'chrome', 'blink_unittests', 'python_runtime_unittests',
    'python_document_context_unittests', 'native_python_settings_unittests'
]
SUITES = [
    ('runtime', 'python_runtime_unittests', [], 67),
    ('adapter', 'python_document_context_unittests', [], 15),
    ('canvas-unit', 'blink_unittests', [
        '--gtest_filter=PythonCanvasBatchTest.*:PythonCanvasBridgeTest.*'
    ], 19),
    ('canvas-native', 'blink_unittests', [
        '--gtest_filter=CanvasRenderingContext2DAPITest.DefaultAttributeValues:'
        'CanvasRenderingContext2DAPITest.LineDashStateSave'
    ], 2),
    ('blink', 'blink_unittests', ['--gtest_filter=HTMLPyScriptElementTest.*'],
     74),
    ('csp', 'blink_unittests', [
        '--gtest_filter=ContentSecurityPolicyTest.'
        'NonceSinglePolicy:ContentSecurityPolicyTest.'
        'NonceInline:ContentSecurityPolicyTest.'
        'NonceMultiplePolicy'
    ], 3),
    ('settings-unit', 'native_python_settings_unittests', [], 8),
    ('events', 'pyscript_event_smoke_test.mjs', [], 34),
    ('input', 'pyscript_input_smoke_test.mjs', [], 25),
    ('canvas', 'pyscript_canvas_smoke_test.mjs', [], 23),
    ('animation', 'pyscript_animation_smoke_test.mjs', [], 19),
    ('dom', 'pyscript_dom_smoke_test.mjs', [], 17),
    ('parser', 'pyscript_smoke_test.mjs', [], 27),
    ('settings-browser', 'pyscript_settings_smoke_test.mjs', [], 12),
]


def now():
  return datetime.datetime.now(datetime.timezone.utc).isoformat()


def sha256(file):
  digest = hashlib.sha256()
  with file.open('rb') as stream:
    for chunk in iter(lambda: stream.read(1024 * 1024), b''):
      digest.update(chunk)
  return digest.hexdigest()


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--build-dir', default='out/Debug')
  parser.add_argument('--output-dir')
  args = parser.parse_args()
  repo = Path(__file__).resolve().parents[2]
  build = (repo / args.build_dir).resolve()
  output = (
      (repo / args.output_dir).resolve() if args.output_dir else build /
      'pyscript-event-validation' /
      datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
  output.mkdir(parents=True, exist_ok=False)
  windows = os.name == 'nt'
  flags = {'creationflags': subprocess.CREATE_NO_WINDOW} if windows else {}
  node = shutil.which('node')
  autoninja = shutil.which('autoninja.bat' if windows else 'autoninja')
  if not node or not autoninja:
    parser.error('node and autoninja must be available on PATH')

  def git(*arguments):
    return subprocess.check_output(['git', *arguments],
                                   cwd=repo,
                                   encoding='utf-8').strip()

  # Record the feature's actual inputs, including uncommitted test changes.
  inputs = git(
      'ls-files', '--cached', '--others', '--exclude-standard',
      'components/python_runtime', 'chrome/renderer/python*',
      'chrome/test/pyscript*', 'chrome/test/run_pyscript_event_validation.py',
      'chrome/test/data/pyscript', 'chrome/common/native_python*',
      'chrome/browser/native_python',
      'chrome/browser/resources/settings/kernel_debug_page',
      'third_party/blink/public/web/web_python*',
      'third_party/blink/renderer/core/html/*py*script*',
      'third_party/blink/renderer/core/html/python_canvas*',
      'third_party/blink/renderer/modules/canvas/canvas2d/python_canvas*',
      'third_party/blink/renderer/modules/canvas/canvas2d/canvas_2d_recorder_context.h',
      'third_party/blink/renderer/modules/modules_initializer.cc',
      'third_party/blink/renderer/modules/BUILD.gn',
      'third_party/blink/renderer/modules/canvas/BUILD.gn',
      'third_party/blink/renderer/core/html/build.gni',
      'third_party/blink/public/BUILD.gn',
      'third_party/blink/renderer/core/frame/csp/content_security_policy.cc')
  paths = sorted(
      set(inputs.splitlines()) | {Path(__file__).relative_to(repo).as_posix()})

  def input_hashes():
    return {name: sha256(repo / name) for name in paths}

  record = {
      'startedAt': now(),
      'sourceCommit': git('rev-parse', 'HEAD'),
      'workingTreeStatus': git('status', '--short'),
      'platform': platform.platform(),
      'pythonVersion': platform.python_version(),
      'nodeVersion': subprocess.check_output([node, '--version'],
                                             text=True).strip(),
      'buildDirectory': str(build),
      'outputDirectory': str(output),
      'buildArguments': (build / 'args.gn').read_text(encoding='utf-8'),
      'sourceSha256': input_hashes(),
      'checks': {},
      'passed': False
  }

  def save():
    temporary = output / 'summary.tmp'
    temporary.write_text(json.dumps(record, indent=2) + '\n',
                         encoding='utf-8',
                         newline='\n')
    temporary.replace(output / 'summary.json')

  def run(name, command):
    print('Starting ' + name, flush=True)
    started = time.monotonic()
    with (output / (name + '.log')).open('wb') as stream:
      process = subprocess.run(command,
                               cwd=repo,
                               stdout=stream,
                               stderr=subprocess.STDOUT,
                               **flags)
    return {
        'command': command,
        'exitCode': process.returncode,
        'elapsedSeconds': round(time.monotonic() - started, 2)
    }

  save()
  try:
    build_command = [autoninja, '-C', str(build), *TARGETS]
    if windows:
      build_command = [os.environ['COMSPEC'], '/d', '/c', *build_command]
    record['build'] = run('build', build_command)
    save()
    if record['build']['exitCode'] != 0:
      raise RuntimeError('Standard build failed; preserve the log and inputs')
    suffix = '.exe' if windows else ''
    binaries = [build / (target + suffix) for target in TARGETS]
    if windows:
      binaries += [
          build / name for name in ('chrome.dll', 'blink_core.dll',
                                    'blink_modules.dll', 'blink_platform.dll',
                                    'python_runtime.dll')
      ]
    record['binarySha256'] = {str(file): sha256(file) for file in binaries}
    save()
    for name, target, switches, expected in SUITES:
      browser = target.endswith('.mjs')
      result_file = output / (name + '.json')
      command = ([
          node,
          str(repo / 'chrome/test' / target), '--chrome-binary=' +
          str(build / ('chrome' + suffix)), '--output=' + str(result_file)
      ] if browser else [
          str(build / (target + suffix)), *switches, '--test-launcher-jobs=1',
          '--test-launcher-summary-output=' + str(result_file)
      ])
      check = run(name, command)
      check['expected'] = expected
      if result_file.exists():
        check['rawResultSha256'] = sha256(result_file)
        data = json.loads(result_file.read_text(encoding='utf-8'))
        if browser:
          cases = data['cases']
          check.update(total=len(cases),
                       passed=sum(c.get('passed') is True for c in cases),
                       suitePassed=data.get('passed') is True,
                       sandboxDisabled=data.get('sandboxDisabled'))
        else:
          cases = [
              attempts for iteration in data['per_iteration_data']
              for attempts in iteration.values()
          ]
          check.update(total=len(cases),
                       passed=sum(
                           bool(attempts) and all(a['status'] == 'SUCCESS'
                                                  for a in attempts)
                           for attempts in cases))
          # Blink's launcher includes tens of thousands of unrelated
          # test names. Keep executed results and locations for review.
          for key in ('all_tests', 'disabled_tests'):
            data.pop(key, None)
          executed = {
              name
              for iteration in data['per_iteration_data']
              for name in iteration
          }
          data['test_locations'] = {
              name: location
              for name, location in data.get('test_locations', {}).items()
              if name in executed
          }
          result_file.write_text(json.dumps(data, indent=2) + '\n',
                                 encoding='utf-8',
                                 newline='\n')
        check['resultSha256'] = sha256(result_file)
      check['verified'] = (check['exitCode'] == 0
                           and check.get('total') == expected
                           and check.get('passed') == expected
                           and check.get('suitePassed', True)
                           and check.get('sandboxDisabled', False) is False)
      record['checks'][name] = check
      save()
      print(name + ': ' + json.dumps(check), flush=True)
    record['sourceUnchanged'] = input_hashes() == record['sourceSha256']
    record['unitTestsPassed'] = sum(record['checks'][name]['passed']
                                    for name, target, _, _ in SUITES
                                    if not target.endswith('.mjs'))
    record['browserTestsPassed'] = sum(record['checks'][name]['passed']
                                       for name, target, _, _ in SUITES
                                       if target.endswith('.mjs'))
    record['passed'] = (record['sourceUnchanged']
                        and all(c['verified']
                                for c in record['checks'].values()))
  except Exception:
    record['error'] = traceback.format_exc()
    print(record['error'], file=sys.stderr, flush=True)
  finally:
    record['finishedAt'] = now()
    save()
  print(json.dumps({
      'passed': record['passed'],
      'output': str(output)
  }),
        flush=True)
  return 0 if record['passed'] else 1


if __name__ == '__main__':
  sys.exit(main())
