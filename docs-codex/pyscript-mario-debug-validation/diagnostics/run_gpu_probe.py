from pathlib import Path
import datetime, hashlib, json, shutil, subprocess, sys

repo = Path.cwd()
out = repo / 'out/Debug/pyscript-mario-debug-stability'
baseline = json.loads((out / 'final-1/summary.json').read_text(encoding='utf-8'))
label, seconds = sys.argv[1:3]
assert label.replace('-', '').isalnum()
target = out / (label + '.json')
assert not target.exists()
sources = [repo / name for name in baseline['sourceSha256']]
binaries = [Path(name) for name in baseline['binarySha256']]
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
snapshot = lambda paths: {str(p.relative_to(repo)): sha(p) for p in paths}
record = dict(startedAt=datetime.datetime.now(datetime.timezone.utc).isoformat(),
              sourceSha256=snapshot(sources), binarySha256=snapshot(binaries))
command = [shutil.which('node'), 'chrome/test/pyscript_mario_smoke_test.mjs',
           '--graphics=default', '--soak-seconds=' + seconds, '--output=' + str(target)]
if len(sys.argv) > 3:
    command.append('--case-filter=' + sys.argv[3])
record['command'] = command
with (out / (label + '.log')).open('wb') as log:
    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
record.update(exitCode=result.returncode,
               finishedAt=datetime.datetime.now(datetime.timezone.utc).isoformat(),
               sourceUnchanged=snapshot(sources) == record['sourceSha256'],
               binariesUnchanged=snapshot(binaries) == record['binarySha256'])
(out / (label + '-inputs.json')).write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
print(json.dumps({k: v for k, v in record.items() if not k.endswith('Sha256')}, indent=2))
sys.exit(result.returncode or int(not record['sourceUnchanged'] or not record['binariesUnchanged']))
