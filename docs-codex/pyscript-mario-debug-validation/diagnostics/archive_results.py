from pathlib import Path
import datetime,hashlib,json,re
repo=Path.cwd()
src=repo/'out/Debug/pyscript-mario-debug-stability'
dst=repo/'docs-codex/pyscript-mario-debug-validation'
summary=json.loads((src/'final-2/summary.json').read_text(encoding='utf-8'))
assert summary['passed'] and summary['sourceUnchanged'] and len(summary['checks'])==18
assert summary['unitTestsPassed']==253 and summary['browserTestsPassed']==207
assert all(c['verified'] for c in summary['checks'].values())
soak=json.loads((src/'soak-gpu-final.json').read_text(encoding='utf-8'))
assert soak['passed'] and len(soak['cases'])==36 and all(c['passed'] for c in soak['cases'])
assert soak['sandboxDisabled'] is False and soak['graphicsMode']=='default'
sha=lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
assert all(sha(repo/p)==h for p,h in summary['sourceSha256'].items())
assert all(sha(Path(p))==h for p,h in summary['binarySha256'].items())
preserved=json.loads((src/'preserved-inputs.json').read_text(encoding='utf-8'))
assert all(sha(repo/p)==h for p,h in preserved.items())
probe=json.loads((src/'soak-gpu-final-inputs.json').read_text(encoding='utf-8'))
assert probe['exitCode']==0 and probe['sourceUnchanged'] and probe['binariesUnchanged']
assert {Path(p).as_posix():h for p,h in probe['sourceSha256'].items()}==summary['sourceSha256']
assert all(summary['binarySha256'][str(repo/p)]==h for p,h in probe['binarySha256'].items())
recovered=[]
for name in ('object-recovery.json','core-object-recovery.json','string-table-object-recovery.json','codeview-object-recovery.json'):
 item=json.loads((src/name).read_text(encoding='utf-8-sig'))
 assert sha(src/item['backup'])==item['beforeSha256']
 item['afterSha256']=sha(repo/'out/Debug'/item['object'])
 item['hashChanged']=item['afterSha256']!=item['beforeSha256']
 assert item['hashChanged']
 recovered.append(item)
(src/'recovered-objects.json').write_text(json.dumps(recovered,indent=2)+'\n',encoding='utf-8')
entries=[]
def copy(p,relative):
 data=p.read_bytes(); original=hashlib.sha256(data).hexdigest()
 if p.suffix!='.png':
  text=data.decode('utf-8-sig',errors='replace')
  text=''.join(c if ord(c)>=32 or c in '\n\r\t' else f'\\x{ord(c):02x}' for c in text)
  data=('\n'.join(line.rstrip() for line in text.splitlines()).rstrip()+'\n').encode('utf-8')
 target=dst/relative; target.parent.mkdir(parents=True,exist_ok=True); target.write_bytes(data)
 entries.append(dict(source=p.relative_to(repo).as_posix(),path=relative.as_posix(),originalSha256=original,archivedSha256=hashlib.sha256(data).hexdigest()))
for p in sorted(src.iterdir()):
 if p.is_file(): copy(p,Path('diagnostics')/p.name)
for directory,destination in (('final-1','first-unified'),('final-2','final')):
 for p in sorted((src/directory).iterdir()):
  if p.is_file(): copy(p,Path(destination)/p.name)
assert len(entries)==len({e['path'] for e in entries})
(dst/'archive-manifest.json').write_text(json.dumps(entries,indent=2)+'\n',encoding='utf-8',newline='\n')
check=next(c for c in soak['cases'] if c['name'].startswith('Debug soak'))
assert check['detail']['elapsedMs']>=300000
samples=check['detail']['samples']
status=dict(recordedAt=datetime.datetime.now(datetime.timezone.utc).isoformat(),debugOnly=True,releaseBuilt=False,stageSixDebugComplete=True,unifiedPassed=True,unitIncludingHostPassed=summary['unitTestsPassed'],browserPassed=summary['browserTestsPassed'],totalPassed=summary['unitTestsPassed']+summary['browserTestsPassed'],functionalInputsVerified=len(summary['sourceSha256']),preservedInputsVerified=len(preserved),recoveredObjects=len(recovered),soakCasesPassed=36,soakElapsedMs=check['detail']['elapsedMs'],soakCycles=check['detail']['cycles'],soakSamples=len(samples),graphics=soak['graphics'],manualSpeakerListeningVerified=False,forcedDeschedulingStillStops=True)
(dst/'validation-status.json').write_text(json.dumps(status,indent=2)+'\n',encoding='utf-8',newline='\n')
print(json.dumps(dict(files=len(entries),bytes=sum((dst/e['path']).stat().st_size for e in entries),status=status),indent=2))
