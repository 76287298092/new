cmake_minimum_required(VERSION 3.10)
project(alvr_vio_native LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(USE_OPENVINS "Enable building against OpenVINS if available in third_party/openvins" OFF)

add_library(alvr_vio SHARED
    src/native-lib.cpp
    src/tracker.cpp
)

if(USE_OPENVINS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvins/CMakeLists.txt")
        message(STATUS "Configuring with OpenVINS support")
        add_subdirectory(third_party/openvins)
        # Example: link to an OpenVINS library if it defines one (adjust per upstream)
        # target_link_libraries(alvr_vio PRIVATE openvins_core)
    else()
        message(WARNING "USE_OPENVINS is ON but third_party/openvins is missing. Run fetch_openvins.sh to fetch it.")
    endif()
else()
    message(STATUS "Building with stub tracker (USE_OPENVINS=OFF)")
endif()

find_library(log-lib log)

target_include_directories(alvr_vio PRIVATE include)

target_link_libraries(alvr_vio PRIVATE ${log-lib})
