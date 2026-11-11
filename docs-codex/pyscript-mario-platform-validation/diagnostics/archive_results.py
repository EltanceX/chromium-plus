from pathlib import Path
import hashlib
import json

repo = Path.cwd().resolve()
source = repo / 'out/Debug/pyscript-mario-phase6'
destination = repo / 'docs-codex/pyscript-mario-platform-validation'
entries = []

def sha(data):
  return hashlib.sha256(data).hexdigest()

def archive(file, relative):
  data = file.read_bytes()
  if file.suffix not in ('.png',):
    text = data.decode('utf-8', errors='replace')
    text = ''.join(c if ord(c) >= 32 or c in '\n\r\t' else f'\\x{ord(c):02x}' for c in text)
    data = ('\n'.join(line.rstrip() for line in text.splitlines()).rstrip() + '\n').encode('utf-8')
  target = destination / relative
  target.parent.mkdir(parents=True, exist_ok=True)
  target.write_bytes(data)
  entries.append(dict(source=str(file.relative_to(repo)), path=str(relative),
                      originalSha256=sha(file.read_bytes()),
                      archivedSha256=sha(data)))

final = source / 'final-2'
summary = json.loads((final / 'summary.json').read_text(encoding='utf-8'))
assert summary.get('finishedAt') and summary['sourceUnchanged']
assert len(summary['checks']) == 18
# Archive the actual outcome, including a non-green final run. Never replace
# it with the earlier passing standalone Mario suite.
assert [name for name, check in summary['checks'].items()
        if not check['verified']] == ['mario']
for file in sorted(final.iterdir()):
  if file.is_file():
    archive(file, Path('final') / file.name)

diagnostics = set()
for pattern in ('build-*.log', '*scan*.json', '*recovery.json', 'scan_*.py',
                'link-input*.json', 'coff-symbol-corrected.json', '*failure*.rsp'):
  diagnostics.update(source.glob(pattern))
for file in sorted(diagnostics):
  if file.name != 'build-explain-21.log':
    archive(file, Path('diagnostics') / file.name)

for file in sorted((source / 'final-1').iterdir()):
  if file.is_file():
    archive(file, Path('first-unified') / file.name)

for name in ('adapter-initial.log', 'adapter-initial.json', 'adapter-built.log',
             'adapter-built.json', 'runtime-initial.log', 'runtime-initial.json',
             'blink-browser-initial.log', 'static-checks.json',
             'fullscreen-control2.log', 'fullscreen-control2.json.chrome.log',
             'probe_fullscreen.mjs', 'export-before.json', 'export-before.log',
             'export-before.json.chrome.log', 'probe_export.mjs',
             'static_checks.py', 'summarize_recovery.py',
             'archive_results.py', 'final_audit.py',
             'coff-symbol-followup-corrections.json',
             'build-dependency-explanation.json',
             'blink-browser-after-build.log', 'final-1-driver.log', 'final-2-driver.log',
             'metadata-before.log', 'metadata-before-executed.json',
             'metadata-after.log', 'metadata-after-executed.json'):
  archive(source / name, Path('diagnostics') / name)
probes = set()
for pattern in ('browser-first*', 'browser-async-*', 'browser-after-build-*',
                'browser-snapshot-*.json', 'browser-snapshot-*.log',
                'mario-after-build-*', 'mario-autofocus-*'):
  probes.update(file for file in source.glob(pattern) if file.is_file())
for file in sorted(probes):
  archive(file, Path('probes') / file.name)

assert len(entries) == len({item['path'] for item in entries})
(destination / 'archive-manifest.json').write_text(
    json.dumps(entries, indent=2) + '\n', encoding='utf-8', newline='\n')
status = dict(
    featureImplementationComplete=True,
    standardBuildComplete=summary['build']['exitCode'] == 0,
    stageComplete=False,
    unifiedPassed=summary['passed'],
    sourceUnchanged=summary['sourceUnchanged'],
    functionalInputFiles=len(summary['sourceSha256']),
    expectedCases=sum(c['expected'] for c in summary['checks'].values()),
    passedCases=sum(c['passed'] for c in summary['checks'].values()),
    executedCases=sum(c['total'] for c in summary['checks'].values()),
    failedSuites=['mario'],
    remainingIssue='Debug full-level-route stopped at frame 2340: 16 ms budget',
    finalRun='final/summary.json',
    standaloneMario='probes/mario-autofocus-3.json',
    standaloneMarioPassed=json.loads((source / 'mario-autofocus-3.json').read_text(
        encoding='utf-8'))['passed'])
(destination / 'validation-status.json').write_text(
    json.dumps(status, indent=2) + '\n', encoding='utf-8', newline='\n')
print(json.dumps(dict(files=len(entries), bytes=sum(
    (destination / item['path']).stat().st_size for item in entries)), indent=2))
