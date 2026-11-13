from pathlib import Path
import contextlib, io, json, runpy, struct, subprocess, sys

with contextlib.redirect_stdout(io.StringIO()):
    sys.argv = ['scan_sections.py', 'blink_core.dll.rsp', 'gpu-native']
    scan = runpy.run_path('out/Debug/pyscript-mario-debug-stability/scan_sections.py')
root, out = scan['root'], scan['out']
candidates = []
for name in scan['objects']:
    with (root / name).open('rb') as f:
        head = f.read(56)
    count = struct.unpack_from('<I', head, 44)[0] if head[:4] == b'\0\0\xff\xff' else struct.unpack_from('<H', head, 2)[0]
    if count >= 312:
        candidates.append(name)
record = dict(candidates=len(candidates), chunks=[])
for i in range(0, len(candidates), 40):
    names = candidates[i:i + 40]
    result = subprocess.run([str(Path('third_party/llvm-build/Release+Asserts/bin/llvm-readobj.exe').resolve()),
                             '--sections', *names], cwd=root, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    if result.returncode or result.stderr:
        record['chunks'].append(dict(objects=names, exitCode=result.returncode,
                                     stderr=result.stderr.decode('utf-8', errors='replace')))
(out / 'gpu-native-readobj.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
print(json.dumps(record, indent=2))
