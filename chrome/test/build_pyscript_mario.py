# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Assemble ordered inline Python blocks; no external module loader or exec.

Use --check in validation to reject a stale generated sample.
"""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent / 'data/pyscript/mario'
OUTPUT = ROOT.parent / 'mario.html'
SOURCES = ('state.py', 'physics.py', 'drawing.py', 'controls.py',
           'cacheLandscape()', 'cacheActors()', 'cacheBlocks()',
           'createLevel(); cacheTerrain()', 'boot.py')


def blocks():
  result = []
  for name in SOURCES:
    source = (ROOT / name).read_text(encoding='utf-8') if name.endswith('.py') else name+'\n'
    if len(source.encode('utf-8')) > 65536 or '</pyscript' in source.lower():
      raise ValueError('Invalid inline Python block: '+name)
    compile(source, name, 'exec')
    result.append((name, source))
  return result


def build():
  shell = (ROOT / 'shell.html').read_text(encoding='utf-8')
  scripts = '\n'.join('<!-- '+name+' -->\n<pyscript nonce="python-mario">\n'+source+'</pyscript>'
                      for name, source in blocks())
  return shell.replace('<!-- PYTHON_BLOCKS -->', scripts)


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--check', action='store_true')
  args = parser.parse_args()
  result = build()
  if args.check:
    if not OUTPUT.exists() or OUTPUT.read_text(encoding='utf-8') != result:
      parser.error('mario.html is stale; run chrome/test/build_pyscript_mario.py')
  else:
    OUTPUT.write_text(result, encoding='utf-8', newline='\n')
  print(('Verified ' if args.check else 'Generated ')+str(OUTPUT))


if __name__ == '__main__':
  main()
