# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Verify the imported upstream subset byte-for-byte against its manifest."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent
manifest = json.loads((root / 'source_manifest.json').read_text(encoding='utf-8'))
actual = {p.relative_to(root / 'src').as_posix():
          hashlib.sha256(p.read_bytes()).hexdigest()
          for p in (root / 'src').rglob('*') if p.is_file()}
if actual != manifest:
    raise SystemExit('CPython upstream source differs from source_manifest.json')
print(f'Verified {len(actual)} unmodified upstream files')
