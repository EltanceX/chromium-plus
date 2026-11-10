from pathlib import Path
import mmap,struct,re,json,sys
root=Path('out/Debug').resolve();out=root/'pyscript-canvas-phase3';rsp=root/(sys.argv[1] if len(sys.argv)>1 else 'blink_platform.dll.rsp')
objects=re.findall(r'[^\s"]+\.obj',rsp.read_text());problems=[]
for name in objects:
 p=root/name
 if not p.exists():continue
 errors=[]
 with p.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as b:
  big=b[:4]==b'\0\0\xff\xff'
  if big:count,ptr,num=struct.unpack_from('<III',b,44);start=56;stride=20
  else:count=struct.unpack_from('<H',b,2)[0];ptr,num=struct.unpack_from('<II',b,8);start=20+struct.unpack_from('<H',b,16)[0];stride=18
  table=ptr+num*stride
  type_kinds=[];procedures=[]
  for i in range(count):
   off=start+i*40
   if off+40>len(b):errors.append('truncated section header');break
   sec=b[off:off+8].rstrip(b'\0');size,data=struct.unpack_from('<II',b,off+16)
   if not size:continue
   if data+size>len(b):errors.append(f'section {i+1} extends beyond object');continue
   if sec not in (b'.debug$T',b'.debug$S'):continue
   if size<4 or b[data:data+4]!=b'\4\0\0\0':errors.append(f'{sec!r} invalid signature');continue
   cursor=data+4;end=data+size
   if sec==b'.debug$T':
    while cursor+4<=end:
     length,kind=struct.unpack_from('<HH',b,cursor)
     if length<2 or cursor+2+length>end or not (kind==0xa or 0x1000<=kind<0x2000):
      errors.append(f'Invalid CodeView type at {cursor:#x}: length {length}, kind {kind:#x}');break
     type_kinds.append(kind)
     cursor+=length+2
    if cursor!=end:errors.append(f'Invalid CodeView type extent {cursor:#x} != {end:#x}')
   else:
    while cursor+8<=end:
     kind,length=struct.unpack_from('<II',b,cursor)
     if not 0xf0<=kind<0x1000 or cursor+8+length>end:
      errors.append(f'Invalid CodeView symbol subsection {cursor:#x}: kind {kind:#x}, size {length}');break
     if kind==0xf1:
      sym=cursor+8;symend=sym+length
      while sym+4<=symend:
       reclen,reccode=struct.unpack_from('<HH',b,sym)
       if reclen<2 or sym+2+reclen>symend or not (reccode==6 or 0x1000<=reccode<0x1200):
        errors.append(f'Invalid CodeView symbol record at {sym:#x}: length {reclen}, kind {reccode:#x}');break
       if reccode in (0x1146,0x1147) and reclen>=39:
        procedures.append((struct.unpack_from('<I',b,sym+28)[0],sym))
       sym+=2+reclen
     cursor=data+((cursor-data+8+length+3)&~3)
   if len(errors)>10:break
  for index,sym in procedures:
   local=index-0x1000
   if local<0 or local>=len(type_kinds) or type_kinds[local] not in (0x1601,0x1602):
    errors.append(f'Invalid procedure ID {index:#x} at {sym:#x}, type kind '+ (hex(type_kinds[local]) if 0<=local<len(type_kinds) else 'out of range'))
    if len(errors)>15:break
  if table+4<=len(b):
   length=struct.unpack_from('<I',b,table)[0]
   if 4<=length and table+length<=len(b):
    try:b[table+4:table+length].decode('utf-8')
    except UnicodeDecodeError as e:errors.append(f'Invalid UTF8 string table at {table+4+e.start:#x}')
 if errors:problems.append({'object':name,'errors':errors})
record={'response':str(rsp),'directObjects':len(objects),'problems':problems};(out/(rsp.name+'-debug-scan.json')).write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8');print(json.dumps({'response':str(rsp),'directObjects':len(objects),'problemCount':len(problems),'firstProblems':problems[:8]},indent=2))
