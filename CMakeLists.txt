cmake_minimum_required(VERSION 3.20)
project(FileDrop VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
endif()

option(FILEDROP_BUILD_TOOLS "Build fdcli và bộ kiểm thử lõi" ON)

enable_testing()
add_subdirectory(shared)

if(WIN32)
    add_subdirectory(Windows)
endif()
