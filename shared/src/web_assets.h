// Tài nguyên web nhúng sẵn (sinh từ shared/web/ lúc build — xem shared/cmake/embed_file.cmake).
#pragma once

#include <cstddef>

namespace filedrop {
extern const unsigned char kWebIndexHtml[];
extern const size_t kWebIndexHtmlSize;
}  // namespace filedrop
