from pathlib import Path
import hashlib, json

root = Path('out/Debug/pyscript-mario-debug-stability')
record = []
for file in sorted(root.glob('gpu-*.json')):
    data = json.loads(file.read_text(encoding='utf-8'))
    if 'per_iteration_data' not in data or 'all_tests' not in data:
        continue
    original = hashlib.sha256(file.read_bytes()).hexdigest()
    for key in ('all_tests', 'disabled_tests'):
        data.pop(key, None)
    executed = {name for iteration in data['per_iteration_data'] for name in iteration}
    data['test_locations'] = {name: location for name, location in
                              data.get('test_locations', {}).items() if name in executed}
    file.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8', newline='\n')
    record.append(dict(path=file.name, originalSha256=original,
                       resultSha256=hashlib.sha256(file.read_bytes()).hexdigest(),
                       executed=sorted(executed)))
if record:
    target = root / 'launcher-compaction.json'
    previous = json.loads(target.read_text(encoding='utf-8')) if target.exists() else []
    target.write_text(json.dumps(previous + record, indent=2) + '\n', encoding='utf-8')
print(json.dumps(record, indent=2))
