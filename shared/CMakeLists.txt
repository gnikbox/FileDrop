add_library(filedrop_core STATIC
    src/protocol.cpp
    src/net.cpp
    src/util.cpp
    src/transfer.cpp
    src/engine.cpp
    src/filedrop_c.cpp
)
target_include_directories(filedrop_core PUBLIC include)
target_compile_features(filedrop_core PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(filedrop_core PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(filedrop_core PUBLIC ws2_32 iphlpapi)
    target_compile_definitions(filedrop_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
endif()

if(MSVC)
    target_compile_options(filedrop_core PUBLIC /utf-8)
    target_compile_options(filedrop_core PRIVATE /W4)
else()
    target_compile_options(filedrop_core PRIVATE -Wall -Wextra)
endif()

if(FILEDROP_BUILD_TOOLS)
    add_executable(fdcli tools/fdcli.cpp)
    target_link_libraries(fdcli PRIVATE filedrop_core)

    add_executable(filedrop_tests tests/core_tests.cpp)
    target_link_libraries(filedrop_tests PRIVATE filedrop_core)
    add_test(NAME filedrop_core_tests COMMAND filedrop_tests)
endif()
