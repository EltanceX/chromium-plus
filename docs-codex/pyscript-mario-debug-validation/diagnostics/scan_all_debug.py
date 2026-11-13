from pathlib import Path
import sys

# Expand direct objects and thin archives before using the read-only scanner.
response = sys.argv[1]
prefix = Path('out/Debug/pyscript-mario-debug-stability/scan_link_inputs.py').read_text().split('problems=[]')[0]
prefix = prefix.replace('blink_core.dll.rsp', response)
scope = {}
exec(compile(prefix, 'collect_link_objects', 'exec'), scope)
scope['all_objects'] = [str(p.relative_to(scope['root'])) for p in sorted(scope['objects'])]
code = Path('out/Debug/pyscript-mario-debug-stability/scan_debug.py').read_text()
code = code.replace("objects=re.findall(r'[^\\s\"]+\\.obj',rsp.read_text());problems=[]", 'objects=all_objects;problems=[]')
code = code.replace("'-debug-scan.json'", "'-all-debug-scan.json'")
code = code.replace("if data+size>len(b):", "if data+size>len(b) and not (data==0 and struct.unpack_from('<I',b,off+36)[0]&0x80):")
code = code.replace("  big=b[:4]==b'\\0\\0\\xff\\xff'", "  if b[:2]!=b'\\x64\\x86' and b[:4]!=b'\\0\\0\\xff\\xff' and b[:4]!=b'BC\\xc0\\xde':\n   problems.append({'object':name,'errors':['unrecognized COFF/bitcode header']});continue\n  if b[:4]==b'BC\\xc0\\xde':continue\n  big=b[:4]==b'\\0\\0\\xff\\xff'")
code = code.replace("   if sec not in (b'.debug$T',b'.debug$S'):continue", "   if sec==b'.drectve' and any(c>126 or (c<32 and c not in (0,9,10,13)) for c in b[data:data+size]):errors.append('invalid non-ASCII linker directive')\n   if sec not in (b'.debug$T',b'.debug$S'):continue")
exec(compile(code, 'scan_all_debug', 'exec'), scope)
