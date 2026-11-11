# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Mario core: port of C:/VD/Projects/mario/index.html (see README.md)."""
import math
import time

WIDTH, HEIGHT, GROUND, WORLD_WIDTH = 1120, 608, 504, 7040
FLAG_X, CHECKPOINT_X, STEP = 6576, 3360, 1 / 120
MAX_STEPS, MAX_DT = 6, 0.05
DEFAULT_SEED = 0x4D415249


class Record:
    def __init__(self, **values):
        self.__dict__.update(values)


class ReplayRandom:
    """xorshift32 (13,17,5); unsigned seed, zero maps to DEFAULT_SEED.

    Game effects only. Not cryptographic; never used for interpreter hash seeds.
    """
    def __init__(self, seed=DEFAULT_SEED):
        self.state = (seed & 0xFFFFFFFF) or DEFAULT_SEED

    def random(self):
        x = self.state
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.state = x & 0xFFFFFFFF
        return self.state / 4294967296


settings = Record(speed=280, jump=660, gravity=1600)
input = Record(left=False, right=False, jump=False)
player = Record(x=96, y=460, w=28, h=44, vx=0, vy=0, facing=1,
                onGround=True, coyote=0, jumpBuffer=0, invincible=0)
game = Record(state='ready', score=0, coins=0, lives=3, remaining=180,
              camera=0, checkpoint=False, elapsed=0, deathTime=0,
              finishTime=0, best=0, deaths=0)
groundSegments = [Record(x=x, w=w, y=GROUND, h=HEIGHT+200, type='ground')
                  for x, w in ((0, 1632), (1776, 1152), (3072, 1296),
                               (4512, 1248), (5904, 1136))]
pipes = [Record(x=x, y=y, w=80, h=GROUND-y)
         for x, y in ((1104, 408), (2496, 384), (3792, 408), (5184, 384))]
blocks, coins, enemies, particles, floatTexts, solids = [], [], [], [], [], []
enemyObstacles = []
shake, visualTime = 0, 0
reducedMotion = False
rng = ReplayRandom()
message = 'Space / Enter to start'


def clearInput():
    input.left = input.right = input.jump = False
    player.jumpBuffer = 0


def createLevel():
    global blocks, coins, enemies, particles, floatTexts, solids, enemyObstacles
    blocks, coins, enemies, particles, floatTexts = [], [], [], [], []
    rows = ((432, 376, 'bqb'), (816, 328, 'q'), (1920, 376, 'bqqb'),
            (2160, 232, 'bqb'), (3168, 376, 'bqb'), (3504, 280, 'qq'),
            (4656, 376, 'qbq'), (4944, 280, 'bqb'), (6096, 376, 'qb'))
    for x, y, row in rows:
        for j, kind in enumerate(row):
            blocks.append(Record(x=x+j*48, y=y, w=48, h=48,
                                 type='question' if kind == 'q' else 'brick',
                                 used=False, bump=0))
    for column in range(5):
        for row in range(column+1):
            blocks.append(Record(x=6240+column*48, y=GROUND-(row+1)*48,
                                 w=48, h=48, type='stone', used=False, bump=0))
    for x, y, count, spacing in ((644, 416, 4, 40), (1584, 352, 6, 42),
                               (1950, 302, 3, 48), (2175, 165, 3, 40),
                               (2860, 350, 7, 40), (3200, 310, 3, 40),
                               (3530, 216, 2, 48), (4300, 352, 7, 40),
                               (4680, 307, 3, 40), (5710, 354, 7, 40),
                               (6150, 307, 2, 40)):
        for i in range(count):
            coins.append(Record(x=x+i*spacing, y=y, w=22, h=28, collected=False))
    for i, x in enumerate((760, 1360, 2080, 2340, 3470, 4060, 4870, 5480, 6040)):
        enemies.append(Record(x=x, y=GROUND-32, w=34, h=32,
                              vx=-55 if i % 2 == 0 else 55, alive=True, squish=0))
    # Geometry is immutable; bump affects drawing, not collision coordinates.
    solids = groundSegments + pipes + blocks
    enemyObstacles = pipes + [b for b in blocks if b.y+b.h >= GROUND]


def resetPlayer():
    player.x = CHECKPOINT_X+24 if game.checkpoint else 96
    player.y, player.vx, player.vy = GROUND-player.h, 0, 0
    player.onGround, player.coyote, player.jumpBuffer = True, 0, 0
    player.invincible, player.facing = 2.2, 1
    game.camera = max(0, player.x-WIDTH*0.32)
    clearInput()
    for enemy in enemies:
        if abs(enemy.x-player.x) < 160:
            enemy.x = player.x+220


def startGame(seed=DEFAULT_SEED):
    global rng, shake, message
    rng = ReplayRandom(seed)
    createLevel()
    game.state, game.score, game.coins, game.lives = 'playing', 0, 0, 3
    game.remaining, game.elapsed, game.checkpoint = 180, 0, False
    game.deaths, game.finishTime, shake = 0, 0, 0
    resetPlayer()
    player.invincible = 0
    message = 'Go right! Hold jump for a higher jump.'


def pressJump():
    if game.state in ('ready', 'over', 'won'):
        startGame()
    elif game.state == 'paused':
        togglePause()
    elif game.state == 'playing':
        input.jump, player.jumpBuffer = True, 0.13


def togglePause():
    if game.state == 'playing':
        game.state = 'paused'
        clearInput()
    elif game.state == 'paused':
        game.state = 'playing'


def burst(x, y, color, count):
    # A finite visual pool; gameplay, scoring and RNG consumption are unaffected.
    for _ in range(count):
        p = Record(x=x, y=y, vx=(rng.random()-0.5)*190,
                   vy=-60-rng.random()*190, life=0.45+rng.random()*0.35,
                   color=color, size=3+rng.random()*4)
        if not reducedMotion and len(particles) < 128:
            particles.append(p)


def addText(x, y, text, color):
    if len(floatTexts) < 32:
        floatTexts.append(Record(x=x, y=y, text=text, color=color, life=0.9))


def snapshot():
    """Serializable deterministic checkpoints, shared by host and browser tests."""
    return dict(game=vars(game).copy(), player=vars(player).copy(),
                blocks=[vars(b).copy() for b in blocks],
                coins=[vars(c).copy() for c in coins],
                enemies=[vars(e).copy() for e in enemies],
                particles=[vars(p).copy() for p in particles],
                floatTexts=[vars(t).copy() for t in floatTexts], rng=rng.state)
