from pathlib import Path
import hashlib, json, re, struct

root = Path('out/Debug').resolve()
out = root / 'pyscript-mario-debug-stability'
problems, members = [], 0
for name in re.findall(r'[^\s"]+\.(?:lib|rlib|a)', (root / 'blink_core.dll.rsp').read_text()):
    p = root / name
    if not p.exists():
        continue
    data = p.read_bytes()
    if data[:8] != b'!<arch>\n':
        continue
    pos, names = 8, b''
    while pos + 60 <= len(data):
        head = data[pos:pos + 60]
        nm = head[:16].decode('ascii', errors='replace').strip()
        size = int(head[48:58])
        pos += 60
        b, offset = data[pos:pos + size], pos
        pos += size + size % 2
        if nm in ('/', '//', '/SYM64/'):
            if nm == '//':
                names = b
            continue
        if nm.startswith('/') and nm[1:].isdigit():
            i = int(nm[1:])
            end = names.find(b'/\n', i)
            if end < 0:
                end = names.find(b'\0', i)
            nm = names[i:end].decode('utf-8', errors='replace')
        members += 1
        if len(b) < 20:
            continue
        big = b[:4] == b'\0\0\xff\xff'
        if big and struct.unpack_from('<H', b, 4)[0] != 2:
            continue  # Import descriptor, not a big COFF object.
        if big:
            count, ptr, num = struct.unpack_from('<III', b, 44)
            start, stride = 56, 20
        elif b[:2] == b'\x64\x86':
            count = struct.unpack_from('<H', b, 2)[0]
            ptr, num = struct.unpack_from('<II', b, 8)
            start, stride = 20 + struct.unpack_from('<H', b, 16)[0], 18
        else:
            continue  # Rust metadata or bitcode.
        errors, table = [], ptr + num * stride
        for i in range(count):
            off = start + i * 40
            if off + 40 > len(b):
                errors.append('Truncated section header')
                break
            sec = b[off:off + 8].rstrip(b'\0')
            if sec.startswith(b'/'):
                try:
                    idx = int(sec[1:])
                    length = struct.unpack_from('<I', b, table)[0]
                    if (not ptr or not num or length < 4 or table + length > len(b) or
                            idx < 4 or idx >= length or b[table + length - 1] != 0 or
                            b.find(b'\0', table + idx, table + length) < 0):
                        raise ValueError()
                except (ValueError, struct.error):
                    errors.append(f'Section {i + 1}: invalid name {sec!r}')
        if errors:
            problems.append(dict(archive=name, member=nm, offset=offset,
                                 errors=errors[:10],
                                 sha256=hashlib.sha256(data).hexdigest()))
record = dict(members=members, problems=problems)
(out / 'gpu-core-archive-scan.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
print(json.dumps(record, indent=2))
