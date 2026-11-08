# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Disable native extension loading in the otherwise upstream Windows config."""
from pathlib import Path
import sys

text = Path(sys.argv[1]).read_text(encoding='utf-8')
assert text.count('#define HAVE_DYNAMIC_LOADING') == 1
text = text.replace('#define HAVE_DYNAMIC_LOADING',
                    '/* Native extension loading is disabled by the embedder. */')
target = Path(sys.argv[2])
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text(text, encoding='utf-8')
