from pathlib import Path
import mmap,struct,re,json,sys
root=Path('out/Debug').resolve();out=root/'pyscript-mario-phase6';rsp=root/sys.argv[1]
objects=re.findall(r'[^\s"]+\.obj',rsp.read_text())
def read(p):
 with p.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as b:
  big=b[:4]==b'\0\0\xff\xff';count=struct.unpack_from('<I',b,44)[0] if big else struct.unpack_from('<H',b,2)[0];start=56 if big else 20+struct.unpack_from('<H',b,16)[0];kinds=[];hashes=b''
  for i in range(count):
   off=start+i*40;name=b[off:off+8].rstrip(b'\0');size,data=struct.unpack_from('<II',b,off+16)
   if name==b'.debug$H':hashes=b[data+8:data+size]
   if name==b'.debug$T':
    pos=data+4;end=data+size
    while pos+4<=end:
     length,kind=struct.unpack_from('<HH',b,pos);kinds.append(kind);pos+=length+2
  return kinds,hashes
sources=[n for n in objects if Path(n).name in sys.argv[2:]];targets={}
for n in sources:
 kinds,hashes=read(root/n)
 for i,kind in enumerate(kinds):
  if kind in (0x1601,0x1602):targets[hashes[i*8:(i+1)*8]]=kind
problems=[]
for n in objects:
 kinds,hashes=read(root/n);errors=[]
 if not hashes:continue
 if len(hashes)!=8*len(kinds):errors.append(f'GHASH count {len(hashes)//8} differs from type count {len(kinds)}')
 for i,kind in enumerate(kinds):
  h=hashes[i*8:(i+1)*8];expected=targets.get(h)
  if expected is not None and expected!=kind:
   errors.append(f'GHASH {h.hex()} at type {i+0x1000:#x}: leaf {kind:#x} conflicts with {expected:#x}')
   if len(errors)==8:break
 if errors:problems.append({'object':n,'errors':errors})
record={'response':str(rsp),'sources':sources,'targetHashCount':len(targets),'problems':problems};(out/(rsp.name+'-hash-scan.json')).write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8');print(json.dumps(record,indent=2))
