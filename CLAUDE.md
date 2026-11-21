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
- CLI thử nghiệm (đóng vai máy thứ 2 trên cùng Mac): `./build/macOS/core/shared/fdcli --id cli --name CLI list | listen [--yes] | send "<tên máy>" <file...>`
- Kiểm tra build Windows từ macOS (MinGW): `cmake -S . -B build/Windows-mingw -DCMAKE_TOOLCHAIN_FILE=Windows/cmake/mingw-w64-x86_64.cmake && cmake --build build/Windows-mingw`
- Build Android (APK debug → `android/app/build/outputs/apk/debug/`): `cd android && JAVA_HOME=~/Library/Java/JavaVirtualMachines/jbr-17.0.14/Contents/Home ./gradlew assembleDebug` (thêm `--offline` nếu không có mạng)
- Cài lên điện thoại: `adb install -r -g android/app/build/outputs/apk/debug/app-debug.apk` (Xiaomi: phải bấm xác nhận trên máy)
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
- Chưa làm: iOS (share extension), icon app desktop (.icns/.ico), ký số/notarize, keystore phát hành Android.

## Lưu ý
- Mỗi lần lập trình, viết mã song song cho 2 nền tảng macOS, Windows.
- Khi test, chỉ cần test trên macOS.