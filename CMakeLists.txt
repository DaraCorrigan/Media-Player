cmake_minimum_required(VERSION 3.10)
project(MediaPlayer)

set(CMAKE_CXX_STANDARD 17)

include_directories("C:/Users/darac/vcpkg/installed/x64-windows/include")
link_directories("C:/Users/darac/vcpkg/installed/x64-windows/lib")

add_executable(MediaPlayer main.cpp)

target_link_libraries(MediaPlayer
    avformat
    avcodec
    avutil
)
