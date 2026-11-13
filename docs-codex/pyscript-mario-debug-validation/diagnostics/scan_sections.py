from pathlib import Path
import hashlib, json, mmap, re, struct, sys

root = Path('out/Debug').resolve()
out = root / 'pyscript-mario-debug-stability'
rsp = root / (sys.argv[1] if len(sys.argv) > 1 else 'blink_unittests.exe.rsp')
prefix = sys.argv[2] if len(sys.argv) > 2 else 'gpu-regression'
(out / f'{prefix}-link.rsp').write_bytes(rsp.read_bytes())
objects = re.findall(r'[^\s"]+\.obj', rsp.read_text())
direct_count = len(objects)
archives = []
for name in re.findall(r'[^\s"]+\.lib', rsp.read_text()):
    p = root / name
    if not p.exists():
        continue
    b = p.read_bytes()
    if b[:8] != b'!<thin>\n':
        continue
    archives.append(name)
    pos, table = 8, b''
    while pos + 60 <= len(b):
        header = b[pos:pos + 60]
        nm = header[:16].decode('ascii').strip()
        size = int(header[48:58])
        pos += 60
        if nm in ('/', '//', '/SYM64/'):
            if nm == '//':
                table = b[pos:pos + size]
            pos += size + size % 2
        elif nm.startswith('/') and nm[1:].isdigit():
            i = int(nm[1:])
            end = table.find(b'/\n', i)
            if end < 0:
                end = table.find(b'\0', i)
            objects.append((p.parent / table[i:end].decode('utf-8')).resolve().relative_to(root).as_posix())
        else:
            objects.append((p.parent / nm.rstrip('/')).resolve().relative_to(root).as_posix())
objects = sorted(set(objects))
problems = []
for name in objects:
    p = root / name
    if not p.exists():
        continue
    errors = []
    with p.open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as b:
        big = b[:4] == b'\0\0\xff\xff'
        if big and len(b) >= 56:
            count, ptr, num = struct.unpack_from('<III', b, 44)
            start, stride = 56, 20
        elif len(b) >= 20 and b[:2] == b'\x64\x86':
            count = struct.unpack_from('<H', b, 2)[0]
            ptr, num = struct.unpack_from('<II', b, 8)
            start, stride = 20 + struct.unpack_from('<H', b, 16)[0], 18
        else:
            problems.append(dict(object=name, errors=['Unknown COFF type: '+b[:20].hex()],
                                 sha256=hashlib.sha256(b).hexdigest()))
            continue
        table = ptr + num * stride
        for i in range(count):
            off = start + i * 40
            if off + 40 > len(b):
                errors.append('truncated section header')
                break
            sec = b[off:off + 8].rstrip(b'\0')
            if sec.startswith(b'/'):
                try:
                    if not ptr or not num:
                        raise ValueError()
                    offset = int(sec[1:])
                    length = struct.unpack_from('<I', b, table)[0]
                    if (length < 4 or table + length > len(b) or
                            offset < 4 or offset >= length or
                            b[table + length - 1] != 0):
                        raise ValueError()
                    end = b.find(b'\0', table + offset, table + length)
                    if end < 0:
                        raise ValueError()
                except (ValueError, struct.error):
                    errors.append(f'Section {i + 1}: invalid name {sec!r}')
    if errors:
        problems.append(dict(object=name, errors=errors,
                             sha256=hashlib.sha256(p.read_bytes()).hexdigest()))
record = dict(response=str(rsp), directObjects=direct_count,
              totalObjects=len(objects), thinArchives=archives, problems=problems)
(out / f'{prefix}-section-scan.json').write_text(
    json.dumps(record, indent=2) + '\n', encoding='utf-8')
print(json.dumps(record, indent=2))
