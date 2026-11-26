# FileDrop — Chia sẻ file đa nền tảng khi cùng mạng Wifi

## Tổng quan
- Ứng dụng chia sẻ file đa nền tảng trong cùng mạng wifi tương tự AirDrop của Apple: macOS, Windows, android, iOS. Phần UI phải viết bằng Native của từng hệ điều hành.
- Phần lõi các hàm dùng chung, các chức năng dùng chung có thể viết bằng C++.
- Người dùng: PC, Mac cá nhân hoặc tổ chức doanh nghiệp.

## Công nghệ
- Ngôn ngữ lõi: C++
- Ngôn ngữ App: trên iOS, macOS dùng Swift, Window phải dùng C++, Android dùng Kotlin
- Giao diện phải đồng nhất giữa 2 nền tảng Desktop: macOS, Windows.

## Mô tả tính năng:
- Có tính năng on/off chạy cùng macOS, Windows, Linux (Tức là khi khởi động xong hệ điều hành, phải chạy ngay ứng dụng này.
- macOS: khi chạy, 1 icon trên thanh menu hiện ra, khi bấm vào nó, 1 ô giao diện hiện ra, bo tròn đổ bóng đúng với hệ điều hành, đẹp đẽ vào, cho phép hiện ra các máy tính có thể gửi file tới, bấm vào máy tính đó để mặc định gửi file tới đó, giao diện hiện thị dòng chữ Drop file vào để gửi file.
- Windows: tương tự, 1 icon hiện ở system tray, tính năng tương tự bản macOS.
- Khi khởi chạy, app sẽ có khởi tạo UDP trên phạm vi toàn bộ wifi mạng cục bộ.
- Khi app mở ra lần đầu, để lấy danh sách các máy tính (cả mac và PC), app sẽ gửi gói tin UDP broadcast tới toàn bộ phạm vi wifi.
- Khi app nhận được gói tin UDP loại truy vấn hỏi danh tính, gửi lại thông tin tên máy, IP, port UDP, hệ điều hành cho máy gửi.
- Máy gửi sau khi nhận được gói tin hồi đáp danh tính, hiện ngay item theo dạng lưới trên view, nếu là PC hiện icon PC, mac thì hiện icon máy mac, ở dưới là tên máy, IP lưu trong bộ nhớ, không cần hiện ra.
- Sau khi bấm vào máy đích, ghi nhớ máy đó.
- Khi kéo file ném vào view đang đợi file, gửi 1 udp yêu cầu gửi file tới máy đó nhờ IP, port UDP của máy đó. Nếu 1 file thì gửi tên file, nếu nhiều file thì gửi số lượng file, kèm theo tên máy gửi.
- Máy nhận file khi nhận được gói tin yêu cầu nhận file, đối với máy mac, hiện popup hỏi bạn có chấp nhận nhận file/số lượng file từ máy gửi không?
- Máy nhận bấm "chấp nhận", máy nhận sẽ tạo ra TCP Server để nhận file cho toàn vẹn từ máy gửi, sau khi listen xong, gửi lại IP, Port của TCP Server mới tạo cho máy gửi thông qua UDP, đồng thời gửi thêm password là 16 ký tự ngẫu nhiên cho máy gửi, nếu bấm "từ chối", cũng gửi lại thông tin từ chối cho máy gửi
- Máy gửi nhận được gói tin hồi đáp qua UDP, nếu là chấp nhận, thì lấy IP, port TCP, Password, tạo kết nối tới máy nhận đó qua TCP, kết nối với máy nhận thông qua TCP, khi kết nối thành công, gửi tiếp password đã nhận trước đó.
- Máy nhận nhận gói đầu tiên là password nếu khớp, gửi thông tin bắt đầu chấp nhận.
- Máy gửi nhận được tín hiệu nhận, bắt đầu gửi data cho máy nhận, có hiện progress bar theo hình tròn ở trên cả máy gửi và máy nhận.
- Sau khi gửi nhận xong, hủy TCP của cả 2 máy. Máy nhận nhận file và lưu mặc định vào thư mục Download, nhận xong sẽ thông báo nho nhỏ là nhận xong, máy gửi cũng thế.
- Đối với iOS và Android, khi chọn 1 file, ảnh hay bất kỳ dữ liệu nào dùng tính năng share, phải xuất hiện FileDrop trong danh sách share, nếu bấm vào FileDrop, tương tự như Windows và macOS, hiện danh sách các máy share được,... luồng tương tự như app Desktop.

## Cấu trúc thư mục
- `shared/` — Các file hàm, class dùng chung.
- `macOS/` — phiên bản cho macOS
- `Windows/` — phiên bản cho Windows
- `android` - phiên bản cho Android
- `iOS` - phiên bản iOS
- `thirdparty` — các bộ thư viện nguồn ở ngoài.
- `build/` — Các bản build của từng nền tảng sẽ được đặt ở đây.

## Lệnh thường dùng
- Build app macOS (lõi C++ + Swift → `build/macOS/FileDrop.app`): `./macOS/build.sh` (`CONFIG=Debug ./macOS/build.sh` để debug)
- Chạy kiểm thử lõi: `./build/macOS/core/shared/filedrop_tests`
- CLI thử nghiệm (đóng vai máy thứ 2 trên cùng Mac): `./build/macOS/core/shared/fdcli --id cli --name CLI list | listen [--yes] | send "<tên máy>" <file...> | web [--yes] [--packages <thư mục>]`
- Kiểm tra build Windows từ macOS (MinGW): `cmake -S . -B build/Windows-mingw -DCMAKE_TOOLCHAIN_FILE=Windows/cmake/mingw-w64-x86_64.cmake && cmake --build build/Windows-mingw`
- Build Android (APK debug → `android/app/build/outputs/apk/debug/`): `cd android && JAVA_HOME=~/Library/Java/JavaVirtualMachines/jbr-17.0.14/Contents/Home ./gradlew assembleDebug` (thêm `--offline` nếu không có mạng)
- Cài lên điện thoại: `adb install -r -g android/app/build/outputs/apk/debug/app-debug.apk` (Xiaomi: phải bấm xác nhận trên máy)
- Tài liệu song ngữ (build, tính năng, cách dùng, giao thức): `docs/index.html` — cập nhật khi thêm tính năng hoặc đổi cách build
- Build Windows trên máy Windows (Visual Studio 2022): `cmake -S . -B build/Windows -G "Visual Studio 17 2022" && cmake --build build/Windows --config Release`

## Quy ước viết mã
- C++17, namespace `filedrop`.

## Tình trạng hiện tại
- Lõi `shared/` (C++17): giao thức UDP dạng text `FILEDROP/1 <TYPE>` + `key=value` (protocol.h), socket đa nền tảng (net.h),
  truyền file TCP có mật khẩu 16 ký tự (transfer.h), `Engine` điều phối (engine.h), C API cho Swift/JNI (filedrop_c.h).
  - Discovery: socket nghe broadcast ở port 45454 (SO_REUSEPORT) + socket điều khiển port riêng (được báo trong gói ANNOUNCE).
  - Tự tìm lại mỗi 30 giây, máy không phản hồi > 95 giây bị xoá; yêu cầu gửi hết hạn sau 120 giây.
- macOS (`macOS/`): app menu bar Swift/SwiftUI, popover, lưới máy, kéo thả, progress tròn (cả trên icon menu bar),
  NSAlert hỏi nhận file, thông báo, Login Item (SMAppService). Đã test: discovery + nhận file từ fdcli.
- Windows (`Windows/`): Win32 + GDI+, tray icon, popup bo tròn (Win11) đổ bóng, bố cục giống macOS, kéo thả OLE,
  TaskDialog hỏi nhận file, balloon thông báo, autostart qua HKCU\...\Run. Mới kiểm tra biên dịch bằng MinGW, chưa chạy thật.
- Android (`android/`): Kotlin + Jetpack Compose, lõi C++ qua JNI (`app/src/main/cpp/filedrop_jni.cpp`), minSdk 29.
  - `ShareActivity` có trong bảng Chia sẻ (SEND/SEND_MULTIPLE `*/*`, cả văn bản); bấm vào máy là gửi.
  - File `content://` được gửi thẳng từ file descriptor (`fileEntryFromFd`, `Engine::sendEntries`), không chép tạm.
  - `FileDropService` (foreground, dataSync) giữ engine + MulticastLock; tự dừng khi tắt "Nhận file khi chạy nền" và đóng app.
  - File nhận về lưu tạm `filesDir/incoming` rồi chuyển vào `Download/FileDrop` qua MediaStore.
  - Hỏi nhận file: hộp thoại trong app + thông báo có nút Chấp nhận/Từ chối.
- Icon app: vẽ bằng `tools/icon/render_icon.swift` (PNG nhiều cỡ) → `tools/icon/make_ico.py` ra `Windows/FileDrop.ico`
  (icon exe = icon tray lúc rảnh) và `iconutil` ra `macOS/FileDrop/Resources/AppIcon.icns`. Menu bar macOS vẫn dùng SF Symbol đơn sắc.
- Kéo thả nhận cả file promise / file ảo / dữ liệu ảnh (ảnh chụp màn hình góc dưới phải, Photos, Mail, Outlook, ảnh trình duyệt):
  macOS `DropSupport.swift` (`DropReader` + `FileDropTarget` AppKit làm background vùng thả, thay `dropDestination`),
  Windows `DropData.cpp` (CF_HDROP → FileGroupDescriptorW/FileContents → PNG → CF_DIB). File tạm lưu ở Caches / %TEMP%\FileDrop\Dropped, dọn sau 1 ngày.
  Đã thử kéo ảnh chụp màn hình thật trên macOS.
- Popup Windows tự đóng khi bấm ra ngoài / Alt+Tab / Esc, nhưng giữ nguyên khi đang kéo file từ Explorer vào (theo dõi chuột trong `pollOutsideClick`).
- Tạm ẩn (cả 3 app, lưu qua các lần mở): `Engine::setHidden` — gửi BYE, không trả lời DISCOVER, gói của máy ẩn mang `hidden=1`
  (máy khác không thêm vào danh sách), yêu cầu gửi file tới bị tự từ chối với `reason=hidden`. Vẫn thấy máy khác và gửi đi được.
  UI: checkbox/công tắc "Tạm ẩn — không nhận file", dòng phụ màu cam ở header, icon tray/menu bar mờ.
- Máy chủ nhận file qua trình duyệt (macOS + Windows + Android): `WebServer` (webserver.h) do `Engine` sở hữu — `startWebServer/stopWebServer`,
  `respondWebClient`, `disconnectWebClient`, callback `onWebClientChanged`. HTTP/1.1 tự viết, cổng TCP ngẫu nhiên, `Connection: close`.
  - Trang web: `shared/web/index.html` (song ngữ, sáng/tối, kéo thả cả thư mục), nhúng vào lõi lúc build qua `shared/cmake/embed_file.cmake`.
  - Mỗi lần thả trên trang = 1 batch = 1 phiên nhận (Direction::Receive, peer.os = "web") nên dùng chung progress/thông báo với UDP/TCP.
  - Kết nối không tự hết hạn; chỉ kết thúc khi người dùng ngắt máy khách / tắt máy chủ / máy khách tự ngắt. Không lưu bật/tắt qua các lần mở.
  - Địa chỉ hiện ra dùng `net::lanIp()` (bỏ qua VPN/card ảo), không dùng route mặc định.
  - Trang web gợi ý máy khách tải app hợp HĐH (`/api/info` có `apps`, `GET /download/<platform>`, không cần duyệt):
    `Engine::setWebDownloads(list, packageDirs)` + `scanWebDownloads` (nhận theo đuôi/tên file).
    macOS nén chính .app (`AppPackages.swift`, cache trong Caches) + `Resources/Packages` (build.sh chép từ `build/packages`)
    + `~/Library/Application Support/FileDrop/Packages`; Windows phục vụ chính exe (MSVC runtime tĩnh) + `packages\` cạnh exe;
    Android phục vụ APK đã cài + `getExternalFilesDir("packages")`.
  - Android: JNI `nativeWeb*`, `FileDrop.setWebServer/respondWeb/disconnectWeb`; service giữ chạy khi máy chủ bật. Đã thử trên Xiaomi thật (nhận file, ngắt kết nối).
- Chưa làm: iOS (share extension), ký số/notarize, keystore phát hành Android.

## Lưu ý
- Mỗi lần lập trình, viết mã song song cho 2 nền tảng macOS, Windows.
- Khi test, chỉ cần test trên macOS.