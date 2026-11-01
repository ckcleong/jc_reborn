# ESP32-S3 IDF project for the jc_reborn engine (see docs/ESP32.md).
#
# The engine sources themselves live at the repository root and are shared
# with the PC SDL build; they are compiled as the jc_engine component
# (esp32/components/jc_engine). This root CMakeLists only ties the project
# together, exactly like the IDF examples.

cmake_minimum_required(VERSION 3.16)

# S3 only. Without this a fresh clone (no sdkconfig yet) would fall back to
# IDF's default target, esp32, and fail in confusing ways. 'idf.py
# set-target esp32s3' remains the way to regenerate sdkconfig for the target.
set(IDF_TARGET "esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(jc_reborn)
