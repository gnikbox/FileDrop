#include "DropData.h"

#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "filedrop/util.h"

namespace fs = std::filesystem;

namespace filedrop::win {

namespace {

CLIPFORMAT clipFormat(const wchar_t* name) { return static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(name)); }

CLIPFORMAT cfFileDescriptor() {
    static CLIPFORMAT f = clipFormat(CFSTR_FILEDESCRIPTORW);
    return f;
}
CLIPFORMAT cfFileContents() {
    static CLIPFORMAT f = clipFormat(CFSTR_FILECONTENTS);
    return f;
}
CLIPFORMAT cfPng() {
    static CLIPFORMAT f = clipFormat(L"PNG");
    return f;
}

bool has(IDataObject* data, CLIPFORMAT cf, DWORD tymed) {
    FORMATETC fmt{cf, nullptr, DVASPECT_CONTENT, -1, tymed};
    return data->QueryGetData(&fmt) == S_OK;
}

// Thư mục tạm cho mỗi lần thả: %TEMP%\FileDrop\Dropped\<ngẫu nhiên>. Lần thả cũ hơn 1 ngày bị dọn
// (không xoá ngay vì lõi đọc file bất đồng bộ trong lúc truyền).
fs::path makeStagingDir() {
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / L"FileDrop" / L"Dropped";
    if (ec) return {};
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24);
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->last_write_time(e2) < cutoff && !e2) fs::remove_all(it->path(), e2);
    }
    fs::path dir = root / fromUtf8(randomHex(8));
    fs::create_directories(dir, ec);
    return ec ? fs::path() : dir;
}

// Tên file từ ứng dụng nguồn: bỏ ký tự cấm, không cho "..", giữ thư mục con ("a\b.txt").
fs::path safeRelative(const std::wstring& raw) {
    fs::path out;
    std::wstring part;
    auto flush = [&] {
        while (!part.empty() && (part.back() == L' ' || part.back() == L'.')) part.pop_back();
        if (!part.empty() && part != L"..") out /= part;
        part.clear();
    };
    for (wchar_t c : raw) {
        if (c == L'\\' || c == L'/') {
            flush();
        } else if (c < 0x20 || std::wcschr(L"<>:\"|?*", c)) {
            part.push_back(L'_');
        } else {
            part.push_back(c);
        }
    }
    flush();
    return out;
}

bool writeStream(IStream* stream, const fs::path& target) {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    char buf[64 * 1024];
    for (;;) {
        ULONG got = 0;
        HRESULT hr = stream->Read(buf, sizeof(buf), &got);
        if (got > 0) out.write(buf, got);
        if (FAILED(hr) || got == 0) break;
    }
    return static_cast<bool>(out);
}

bool writeMedium(STGMEDIUM& medium, const fs::path& target, SIZE_T sizeHint) {
    if (medium.tymed == TYMED_ISTREAM && medium.pstm) return writeStream(medium.pstm, target);
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal) {
        SIZE_T size = ::GlobalSize(medium.hGlobal);
        if (sizeHint && sizeHint < size) size = sizeHint;
        const void* p = ::GlobalLock(medium.hGlobal);
        if (!p) return false;
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(p), static_cast<std::streamsize>(size));
        ::GlobalUnlock(medium.hGlobal);
        return static_cast<bool>(out);
    }
    return false;
}

std::vector<std::wstring> readHdrop(IDataObject* data) {
    std::vector<std::wstring> paths;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (data->GetData(&fmt, &medium) != S_OK) return paths;
    if (auto drop = static_cast<HDROP>(::GlobalLock(medium.hGlobal))) {
        UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(len + 1, L'\0');
            ::DragQueryFileW(drop, i, path.data(), len + 1);
            path.resize(len);
            paths.push_back(path);
        }
        ::GlobalUnlock(medium.hGlobal);
    }
    ::ReleaseStgMedium(&medium);
    return paths;
}

std::vector<std::wstring> readVirtualFiles(IDataObject* data) {
    std::vector<std::wstring> paths;
    FORMATETC fmt{cfFileDescriptor(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (data->GetData(&fmt, &medium) != S_OK) return paths;
    std::vector<FILEDESCRIPTORW> items;
    if (auto group = static_cast<FILEGROUPDESCRIPTORW*>(::GlobalLock(medium.hGlobal))) {
        items.assign(group->fgd, group->fgd + group->cItems);
        ::GlobalUnlock(medium.hGlobal);
    }
    ::ReleaseStgMedium(&medium);
    if (items.empty()) return paths;

    fs::path dir = makeStagingDir();
    if (dir.empty()) return paths;
    std::vector<fs::path> top;  // mục cấp cao nhất (file hoặc thư mục) để gửi
    auto addTop = [&](const fs::path& rel) {
        fs::path first = dir / *rel.begin();
        for (auto& t : top) {
            if (t == first) return;
        }
        top.push_back(first);
    };
    for (LONG i = 0; i < static_cast<LONG>(items.size()); ++i) {
        const FILEDESCRIPTORW& fd = items[i];
        fs::path rel = safeRelative(fd.cFileName);
        if (rel.empty()) continue;
        std::error_code ec;
        fs::path target = dir / rel;
        if ((fd.dwFlags & FD_ATTRIBUTES) && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            fs::create_directories(target, ec);
            addTop(rel);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        FORMATETC cfmt{cfFileContents(), nullptr, DVASPECT_CONTENT, i, TYMED_ISTREAM | TYMED_HGLOBAL};
        STGMEDIUM content{};
        if (data->GetData(&cfmt, &content) != S_OK) continue;
        SIZE_T hint = 0;
        if (fd.dwFlags & FD_FILESIZE) hint = static_cast<SIZE_T>((static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow);
        bool ok = writeMedium(content, target, hint);
        ::ReleaseStgMedium(&content);
        if (ok) addTop(rel);
    }
    for (auto& t : top) paths.push_back(t.wstring());
    return paths;
}

bool pngEncoder(CLSID* clsid) {
    UINT num = 0, size = 0;
    if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || size == 0) return false;
    std::vector<BYTE> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    if (Gdiplus::GetImageEncoders(num, size, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < num; ++i) {
        if (std::wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            *clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

fs::path imageTarget() {
    fs::path dir = makeStagingDir();
    if (dir.empty()) return {};
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t name[64];
    std::swprintf(name, 64, L"Ảnh %04u-%02u-%02u lúc %02u.%02u.%02u.png", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
    return dir / name;
}

// Ảnh PNG có sẵn.
std::vector<std::wstring> readPng(IDataObject* data) {
    FORMATETC fmt{cfPng(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL | TYMED_ISTREAM};
    STGMEDIUM medium{};
    if (data->GetData(&fmt, &medium) != S_OK) return {};
    fs::path target = imageTarget();
    bool ok = !target.empty() && writeMedium(medium, target, 0);
    ::ReleaseStgMedium(&medium);
    if (!ok) return {};
    return {target.wstring()};
}

// Ảnh DIB (CF_DIBV5/CF_DIB): ghép header BMP rồi để GDI+ chuyển sang PNG.
std::vector<std::wstring> readDib(IDataObject* data) {
    for (CLIPFORMAT cf : {static_cast<CLIPFORMAT>(CF_DIBV5), static_cast<CLIPFORMAT>(CF_DIB)}) {
        FORMATETC fmt{cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (data->GetData(&fmt, &medium) != S_OK) continue;
        std::vector<std::wstring> result;
        SIZE_T size = ::GlobalSize(medium.hGlobal);
        const auto* dib = static_cast<const BYTE*>(::GlobalLock(medium.hGlobal));
        if (dib && size >= sizeof(BITMAPINFOHEADER)) {
            const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(dib);
            DWORD palette = bih->biClrUsed ? bih->biClrUsed : (bih->biBitCount <= 8 ? (1u << bih->biBitCount) : 0);
            DWORD masks = (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER)) ? 12 : 0;
            BITMAPFILEHEADER bfh{};
            bfh.bfType = 0x4D42;  // "BM"
            bfh.bfSize = static_cast<DWORD>(sizeof(bfh) + size);
            bfh.bfOffBits = static_cast<DWORD>(sizeof(bfh) + bih->biSize + masks + palette * sizeof(RGBQUAD));
            HGLOBAL bmp = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(bfh) + size);
            if (bmp) {
                if (auto* p = static_cast<BYTE*>(::GlobalLock(bmp))) {
                    std::memcpy(p, &bfh, sizeof(bfh));
                    std::memcpy(p + sizeof(bfh), dib, size);
                    ::GlobalUnlock(bmp);
                }
                IStream* stream = nullptr;
                if (::CreateStreamOnHGlobal(bmp, TRUE, &stream) == S_OK) {
                    CLSID png;
                    fs::path target = imageTarget();
                    Gdiplus::Bitmap image(stream);
                    if (!target.empty() && image.GetLastStatus() == Gdiplus::Ok && pngEncoder(&png) &&
                        image.Save(target.c_str(), &png, nullptr) == Gdiplus::Ok) {
                        result.push_back(target.wstring());
                    }
                    stream->Release();
                } else {
                    ::GlobalFree(bmp);
                }
            }
        }
        if (dib) ::GlobalUnlock(medium.hGlobal);
        ::ReleaseStgMedium(&medium);
        if (!result.empty()) return result;
    }
    return {};
}

}  // namespace

bool hasDroppableData(IDataObject* data) {
    if (!data) return false;
    return has(data, CF_HDROP, TYMED_HGLOBAL) || has(data, cfFileDescriptor(), TYMED_HGLOBAL) ||
           has(data, cfPng(), TYMED_HGLOBAL | TYMED_ISTREAM) || has(data, CF_DIBV5, TYMED_HGLOBAL) ||
           has(data, CF_DIB, TYMED_HGLOBAL);
}

std::vector<std::wstring> readDroppedFiles(IDataObject* data) {
    if (!data) return {};
    if (auto files = readHdrop(data); !files.empty()) return files;
    if (auto files = readVirtualFiles(data); !files.empty()) return files;
    if (auto files = readPng(data); !files.empty()) return files;
    return readDib(data);
}

}  // namespace filedrop::win
