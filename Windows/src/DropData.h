// FileDrop (Windows) — đọc dữ liệu kéo thả (OLE) thành danh sách file để gửi.
// Tương đương DropSupport.swift bên macOS:
//  - File thật (CF_HDROP, từ Explorer...).
//  - File ảo (FileGroupDescriptorW + FileContents: tệp đính kèm Outlook, ảnh từ một số ứng dụng...) → ghi ra thư mục tạm.
//  - Dữ liệu ảnh thô ("PNG", CF_DIBV5/CF_DIB: ảnh kéo từ trình duyệt, Snipping Tool...) → lưu thành file PNG tạm.
#pragma once

#include <windows.h>
#include <ole2.h>

#include <string>
#include <vector>

namespace filedrop::win {

// Có định dạng nào đọc được thành file không (gọi trong DragEnter).
bool hasDroppableData(IDataObject* data);
// Đọc thành đường dẫn file (gọi trong Drop). Rỗng nếu không đọc được.
std::vector<std::wstring> readDroppedFiles(IDataObject* data);

}  // namespace filedrop::win
