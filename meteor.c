// Meteor Survival - Game Boy DMG Asteroids (GBDK-2020)
//
// Ship (rotate / thrust / wrap) + bullets + splitting asteroids +
// collisions, waves and lives.
//
// Build:  lcc -o meteor.gb meteor.c
// Needs gfx.h (run gen_gfx.py for placeholder art, or export your own tiles
// with the same layout - see the T_* defines below).
//
// Controls: LEFT/RIGHT rotate, A thrust, B fire, START begins.
//
// Fixed point: 8.8 (high byte = pixels). Positions are uint16_t (160 << 8
// overflows int16_t), velocities are int16_t. Every object's position is its
// CENTRE, which keeps wrapping, collisions and drawing simple.

#include <gb/gb.h>
#include <stdint.h>
#include <stdio.h>
#include <rand.h>
#include <gbdk/font.h>
#include <gbdk/console.h>

#include "gfx.h"

// ------------------------------------------------------------ tile layout
// Sprite tiles live at 128+ so they never collide with the BG font that
// printf() loads at tile 0 upwards.
#define SPR_BASE   128u
#define T_SHIP     ((uint8_t)(SPR_BASE + 0))    // 9 frames: angles 0..8 (0 = up, 8 = right)
#define T_BULLET   ((uint8_t)(SPR_BASE + 9))
#define T_LARGE    ((uint8_t)(SPR_BASE + 10))   // 4 tiles: TL, TR, BL, BR of a 16x16
#define T_MEDIUM   ((uint8_t)(SPR_BASE + 14))
#define T_SMALL    ((uint8_t)(SPR_BASE + 15))

// ------------------------------------------------------- OAM slot budget
// 40 sprites total: 1 ship + 4 bullets + 8 rocks * 4 = 37.
// Each rock owns 4 slots so its sprites never need to be shuffled around;
// medium/small rocks just use the first and hide the rest.
#define MAX_BULLETS  4u
#define MAX_ROCKS    8u
#define OAM_SHIP     0u
#define OAM_BULLET0  1u
#define OAM_ROCK0    (OAM_BULLET0 + MAX_BULLETS)

// --------------------------------------------------------------- tuning
#define SCREEN_W     160u
#define SCREEN_H     144u
#define W_FP         ((uint16_t)(SCREEN_W << 8))
#define H_FP         ((uint16_t)(SCREEN_H << 8))

#define ANGLE_MASK   31u
#define ROT_DELAY    2u
#define MAX_SPEED    0x180
#define BULLET_SPEED 40         // multiplies dir_tab (max 16) -> 640 = 2.5 px/frame
#define BULLET_LIFE  45u        // frames (~110 px of travel)
#define SHIP_RADIUS  3u
#define BULLET_RADIUS 1u
#define INVULN_FRAMES 120u
#define RESPAWN_DELAY 90u
#define START_LIVES  3u

// Wave ramp. Rock COUNT saturates quickly (the pool is MAX_ROCKS), so the
// real difficulty lever is speed: this much 8.8 velocity per wave, added on
// top of the per-size table, capped so late waves stay playable. Large rocks
// start at 0.125-0.25 px/frame and the ship tops out at 1.5, so there is room.
#define WAVE_SPEED_STEP 0x10u   // 0.0625 px/frame per wave
#define ROCK_SPEED_MAX  0xE0u   // ceiling for the summed velocity (8.8)

// hud_dirty bit flags
#define HUD_SCORE    1u
#define HUD_LIVES    2u

// Rock sizes (0 = slot unused)
#define ROCK_NONE    0u
#define ROCK_SMALL   1u
#define ROCK_MEDIUM  2u
#define ROCK_LARGE   3u

//                                   none small medium large
static const uint8_t rock_radius[4]    = { 0,    3,    4,     8 };
static const uint16_t rock_score[4]    = { 0,  100,   50,    20 };
static const uint8_t rock_speed_min[4] = { 0, 0x70, 0x40,  0x20 };   // 8.8 px/frame
static const uint8_t rock_speed_rng[4] = { 1, 0x50, 0x30,  0x20 };

// sin(angle) * 16, angle 0 = up, clockwise, 32 steps. Hand-rounded so + and -
// are exact mirrors (shifting negative numbers right is biased).
//   dx =  DIR_SIN(a)     dy = -DIR_COS(a)     (cos = sin + 90 degrees = +8)
static const int8_t dir_tab[32] = {
      0,   3,   6,   9,  11,  13,  15,  16,
     16,  16,  15,  13,  11,   9,   6,   3,
      0,  -3,  -6,  -9, -11, -13, -15, -16,
    -16, -16, -15, -13, -11,  -9,  -6,  -3
};
#define DIR_SIN(a)   (dir_tab[(a) & ANGLE_MASK])
#define DIR_COS(a)   (dir_tab[((a) + 8u) & ANGLE_MASK])

// ----------------------------------------------------------------- types

typedef struct {
    uint16_t x, y;
    int16_t  vx, vy;
    uint8_t  angle;
    uint8_t  alive;
    uint8_t  invuln;      // frames of spawn protection left
} Ship;

typedef struct {
    uint16_t x, y;
    int16_t  vx, vy;
    uint8_t  life;        // 0 = slot free
} Bullet;

typedef struct {
    uint16_t x, y;
    int16_t  vx, vy;
    uint8_t  size;        // ROCK_NONE = slot free
} Rock;

static Ship   ship;
static Bullet bullets[MAX_BULLETS];
static Rock   rocks[MAX_ROCKS];

static uint8_t  rot_timer, frame, respawn_timer;
static uint8_t  wave, lives, hud_dirty;
static uint16_t score;

// --------------------------------------------------------------- helpers

static uint8_t px(uint16_t v) { return (uint8_t)(v >> 8); }

// Wrap into [0, limit). Underflow lands near 65535 (>= 0xC000) -> add limit;
// overflow lands just past limit -> subtract limit.
static uint16_t wrap(uint16_t p, uint16_t limit) {
    if (p >= limit) {
        if (p >= 0xC000u) p = (uint16_t)(p + limit);
        else              p = (uint16_t)(p - limit);
    }
    return p;
}

// Move a position by a signed 8.8 velocity, with wrapping.
static uint16_t advance(uint16_t p, int16_t v, uint16_t limit) {
    return wrap((uint16_t)(p + (uint16_t)v), limit);
}

static int16_t clamp_speed(int16_t v) {
    if (v >  MAX_SPEED) return  MAX_SPEED;
    if (v < -MAX_SPEED) return -MAX_SPEED;
    return v;
}

// Symmetric drag: at least 1/256 px/frame plus a proportional part, never
// overshoots zero.
static int16_t drag(int16_t v) {
    if (v > 0) return v - (1 + (v >> 6));
    if (v < 0) return v + (1 + ((-v) >> 6));
    return 0;
}

// dir_tab entry (+/-16) * speed (8.8) / 16, sign-magnitude to avoid the
// negative right-shift bias.
static int16_t scale_dir(int8_t d, uint8_t speed) {
    uint8_t m = d < 0 ? (uint8_t)(-d) : (uint8_t)d;
    int16_t v = (int16_t)(((uint16_t)m * speed) >> 4);
    return d < 0 ? -v : v;
}

// Pixel coordinate of an 8.8 lvalue = its high byte (little-endian). Reading
// the byte directly is much cheaper than a function call plus a 16-bit shift.
#define PXH(v)  (((uint8_t *)&(v))[1])

// Wrap-aware |a - b| between two pixel coordinates on an axis of length `size`.
// Pure 8-bit maths: this runs in the collision inner loops, and 16-bit signed
// maths there made the game drop below 60 fps with a few rocks and bullets.
static uint8_t adist(uint8_t a, uint8_t b, uint8_t size) {
    uint8_t d = (a > b) ? (uint8_t)(a - b) : (uint8_t)(b - a);
    if (d > (size >> 1)) d = size - d;
    return d;
}

// Wrap-aware box overlap between two pixel-space centres.
static uint8_t overlap(uint8_t ax, uint8_t ay, uint8_t bx, uint8_t by, uint8_t r) {
    if (adist(ax, bx, SCREEN_W) > r) return 0;
    return adist(ay, by, SCREEN_H) <= r;
}

static void hide(uint8_t oam) { move_sprite(oam, 0, 0); }   // x = 0 is off-screen

// ----------------------------------------------------------------- sound
// Direct-register SFX (no driver needed for effects). Channel plan:
//   CH1 pulse + sweep : laser
//   CH2 pulse         : heartbeat thump (alternating two pitches)
//   CH4 noise         : thrust < rock explosions < ship death  (priority)
// CH3 (wave) is left free for a future jingle / hUGEDriver music.
//
// Writing NRx2 = 0 switches that channel's DAC off, which silences it.

#define PRIO_NONE    0u
#define PRIO_THRUST  1u
#define PRIO_BOOM    2u
#define PRIO_DEATH   3u

static uint8_t noise_prio, noise_timer;     // CH4 arbitration
static uint8_t hb_on, hb_timer, hb_phase;   // heartbeat

static void sfx_init(void) {
    NR52_REG = 0x80;      // sound on
    NR51_REG = 0xFF;      // all channels to both speakers
    NR50_REG = 0x77;      // max master volume
}

static void sfx_silence(void) {
    NR12_REG = 0; NR22_REG = 0; NR42_REG = 0;
    noise_prio = PRIO_NONE; noise_timer = 0;
    hb_on = 0;
}

// Falling "pew": ~2 kHz sweeping down, short decaying envelope.
static void sfx_laser(void) {
    NR10_REG = 0x1D;      // sweep: every 7.8 ms, frequency down, shift 5
    NR11_REG = 0x80;      // 50% duty
    NR12_REG = 0xC1;      // volume 12, decaying (~0.2 s)
    NR13_REG = 0xC0;      // period 0x7C0
    NR14_REG = 0x87;      // trigger
}

// Start a noise burst unless something more important is playing.
static void noise_play(uint8_t prio, uint8_t frames, uint8_t nr42, uint8_t nr43) {
    if (prio < noise_prio) return;
    if (prio == noise_prio && frames < noise_timer) return;   // keep the longer rumble
    noise_prio  = prio;
    noise_timer = frames;
    NR41_REG = 0x00;
    NR42_REG = nr42;      // initial volume + envelope
    NR43_REG = nr43;      // noise pitch (bigger shift = lower rumble)
    NR44_REG = 0x80;      // trigger
}

//                      none small medium large
static const uint8_t boom_frames[4] = {  0,   9,   22,   42 };
static const uint8_t boom_env[4]    = {  0, 0x91, 0xC2, 0xF3 };   // vol/decay
static const uint8_t boom_pitch[4]  = {  0, 0x61, 0x81, 0xA1 };   // ~4 kHz / 1 kHz / 256 Hz

static void sfx_boom(uint8_t rock_size) {
    noise_play(PRIO_BOOM, boom_frames[rock_size], boom_env[rock_size], boom_pitch[rock_size]);
}

static void sfx_death(void) {
    noise_play(PRIO_DEATH, 100u, 0xF7, 0xB1);     // long, slow-decaying low rumble
}

// Thrust is a constant-volume rumble: start on press, cut on release. If an
// explosion steals CH4 meanwhile, it simply restarts after the explosion.
static void sfx_thrust(uint8_t held) {
    if (held) {
        if (noise_prio == PRIO_NONE) {
            noise_prio = PRIO_THRUST;
            NR41_REG = 0x00;
            NR42_REG = 0x40;   // volume 4, no envelope
            NR43_REG = 0x91;
            NR44_REG = 0x80;
        }
    } else if (noise_prio == PRIO_THRUST) {
        NR42_REG = 0;
        noise_prio = PRIO_NONE;
    }
}

// Per-frame: expire CH4 priority, and drive the classic two-tone heartbeat,
// which speeds up as the rock count falls.
static void sfx_frame(uint8_t rocks) {
    if (noise_timer && --noise_timer == 0) noise_prio = PRIO_NONE;

    if (!hb_on) return;
    if (hb_timer) { hb_timer--; return; }
    hb_timer = 14u + rocks * 6u;                  // 20 frames (1 rock) .. 62 (8 rocks)
    NR21_REG = 0x80;
    NR22_REG = 0x81;                              // volume 8, short decay
    hb_phase ^= 1u;
    if (hb_phase) { NR23_REG = 0x58; NR24_REG = 0x83; }   // ~110 Hz
    else          { NR23_REG = 0x7F; NR24_REG = 0x82; }   // ~93 Hz
}

// ------------------------------------------------------------------ ship

static void ship_spawn(void) {
    ship.x = (uint16_t)((SCREEN_W / 2u) << 8);
    ship.y = (uint16_t)((SCREEN_H / 2u) << 8);
    ship.vx = ship.vy = 0;
    ship.angle = 0;
    ship.alive = 1;
    ship.invuln = INVULN_FRAMES;
    rot_timer = 0;
}

static void ship_update(uint8_t keys) {
    if (keys & (J_LEFT | J_RIGHT)) {
        if (rot_timer == 0) {
            if (keys & J_RIGHT) ship.angle = (ship.angle + 1u) & ANGLE_MASK;
            else                ship.angle = (ship.angle - 1u) & ANGLE_MASK;
            rot_timer = ROT_DELAY;
        } else {
            rot_timer--;
        }
    } else {
        rot_timer = 0;
    }

    if ((keys & J_A) && (frame & 1u)) {              // thrust every 2nd frame
        ship.vx = clamp_speed(ship.vx + DIR_SIN(ship.angle));
        ship.vy = clamp_speed(ship.vy - DIR_COS(ship.angle));
    }
    if ((frame & 3u) == 0) {                         // drag every 4th frame
        ship.vx = drag(ship.vx);
        ship.vy = drag(ship.vy);
    }

    ship.x = advance(ship.x, ship.vx, W_FP);
    ship.y = advance(ship.y, ship.vy, H_FP);
    if (ship.invuln) ship.invuln--;
}

// Only angles 0..8 are stored; the rest are flips (see ship.c notes).
static void ship_draw(void) {
    if (!ship.alive || (ship.invuln && (ship.invuln & 4u))) {   // blink while invulnerable
        hide(OAM_SHIP);
        return;
    }
    uint8_t a = ship.angle, tile, prop;
    if (a <= 8u)        { tile = a;       prop = 0; }
    else if (a <= 16u)  { tile = 16u - a; prop = S_FLIPY; }
    else if (a <= 24u)  { tile = a - 16u; prop = S_FLIPX | S_FLIPY; }
    else                { tile = 32u - a; prop = S_FLIPX; }

    set_sprite_tile(OAM_SHIP, T_SHIP + tile);
    set_sprite_prop(OAM_SHIP, prop);
    // centre -> OAM: 8x8 sprite, so -4, then the (8,16) hardware offset
    move_sprite(OAM_SHIP, px(ship.x) + 4u, px(ship.y) + 12u);
}

// --------------------------------------------------------------- bullets

static void fire_bullet(void) {
    for (uint8_t i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &bullets[i];
        if (b->life) continue;
        int8_t sx = DIR_SIN(ship.angle);
        int8_t sy = -DIR_COS(ship.angle);
        // spawn at the nose (4 px out: dir * 64 in 8.8)
        b->x  = wrap((uint16_t)(ship.x + (uint16_t)(sx * 64)), W_FP);
        b->y  = wrap((uint16_t)(ship.y + (uint16_t)(sy * 64)), H_FP);
        b->vx = (int16_t)sx * BULLET_SPEED;
        b->vy = (int16_t)sy * BULLET_SPEED;
        b->life = BULLET_LIFE;
        sfx_laser();
        return;
    }
}

static void bullets_update(void) {
    for (uint8_t i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &bullets[i];
        if (!b->life) continue;
        b->x = advance(b->x, b->vx, W_FP);
        b->y = advance(b->y, b->vy, H_FP);
        b->life--;
    }
}

static void bullets_draw(void) {
    for (uint8_t i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].life)
            move_sprite(OAM_BULLET0 + i, px(bullets[i].x) + 4u, px(bullets[i].y) + 12u);
        else
            hide(OAM_BULLET0 + i);
    }
}

// ----------------------------------------------------------------- rocks

static uint8_t rock_free_slot(void) {
    for (uint8_t i = 0; i < MAX_ROCKS; i++)
        if (rocks[i].size == ROCK_NONE) return i;
    return 0xFF;
}

static uint8_t rocks_alive(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_ROCKS; i++)
        if (rocks[i].size != ROCK_NONE) n++;
    return n;
}

static void rock_randomize_velocity(Rock *r) {
    uint8_t a  = rand() & ANGLE_MASK;
    // size table picks the base speed, the wave adds the ramp
    uint16_t sp = (uint16_t)rock_speed_min[r->size] +
                  (rand() % rock_speed_rng[r->size]) +
                  (uint16_t)(wave - 1u) * WAVE_SPEED_STEP;
    if (sp > ROCK_SPEED_MAX) sp = ROCK_SPEED_MAX;
    r->vx = scale_dir(DIR_SIN(a), (uint8_t)sp);
    r->vy = scale_dir(DIR_COS(a), (uint8_t)sp);
}

static void rock_spawn(uint16_t x, uint16_t y, uint8_t size) {
    uint8_t i = rock_free_slot();
    if (i == 0xFF) return;                       // pool full: skip this child
    rocks[i].x = x;
    rocks[i].y = y;
    rocks[i].size = size;
    rock_randomize_velocity(&rocks[i]);
}

// Hit a rock: score it, then split into two of the next size down (if there
// is a free slot for the second one) or remove it if it was already small.
static void rock_hit(uint8_t i) {
    Rock *r = &rocks[i];
    uint8_t size = r->size;
    {   // saturate at 65535 instead of wrapping to 0 (the HUD only has 5 digit cells)
        uint16_t s = score + rock_score[size];
        score = (s < score) ? 0xFFFFu : s;
    }
    hud_dirty |= HUD_SCORE;
    sfx_boom(size);

    if (size == ROCK_SMALL) {
        r->size = ROCK_NONE;
        return;
    }
    r->size = size - 1u;
    rock_randomize_velocity(r);
    rock_spawn(r->x, r->y, size - 1u);           // the second fragment
}

static void rocks_update(void) {
    for (uint8_t i = 0; i < MAX_ROCKS; i++) {
        Rock *r = &rocks[i];
        if (r->size == ROCK_NONE) continue;
        r->x = advance(r->x, r->vx, W_FP);
        r->y = advance(r->y, r->vy, H_FP);
    }
}

static void rocks_draw(void) {
    for (uint8_t i = 0; i < MAX_ROCKS; i++) {
        uint8_t o = OAM_ROCK0 + (i << 2);
        Rock *r = &rocks[i];
        uint8_t cx = px(r->x), cy = px(r->y);

        if (r->size == ROCK_LARGE) {             // 2x2 tiles, centre-anchored
            set_sprite_tile(o,      T_LARGE);
            set_sprite_tile(o + 1u, T_LARGE + 1u);
            set_sprite_tile(o + 2u, T_LARGE + 2u);
            set_sprite_tile(o + 3u, T_LARGE + 3u);
            move_sprite(o,      cx,       cy + 8u);    // (cx-8)+8, (cy-8)+16
            move_sprite(o + 1u, cx + 8u,  cy + 8u);
            move_sprite(o + 2u, cx,       cy + 16u);
            move_sprite(o + 3u, cx + 8u,  cy + 16u);
        } else if (r->size != ROCK_NONE) {       // 1 tile, other 3 slots hidden
            set_sprite_tile(o, r->size == ROCK_MEDIUM ? T_MEDIUM : T_SMALL);
            move_sprite(o, cx + 4u, cy + 12u);
            hide(o + 1u); hide(o + 2u); hide(o + 3u);
        } else {
            hide(o); hide(o + 1u); hide(o + 2u); hide(o + 3u);
        }
    }
}

// Spawn `n` large rocks away from the ship.
static void spawn_wave(uint8_t n) {
    for (uint8_t k = 0; k < n; k++) {
        uint16_t x = 0, y = 0;
        for (uint8_t tries = 0; tries < 20u; tries++) {
            x = (uint16_t)(rand() % SCREEN_W) << 8;
            y = (uint16_t)(rand() % SCREEN_H) << 8;
            if (adist(px(x), PXH(ship.x), SCREEN_W) > 40 ||
                adist(px(y), PXH(ship.y), SCREEN_H) > 40) break;
        }
        rock_spawn(x, y, ROCK_LARGE);
    }
}

// ------------------------------------------------------------ collisions

static void collide_bullets_rocks(void) {
    for (uint8_t b = 0; b < MAX_BULLETS; b++) {
        if (!bullets[b].life) continue;
        uint8_t bx = PXH(bullets[b].x), by = PXH(bullets[b].y);
        for (uint8_t i = 0; i < MAX_ROCKS; i++) {
            if (rocks[i].size == ROCK_NONE) continue;
            if (overlap(bx, by, PXH(rocks[i].x), PXH(rocks[i].y),
                        rock_radius[rocks[i].size] + BULLET_RADIUS)) {
                bullets[b].life = 0;
                rock_hit(i);
                break;                           // one bullet, one rock
            }
        }
    }
}

static void collide_ship_rocks(void) {
    if (!ship.alive || ship.invuln) return;
    uint8_t sx = PXH(ship.x), sy = PXH(ship.y);
    for (uint8_t i = 0; i < MAX_ROCKS; i++) {
        if (rocks[i].size == ROCK_NONE) continue;
        if (overlap(sx, sy, PXH(rocks[i].x), PXH(rocks[i].y),
                    rock_radius[rocks[i].size] + SHIP_RADIUS)) {
            ship.alive = 0;
            if (lives) lives--;
            respawn_timer = RESPAWN_DELAY;
            hud_dirty |= HUD_LIVES;
            sfx_death();
            return;
        }
    }
}

// Is the centre of the screen clear enough to respawn?
static uint8_t centre_clear(void) {
    for (uint8_t i = 0; i < MAX_ROCKS; i++) {
        if (rocks[i].size == ROCK_NONE) continue;
        if (overlap(PXH(rocks[i].x), PXH(rocks[i].y), SCREEN_W / 2u, SCREEN_H / 2u,
                    rock_radius[rocks[i].size] + 24u))
            return 0;
    }
    return 1;
}

// ------------------------------------------------------------------- HUD

// Labels are drawn once; only the changed number is reprinted. printf() is
// slow, and redrawing the whole line on every hit cost a full frame each time.
// Layout: "SCORE nnnnn LIVES n"  (score only ever grows, so it never needs clearing)
static void hud_labels(void) {
    gotoxy(0, 0);  printf("SCORE");
    gotoxy(12, 0); printf("LIVES");
    hud_dirty = HUD_SCORE | HUD_LIVES;
}

static void hud_draw(void) {
    if (hud_dirty & HUD_SCORE) { gotoxy(6, 0);  printf("%u", score); }
    if (hud_dirty & HUD_LIVES) { gotoxy(18, 0); printf("%u", (uint16_t)lives); }
    hud_dirty = 0;
}

// ------------------------------------------------------------ game flow

static void title_screen(void) {
    for (uint8_t i = 0; i < 40u; i++) hide(i);
    sfx_silence();
    cls();
    gotoxy(2, 6);  printf("METEOR SURVIVAL");
    gotoxy(4, 9);  printf("PRESS START");

    // Seed the RNG from how long the player takes to press START.
    uint16_t seed = 0;
    while (!(joypad() & J_START)) { seed++; vsync(); }
    initrand(seed ^ ((uint16_t)DIV_REG << 8));
    waitpadup();
    cls();
}

static void play(void) {
    uint8_t prev = 0, over_timer = 0;

    for (uint8_t i = 0; i < MAX_BULLETS; i++) bullets[i].life = 0;
    for (uint8_t i = 0; i < MAX_ROCKS;   i++) rocks[i].size = ROCK_NONE;
    score = 0; wave = 1; lives = START_LIVES; frame = 0;
    hb_on = 1; hb_timer = 0; hb_phase = 0;

    ship_spawn();
    spawn_wave(2u + wave);
    hud_labels();
    hud_draw();

    while (1) {
        vsync();
        frame++;
        uint8_t keys = joypad();

        if (ship.alive) {
            ship_update(keys);
            if ((keys & J_B) && !(prev & J_B)) fire_bullet();   // fire on press, not hold
        } else if (lives == 0) {
            if (++over_timer == 1u) { gotoxy(5, 8); printf("GAME OVER"); }
            if (over_timer > 180u) return;
        } else if (respawn_timer) {
            respawn_timer--;
        } else if (centre_clear()) {
            ship_spawn();
        }
        prev = keys;
        sfx_thrust(ship.alive && (keys & J_A));
        hb_on = (lives != 0);

        bullets_update();
        rocks_update();
        collide_bullets_rocks();
        collide_ship_rocks();

        uint8_t n = rocks_alive();
        if (n == 0) {                             // wave cleared
            if (wave < 255u) wave++;              // never wraps back to an easy wave
            uint16_t want = 2u + wave;            // 3, 4, 5, ... up to the pool size
            if (want > MAX_ROCKS) want = MAX_ROCKS;
            spawn_wave((uint8_t)want);
            n = rocks_alive();
        }
        sfx_frame(n);

        ship_draw();
        bullets_draw();
        rocks_draw();
        if (hud_dirty) hud_draw();
    }
}

void main(void) {
    OBP0_REG = 0xE4;
    BGP_REG  = 0xE4;

    sfx_init();
    font_init();
    font_set(font_load(font_min));

    set_sprite_data(SPR_BASE,      9, ship_tiles);   // ship frames
    set_sprite_data(SPR_BASE + 9u, 7, misc_tiles);   // bullet, rocks
    for (uint8_t i = 0; i < MAX_BULLETS; i++)
        set_sprite_tile(OAM_BULLET0 + i, T_BULLET);  // bullet tile never changes

    SHOW_BKG;
    SHOW_SPRITES;
    DISPLAY_ON;

    while (1) {
        title_screen();
        play();
    }
}
