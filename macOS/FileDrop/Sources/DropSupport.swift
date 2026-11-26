import AppKit
import SwiftUI

/// Đọc nội dung được kéo thả thành danh sách file để gửi.
/// Hỗ trợ:
///  - File/thư mục thật (Finder...): `.fileURL`.
///  - File promise (`NSFilePromiseReceiver`): ảnh thu nhỏ của ảnh chụp màn hình ở góc dưới phải, ảnh từ Photos,
///    file đính kèm trong Mail... — file chưa có trên đĩa, nên được ghi vào thư mục tạm rồi mới gửi.
///  - Dữ liệu ảnh thô (PNG/TIFF, vd. kéo ảnh từ trình duyệt): lưu thành file PNG tạm.
enum DropReader {
    static var draggedTypes: [NSPasteboard.PasteboardType] {
        [.fileURL] + NSFilePromiseReceiver.readableDraggedTypes.map { NSPasteboard.PasteboardType($0) } + [.png, .tiff]
    }

    private static let promiseQueue: OperationQueue = {
        let q = OperationQueue()
        q.qualityOfService = .userInitiated
        q.name = "filedrop.file-promises"
        return q
    }()

    /// Thư mục tạm cho file nhận từ promise/ảnh. File cũ hơn 1 ngày bị dọn ở mỗi lần thả
    /// (không xoá ngay sau khi gửi vì lõi đọc file bất đồng bộ trong lúc truyền).
    private static var stagingRoot: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("com.filedrop.macos/Dropped", isDirectory: true)
    }

    static func canRead(_ pasteboard: NSPasteboard) -> Bool {
        pasteboard.availableType(from: draggedTypes) != nil
    }

    /// Gọi trong performDragOperation. completion chạy trên main với các file đọc được (có thể rỗng).
    static func read(_ pasteboard: NSPasteboard, completion: @escaping ([URL]) -> Void) {
        // 1. File thật.
        let urls = pasteboard.readObjects(forClasses: [NSURL.self],
                                          options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
        if !urls.isEmpty {
            completion(urls)
            return
        }

        // 2. File promise.
        let receivers = pasteboard.readObjects(forClasses: [NSFilePromiseReceiver.self]) as? [NSFilePromiseReceiver] ?? []
        if !receivers.isEmpty, let dir = makeStagingDir() {
            let group = DispatchGroup()
            let lock = NSLock()
            var received: [URL] = []
            for receiver in receivers {
                let count = max(receiver.fileTypes.count, 1)
                for _ in 0..<count { group.enter() }
                var remaining = count
                receiver.receivePromisedFiles(atDestination: dir, options: [:], operationQueue: promiseQueue) { url, error in
                    lock.lock()
                    if error == nil { received.append(url) }
                    let shouldLeave = remaining > 0
                    remaining -= 1
                    lock.unlock()
                    if shouldLeave { group.leave() }
                }
            }
            group.notify(queue: .main) { completion(received) }
            return
        }

        // 3. Dữ liệu ảnh.
        if let png = imagePNG(from: pasteboard), let dir = makeStagingDir() {
            let f = DateFormatter()
            f.dateFormat = "yyyy-MM-dd 'lúc' HH.mm.ss"
            let url = dir.appendingPathComponent("Ảnh \(f.string(from: Date())).png")
            if (try? png.write(to: url)) != nil {
                completion([url])
                return
            }
        }
        completion([])
    }

    private static func imagePNG(from pasteboard: NSPasteboard) -> Data? {
        if let png = pasteboard.data(forType: .png) { return png }
        guard let tiff = pasteboard.data(forType: .tiff), let rep = NSBitmapImageRep(data: tiff) else { return nil }
        return rep.representation(using: .png, properties: [:])
    }

    private static func makeStagingDir() -> URL? {
        let fm = FileManager.default
        let root = stagingRoot
        // Dọn các lần thả cũ.
        if let items = try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: [.contentModificationDateKey]) {
            let cutoff = Date().addingTimeInterval(-24 * 3600)
            for item in items {
                let date = (try? item.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
                if let date, date < cutoff { try? fm.removeItem(at: item) }
            }
        }
        let dir = root.appendingPathComponent(UUID().uuidString, isDirectory: true)
        do {
            try fm.createDirectory(at: dir, withIntermediateDirectories: true)
            return dir
        } catch {
            return nil
        }
    }
}

/// Vùng nhận kéo thả bằng AppKit (đặt làm background của vùng thả trong popover) — SwiftUI `dropDestination`
/// chỉ nhận URL, không nhận được file promise như ảnh chụp màn hình vừa chụp.
struct FileDropTarget: NSViewRepresentable {
    var canDrop: () -> Bool
    var onTargeted: (Bool) -> Void
    var onDrop: ([URL]) -> Void

    func makeNSView(context: Context) -> DropTargetView {
        let v = DropTargetView()
        v.target = self
        return v
    }

    func updateNSView(_ nsView: DropTargetView, context: Context) {
        nsView.target = self
    }

    final class DropTargetView: NSView {
        var target: FileDropTarget?

        override init(frame: NSRect) {
            super.init(frame: frame)
            registerForDraggedTypes(DropReader.draggedTypes)
        }

        required init?(coder: NSCoder) { fatalError("init(coder:) không dùng") }

        private func accepts(_ sender: NSDraggingInfo) -> Bool {
            (target?.canDrop() ?? false) && DropReader.canRead(sender.draggingPasteboard)
        }

        override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
            let ok = accepts(sender)
            target?.onTargeted(ok)
            return ok ? .copy : []
        }

        override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
            accepts(sender) ? .copy : []
        }

        override func draggingExited(_ sender: NSDraggingInfo?) { target?.onTargeted(false) }

        override func draggingEnded(_ sender: NSDraggingInfo) { target?.onTargeted(false) }

        override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
            target?.onTargeted(false)
            guard accepts(sender), let target else { return false }
            DropReader.read(sender.draggingPasteboard) { urls in
                if !urls.isEmpty { target.onDrop(urls) }
            }
            return true
        }
    }
}
