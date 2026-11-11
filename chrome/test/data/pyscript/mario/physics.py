# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Original default physics, scoring, lives and state transitions."""


def overlaps(a, b):
    return (a.x < b.x+b.w and a.x+a.w > b.x and
            a.y < b.y+b.h and a.y+a.h > b.y)


def hitBlock(block):
    if block.type == 'stone':
        return
    block.bump = 0.18
    if block.type == 'question' and not block.used:
        block.used = True
        game.coins += 1
        game.score += 200
        burst(block.x+24, block.y-12, '#f8d458', 8)
        addText(block.x+24, block.y-15, '+200', '#fff7d2')


def loseLife(reason):
    global shake, message
    if game.state != 'playing':
        return
    game.lives -= 1
    game.deaths += 1
    game.state, game.deathTime = 'dying', 0
    player.vy, player.vx, player.onGround = -440, 0, False
    shake = 0 if reducedMotion else 0.22
    clearInput()
    message = reason


def winGame():
    global message
    game.state, game.finishTime = 'won', 0
    game.score += math.ceil(game.remaining)*10
    game.best = max(game.best, game.score)
    clearInput()
    message = 'Finished! Space to play again.'


def updateEnemies(dt):
    for enemy in enemies:
        if not enemy.alive:
            enemy.squish = max(0, enemy.squish-dt)
            continue
        nextX = enemy.x+enemy.vx*dt
        edgeX = nextX+enemy.w+2 if enemy.vx > 0 else nextX-2
        groundAhead, blocked = False, False
        for s in groundSegments:
            if s.x <= edgeX <= s.x+s.w:
                groundAhead = True
                break
        for obstacle in enemyObstacles:
            if nextX+enemy.w > obstacle.x and nextX < obstacle.x+obstacle.w:
                blocked = True
                break
        if not groundAhead or blocked or nextX < 0:
            enemy.vx *= -1
        else:
            enemy.x = nextX


def updatePlayer(dt):
    direction = int(input.right)-int(input.left)
    targetSpeed = direction*settings.speed
    acceleration = 2200 if player.onGround else 1600
    if player.vx < targetSpeed:
        player.vx = min(targetSpeed, player.vx+acceleration*dt)
    if player.vx > targetSpeed:
        player.vx = max(targetSpeed, player.vx-acceleration*dt)
    if direction:
        player.facing = direction
    player.coyote = 0.10 if player.onGround else max(0, player.coyote-dt)
    player.jumpBuffer = max(0, player.jumpBuffer-dt)
    player.invincible = max(0, player.invincible-dt)
    if player.jumpBuffer > 0 and player.coyote > 0:
        player.vy, player.onGround, player.coyote, player.jumpBuffer = -settings.jump, False, 0, 0
        burst(player.x+player.w/2, player.y+player.h, '#ece7c0', 5)
    gravity = settings.gravity
    if not input.jump and player.vy < 0:
        gravity *= 2.3
    player.vy = min(player.vy+gravity*dt, 1100)
    player.x = max(2, min(WORLD_WIDTH-player.w, player.x+player.vx*dt))
    # Only rectangles that could overlap this fixed step; retain original order.
    nearby = [s for s in solids if s.x < player.x+player.w+12 and s.x+s.w > player.x-12]
    for solid in nearby:
        if overlaps(player, solid):
            if player.vx > 0:
                player.x = solid.x-player.w
            elif player.vx < 0:
                player.x = solid.x+solid.w
            player.vx = 0
    previousBottom = player.y+player.h
    player.y += player.vy*dt
    player.onGround = False
    for solid in nearby:
        if overlaps(player, solid):
            if player.vy > 0:
                player.y, player.onGround = solid.y-player.h, True
            elif player.vy < 0:
                player.y = solid.y+solid.h
                if hasattr(solid, 'type'):
                    hitBlock(solid)
            player.vy = 0
    for coin in coins:
        if (not coin.collected and coin.x < player.x+player.w
                and coin.x+coin.w > player.x and overlaps(player, coin)):
            coin.collected = True
            game.coins += 1
            game.score += 100
            burst(coin.x+11, coin.y+14, '#ffde6b', 6)
            addText(coin.x+11, coin.y, '+100', '#fff6c5')
    for enemy in enemies:
        if (not enemy.alive or enemy.x >= player.x+player.w
                or enemy.x+enemy.w <= player.x or not overlaps(player, enemy)):
            continue
        if player.vy > 0 and previousBottom <= enemy.y+14:
            enemy.alive, enemy.squish = False, 0.35
            player.vy = -490 if input.jump else -340
            player.onGround = False
            game.score += 300
            burst(enemy.x+17, enemy.y+20, '#e3bc80', 7)
            addText(enemy.x+17, enemy.y-12, '+300', '#fff8da')
        elif player.invincible <= 0:
            loseLife('Enemy hit. Land on its head!')
            return
    if player.y > HEIGHT+80:
        loseLife('Fell into a pit. Hold jump to cross it.')
        return
    if not game.checkpoint and player.x >= CHECKPOINT_X:
        game.checkpoint = True
        game.score += 500
        burst(CHECKPOINT_X, GROUND-75, '#ffd58a', 20)
    if player.x+player.w >= FLAG_X:
        winGame()


def updateEffects(dt):
    for p in particles:
        p.x += p.vx*dt
        p.y += p.vy*dt
        p.vy += 440*dt
        p.life -= dt
    particles[:] = [p for p in particles if p.life > 0]
    for t in floatTexts:
        t.y -= 38*dt
        t.life -= dt
    floatTexts[:] = [t for t in floatTexts if t.life > 0]


def update(dt):
    global shake
    if game.state == 'playing':
        game.elapsed += dt
        game.remaining = max(0, game.remaining-dt)
        updateEnemies(dt)
        updatePlayer(dt)
        if game.remaining <= 0 and game.state == 'playing':
            loseLife('Time up!')
    elif game.state == 'dying':
        game.deathTime += dt
        player.vy += 1300*dt
        player.y += player.vy*dt
        if game.deathTime > 1.1:
            if game.lives > 0:
                game.state = 'playing'
                if game.remaining <= 0:
                    game.remaining = 180
                resetPlayer()
            else:
                game.state = 'over'
    elif game.state == 'won':
        game.finishTime += dt
        if player.x < FLAG_X+140:
            player.x += 85*dt
        player.y, player.onGround, player.vx = GROUND-player.h, True, 85
        if game.finishTime < 2.8 and rng.random() < dt*10:
            burst(FLAG_X+rng.random()*240-80, 130+rng.random()*180,
                  ('#ffd568', '#f58e62', '#fff9d5', '#8cbf83')[int(rng.random()*4)], 12)
    if game.state in ('playing', 'won'):
        target = max(0, min(WORLD_WIDTH-WIDTH, player.x-WIDTH*0.32))
        game.camera += (target-game.camera)*min(1, dt*6)
    if game.state != 'paused':
        for block in blocks:
            block.bump = max(0, block.bump-dt)
        updateEffects(dt)
        shake = max(0, shake-dt)


class FixedClock:
    """Never catch up a hidden epoch. At most six 120 Hz steps per frame."""
    def __init__(self, work_quota_ms=None, work_clock=time.perf_counter):
        self.previous, self.accumulator, self.dropped = None, 0, 0
        self.work_quota_ms, self.work_clock = work_quota_ms, work_clock

    def reset(self):
        self.previous, self.accumulator = None, 0

    def advance(self, timestamp):
        global visualTime
        raw = 0 if self.previous is None else max(0, (timestamp-self.previous)/1000)
        self.previous = timestamp
        dt = min(MAX_DT, raw)
        self.dropped += max(0, raw-dt)
        if game.state in ('ready', 'paused', 'over'):
            self.accumulator = 0
            return raw, 0
        visualTime += dt
        self.accumulator += dt
        limit = min(MAX_STEPS, int((self.accumulator+1e-12)/STEP))
        count = 0
        started = self.work_clock() if limit and self.work_quota_ms is not None else 0
        while count < limit:
            update(STEP)
            count += 1
            if (self.work_quota_ms is not None
                    and (self.work_clock()-started)*1000 >= self.work_quota_ms):
                break
        self.accumulator = max(0, self.accumulator-count*STEP)
        if self.accumulator >= STEP:
            self.dropped += self.accumulator
            self.accumulator = 0
        return raw, count
