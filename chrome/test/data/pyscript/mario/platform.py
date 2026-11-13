# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Bounded native platform services; failures never change game physics."""
import browser

soundEnabled, soundRunning = False, False
soundQueue, platformPending = [], {}
audioNeedsStop, bestNeedsSave = False, False
platformLastTick, platformLastState = -1000, None
platformActive = True
storageAvailable = True
BEST_KEY = 'marioLabBest'
SOUNDS = {
    'jump': (('square', 220, 620, .15, 0, .035),),
    'coin': (('square', 990, 990, .08, 0, .025),
             ('square', 1320, 1320, .18, .07, .025)),
    'bump': (('triangle', 150, 65, .08, 0, .10),),
    'stomp': (('square', 220, 65, .14, 0, .045),),
    'hurt': (('square', 350, 90, .4, 0, .035),
             ('triangle', 220, 60, .3, .2, .06)),
}
for name, notes in (('start', (523, 659, 784)),
                     ('checkpoint', (523, 659, 784)),
                     ('win', (523, 659, 784, 1046, 784, 1046))):
    SOUNDS[name] = tuple(('triangle', f, f, .18, i*.11, .09)
                         for i, f in enumerate(notes))

def loadBest():
    global storageAvailable
    try:
        text = browser.storage_get(BEST_KEY)
        game.best = min(999999999, max(0, int(text or '0')))
    except (ValueError, TypeError):
        game.best = 0
    except (PermissionError, RuntimeError, ReferenceError):
        storageAvailable = False
        game.best = 0


def persistBest():
    global bestNeedsSave
    bestNeedsSave = True


def playSound(name):
    if soundEnabled and len(soundQueue) < 8:
        soundQueue.append(name)


def resetSound():
    global audioNeedsStop
    soundQueue.clear()
    audioNeedsStop = True


def transitionAudio(resume):
    global soundEnabled, soundRunning
    # One outstanding audio transition; the native queue also has a hard cap.
    if any(action == 'audio' for action in platformPending.values()): return
    try:
        token = browser.audio_resume() if resume else browser.audio_suspend()
        platformPending[token] = 'audio'
    except (PermissionError, RuntimeError, ValueError):
        if resume:
            soundEnabled = soundRunning = False
            soundQueue.clear()
            queueUI('soundButton', 'pressed', False)
            queueUI('soundButton', 'text', '开启音效')
        showToast('音效暂不可用，可再次点击开启。')


def toggleSound(event):
    global soundEnabled, soundRunning
    if any(action == 'audio' for action in platformPending.values()): return
    soundEnabled = not soundEnabled
    resetSound()
    if not soundEnabled:
        browser.audio_stop()
        soundRunning = False
    transitionAudio(soundEnabled)
    queueUI('soundButton', 'pressed', soundEnabled)
    queueUI('soundButton', 'text', '关闭音效' if soundEnabled else '开启音效')
    if soundEnabled: playSound('coin')


def toggleFullscreen(event):
    if 'fullscreen' in platformPending.values(): return
    try:
        token = (browser.exit_fullscreen() if browser.is_fullscreen()
                 else browser.request_fullscreen(node('stage')))
        platformPending[token] = 'fullscreen'
    except (PermissionError, RuntimeError, ValueError):
        showToast('浏览器未允许全屏，仍可继续游玩。')


def exportSource(event):
    try:
        browser.export_document('python-mario-lab.html')
        showToast('源码已提交下载，请查看浏览器下载列表。')
    except (PermissionError, RuntimeError, ValueError):
        showToast('源码下载未获允许，请稍后通过按钮重试。')


def platformPageChanged(page):
    global platformActive
    active = not page.hidden and page.has_focus
    if platformActive != active:
        platformActive = active
        resetSound()
        if soundEnabled: transitionAudio(active and game.state != 'paused')


def platformTick(timestamp, started):
    global soundRunning, soundEnabled, audioNeedsStop, bestNeedsSave
    global storageAvailable, platformLastTick, platformLastState
    # At most one platform action per frame, leaving rendering/UI headroom.
    if time.perf_counter()-started >= .003: return
    if audioNeedsStop:
        audioNeedsStop = False
        browser.audio_stop()
        return
    if bestNeedsSave:
        bestNeedsSave = False
        try:
            browser.storage_set(BEST_KEY, str(game.best))
            storageAvailable = True
        except (PermissionError, RuntimeError, ValueError):
            storageAvailable = False
            showToast('无法保存最高分，本局仍可正常游玩。')
        return
    if platformPending:
        completion = browser.poll()
        if completion:
            token, success, detail = completion
            action = platformPending.pop(token, None)
            if action == 'audio':
                soundRunning = success and browser.audio_state() == 'running'
                if soundEnabled and not success:
                    soundEnabled = False
                    soundQueue.clear()
                    queueUI('soundButton', 'pressed', False)
                    queueUI('soundButton', 'text', '开启音效')
                    showToast('音效未能开启，可再次点击重试。')
            elif action == 'fullscreen' and not success:
                showToast('浏览器未允许全屏操作，可继续在页面内游玩。')
            return
    if game.state != platformLastState:
        platformLastState = game.state
        if game.state == 'paused':
            resetSound()
        return
    desired = soundEnabled and platformActive and game.state != 'paused'
    if soundRunning != desired and 'audio' not in platformPending.values():
        transitionAudio(desired)
        return
    if soundEnabled and soundRunning and platformActive and soundQueue:
        tones = []
        while soundQueue and len(tones)+len(SOUNDS[soundQueue[0]]) <= 16:
            tones.extend(SOUNDS[soundQueue.pop(0)])
        try: browser.play_tones(tones)
        except (RuntimeError, ValueError): pass  # Busy/limited output may drop effects.
        return
    if timestamp-platformLastTick >= 250:
        platformLastTick = timestamp
        fullscreen = browser.is_fullscreen()
        queueUI('fullscreenButton', 'text', '退出全屏' if fullscreen else '全屏游戏')
        queueUI('bestCount', 'text', str(game.best))


loadBest()
