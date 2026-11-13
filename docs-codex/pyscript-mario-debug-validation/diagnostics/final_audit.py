from pathlib import Path
import datetime
import hashlib
import json
import re
from urllib.parse import unquote

repo = Path.cwd()
source = repo / 'out/Debug/pyscript-mario-debug-stability'
base = repo / 'docs-codex/pyscript-mario-debug-validation'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
summary = json.loads((base / 'final/summary.json').read_text(encoding='utf-8'))
assert summary['passed'] and summary['sourceUnchanged'] and len(summary['checks']) == 18
assert summary['unitTestsPassed'] == 253 and summary['browserTestsPassed'] == 207
assert all(check['verified'] for check in summary['checks'].values())
assert all(sha(repo / p) == h for p, h in summary['sourceSha256'].items())
assert all(sha(Path(p)) == h for p, h in summary['binarySha256'].items())
manifest = json.loads((base / 'archive-manifest.json').read_text(encoding='utf-8'))
assert len(manifest) == len({e['path'] for e in manifest})
for entry in manifest:
    assert sha(base / entry['path']) == entry['archivedSha256'], entry['path']
    assert sha(repo / entry['source']) == entry['originalSha256'], entry['source']
preserved = json.loads((source / 'preserved-inputs.json').read_text(encoding='utf-8'))
assert all(sha(repo / p) == h for p, h in preserved.items())
original = Path('C:/VD/Projects/mario/index.html')
assert sha(original) == '53b556316bbc25a5c2c80b046be836dbaaea8aa5c25e85a2cf5f29a4b4bc9ef1'
soak = json.loads((base / 'diagnostics/soak-gpu-final.json').read_text(encoding='utf-8'))
assert soak['passed'] and len(soak['cases']) == 36 and all(c['passed'] for c in soak['cases'])
assert soak['sandboxDisabled'] is False and soak['caseFilter'] == ''
assert soak['graphicsMode'] == 'default'
renderer = soak['graphics']['auxAttributes']['glRenderer']
assert 'AMD' in renderer and 'Direct3D11' in renderer and 'SwiftShader' not in renderer
assert soak['graphics']['featureStatus']['2d_canvas'] == 'enabled'
assert soak['graphics']['featureStatus']['gpu_compositing'] == 'enabled'
detail = next(c['detail'] for c in soak['cases'] if c['name'].startswith('Debug soak'))
assert detail['elapsedMs'] >= 300000 and detail['cycles'] >= 20
assert len(detail['lifecycleTransitions']) == detail['cycles'] // 10
for entry in detail['lifecycleTransitions']:
    assert entry['frozenPage']['hidden']
    assert entry['resumedPage'] == {'hidden': False, 'hasFocus': True}
    assert entry['resumedFrames'] >= entry['frozenFrames'] + 2
assert all(int(b['state']['frames']) > int(a['state']['frames'])
           for a, b in zip(detail['samples'], detail['samples'][1:]))
probe = json.loads((base / 'diagnostics/soak-gpu-final-inputs.json').read_text(encoding='utf-8'))
assert {Path(p).as_posix(): h for p, h in probe['sourceSha256'].items()} == summary['sourceSha256']
assert probe['sourceUnchanged'] and probe['binariesUnchanged'] and probe['exitCode'] == 0
assert all(summary['binarySha256'][str(repo / p)] == h for p, h in probe['binarySha256'].items())
for name, expected in (('gpu-before.json', 'FAILURE'), ('gpu-after.json', 'SUCCESS')):
    data = json.loads((base / 'diagnostics' / name).read_text(encoding='utf-8'))
    cases = [attempts for iteration in data['per_iteration_data'] for attempts in iteration.values()]
    assert len(cases) == 2
    assert all(attempts and all(a['status'] == expected for a in attempts) for attempts in cases)
recovery = json.loads((base / 'diagnostics/recovered-objects.json').read_text(encoding='utf-8'))
assert len(recovery) == 4 and all(item['hashChanged'] for item in recovery)
for item in recovery:
    assert sha(source / item['backup']) == item['beforeSha256']
    assert sha(repo / 'out/Debug' / item['object']) == item['afterSha256']
static = json.loads((base / 'diagnostics/static-checks.json').read_text(encoding='utf-8'))
assert static['passed'] and len(static['checks']) == 11
documents = [base / 'README.md', repo / 'docs-codex/pyscript-mario-debug-validation.md',
             repo / 'docs-codex/pyscript-mario-phase6.md',
             repo / 'docs-codex/pyscript-mario-implementation.md',
             repo / 'chrome/test/data/pyscript/mario/README.md']
links = []
resolved_links = []
for doc in documents:
    for target in re.findall(r'\]\(([^)]+)\)', doc.read_text(encoding='utf-8')):
        if '://' in target or target.startswith('#'):
            continue
        target = unquote(target.split('#', 1)[0].strip('<>'))
        resolved = (doc.parent / target).resolve()
        # This audit writes its own report after checking all other links.
        assert resolved == base / 'final-audit.json' or resolved.exists(), (doc, target)
        resolved_links.append(resolved)
        links.append(target)
counts = {name: [sample['domCounters'][name] for sample in detail['samples']]
          for name in ('documents', 'nodes', 'jsEventListeners')}
report = dict(auditedAt=datetime.datetime.now(datetime.timezone.utc).isoformat(),
              integrityChecksPassed=True, debugOnly=True, releaseBuilt=False,
              unifiedPassed=True, totalPassed=460, archivedFilesVerified=len(manifest),
              functionalInputsVerified=len(summary['sourceSha256']),
              preservedInputsVerified=len(preserved), recoveredObjects=4,
              documentationLinksVerified=len(links), soakElapsedMs=detail['elapsedMs'],
              soakCycles=detail['cycles'], lifecycleTransitions=len(detail['lifecycleTransitions']),
              domCounterRanges={name: dict(min=min(values), max=max(values))
                                for name, values in counts.items()},
              gpuRenderer=renderer, externalOriginalSha256=sha(original))
(base / 'final-audit.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
assert all(p.exists() for p in resolved_links)
print(json.dumps(report, indent=2))
