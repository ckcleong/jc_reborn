set(JC_ESP32_MAIN ${CMAKE_CURRENT_LIST_DIR})

idf_component_register(
    SRCS "main.c" "platform_glue.c" "setup.c" "qr.c"
    INCLUDE_DIRS "."
    # The engine itself is the jc_engine component
    # (components/jc_engine); graphics_pc.c/events.c/sound.c are the
    # PC SDL half and are not built here — the same functions come
    # from platform_glue.c.
    PRIV_REQUIRES jc_engine esp_timer esp_lcd driver fatfs esp_driver_i2s
                  esp_wifi esp_netif esp_http_server nvs_flash dns_server
)

# Optional replay footage: generated from a PC-captured frames.bin
# (tools/run_baseline.sh output) by replay_gen.py. If present, main will
# replay the captured frames cyclically so the panel output can be A/B'd
# against the PC reference visually / by photo.
set(JC_REPLAY_HEADER "${JC_ESP32_MAIN}/replay_frames.h")
if(EXISTS "${JC_REPLAY_HEADER}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE JC_REPLAY_FRAMES=1)
    message(STATUS "jc_reborn: replay footage found, embedding")
endif()
