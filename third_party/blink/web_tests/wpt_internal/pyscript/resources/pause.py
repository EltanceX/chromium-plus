# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import time


def main(request, response):
    time.sleep(0.1)
    return [(b'Content-Type', b'text/javascript')], b'/* parser pause */'
