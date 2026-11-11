from pathlib import Path
import struct,re,mmap,json,hashlib
root=Path('out/Debug').resolve(); out=root/'pyscript-mario-phase6'
objects={root/x for x in re.findall(r'[^\s"]+\.obj',(root/'blink_core.dll.rsp').read_text())}
archives=[]
for name in re.findall(r'[^\s"]+\.lib',(root/'blink_core.dll.rsp').read_text()):
 p=root/name
 if not p.exists():continue
 b=p.read_bytes()
 if b[:8]!=b'!<thin>\n':continue
 pos=8;table=b''
 while pos+60<=len(b):
  header=b[pos:pos+60];nm=header[:16].decode('ascii').strip();size=int(header[48:58]);pos+=60
  if nm in ('/','//','/SYM64/'):
   if nm=='//':table=b[pos:pos+size]
   pos+=size+(size%2)
  elif nm.startswith('/') and nm[1:].isdigit():
   i=int(nm[1:]); end=table.find(b'/\n',i)
   if end<0:end=table.find(b'\0',i)
   text=table[i:end].decode('utf-8')
   objects.add((p.parent/text).resolve())
  else:objects.add((p.parent/nm.rstrip('/')).resolve())
 archives.append(str(p.relative_to(root)))
problems=[]
for p in sorted(objects):
 if not p.exists():continue
 with p.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as b:
  if len(b)<20:continue
  big=b[:4]==b'\0\0\xff\xff'
  if big: count,ptr,num=struct.unpack_from('<III',b,44);start=56;stride=20
  else:count=struct.unpack_from('<H',b,2)[0];ptr,num=struct.unpack_from('<II',b,8);start=20+struct.unpack_from('<H',b,16)[0];stride=18
  errors=[];table=ptr+num*stride
  for i in range(count):
   off=start+i*40
   if off+40>len(b):errors.append('truncated section header');break
   sec=b[off:off+8].rstrip(b'\0')
   if sec.startswith(b'/'):
    try:
     offset=int(sec[1:]);length=struct.unpack_from('<I',b,table)[0]
     if length<4 or table+length>len(b) or offset<4 or offset>=length or b[table+length-1]!=0:raise ValueError()
    except(ValueError,struct.error):errors.append('invalid section string table at '+str(i+1))
  if errors:problems.append(dict(object=str(p.relative_to(root)),errors=errors[:10],sha256=hashlib.sha256(b).hexdigest()))
record=dict(objects=len(objects),thinArchives=len(archives),problems=problems)
(out/'link-input-scan.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
