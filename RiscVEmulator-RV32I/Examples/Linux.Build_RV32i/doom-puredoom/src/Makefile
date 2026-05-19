# Single-TU build: doom_linux.c #includes PureDOOM.h with DOOM_IMPLEMENTATION,
# so everything compiles in one cc invocation. PureDOOM.h is large (~48k LOC)
# so -O0 vs -Os matters a lot for boot time; the package recipe sets -Os.
CC      ?= cc
# -fsigned-char is LOAD-BEARING: RISC-V GCC defaults `char` to unsigned,
# but PureDOOM's ticcmd struct uses bare `char forwardmove / sidemove`
# fields and assigns negative ints (backward / strafe-left) into them.
# Without -fsigned-char those negatives wrap to large positives, and
# pressing S or A flings the player FORWARD / RIGHT at ~9x speed
# instead of going backward / strafing left. The bare-metal
# Examples/Doom builds with this same flag for the same reason.
CFLAGS  += -Wall -Wno-unused-parameter -Wno-unused-but-set-variable \
           -Wno-sign-compare -Wno-misleading-indentation \
           -fPIC -fno-strict-aliasing -fsigned-char -DNORMALUNIX -DLINUX
LDFLAGS += -static -Wl,-elf2flt=-r

doom: doom_linux.c PureDOOM.h
	$(CC) $(CFLAGS) -o $@ doom_linux.c $(LDFLAGS) -lm

clean:
	rm -f doom

.PHONY: clean
