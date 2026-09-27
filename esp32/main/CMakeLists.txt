# Metal Gear Solid on the ESP32-S3.
#
# Mirrors port/build.sh, which is the reference for what compiles and why. The
# flags below are not decoration -- each one is load-bearing:
#
#   -std=gnu17    GCC 14 defaults to gnu23, which rejects PSY-Q-era constructs
#   -mlongcalls   Xtensa call8 reaches +/-512 KB; this image is several MB
#   include order the game's own dirs MUST precede PSY-Z's: both ship common.h

set(REPO ${CMAKE_CURRENT_LIST_DIR}/../..)
set(SOTN "E:/Hardware/sotn/sotn-decomp" CACHE PATH "sotn-decomp checkout (PSY-Z)")
set(PSYZ ${SOTN}/tools/psyz/psyz)
set(PSYQ ${SOTN}/tools/psyz/decomp)
set(SHIM "C:/Users/David/.claude/skills/retro-port-triage/scripts/shim"
    CACHE PATH "PSY-Q compatibility headers")

# Exactly one stage may be linked: each of the 93 files in source/stage/ defines
# its own _StageCharacterEntries[], because the PSX keeps one stage resident.
set(MGS_STAGE "s07a" CACHE STRING "stage to link statically")

# Where the game drops you. Any name from Mgs_PlayableStages in
# port/stage_union.c works, provided the microSD carries the full STAGE.DIR.
#   s00a  the dock, where MGS actually begins (underwater, no enemies)
#   s01a  the heliport -- patrolling guards, snow, the first real encounter
#   s02a  the tank hangar -- guards and a large interior
#   s07a  the single room the earliest builds shipped with
#   title the game's own boot chain -- CRASHES: the title screen runs the
#         memory-card flow ("this memcard is OK" is the last line before the
#         panic) and that subsystem is not implemented. It is also the path
#         that would play the swimming intro and the opening codec, both of
#         which need the CD streaming that is declined. Restore once memcard
#         and streaming exist.
set(MGS_START_STAGE "s00a" CACHE STRING "stage to boot into")

# Spin every character in place so the rig can be judged from all sides. A
# debug view, not a feature: set to 0 for normal play.
set(MGS_TURNTABLE 0 CACHE STRING "rotate characters in place for inspection")

# ---- the game -------------------------------------------------------------
file(GLOB_RECURSE SRC_GAME ${REPO}/source/*.c)
list(FILTER SRC_GAME EXCLUDE REGEX "/overlays/")
list(FILTER SRC_GAME EXCLUDE REGEX "/contrib/")
list(FILTER SRC_GAME EXCLUDE REGEX "/stagevr/")
list(FILTER SRC_GAME EXCLUDE REGEX "/snake_vr/")   # VR-disc variant of Snake
list(FILTER SRC_GAME EXCLUDE REGEX "/stage/")
list(APPEND SRC_GAME
    ${REPO}/source/stage/_stage.c
    ${REPO}/port/stage_union.c)   # one merged actor table for all packed stages

# ---- the overlays that are already 100% C ---------------------------------
foreach(ovl s07a s11e s11i d18ar s08br s19br)
    file(GLOB OVL_SRC ${REPO}/source/overlays/${ovl}/*.c)
    list(APPEND SRC_OVERLAYS ${OVL_SRC})
endforeach()

# ---- PSY-Z, minus the pieces MGS supplies itself --------------------------
file(GLOB SRC_PSYZ ${PSYZ}/src/psyz/*.c)
list(FILTER SRC_PSYZ EXCLUDE REGEX "libgte_hw_sqrt")
list(FILTER SRC_PSYZ EXCLUDE REGEX "libsn")   # MGS has PC* in libfs/select.c

# ---- the PSY-Q decomp libraries -------------------------------------------
# Most of that tree is still INCLUDE_ASM stubs, so a glob does not compile.
# Reuse the curated list the SOTN port already proved builds, and add the few
# extra libspu/libpress translation units MGS needs on top of it.
include(${SOTN}/esp32/main/psyq_sources.cmake)
set(SRC_PSYQ ${GAME_SOURCE_FILES_PSYQ})
foreach(extra
        libspu/s_ini.c libspu/s_crwa.c libspu/s_gva.c
        libspu/s_i.c   libspu/s_ih.c   libspu/s_snv.c
        libpress/dec.c)
    if(EXISTS ${PSYQ}/src/${extra})
        list(APPEND SRC_PSYQ ${PSYQ}/src/${extra})
    endif()
endforeach()
list(REMOVE_DUPLICATES SRC_PSYQ)

# ---- the port layer -------------------------------------------------------
set(SRC_PORT
    ${REPO}/port/psyz_port.c
    ${REPO}/port/psyq_compat.c
    ${REPO}/port/soft_render.c
    ${REPO}/port/platform_headless.c
    ${REPO}/port/stage_stubs.c
    ${REPO}/port/esp32_threads.c
    ${REPO}/port/esp32_vblank.c
    ${REPO}/port/virtual_cd.c)

idf_component_register(
    SRCS app_main.c lcd_esp32.c esp_input.c esp_sd.c ${SRC_GAME} ${SRC_OVERLAYS} ${SRC_PORT} ${SRC_PSYZ} ${SRC_PSYQ}
    INCLUDE_DIRS
        .
        ${REPO}/source            # MUST come before PSY-Z: both have common.h
        ${REPO}/source/include
        ${SHIM}
        ${PSYZ}/include
        ${PSYZ}/src
        ${SOTN}/include
    # esp_adc: the handheld reads its button ladder and analog stick through
    # ADC1. Harmless on the Waveshare build, which never calls it.
    REQUIRES esp_timer fatfs spi_flash esp_lcd driver sdmmc esp_adc)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    INTEGRAL __psyz VERSION_PC NON_MATCHING PERMUTER HARD_LINK
    _internal_version_us
    MGS_START_STAGE="${MGS_START_STAGE}"
    $<$<BOOL:${MGS_TURNTABLE}>:MGS_TURNTABLE>
    # Which physical machine this image is for. Set by build.ps1; main/board_pins.h
    # picks a whole pin map from it rather than sprinkling ifdefs.
    $<$<STREQUAL:${MGS_BOARD},XIAO>:MGS_BOARD_XIAO>)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu17          # gnu23 (GCC 14's default) rejects this code
    -mlongcalls         # call8 cannot reach across a multi-MB image
    -w
    # This is 1998 PSY-Q C and it type-puns everywhere -- *(int*)&pack->r0, the
    # whole scratchpad reached through a struct cast, LCOPY2. On MIPS with the
    # compilers of the day that was simply how you wrote it. GCC 14 at -O2
    # assumes it does NOT happen, and silently deletes stores it believes
    # nothing can read: that is how the translation vanished out of CompMatrix
    # and detached every limb from every character on screen, with no warning
    # and no crash. Turn the assumption off for the game rather than hunt the
    # next one -- it costs a little scheduling freedom in code that is bound by
    # PSRAM latency anyway.
    -fno-strict-aliasing
    # PSY-Q code mixes int/long and pointer types freely: on MIPS they were the
    # same width. Same set the SOTN port uses.
    -Wno-error
    -Wno-incompatible-pointer-types
    -Wno-int-conversion
    -Wno-implicit-function-declaration
    -Wno-builtin-declaration-mismatch
    -Wno-return-mismatch
    # SHELL: keeps CMake from de-duplicating the repeated -include flag and
    # collapsing these into a single "-include string.h gtemac.h"
    "SHELL:-include string.h"
    "SHELL:-include gtemac.h"
    # Telemetry that drops instead of blocking when no terminal is attached.
    # Inert on the Waveshare build; see port/mgs_printf.h for why the handheld
    # cannot run without it.
    "SHELL:-include ${REPO}/port/mgs_printf.h")

# the game's main() becomes callable from app_main
set_source_files_properties(${REPO}/source/main/main.c PROPERTIES
    COMPILE_DEFINITIONS "main=mgs_main")

