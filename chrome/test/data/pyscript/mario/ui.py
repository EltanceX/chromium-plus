# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Predeclared native controls; no HTML injection or JavaScript bridge."""

lessons = (
    ('键盘事件', '让键盘成为你的手柄',
     "def key(event):\n    if event.code == 'KeyD':\n        input.right = True\n\ndom.add_event_listener(\n    dom.get_document(), 'keydown', key)",
     'keydown 记录按下，keyup 记录松开。主循环读取输入状态，让角色连续移动。',
     '按 D 再松开，切到运行状态，观察 vx 的加速与归零。'),
    ('变量与对象', '把角色装进一个对象',
     'player = Record(\n    x=96, y=460, vx=0, vy=0,\n    onGround=True)\nplayer.x += player.vx * dt',
     '属性把位置、速度和落地状态放在一起。Python 的 True / False 表示布尔值。',
     '跳一次，观察 vy 为什么先为负数，落地后又变为零。'),
    ('条件与逻辑', '什么时候，才可以起跳？',
     'if (player.jumpBuffer > 0\n        and player.coyote > 0):\n    player.vy = -settings.jump\n    player.onGround = False',
     'and 要求两个条件同时成立。跳跃缓冲与落地宽限让输入更宽容，负速度代表向上。',
     '空中再次按跳跃，观察为什么不会无限飞起。'),
    ('列表与循环', '用数据创造一排金币',
     'for i in range(count):\n    coins.append(Record(\n        x=x+i*spacing, y=y,\n        collected=False))',
     '列表保存多个金币对象，for 重复创建数据。碰撞与绘图分别读取同一份状态。',
     '在 state.py 的 createLevel() 中，将第一排金币数量 4 改为 8，再重新生成页面。'),
    ('函数与碰撞', '碰到了吗？交给一个函数',
     'def overlaps(a, b):\n    return (a.x < b.x+b.w\n        and a.x+a.w > b.x\n        and a.y < b.y+b.h\n        and a.y+a.h > b.y)',
     '两个矩形在水平和垂直方向同时重叠就发生碰撞。return 返回布尔值。',
     '开启碰撞框和慢动作，观察金币、小怪与角色的矩形。'),
    ('DOM 与输入', '让滑块改变整个世界',
     "def speed_input(event):\n    settings.speed = float(event.value)\n    dom.set_text(speed_label, event.value)\n\ndom.add_event_listener(\n    speed_range, 'input', speed_input)",
     'dom 是 Python 与原生页面之间的桥。事件提供控件快照，显式函数更新值、文字及显示状态。',
     '降低重力、提高跳跃，再点击恢复默认，比较角色手感。'),
    ('动画与时间', '一帧一帧，世界动起来',
     'player.vy += settings.gravity * dt\nplayer.y += player.vy * dt\n\n# 申请下一次原生回调\nanimation.request_frame(frame)',
     '速度乘时间得到位移。固定 1/120 秒步长；过载时有界丢弃积压，绘图构建与提交分帧进行。',
     '开启慢动作，观察游戏时间同步变慢；暂停后位置和倒计时都会冻结。'),
    ('成绩与平台能力', '先记录成绩，再连接平台',
     'game.best = max(\n    game.best, game.score)\n\ntry:\n    value = int(text)\nexcept ValueError:\n    value = 0',
     '当前最高分保存在文档内，刷新后重置。跨刷新存储、声音、全屏和源码导出将在阶段六接入。',
     '阅读 winGame() 的计分规则。不要把文档内变量误认为持久化存储。'))

uiNodes, uiQueue, uiApplied = {}, {}, {}
activeTab, lessonNumber = 0, 0
eventMessages = []
overlayState = None
toastDeadline = 0
uiLastHud, uiLastFrames = -1000, 0
pendingFocus, pendingScroll = None, None


def node(name):
    if name not in uiNodes:
        uiNodes[name] = dom.get_element_by_id(name)
    return uiNodes[name]


def queueUI(name, operation, value):
    key = (name, operation)
    if uiApplied.get(key) != value:
        uiQueue[key] = value
    else:
        uiQueue.pop(key, None)


def applyUI(key, value):
    name, operation = key
    handle = node(name)
    if operation == 'text': dom.set_text(handle, value)
    elif operation == 'hidden': dom.set_hidden(handle, value)
    elif operation == 'disabled': dom.set_disabled(handle, value)
    elif operation == 'progress': dom.set_progress(handle, value)
    elif operation == 'aria': dom.set_attribute(handle, 'aria-valuenow', str(value))
    elif operation == 'selected': dom.set_attribute(handle, 'aria-selected', str(value).lower())
    elif operation == 'tabindex': dom.set_tab_index(handle, value)
    elif operation == 'class': dom.toggle_class(handle, 'selected', value)
    uiApplied[key] = value


def flushUI(started, limit=2):
    global pendingFocus, pendingScroll
    if time.perf_counter()-started >= 0.004: return
    # Let native rendering settle the preceding frame's writes before an
    # operation that can synchronously request layout. One such call per frame.
    if not uiQueue:
        if pendingScroll:
            target, pendingScroll = pendingScroll, None
            dom.scroll_into_view(node(target))
            return
        if pendingFocus:
            target, pendingFocus = pendingFocus, None
            dom.focus(node(target))
            return
    count = 0
    while uiQueue and count < limit and time.perf_counter()-started < 0.004:
        key = next(iter(uiQueue))
        value = uiQueue.pop(key)
        applyUI(key, value)
        count += 1


def logEvent(text):
    eventMessages.insert(0, text[:160])
    del eventMessages[3:]
    for i in range(3):
        queueUI('event'+str(i), 'text', '› '+eventMessages[i] if i < len(eventMessages) else '')


def showToast(text):
    global toastDeadline
    toastDeadline = time.perf_counter()+3
    queueUI('toast', 'text', text)
    queueUI('toast', 'hidden', False)
    queueUI('announcement', 'text', text)


def selectLesson(index):
    global lessonNumber
    lessonNumber = index % len(lessons)
    name, title, code, copy, task = lessons[lessonNumber]
    for key, text in (('lessonIndex', '%02d / 08' % (lessonNumber+1)),
                      ('lessonTitle', title), ('lessonCode', code),
                      ('explainTitle', name), ('explainCopy', copy), ('exerciseText', task)):
        queueUI(key, 'text', text)
    for i, lesson in enumerate((0, 2, 1, 6)):
        queueUI('flow'+str(i), 'class', lesson == lessonNumber)


def selectTab(index, focus=False):
    global activeTab, pendingFocus
    resetInput()
    activeTab = index % 3
    for i, name in enumerate(('Experiment', 'State', 'Explain')):
        selected = i == activeTab
        queueUI('tab'+name, 'selected', selected)
        queueUI('tab'+name, 'tabindex', 0 if selected else -1)
        queueUI('panel'+name, 'hidden', not selected)
    pendingFocus = 'tab'+('Experiment', 'State', 'Explain')[activeTab] if focus else None
    queueHud()


def tabKey(index):
    def receive(event):
        if event.code not in ('ArrowLeft', 'ArrowRight', 'Home', 'End'): return
        event.prevent_default()
        event.stop_propagation()
        nextTab = 0 if event.code == 'Home' else 2 if event.code == 'End' else activeTab+(-1 if event.code == 'ArrowLeft' else 1)
        selectTab(nextTab, True)
    return receive


def changeParameter(name, committed=False):
    def receive(event):
        if event.value_truncated: return
        value = float(event.value)
        setattr(settings, name, value)
        dom.set_text(node(name+'Value'), str(int(value)))
        if committed: logEvent('settings.%s = %d' % (name, value))
    return receive


def changeToggle(name):
    def receive(event):
        setattr(settings, name, event.checked)
        invalidateDrawing()
        logEvent('settings.%s = %s' % (name, event.checked))
    return receive


def resetParameters(event):
    for name, value in (('speed', 280), ('jump', 660), ('gravity', 1600)):
        setattr(settings, name, value)
        dom.set_value(node(name+'Range'), str(value))
        dom.set_text(node(name+'Value'), str(value))
    settings.hitboxes = settings.slow = False
    dom.set_checked(node('hitboxToggle'), False)
    dom.set_checked(node('slowToggle'), False)
    invalidateDrawing()
    logEvent('settings → 恢复默认')
    showToast('实验参数已恢复默认。')


def invalidateDrawing():
    global lastRenderedState, pendingFrame
    lastRenderedState, pendingFrame = None, None


def resizeUI(page):
    global WIDTH, screen
    width = 790 if page.width <= 600 else 1120
    if width != WIDTH:
        WIDTH = width
        screen = canvas.resize(screen, WIDTH, HEIGHT)
        invalidateDrawing()
        clock.reset()


def openDialog(name):
    global pendingFocus, pendingScroll
    pendingFocus = pendingScroll = None
    resetInput()
    if game.state == 'playing': togglePause()
    clock.reset()
    syncOverlay()
    dom.show_modal(node(name))


def closeDialog(name):
    dom.close_dialog(node(name))
    resetInput()
    clock.reset()


def syncOverlay():
    global overlayState
    state = (game.state, game.state == 'won' and game.finishTime >= 1.15)
    if state == overlayState: return
    overlayState = state
    hidden = game.state in ('playing', 'dying') or (game.state == 'won' and not state[1])
    queueUI('gameOverlay', 'hidden', hidden)
    queueUI('pause', 'disabled', game.state not in ('playing', 'paused'))
    queueUI('pauseText', 'text', '继续' if game.state == 'paused' else '暂停')
    title, copy, button = {
        'ready': ('每一次起跳，都是一次探索。', '越过水管，收集金币，踩扁小怪。抵达绿色旗杆即可通关。', '开始冒险'),
        'paused': ('世界在等你，随时继续。', '进度已保留。可以先调整实验参数，再继续探索。', '继续冒险'),
        'over': ('每次尝试，都离终点更近。', '本次分数 %d，金币 %d。试试慢动作或增大跳跃力度。' % (game.score, game.coins), '再试一次'),
        'won': ('冒险完成，创造才刚开始。', '分数 %d，金币 %d，用时 %d 秒。文档内最佳 %d。' % (game.score, game.coins, math.ceil(game.elapsed), game.best), '再玩一局')
    }.get(game.state, ('', '', '开始冒险'))
    if not hidden:
        for name, text in (('overlayTag', game.state.upper()), ('overlayTitle', title),
                           ('overlayCopy', copy), ('startButtonText', button),
                           ('overlayTip', 'Space 开始 / 继续 · R 重开'), ('announcement', title)):
            queueUI(name, 'text', text)
    logEvent('game.state → '+game.state)


def queueHud():
    progress = max(0, min(100, round((player.x-96)/(FLAG_X-96)*100)))
    for name, text in (('coinCount', '%02d' % game.coins), ('lifeCount', str(game.lives)),
                       ('timeCount', '%03d' % math.ceil(game.remaining)), ('progressText', str(progress)+'%')):
        queueUI(name, 'text', text)
    queueUI('progressFill', 'progress', progress)
    queueUI('progressTrack', 'aria', progress)
    if activeTab == 1:
        for name, text in (('dataX', str(round(player.x))), ('dataY', str(round(player.y))),
                           ('dataVX', str(round(player.vx))), ('dataVY', str(round(player.vy))),
                           ('stateValue', game.state), ('groundValue', 'onGround: '+str(player.onGround))):
            queueUI(name, 'text', text)


def uiTick(timestamp):
    global toastDeadline, uiLastHud, uiLastFrames
    syncOverlay()
    if timestamp-uiLastHud >= 250:
        queueHud()
        if uiLastHud >= 0:
            queueUI('fpsValue', 'text', str(round((frameCount-uiLastFrames)*1000/(timestamp-uiLastHud))))
        uiLastHud, uiLastFrames = timestamp, frameCount
    if toastDeadline and time.perf_counter() >= toastDeadline:
        queueUI('toast', 'hidden', True)
        toastDeadline = 0


def focusChanged(event):
    # A held button must also release when focus moves back to the canvas.
    resetInput()


def initUI():
    global uiReady
    def listen(name, event, fn): dom.add_event_listener(node(name), event, fn)
    for name in ('speed', 'jump', 'gravity'):
        listen(name+'Range', 'input', changeParameter(name))
        listen(name+'Range', 'change', changeParameter(name, True))
    for element, name in (('hitboxToggle', 'hitboxes'), ('slowToggle', 'slow')):
        listen(element, 'change', changeToggle(name))
    listen('resetParams', 'click', resetParameters)
    for i, name in enumerate(('Experiment', 'State', 'Explain')):
        listen('tab'+name, 'click', lambda event, i=i: selectTab(i))
        listen('tab'+name, 'keydown', tabKey(i))
    listen('previousLesson', 'click', lambda event: selectLesson(lessonNumber-1))
    listen('nextLesson', 'click', lambda event: selectLesson(lessonNumber+1))
    for i, lesson in enumerate((0, 2, 1, 6)):
        listen('flow'+str(i), 'click', lambda event, lesson=lesson: chooseLesson(lesson))
    for i in range(8): listen('map'+str(i), 'click', lambda event, i=i: chooseLesson(i, True))
    listen('guideButton', 'click', lambda event: openDialog('guideDialog'))
    listen('keysButton', 'click', lambda event: openDialog('keysDialog'))
    listen('guideClose', 'click', lambda event: closeDialog('guideDialog'))
    listen('keysClose', 'click', lambda event: closeDialog('keysDialog'))
    listen('playNav', 'click', lambda event: playNav())
    listen('overlayStart', 'click', overlayStart)
    for name in ('guideDialog', 'keysDialog'):
        for event in ('close', 'cancel'): listen(name, event, lambda event: resetInput())
    dom.add_event_listener(dom.get_document(), 'focusin', focusChanged)
    resizeUI(dom.get_page_state())
    selectLesson(0)
    uiReady = True
    syncOverlay()


def chooseLesson(index, close=False):
    global pendingScroll, pendingFocus
    selectLesson(index)
    selectTab(2)
    if close: closeDialog('guideDialog')
    pendingScroll, pendingFocus = 'lab', 'tabExplain'


def playNav():
    resetInput()
    dom.scroll_into_view(node('stage'))
    dom.focus(node('game'))


def overlayStart(event):
    if game.state == 'paused': pause(event)
    else: restart(event)
