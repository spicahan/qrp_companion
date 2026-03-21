cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/components)

# Exclude desktop/ from ESP-IDF build scan
set(BUILD_EXCLUDE_COMPONENTS "")
list(APPEND CMAKE_IGNORE_PATH ${CMAKE_CURRENT_LIST_DIR}/desktop)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(qrp_companion)

# Suppress warnings-as-errors in third-party library code
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=maybe-uninitialized" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=literal-suffix" APPEND)
