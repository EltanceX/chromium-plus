import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

repo = Path(__file__).resolve().parents[3]
output = Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path(__file__).parent
output.mkdir(parents=True, exist_ok=True)
cpp = [
    'chrome/renderer/python_document_context.cc',
    'chrome/renderer/python_document_context_unittest.cc',
    'third_party/blink/public/web/web_python_script_runner.h',
    'third_party/blink/renderer/core/html/python_script_controller.h',
    'third_party/blink/renderer/core/html/python_script_controller.cc',
    'third_party/blink/renderer/core/html/html_py_script_element_test.cc',
] + ['components/python_runtime/' + name for name in (
    'python_dom_module.cc', 'python_execution_budget.cc', 'python_execution_budget.h',
    'python_runtime.cc', 'python_runtime.h', 'python_runtime_unittest.cc')]
checks = {}


def run(name, command):
    started = time.monotonic()
    result = subprocess.run(command, cwd=repo, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    (output / (name + '.log')).write_bytes(result.stdout)
    checks[name] = dict(command=command, exitCode=result.returncode,
                        passed=result.returncode == 0,
                        elapsedSeconds=round(time.monotonic()-started, 2))


run('clang-format', ['cmd', '/d', '/c', 'clang-format.bat', '--dry-run',
                     '--Werror', *cpp])
run('gn-format', ['buildtools/win/gn.exe', 'format', '--dry-run',
                  'components/python_runtime/BUILD.gn'])
run('syntax-mario-browser', ['node', '--check', 'chrome/test/pyscript_mario_smoke_test.mjs'])
run('syntax-python', [sys.executable, '-m', 'py_compile',
                      'chrome/test/build_pyscript_mario.py',
                      'chrome/test/pyscript_mario_unittest.py',
                      'chrome/test/run_pyscript_event_validation.py',
                      *[str(p.relative_to(repo)) for p in
                        (repo / 'chrome/test/data/pyscript/mario').glob('*.py')]])
run('generated-page', [sys.executable, 'chrome/test/build_pyscript_mario.py', '--check'])
for index, target in enumerate(('components/python_runtime',
                                'chrome/renderer/python_document_context.cc',
                                'chrome/renderer/python_document_context_unittest.cc',
                                'third_party/blink/renderer/core/html/python_script_controller.cc',
                                'third_party/blink/renderer/core/html/html_py_script_element_test.cc')):
    run('checkdeps-' + str(index),
        [sys.executable, 'buildtools/checkdeps/checkdeps.py', target])
run('diff-check', ['git', 'diff', '--check', '--', *cpp,
                   'chrome/test/build_pyscript_mario.py',
                   'chrome/test/pyscript_mario_unittest.py',
                   'chrome/test/pyscript_mario_smoke_test.mjs',
                   'chrome/test/run_pyscript_event_validation.py',
                   'chrome/test/data/pyscript/mario',
                   'chrome/test/data/pyscript/mario.html'])
manifest = json.loads((repo / 'third_party/cpython/source_manifest.json').read_text())
mismatches = [name for name, digest in manifest.items() if hashlib.sha256(
    (repo / 'third_party/cpython/src' / name).read_bytes()).hexdigest() != digest]
checks['upstream-source-manifest'] = dict(passed=not mismatches,
                                         files=len(manifest), mismatches=mismatches)
record = dict(recordedAt=datetime.datetime.now(datetime.timezone.utc).isoformat(),
              checks=checks, passed=all(c['passed'] for c in checks.values()))
(output / 'static-checks.json').write_text(json.dumps(record, indent=2)+'\n',
                                          encoding='utf-8', newline='\n')
print(json.dumps(record, indent=2))
sys.exit(0 if record['passed'] else 1)
