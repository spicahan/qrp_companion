# Skip when scanned by ESP-IDF build system
if(ESP_PLATFORM)
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(qrp_companion_desktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 REQUIRED COMPONENTS Widgets)

add_executable(qrp_companion_desktop
    main_desktop.cpp
    pal_desktop.cpp
    ../core/app.cpp
    ../core/dsp.cpp
    ../core/fft.cpp
)

target_include_directories(qrp_companion_desktop PRIVATE
    ../pal
    ../core
)

target_link_libraries(qrp_companion_desktop Qt5::Widgets)
