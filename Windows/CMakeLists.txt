enable_language(RC)

add_executable(FileDrop WIN32
    src/main.cpp
    src/App.cpp
    src/PopupWindow.cpp
    src/Drawing.cpp
    src/DropData.cpp
    src/Settings.cpp
    FileDrop.rc
)
target_link_libraries(FileDrop PRIVATE
    filedrop_core
    gdiplus comctl32 dwmapi shcore shell32 ole32 uuid
)
if(MSVC)
    # Manifest đã nhúng qua FileDrop.rc
    target_link_options(FileDrop PRIVATE /MANIFEST:NO)
else()
    target_compile_options(FileDrop PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    target_link_options(FileDrop PRIVATE -municode -static)
endif()

set_target_properties(FileDrop PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
