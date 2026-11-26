# Sinh file .cpp chứa nội dung một file dạng mảng byte.
# Dùng: cmake -DINPUT=<file> -DOUTPUT=<.cpp> -DSYMBOL=<tên biến> -P embed_file.cmake
file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hex_len)
math(EXPR size "${hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
# Xuống dòng mỗi 32 byte cho dễ đọc / tránh dòng quá dài.
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}.tmp"
"// Tự sinh từ ${INPUT} — không sửa tay.\n#include <cstddef>\nnamespace filedrop {\nextern const unsigned char ${SYMBOL}[] = {\n${bytes}0x00};\nextern const size_t ${SYMBOL}Size = ${size};\n}  // namespace filedrop\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUTPUT}.tmp" "${OUTPUT}")
file(REMOVE "${OUTPUT}.tmp")
