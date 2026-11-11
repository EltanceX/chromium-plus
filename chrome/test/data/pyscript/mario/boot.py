# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
reducedMotion = dom.get_page_state().reduced_motion
# Prime every source copy in initialization, before the 16 ms frame callbacks.
stats = canvas.submit(screen, draw())
lastRenderedState = game.state
installControls()
animation.request_frame(frame)
