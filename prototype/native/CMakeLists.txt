cmake_minimum_required(VERSION 3.10)
project(alvr_vio_native LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(alvr_vio SHARED
    src/native-lib.cpp
    src/tracker.cpp
)

find_library(log-lib log)

target_include_directories(alvr_vio PRIVATE include)

target_link_libraries(alvr_vio PRIVATE ${log-lib})
