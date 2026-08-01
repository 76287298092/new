// Add ODYX bridge and JNI
add_library(odyx_bridge SHARED
    src/main/cpp/odyx_bridge.cpp
)

# if libodyx prebuilt in jniLibs/arm64-v8a
add_library(odyx_native_lib SHARED IMPORTED)
set_target_properties(odyx_native_lib PROPERTIES IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/src/main/jniLibs/arm64-v8a/libodyx.so)

target_include_directories(odyx_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/include)

target_link_libraries(odyx_bridge PRIVATE odyx_native_lib log android)
