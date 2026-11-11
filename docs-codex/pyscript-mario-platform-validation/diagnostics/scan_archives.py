from pathlib import Path
import struct,re,json,hashlib
root=Path('out/Debug').resolve();out=root/'pyscript-mario-phase6'
problems=[];members=0
for name in re.findall(r'[^\s"]+\.(?:lib|rlib|a)',(root/'blink_core.dll.rsp').read_text()):
 p=root/name
 if not p.exists():continue
 data=p.read_bytes()
 if data[:8]!=b'!<arch>\n':continue
 pos=8
 while pos+60<=len(data):
  head=data[pos:pos+60];nm=head[:16].decode('ascii',errors='replace').strip();size=int(head[48:58]);pos+=60
  b=data[pos:pos+size];offset=pos;pos+=size+size%2
  if nm in ('/','//','/SYM64/'):continue
  members+=1
  try:
   if len(b)<20:continue
   big=b[:4]==b'\0\0\xff\xff'
   if big and struct.unpack_from('<H',b,4)[0]!=2:continue
   if big: count,ptr,num=struct.unpack_from('<III',b,44);start=56;stride=20
   else:count=struct.unpack_from('<H',b,2)[0];ptr,num=struct.unpack_from('<II',b,8);start=20+struct.unpack_from('<H',b,16)[0];stride=18
   if b[:4]==b'BC\xc0\xde':continue
   errors=[];table=ptr+num*stride
   if count>100000:continue
   for i in range(count):
    off=start+i*40
    if off+40>len(b):errors.append('truncated section header');break
    sec=b[off:off+8].rstrip(b'\0')
    if sec.startswith(b'/'):
     try:
      idx=int(sec[1:]);length=struct.unpack_from('<I',b,table)[0]
      if length<4 or table+length>len(b) or idx<4 or idx>=length or b[table+length-1]!=0:raise ValueError()
     except(ValueError,struct.error):errors.append('invalid section string table '+str(i+1)+' '+repr(sec))
   if errors:problems.append(dict(archive=name,member=nm,offset=offset,errors=errors[:5],sha256=hashlib.sha256(data).hexdigest()))
  except struct.error:pass
record=dict(members=members,problems=problems)
(out/'archive-scan.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))
