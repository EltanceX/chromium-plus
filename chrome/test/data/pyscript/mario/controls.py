# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Native input only; no JavaScript execution or event attributes."""
held, pointers = set(), {}
epoch = dom.get_page_state().input_epoch
LEFT, RIGHT, JUMP = ('ArrowLeft', 'KeyA'), ('ArrowRight', 'KeyD'), ('Space', 'ArrowUp', 'KeyW')


def syncInput():
    active = set(pointers.values())
    input.left = bool(held.intersection(LEFT)) or 'left' in active
    input.right = bool(held.intersection(RIGHT)) or 'right' in active
    input.jump = bool(held.intersection(JUMP)) or 'jump' in active


def resetInput():
    held.clear()
    pointers.clear()
    clearInput()


def key(event):
    if event.code not in LEFT+RIGHT+JUMP+('KeyP', 'Escape', 'KeyR', 'Enter'):
        return
    if event.type == 'keyup':
        held.discard(event.code)
        syncInput()
        return
    if (event.target_is_editable or event.target_in_dialog or event.is_composing
            or event.alt_key or event.ctrl_key or event.meta_key):
        return
    event.prevent_default()
    held.add(event.code)
    syncInput()
    if event.repeat:
        return
    if event.code in JUMP:
        pressJump()
    elif event.code == 'Enter' and game.state in ('ready', 'over', 'won'):
        restart(None)
    elif event.code in ('KeyP', 'Escape'):
        togglePause()
        resetInput()
        clock.reset()
    elif event.code == 'KeyR':
        restart(None)


def pointer(control):
    def receive(event):
        if event.type == 'pointerdown':
            if event.button != 0:
                return
            event.prevent_default()
            dom.set_pointer_capture(event.current_target, event.pointer_id)
            pointers[event.pointer_id] = control
            syncInput()
            if control == 'jump':
                pressJump()
        else:
            pointers.pop(event.pointer_id, None)
            syncInput()
    return receive


def pageChanged(event):
    global epoch, reducedMotion
    page = dom.get_page_state()
    reducedMotion = page.reduced_motion
    if page.input_epoch != epoch or page.hidden or not page.has_focus:
        resetInput()
        clock.reset()
        if game.state == 'playing':
            togglePause()
    epoch = page.input_epoch


def restart(event):
    resetInput()
    startGame()
    clock.reset()


def pause(event):
    togglePause()
    resetInput()
    clock.reset()


def installControls():
    document, window = dom.get_document(), dom.get_window()
    for name in ('keydown', 'keyup'):
        dom.add_event_listener(document, name, key)
    dom.add_event_listener(document, 'visibilitychange', pageChanged)
    for name in ('blur', 'focus', 'resize'):
        dom.add_event_listener(window, name, pageChanged)
    for control in ('left', 'right', 'jump'):
        element = dom.get_element_by_id(control)
        callback = pointer(control)
        for name in ('pointerdown', 'pointerup', 'pointercancel', 'lostpointercapture'):
            dom.add_event_listener(element, name, callback)
    for name, callback in (('start', restart), ('pause', pause)):
        dom.add_event_listener(dom.get_element_by_id(name), 'click', callback)


clock = FixedClock(work_quota_ms=2)
frameCount, lastHud, lastDraw = 0, -1000, -1000
lastRenderedState = None
pendingFrame = None
timings = []
peaks = [0]*6
pendingUI = []
metricsNode, statusNode = dom.get_element_by_id('metrics'), dom.get_element_by_id('status')
telemetryNode = dom.get_element_by_id('telemetry')
stats = dict(commands=0, bytes=0, pixels=0, native_ms=0)


def frame(timestamp):
    global frameCount, lastHud, lastDraw, stats, lastRenderedState, pendingFrame
    started = time.perf_counter()
    pageChanged(None)
    raw, steps = clock.advance(timestamp)
    updated = time.perf_counter()
    built = updated
    # Build and native submission occupy different callbacks. Under CPU load,
    # leave headroom for the unchanged 16 ms limit instead of growing a backlog.
    if pendingFrame is not None and pendingFrame[1] != game.state:
        pendingFrame = None
    if pendingFrame is not None and updated-started < 0.003:
        stats = canvas.submit(screen, pendingFrame[0])
        lastRenderedState = pendingFrame[1]
        pendingFrame = None
    elif (pendingFrame is None and updated-started < 0.003
            and timestamp-lastDraw >= 1000/30-0.1
            and (game.state not in ('ready', 'paused', 'over') or game.state != lastRenderedState)):
        batch = draw()
        built = time.perf_counter()
        pendingFrame = (batch, game.state)
        lastDraw = timestamp
    submitted = time.perf_counter()
    frameCount += 1
    timings.append(((updated-started)*1000, (built-updated)*1000,
                    (submitted-built)*1000, (submitted-started)*1000,
                    raw*1000, steps))
    if len(timings) > 32:
        del timings[0]
    for i in range(6):
        if timings[-1][i] > peaks[i]:
            peaks[i] = timings[-1][i]
    if not pendingUI and timestamp-lastHud >= 250:
        totals = sorted([t[3] for t in timings])
        p95 = totals[int((len(totals)-1)*0.95)]
        maxima = peaks
        statusText = ('%s · 分数 %d · 金币 %d · 生命 %d · 剩余 %d 秒 · %s' %
                     (game.state, game.score, game.coins, game.lives, math.ceil(game.remaining), message))
        telemetryText = ('state:%s|x:%.3f|y:%.3f|vx:%.3f|vy:%.3f|coins:%d|score:%d|lives:%d|checkpoint:%d|frames:%d|elapsed:%.3f|steps:%d|keys:%d|particles:%d' %
                     (game.state, player.x, player.y, player.vx, player.vy, game.coins,
                      game.score, game.lives, game.checkpoint, frameCount, game.elapsed,
                      steps, len(held)+len(pointers), len(particles)))
        metricsText = ('commands:%d bytes:%d pixels:%d native_ms:%.3f | lifetime max physics_ms:%.3f build_ms:%.3f submit_ms:%.3f total_ms:%.3f p95_ms:%.3f gap_ms:%.1f steps:%d dropped_s:%.3f' %
                     (stats['commands'], stats['bytes'], stats['pixels'], stats['native_ms'],
                      maxima[0], maxima[1], maxima[2], maxima[3], p95, maxima[4], maxima[5], clock.dropped))
        pendingUI.extend(((statusNode, statusText), (telemetryNode, telemetryText), (metricsNode, metricsText)))
        lastHud = timestamp
    # One DOM write per callback; full HUD preparation and writes are bounded.
    if pendingUI:
        node, text = pendingUI.pop(0)
        dom.set_text(node, text)
    # Include HUD work in the recorded total; the next HUD reports this sample.
    timings[-1] = (timings[-1][0], timings[-1][1], timings[-1][2],
                   (time.perf_counter()-started)*1000, raw*1000, steps)
    if timings[-1][3] > peaks[3]:
        peaks[3] = timings[-1][3]
    animation.request_frame(frame)
