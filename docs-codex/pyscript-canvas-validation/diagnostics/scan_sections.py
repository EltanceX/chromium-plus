from pathlib import Path
import mmap,struct,re,json
root=Path('out/Debug').resolve()
rsp=root/'blink_core.dll.rsp'
objects=re.findall(r'[^\s"]+\.obj',rsp.read_text())
problems=[]
for name in objects:
 p=root/name
 if not p.exists(): continue
 errors=[]
 with p.open('rb') as f, mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as b:
  big=b[:4]==b'\0\0\xff\xff'
  if big: count,ptr,num=struct.unpack_from('<III',b,44); start=56; stride=20
  else: count=struct.unpack_from('<H',b,2)[0];ptr,num=struct.unpack_from('<II',b,8);start=20+struct.unpack_from('<H',b,16)[0];stride=18
  table=ptr+num*stride
  for i in range(count):
   off=start+i*40
   if off+40>len(b): errors.append('truncated section header');break
   sec=b[off:off+8].rstrip(b'\0')
   if sec.startswith(b'/'):
    try:
     offset=int(sec[1:]); length=struct.unpack_from('<I',b,table)[0]
     if length<4 or table+length>len(b) or offset<4 or offset>=length or table+offset>=len(b): raise ValueError()
     end=b.find(b'\0',table+offset,min(len(b),table+length))
     if end<0: raise ValueError()
    except (ValueError,struct.error): errors.append(f'Section {i+1}: invalid name {sec!r}')
 if errors: problems.append({'object':name,'errors':errors})
record={'response':str(rsp),'directObjects':len(objects),'problems':problems}
(root/'pyscript-canvas-phase3'/'core-section-scan.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
print(json.dumps(record,indent=2))
