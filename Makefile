# Meteor Survival build.
#   make          build meteor.gb  (GBDK if GBDK_HOME is set, else Docker)
#   make gfx      regenerate gfx.h from gen_gfx.py
#   make probe    headless PyBoy check of the wave ramp (implies sym)
#   make usage    ROM/RAM headroom
#   make image    build the gbdk-dev Docker image used when GBDK_HOME is absent
#   make clean
#
# The gbdk-dev image is shared with the other GB projects; build it from
# ../GB-Protector/Dockerfile (`make -C ../GB-Protector image`) if it is missing.

GBDK_HOME ?= $(HOME)/gbdk

ifneq ($(wildcard $(GBDK_HOME)/bin/lcc),)
  RUN   :=
  LCC   := $(GBDK_HOME)/bin/lcc
  USAGE := $(GBDK_HOME)/bin/romusage
else
  # GBDK is not installed on this host, so run the toolchain out of the image.
  # lcc must be the FULL PATH: it is not on PATH inside gbdk-dev, and a bare
  # `lcc` fails with "executable file not found".
  # -u keeps build artefacts owned by the user rather than root.
  RUN   := docker run --rm -u $(shell id -u):$(shell id -g) -v "$(CURDIR)":/work -w /work gbdk-dev
  LCC   := /opt/gbdk/bin/lcc
  USAGE := /opt/gbdk/bin/romusage
endif

# -Wm-yn : the title in the ROM header, so a flash cart names it
# -Wl-m  : the linker map
# -Wl-j  : NoICE symbols, for emulator debuggers
CFLAGS = -Wm-yn"METEORSURV" -Wl-m -Wl-j

# gfx.h is on the line deliberately: it is GENERATED, and a make that does not
# know that says "nothing to do" after `make gfx`, leaving the OLD ROM in place.
meteor.gb: meteor.c gfx.h
	$(RUN) $(LCC) $(CFLAGS) -o $@ meteor.c

# A `-debug` build: the linker map then carries EVERY symbol, not just the
# globals, which is what emulator debuggers want. Bigger ROM, so not the default.
# Phony, so it always re-links: as a file target make would see meteor.gb already
# newer than its sources and leave the release ROM behind.
sym: meteor.c gfx.h
	$(RUN) $(LCC) $(CFLAGS) -debug -o meteor.gb meteor.c

# PyBoy lives in a venv, not on PATH (and not in this repo), so fall back to
# the one GB-Protector keeps:   make probe PY=python3
PY ?= $(firstword $(wildcard .venv/bin/python ../GB-Protector/.venv/bin/python) python3)

# Headless check of the wave ramp. Needs the -debug ROM, because the probe reads
# the game's own statics (wave, rocks) by address out of meteor.noi -- a release
# build's map has no statics. Dev-only; nothing in the build depends on it.
probe: sym
	$(PY) tools/probe_ramp.py meteor.gb

# Regenerate placeholder art (or replace gfx.h with your own tiles, same layout)
gfx:
	python3 gen_gfx.py

usage: meteor.gb
	$(RUN) $(USAGE) meteor.map -g

image:
	docker build -t gbdk-dev .

clean:
	rm -f meteor.gb *.lst *.map *.sym *.noi *.asm *.ihx *.o *.rel *.adb *.cdb

.PHONY: all sym probe gfx usage image clean

all: meteor.gb
