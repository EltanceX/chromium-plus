from pathlib import Path
import datetime
import hashlib
import json
import re
from urllib.parse import unquote

repo = Path.cwd()
base = repo / 'docs-codex/pyscript-mario-platform-validation'
source = repo / 'out/Debug/pyscript-mario-phase6'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
summary = json.loads((source/'final-2/summary.json').read_text(encoding='utf-8'))
assert summary['sourceUnchanged'] and len(summary['checks']) == 18
assert [k for k,v in summary['checks'].items() if not v['verified']] == ['mario']
assert summary['unitTestsPassed'] == 249 and summary['browserTestsPassed'] == 204
assert summary['passed'] is False and summary['build']['exitCode'] == 0
changed = [p for p,h in summary['sourceSha256'].items() if sha(repo/p) != h]
assert not changed, changed
manifest = json.loads((base/'archive-manifest.json').read_text(encoding='utf-8'))
assert len(manifest) == len({e['path'] for e in manifest})
for entry in manifest:
    assert sha(base/entry['path']) == entry['archivedSha256'], entry['path']
    assert sha(repo/entry['source']) == entry['originalSha256'], entry['source']
mcje = json.loads((source/'mcje-preserved-inputs.json').read_text(encoding='utf-8'))
assert all(sha(repo/p) == h for p,h in mcje.items())
original = Path('C:/VD/Projects/mario/index.html')
assert sha(original) == '53b556316bbc25a5c2c80b046be836dbaaea8aa5c25e85a2cf5f29a4b4bc9ef1'
links = []
for doc in [base/'README.md', repo/'docs-codex/pyscript-mario-phase6.md', repo/'docs-codex/pyscript-mario-implementation.md']:
    for target in re.findall(r'\]\(([^)]+)\)', doc.read_text(encoding='utf-8')):
        if '://' in target or target.startswith('#'): continue
        target = unquote(target.split('#',1)[0].strip('<>'))
        assert (doc.parent/target).exists(), (doc, target)
        links.append(target)
recovery = json.loads((source/'object-recovery.json').read_text(encoding='utf-8'))
assert len(recovery) == 56 and all(e['hashChanged'] for e in recovery)
standalone = json.loads((source/'mario-autofocus-3.json').read_text(encoding='utf-8'))
assert standalone['passed'] and len(standalone['cases']) == 35
assert all(c['passed'] for c in standalone['cases'])
report = dict(auditedAt=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    integrityChecksPassed=True, unifiedPassed=False, functionalInputsVerified=len(summary['sourceSha256']),
    archivedFilesVerified=len(manifest), documentationLinksVerified=len(links),
    preservedMcjeInputs=len(mcje), recoveredObjects=56,
    expectedCases=456, executedCases=454, passedCases=453,
    failedCases=1, notRunCases=2, standaloneMarioPassed=35,
    externalOriginalSha256=sha(original))
(base/'final-audit.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
print(json.dumps(report,indent=2))
