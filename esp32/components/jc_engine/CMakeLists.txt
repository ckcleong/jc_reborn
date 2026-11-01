# The jc_engine component: compiles the shared engine sources from the
# repository root for the ESP32 build. The root is also the include
# directory (every source includes its headers unqualified).
#
# IMPORTANT: flags must be TARGET-scoped.
# idf_build_set_property(COMPILE_OPTIONS ... APPEND) propagates to every
# IDF component (including C++ ones, where -Werror turns a stray
# -std=gnu99 into a hard error).

set(JC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

idf_component_register(
    SRCS
        "${JC_ROOT}/utils.c"
        "${JC_ROOT}/uncompress.c"
        "${JC_ROOT}/resource.c"
        "${JC_ROOT}/resmgr.c"
        "${JC_ROOT}/dump.c"
        "${JC_ROOT}/story.c"
        "${JC_ROOT}/walk.c"
        "${JC_ROOT}/calcpath.c"
        "${JC_ROOT}/ads.c"
        "${JC_ROOT}/ttm.c"
        "${JC_ROOT}/island.c"
        "${JC_ROOT}/bench.c"
        "${JC_ROOT}/graphics.c"
        "${JC_ROOT}/config.c"
    INCLUDE_DIRS ${JC_ROOT}

    REQUIRES esp_timer
)

# PC-only SDL glue, excluded from the engine component (it is replaced
# by the ESP32 platform layer):
#   events.c      — SDL event loop + pacing (GPIO replacement lives in
#                   main/platform_glue.c)
#   sound.c       — SDL audio + WAV loading (esp_driver_i2s replacement
#                   lives in main/platform_glue.c)
#   graphics_pc.c — SDL window / grPushFrame PC implementation
#   jc_reborn.c   — PC argv entry point (esp32/main/main.c is the ESP32
#                   entry point)

# The engine is C99 like the PC build (root Makefile: -std=c99). -O2
# comes from CONFIG_COMPILER_OPTIMIZATION_PERF (sdkconfig.defaults).
# -DMEMTRACK matches the PC memstats build (make memtrack); the [MEM]
# resident/peak checkpoints are the device-vs-PC comparison target.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu99
    -Wall
    -DMEMTRACK
)

# Memory routing: heap allocations are routed by
# CONFIG_SPIRAM_USE_MALLOC; grPanel (230 400 B static) is
# placed into the external-RAM BSS section via EXT_RAM_BSS_ATTR +
# CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY (see graphics.c).
