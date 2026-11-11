# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Bounded Canvas batches. Seven explicit cache surfaces plus the DOM canvas."""
import canvas
import dom
import animation

screen = canvas.get_context_2d(dom.get_element_by_id('game'))
background = canvas.create_surface(560, 304)
coin_image = canvas.create_surface(22, 28)
sprite = canvas.create_surface(16, 16)
brick_image = canvas.create_surface(48, 48)
question_image = canvas.create_surface(48, 48)
enemy_image = canvas.create_surface(36, 32)
terrain_image = canvas.create_surface(1760, 152)


def rect(b, x, y, w, h, color):
    b.fill_rects(((round(x), round(y), math.ceil(w), math.ceil(h), color),))


def polygon(b, points, color):
    b.fill_style(color)
    b.begin_path()
    b.move_to(*points[0])
    for x, y in points[1:]:
        b.line_to(x, y)
    b.close_path()
    b.fill()


def cacheLandscape():
    b = canvas.batch()
    sky = b.linear_gradient(0, 0, 0, 304)
    for offset, color in ((0, '#92d0df'), (0.65, '#c3e9e3'), (1, '#e8edc8')):
        sky.add_color_stop(offset, color)
    b.fill_style(sky)
    b.fill_rect(0, 0, 560, 304)
    b.fill_style('#fff9d4')
    b.begin_path(); b.arc(453, 53, 16, 0, math.tau); b.fill()
    for x in (-100, 310):
        polygon(b, ((x, 252), (x+100, 160), (x+120, 160), (x+230, 252)), '#a7cda4')
        polygon(b, ((x+50, 252), (x+110, 205), (x+130, 205), (x+200, 252)), '#87b58b')
    for x, y in ((15, 20), (155, 4), (370, 28)):
        rect(b, x, y+10, 58, 12, '#f9fff3')
        rect(b, x+10, y+2, 35, 17, '#f9fff3')
        rect(b, x+20, y-2, 17, 20, '#f9fff3')
    canvas.submit(background, b)
    b = canvas.batch()
    b.fill_rects(((0, 0, 22, 28, '#e8b43f'), (2, 3, 18, 20, '#ffdc6f'),
                  (10, 7, 3, 13, '#e3ac39'), (3, 4, 3, 7, '#fff0a7')))
    canvas.submit(coin_image, b)


def cacheTerrain():
    # The whole 7040 px world fits one quarter-resolution transparent surface.
    # Group by color to avoid style commands for every immutable rectangle.
    layers = {}
    def add(x, y, w, h, color):
        layers.setdefault(color, []).append((x, y, w, h))
    for s in groundSegments:
        add(s.x, GROUND+10, s.w, 94, '#b88b57')
        add(s.x, GROUND, s.w, 11, '#638b4d')
        add(s.x, GROUND, s.w, 4, '#b9cd79')
        add(s.x, GROUND+11, s.w, 9, '#8b703e')
    for p in pipes:
        add(p.x+2, p.y+22, 76, p.h-22, '#456c44')
        add(p.x+6, p.y+22, 68, p.h-22, '#80a856')
        add(p.x+11, p.y+24, 12, p.h-25, '#b3ce75')
        add(p.x-5, p.y, 90, 26, '#436945')
        add(p.x-1, p.y+3, 82, 18, '#8eae5c')
    for block in blocks:
        if block.type == 'stone':
            add(block.x, block.y, 48, 48, '#9e794f')
            add(block.x+3, block.y+3, 42, 42, '#c4a06a')
            add(block.x+3, block.y+3, 42, 4, '#e3c58c')
    add(6792, 359, 150, 145, '#c4ab7f')
    add(6832, 304, 70, 200, '#c4ab7f')
    add(6848, 451, 40, 53, '#615b46')
    for x, height in ((CHECKPOINT_X, 112), (FLAG_X, 300)):
        add(x, GROUND-height, 6, height, '#f6f1c5')
        add(x-12, GROUND-16, 31, 16, '#b89c6b')
    b = canvas.batch()
    b.scale(0.25, 0.25)
    b.fill_rects(tuple((*shape, color) for color, shapes in layers.items() for shape in shapes))
    canvas.submit(terrain_image, b)


def cacheActors():
    pixels = (
        '.....RRRRR......', '....RRRRRRRRR...', '....BBBTTS......', '...BTBTTTTTS....',
        '...BTBBTTTTTS...', '....BTTTTTTT....', '.....RRBRR......', '....RRRBRBRRR...',
        '...RRRRBBBBRRR..', '...TTBRYBBYRBT..', '...TTBBBBBBBTT..', '.....BBBBBB.....',
        '....BBB..BBB....', '...BBB....BBB...', '...DDD....DDD...', '..DDDD....DDDD..')
    colors = dict(R='#e96041', B='#456b83', T='#f3c394', S='#473d35', Y='#f8db82', D='#6d5140')
    b = canvas.batch()
    runs = {color: [] for color in colors}
    for row, line in enumerate(pixels):
        col = 0
        while col < len(line):
            end = col+1
            while end < len(line) and line[end] == line[col]:
                end += 1
            if line[col] != '.':
                runs[line[col]].append((col, row, end-col, 1))
            col = end
    b.fill_rects(tuple((*span, colors[color]) for color, spans in runs.items() for span in spans))
    rect(b, 4, 2, 3, 1, '#674a35'); rect(b, 3, 3, 1, 2, '#674a35')
    rect(b, 5, 4, 1, 1, '#674a35'); rect(b, 9, 5, 4, 1, '#674a35')
    rect(b, 10, 2, 1, 2, '#303d32')
    canvas.submit(sprite, b)
    b = canvas.batch()
    for x, y, w, h, color in ((0, 27, 15, 5, '#624b3a'), (21, 27, 14, 5, '#624b3a'),
                              (9, 20, 17, 9, '#e6cca0'), (5, 7, 25, 17, '#946b48'),
                              (9, 2, 17, 23, '#946b48'), (1, 14, 33, 11, '#946b48'),
                              (10, 3, 14, 4, '#b58b5d'), (6, 13, 8, 10, '#fff1d2'),
                              (22, 13, 8, 10, '#fff1d2'), (11, 16, 3, 6, '#3f4031'),
                              (22, 16, 3, 6, '#3f4031'), (5, 11, 10, 3, '#493f32'),
                              (20, 11, 11, 3, '#493f32')):
        rect(b, x, y, w, h, color)
    canvas.submit(enemy_image, b)


def cacheBlocks():
    b = canvas.batch()
    for x, y, w, h, color in ((0, 0, 48, 48, '#956243'), (2, 2, 44, 44, '#ce8c5b'),
                              (3, 3, 42, 3, '#e8ad76'), (1, 22, 46, 3, '#915e3e'),
                              (23, 2, 3, 20, '#915e3e'), (10, 25, 3, 21, '#915e3e'),
                              (36, 25, 3, 21, '#915e3e')):
        rect(b, x, y, w, h, color)
    canvas.submit(brick_image, b)
    b = canvas.batch()
    rect(b, 0, 0, 48, 48, '#a26732'); rect(b, 3, 3, 42, 39, '#e9b75b')
    rect(b, 4, 3, 39, 4, '#ffda83'); rect(b, 41, 7, 4, 35, '#ce9143')
    b.font(32, 'monospace', True); b.text_align('center')
    b.fill_style('#fff1b0'); b.fill_text('?', 23, 34)
    canvas.submit(question_image, b)


def drawFlag(b, x, checkpoint):
    height = 112 if checkpoint else 300
    y = GROUND-height
    if not checkpoint and game.state == 'won':
        y += min(225, game.finishTime*165)
    color = ('#ee9d55' if game.checkpoint else '#c1b383') if checkpoint else '#6c9b68'
    rect(b, x+6, y+9, 60, 36, color)


def draw():
    b = canvas.batch()
    b.smoothing(False)
    b.draw_surface(background, 0, 0, WIDTH, HEIGHT)
    b.save()
    b.translate(-round(game.camera)+(math.sin(visualTime*100)*shake*25 if shake else 0), 0)
    left, right = game.camera-64, game.camera+WIDTH+64
    b.draw_surface(terrain_image, 0, 0, WORLD_WIDTH, HEIGHT)
    for block in blocks:
        if block.type != 'stone' and left < block.x < right:
            y = block.y-math.sin(block.bump/0.18*math.pi)*9
            if block.type == 'brick':
                b.draw_surface(brick_image, block.x, y, 48, 48)
            elif block.type == 'question' and not block.used:
                b.draw_surface(question_image, block.x, y, 48, 48)
            else:
                rect(b, block.x, y, 48, 48, '#9e794f')
                rect(b, block.x+3, y+3, 42, 42, '#c4a06a')
                rect(b, block.x+3, y+3, 42, 4, '#e3c58c')
    for coin in coins:
        if not coin.collected and left < coin.x < right:
            width = 17 if reducedMotion else 8+abs(math.cos(visualTime*3.5+coin.x*0.01))*13
            y = coin.y+(0 if reducedMotion else math.sin(visualTime*3+coin.x*0.03)*3)
            b.draw_surface(coin_image, coin.x+11-width/2, y, width, 28)
    for enemy in enemies:
        if left < enemy.x < right and (enemy.alive or enemy.squish > 0):
            h = 32 if enemy.alive else 10
            b.draw_surface(enemy_image, enemy.x, enemy.y+32-h, 36, h)
    if game.state == 'dying' or player.invincible <= 0 or int(player.invincible*12) % 2:
        b.save(); b.translate(round(player.x+14), round(player.y-4))
        b.scale(player.facing, 1); b.draw_surface(sprite, -24, 0, 48, 48); b.restore()
    b.alpha(0.8)
    layers = {}
    for p in particles[::max(1, math.ceil(len(particles)/24))]:
        if left < p.x < right:
            layers.setdefault(p.color, []).append(p)
    for color, group in layers.items():
        b.fill_style(color)
        for p in group:
            b.fill_rect(p.x, p.y, p.size, p.size)
    b.alpha(1); b.font(14, 'monospace', True); b.text_align('center')
    for t in floatTexts:
        if left < t.x < right:
            b.alpha(min(1, t.life*3)); b.fill_style(t.color); b.fill_text(t.text, t.x, t.y)
    b.alpha(1)
    for x, checkpoint in ((CHECKPOINT_X, True), (FLAG_X, False)):
        if left-80 < x < right:
            drawFlag(b, x, checkpoint)
    b.restore()
    b.font(18, 'monospace', True); b.fill_style('#355a4a')
    b.fill_text('SCORE %06d  COINS %02d  LIVES %d  TIME %03d' %
                (game.score, game.coins, game.lives, math.ceil(game.remaining)), 24, 30)
    if game.state in ('ready', 'paused', 'over', 'won'):
        b.fill_style('#193629bb'); b.fill_rect(280, 180, 560, 135)
        b.fill_style('#fff7d2'); b.text_align('center'); b.font(28, 'monospace', True)
        b.fill_text(game.state.upper(), WIDTH/2, 228)
        b.font(16, 'monospace'); b.fill_text('Space: start / resume    R: restart', WIDTH/2, 275)
    return b
