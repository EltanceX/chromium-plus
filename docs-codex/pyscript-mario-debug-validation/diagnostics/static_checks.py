from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

repo = Path.cwd()
out = repo / 'out/Debug/pyscript-mario-debug-stability'
paths = ['components/python_runtime', 'chrome/renderer/python_document_context.cc',
         'chrome/renderer/python_document_context_unittest.cc',
         'third_party/blink/public/web/web_python_script_runner.h',
         'third_party/blink/renderer/core/html/python_script_controller.cc',
         'third_party/blink/renderer/core/html/python_script_controller.h',
         'third_party/blink/renderer/core/html/html_py_script_element_test.cc',
         'third_party/blink/renderer/modules/modules_initializer.cc']
cpp = subprocess.check_output(['git', 'diff', 'cd413a9694', '--name-only', '--', *paths], text=True).splitlines()
cpp += ['components/python_runtime/python_browser_module.cc',
        'components/python_runtime/python_browser_types.h',
        'third_party/blink/renderer/core/html/python_browser_bridge.cc',
        'third_party/blink/renderer/core/html/python_browser_bridge.h',
        'third_party/blink/renderer/modules/python_browser_bridge_impl.cc',
        'third_party/blink/renderer/modules/python_browser_bridge_impl.h',
        'third_party/blink/renderer/modules/python_browser_bridge_test.cc']
cpp = sorted({name for name in cpp if Path(name).suffix in ('.cc', '.h')})
checks = []

def run(name, command):
  result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
  checks.append(dict(name=name, command=command, exitCode=result.returncode,
                     output=result.stdout.decode('utf-8', errors='replace')))

run('cpp-format', [os.environ['COMSPEC'], '/d', '/c', shutil.which('clang-format.bat'),
                   '--dry-run', '--Werror', *cpp])
for file in ('components/python_runtime/BUILD.gn',
             'third_party/blink/renderer/core/html/build.gni',
             'third_party/blink/renderer/modules/BUILD.gn'):
  run('gn-format-' + Path(file).parent.name,
      ['buildtools/win/gn.exe', 'format', '--dry-run', file])
for scope in ('components/python_runtime',
              'third_party/blink/renderer/modules/python_browser_bridge_impl.cc',
              'third_party/blink/renderer/core/html/python_script_controller.cc'):
  run('checkdeps-' + Path(scope).name,
      [sys.executable, 'buildtools/checkdeps/checkdeps.py', scope])
for file in ('chrome/test/pyscript_browser_smoke_test.mjs',
             'chrome/test/pyscript_mario_smoke_test.mjs'):
  run('node-check-' + Path(file).stem, [shutil.which('node'), '--check', file])
run('generated-page', [sys.executable, 'chrome/test/build_pyscript_mario.py', '--check'])
other = ['chrome/test/build_pyscript_mario.py', 'chrome/test/data/pyscript/mario.html',
         'chrome/test/data/pyscript/mario', 'chrome/test/pyscript_browser_smoke_test.mjs',
         'chrome/test/pyscript_mario_smoke_test.mjs', 'chrome/test/pyscript_mario_unittest.py',
         'chrome/test/run_pyscript_event_validation.py',
         'third_party/blink/renderer/modules/BUILD.gn',
         'third_party/blink/renderer/core/html/build.gni']
run('diff-whitespace', ['git', 'diff', '--check', '--', *paths, *other])
for file in [*Path('chrome/test/data/pyscript/mario').glob('*.py'),
             Path('chrome/test/run_pyscript_event_validation.py'),
             Path('chrome/test/pyscript_mario_unittest.py')]:
  compile(file.read_text(encoding='utf-8'), str(file), 'exec')
original = Path('C:/VD/Projects/mario/index.html')
record = dict(passed=all(check['exitCode'] == 0 for check in checks), checks=checks,
              pythonSyntaxPassed=True,
              originalMarioSha256=hashlib.sha256(original.read_bytes()).hexdigest())
(out / 'static-checks.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
print(json.dumps(dict(passed=record['passed'], checks=len(checks))))
sys.exit(0 if record['passed'] else 1)
