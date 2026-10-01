# Meteor Survival (Game Boy DMG Asteroids, GBDK-2020)

Controls: LEFT/RIGHT rotate, A thrust, B fire, START begins.

Files
- meteor.c      game source (ship, bullets, asteroids, collisions, waves, lives, SFX)
- gfx.h         placeholder tile data (generated)
- gen_gfx.py    regenerates gfx.h; replace with real art using the same tile layout
- meteor.gb     prebuilt ROM
- Makefile      `make` (needs GBDK-2020; set GBDK_HOME)

Tile layout (sprite tiles are loaded at index 128+ to avoid the BG font):
  0-8 ship frames (angles 0..8, other angles via sprite flips), 9 bullet,
  10-13 large rock (TL,TR,BL,BR), 14 medium rock, 15 small rock.
