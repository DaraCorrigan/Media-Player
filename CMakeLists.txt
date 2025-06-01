cmake_minimum_required(VERSION 3.14)
project(MediaPlayer)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_TOOLCHAIN_FILE "C:/Users/darac/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
set(CMAKE_PREFIX_PATH "C:/Users/darac/vcpkg/installed/x64-windows")

find_package(SDL2 CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat
  libavcodec
  libswscale
  libswresample
  libavutil
)

add_definitions(-DSDL_MAIN_HANDLED)

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat
  libavcodec
  libswscale
  libavutil
)

add_executable(MediaPlayer main.cpp)

target_link_libraries(MediaPlayer
  PRIVATE
    PkgConfig::FFMPEG
    SDL2::SDL2
)

set_target_properties(MediaPlayer PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:CONSOLE"
)
