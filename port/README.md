# MGS -> psyz portable layer (phase 0)

> **This document describes phase 0 and is out of date.** For the current
> status of the port -- what works, what is missing, how to build and flash, and
> the bugs already fixed with their root cause -- see **[STATUS.md](STATUS.md)**.

Shim that lets the MGS engine (libgv/libdg/libgcl/libhzd/libfs/main,
58 files) compile against the psyz headers instead of the PsyQ SDK. Measured:
58/58 compile with xtensa-esp32s3-elf-gcc.

Flags:
  -DINTEGRAL -D__psyz -Isource -Isource/include -Iport/include -I<psyz>/include
  -include string.h -include stdlib.h -include gtemac.h

Patches applied to the tree (5 root causes, all mechanical):
  libdg/libdg.h    enum DG_CHANL -> DG_CHANL_UNIT  (collides with the struct of
                   the same name; GCC 2.x tolerated it, GCC 14 does not)
  libdg/loader.c   12 casts-as-lvalue `(char*)p += n` -> `p = (typeof p)(...)`
  mts/mts.h        PsyQ's fprintf(int,...) renamed to mts_fprintf
  port/include/gtemac.h  getScratchAddr (1KiB scratchpad), gte_NormalClip,
                   gte_CompMatrix, ApplyMatrixLV

PENDING (not shim work, it is porting work): psyz uses a "fat" 8-byte ordering
table (tag+len) instead of the PSX's packed 4-byte word. The ~168 sites
that declare `u_long *ot` must move to OT_TYPE*. Do NOT cast: it changes the size.
