#!/usr/bin/env python3
"""Step the wave ramp in a headless PyBoy and print what it actually spawns.

WHY THIS EXISTS: meteor.c cannot be compiled on the host (it needs gb/gb.h),
so `make test`-style host checks are not available. This drives the real ROM
instead and reads the game's own statics out of WRAM.

Requires the -debug ROM, whose .noi carries the static addresses:
    make sym
    python3 tools/probe_ramp.py meteor.gb

The addresses below are read from meteor.noi (GDK names them
Fmeteor$<name>$0_0$0). Re-run `make sym` and re-grep if the layout changes;
verify them against the struct sizes -- rocks is 8 x 9 bytes, ship is 11,
bullets is 4 x 9, and each is followed immediately by the next.
"""
import sys

from pyboy import PyBoy

ROCKS = 0xC0E0          # Rock[8]: x u16, y u16, vx i16, vy i16, size u8
WAVE = 0xC12B
SHIP_INVULN = 0xC0BB    # ship.alive is 0xC0BA, .invuln the byte after
ROCK_STRIDE = 9
MAX_ROCKS = 8


def u16(m, a):
    return m[a] | (m[a + 1] << 8)


def i16(m, a):
    v = u16(m, a)
    return v - 0x10000 if v & 0x8000 else v


def rocks(m):
    """[(size, speed_magnitude)] for every live rock slot."""
    out = []
    for i in range(MAX_ROCKS):
        a = ROCKS + i * ROCK_STRIDE
        if m[a + 8]:
            vx, vy = i16(m, a + 4), i16(m, a + 6)
            out.append((m[a + 8], round((vx * vx + vy * vy) ** 0.5)))
    return out


def clear_rocks(m):
    for i in range(MAX_ROCKS):
        m[ROCKS + i * ROCK_STRIDE + 8] = 0


def main():
    rom = sys.argv[1] if len(sys.argv) > 1 else "meteor.gb"
    pyboy = PyBoy(rom, window="null", sound_emulated=False)
    m = pyboy.memory

    pyboy.tick(120, True)                     # boot to the title screen
    m[SHIP_INVULN] = 255                      # do not die while we poke at it
    # button() (press+release in one call) is what the game actually sees;
    # button_press/tick/button_release leaves it on the title screen.
    pyboy.button("start")
    pyboy.tick(30, True)
    assert m[WAVE] == 1, "START did not begin play (wave=%d)" % m[WAVE]

    print("wave  rocks  speed (8.8)      px/frame")
    seen = []
    for _ in range(10):
        m[SHIP_INVULN] = 255                  # ship_update decrements it each frame
        w = m[WAVE]
        rs = rocks(m)
        sizes = sorted(s for s, _ in rs)
        sp = sorted(v for _, v in rs)
        seen.append((w, len(rs), sp[0] if sp else 0, sp[-1] if sp else 0))
        print("%4d  %5d  %4d..%-4d        %.3f..%.3f"
              % (w, len(rs), sp[0] if sp else 0, sp[-1] if sp else 0,
                 (sp[0] if sp else 0) / 256, (sp[-1] if sp else 0) / 256))
        assert all(s == 3 for s in sizes), "expected only large rocks, got %s" % sizes
        clear_rocks(m)
        pyboy.tick(6, True)                   # let play() see the clear and respawn

    pyboy.stop(save=False)

    for (w0, n0, _, _), (w1, n1, _, _) in zip(seen, seen[1:]):
        assert w1 > w0, "wave did not advance: %d -> %d" % (w0, w1)
        assert n1 >= n0, "rock count fell: wave %d %d -> wave %d %d" % (w0, n0, w1, n1)
    assert seen[0][1] == 3, "wave 1 should be 3 rocks, got %d" % seen[0][1]
    assert max(n for _, n, _, _ in seen) == MAX_ROCKS, "count never reached the pool ceiling"

    # The ramp must actually ramp. A rock's speed is
    #     size_floor + rand(size_range) + WAVE_SPEED_STEP*(wave-1)
    # and the random term is bounded by the widest range in rock_speed_rng
    # (0x50). So the SLOWEST rock in a late wave must beat the FASTEST rock in
    # wave 1 by more than that whole range. With the ramp working the gap is
    # far larger; with WAVE_SPEED_STEP at 0 it is impossible, not merely
    # unlikely -- which is what stops this passing by luck.
    NOISE = 0x50
    assert seen[-1][2] > seen[0][3] + NOISE, (
        "speed floor %d did not clear the wave-1 noise (%d + %d): the ramp is "
        "not ramping" % (seen[-1][2], seen[0][3], NOISE))

    print("OK: ramp rises, wave 1 unchanged at 3 rocks, count caps at %d" % MAX_ROCKS)


if __name__ == "__main__":
    main()
