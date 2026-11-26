import Foundation

/// Bản cài FileDrop mà máy chủ nhận file qua web cho máy khách tải về.
///  - Bản macOS: nén chính FileDrop.app đang chạy thành zip (lưu đệm trong Caches, nén lại khi app đổi).
///  - Bản cho nền tảng khác: file trong `FileDrop.app/Contents/Resources/Packages` (build.sh chép từ `build/packages`)
///    và `~/Library/Application Support/FileDrop/Packages` (người dùng tự bỏ vào: .exe, .apk, *Windows*.zip...).
enum AppPackages {
    static let macZipName = "FileDrop-macOS.zip"

    static var userPackagesDir: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("FileDrop/Packages", isDirectory: true)
    }

    static var packageDirs: [String] {
        try? FileManager.default.createDirectory(at: userPackagesDir, withIntermediateDirectories: true)
        var dirs: [String] = []
        if let res = Bundle.main.resourceURL?.appendingPathComponent("Packages", isDirectory: true) {
            dirs.append(res.path)
        }
        dirs.append(userPackagesDir.path)
        return dirs
    }

    private static let queue = DispatchQueue(label: "filedrop.packages", qos: .utility)

    /// Nén FileDrop.app thành zip (chạy nền). completion chạy trên main với đường dẫn zip, nil nếu lỗi.
    static func prepareMacZip(completion: @escaping (URL?) -> Void) {
        queue.async {
            let url = makeMacZip()
            DispatchQueue.main.async { completion(url) }
        }
    }

    private static func makeMacZip() -> URL? {
        let fm = FileManager.default
        let app = Bundle.main.bundleURL
        guard app.pathExtension == "app" else { return nil }
        let cacheDir = fm.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("com.filedrop.macos", isDirectory: true)
        try? fm.createDirectory(at: cacheDir, withIntermediateDirectories: true)
        let zip = cacheDir.appendingPathComponent(macZipName)

        // Dùng lại zip cũ nếu mới hơn file chạy của app.
        func modDate(_ u: URL) -> Date? { (try? fm.attributesOfItem(atPath: u.path))?[.modificationDate] as? Date }
        if let z = modDate(zip), let exe = Bundle.main.executableURL.flatMap(modDate), z >= exe {
            return zip
        }

        let tmp = cacheDir.appendingPathComponent(".\(UUID().uuidString).zip")
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        p.arguments = ["-c", "-k", "--sequesterRsrc", "--keepParent", app.path, tmp.path]
        do {
            try p.run()
            p.waitUntilExit()
        } catch {
            return nil
        }
        guard p.terminationStatus == 0 else {
            try? fm.removeItem(at: tmp)
            return nil
        }
        try? fm.removeItem(at: zip)
        do {
            try fm.moveItem(at: tmp, to: zip)
        } catch {
            try? fm.removeItem(at: tmp)
            return nil
        }
        return zip
    }
}
