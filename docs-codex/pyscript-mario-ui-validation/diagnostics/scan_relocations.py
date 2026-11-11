from pathlib import Path
import mmap, struct, re, json, sys

root = Path('out/Debug').resolve()
out = root / 'pyscript-mario-phase5'
rsp = root / (sys.argv[1] if len(sys.argv)>1 else 'blink_core.dll.rsp')
objects = re.findall(r'[^\s"]+\.obj', rsp.read_text())
problems = []
for name in objects:
    path = root / name
    if not path.exists():
        continue
    errors = []
    with path.open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as b:
        big = b[:4] == b'\0\0\xff\xff'
        if big:
            count, ptr, symbols = struct.unpack_from('<III', b, 44)
            start, stride = 56, 20
        else:
            count = struct.unpack_from('<H', b, 2)[0]
            ptr, symbols = struct.unpack_from('<II', b, 8)
            start, stride = 20 + struct.unpack_from('<H', b, 16)[0], 18
        for i in range(count):
            off = start + i*40
            sec = b[off:off+8].rstrip(b'\0').decode('ascii', errors='replace')
            size, data, relocptr = struct.unpack_from('<III', b, off+16)
            relocs = struct.unpack_from('<H', b, off+32)[0]
            flags = struct.unpack_from('<I', b, off+36)[0]
            first = 0
            if relocs == 0xffff and flags & 0x01000000:
                relocs = struct.unpack_from('<I', b, relocptr)[0]
                first = 1
            if relocptr+relocs*10 > len(b):
                errors.append(dict(section=i+1, name=sec, error='relocation table extent'))
                continue
            for j in range(first, relocs):
                va, symbol, kind = struct.unpack_from('<IIH', b, relocptr+j*10)
                if va > size or symbol >= symbols:
                    errors.append(dict(section=i+1, name=sec, size=size,
                                       relocation=j, offset=va, symbol=symbol, kind=kind))
                    break
            if len(errors) >= 10:
                break
    if errors:
        problems.append(dict(object=name, errors=errors))
record = dict(directObjects=len(objects), problems=problems)
(out/(rsp.name+'-relocation-scan.json')).write_text(json.dumps(record, indent=2)+'\n')
print(json.dumps(record, indent=2))
