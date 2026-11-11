from pathlib import Path
import hashlib
import json

root = Path('out/Debug').resolve()
out = root / 'pyscript-mario-phase6'
records = {}

def sha(file):
  return hashlib.sha256(file.read_bytes()).hexdigest()

initial = {
    'obj/third_party/blink/renderer/core/core/local_dom_window.obj': ['coff-symbol-corrected.json'],
    'obj/third_party/blink/renderer/core/core/local_frame.obj': ['blink_core.dll.rsp-relocation-scan.json'],
    'obj/services/network/public/mojom/mojom/mdns_responder.mojom.obj': ['link-input-scan.json'],
}
for name, reasons in initial.items():
  backup = out / 'object-backups' / name
  records[name] = dict(object=name, beforeSha256=sha(backup), backup=str(backup), reasons=reasons)
for item in json.loads((out / 'module-object-recovery.json').read_text()):
  name = item['object'].replace('\\', '/')
  backup = out / 'object-backups' / name
  assert sha(backup) == item['before']
  records[name] = dict(object=name, beforeSha256=item['before'], backup=str(backup), reasons=item['reasons'])
for item in json.loads((out / 'async-object-recovery.json').read_text()):
  name = item['object'].replace('\\', '/')
  assert name not in records
  assert sha(Path(item['backup'])) == item['beforeSha256']
  records[name] = dict(item, object=name)
for item in json.loads((out / 'export-object-recovery.json').read_text()):
  name = item['object']
  assert name not in records
  assert sha(Path(item['backup'])) == item['beforeSha256']
  records[name] = item
for item in json.loads((out / 'metadata-object-recovery.json').read_text()):
  name = item['object']
  assert name not in records
  assert sha(Path(item['backup'])) == item['beforeSha256']
  records[name] = item
for name, item in records.items():
  file = (root / name).resolve()
  assert file.is_relative_to(root / 'obj') and file.is_file()
  item['afterSha256'] = sha(file)
  item['hashChanged'] = item['afterSha256'] != item['beforeSha256']
  assert item['hashChanged']
(out / 'object-recovery.json').write_text(json.dumps(list(records.values()), indent=2) + '\n')
print('Confirmed rebuilt objects:', len(records))
