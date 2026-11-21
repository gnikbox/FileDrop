cmake_minimum_required(VERSION 3.22.1)
project(filedrop_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# android/app/src/main/cpp -> gốc repo
get_filename_component(FILEDROP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(FILEDROP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${FILEDROP_ROOT}/shared" "${CMAKE_BINARY_DIR}/filedrop_shared")

add_library(filedrop_jni SHARED filedrop_jni.cpp)
target_link_libraries(filedrop_jni PRIVATE filedrop_core log)
target_compile_options(filedrop_jni PRIVATE -Wall -Wextra)
