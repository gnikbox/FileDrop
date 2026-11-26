import AppKit
import SwiftUI

/// App chạy trên thanh menu (LSUIElement): icon trên menu bar, bấm vào hiện popover.
/// - Bấm ra ngoài / bấm lại icon / Esc => đóng popover.
/// - Kéo file từ Finder lên icon => popover tự mở để thả vào; thả thẳng lên icon cũng gửi tới máy đã chọn.
final class AppDelegate: NSObject, NSApplicationDelegate, NSPopoverDelegate {
    private let model = AppModel()
    private var statusItem: NSStatusItem!
    private let popover = NSPopover()
    private var lastClosedAt = Date.distantPast

    func applicationDidFinishLaunching(_ notification: Notification) {
        Notifier.setUp()

        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = statusItem.button {
            button.image = StatusIcon.idle()
            button.toolTip = "FileDrop"
            // View trong suốt phủ lên nút: nhận click + kéo thả file.
            let overlay = StatusItemDropView(frame: button.bounds)
            overlay.autoresizingMask = [.width, .height]
            overlay.onClick = { [weak self] in self?.togglePopover() }
            overlay.onDragEntered = { [weak self] in
                guard let self else { return false }
                if !self.popover.isShown { self.showPopover() }
                return self.model.canDrop
            }
            overlay.onDrop = { [weak self] urls in self?.model.send(urls: urls) }
            button.addSubview(overlay)
        }

        let hosting = NSHostingController(rootView: ContentView(model: model, onQuit: { NSApp.terminate(nil) }))
        hosting.sizingOptions = [.preferredContentSize]  // popover bám theo kích thước SwiftUI => không lệch
        popover.contentViewController = hosting
        popover.behavior = .transient
        popover.animates = true
        popover.delegate = self

        model.onTransferProgressChanged = { [weak self] progress in
            self?.statusItem.button?.image = progress.map { StatusIcon.progress($0) } ?? StatusIcon.idle()
        }
        // Đang tạm ẩn => icon trên thanh menu mờ đi.
        model.onHiddenChanged = { [weak self] hidden in
            self?.statusItem.button?.appearsDisabled = hidden
            self?.statusItem.button?.toolTip = hidden ? "FileDrop (đang tạm ẩn)" : "FileDrop"
        }
        model.onHiddenChanged?(model.isHidden)
        model.start()
    }

    func applicationWillTerminate(_ notification: Notification) {
        model.stop()
    }

    private func togglePopover() {
        if popover.isShown {
            popover.performClose(nil)
        } else if Date().timeIntervalSince(lastClosedAt) > 0.3 {
            // (Popover transient đã tự đóng ở lần nhấn chuột này thì không mở lại.)
            showPopover()
        }
    }

    private func showPopover() {
        guard let button = statusItem.button else { return }
        model.refresh()
        NSApp.activate(ignoringOtherApps: true)
        popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
        popover.contentViewController?.view.window?.makeKey()
        button.highlight(true)
    }

    func popoverDidClose(_ notification: Notification) {
        lastClosedAt = Date()
        statusItem.button?.highlight(false)
    }
}

/// Phủ lên nút trên thanh menu: chuyển click thành toggle và nhận file kéo vào.
final class StatusItemDropView: NSView {
    var onClick: (() -> Void)?
    var onDragEntered: (() -> Bool)?
    var onDrop: (([URL]) -> Void)?
    private var acceptsDrop = false

    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes(DropReader.draggedTypes)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) không dùng") }

    override func mouseDown(with event: NSEvent) { onClick?() }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        acceptsDrop = onDragEntered?() ?? false
        return acceptsDrop ? .copy : []
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        acceptsDrop ? .copy : []
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        guard acceptsDrop, DropReader.canRead(sender.draggingPasteboard) else { return false }
        DropReader.read(sender.draggingPasteboard) { [weak self] urls in
            if !urls.isEmpty { self?.onDrop?(urls) }
        }
        return true
    }
}

/// Vẽ icon thanh menu: icon thường hoặc vòng tiến trình khi đang truyền.
enum StatusIcon {
    static func idle() -> NSImage {
        let config = NSImage.SymbolConfiguration(pointSize: 15, weight: .medium)
        let image = NSImage(systemSymbolName: "dot.radiowaves.left.and.right", accessibilityDescription: "FileDrop")?
            .withSymbolConfiguration(config) ?? NSImage()
        image.isTemplate = true
        return image
    }

    static func progress(_ value: Double) -> NSImage {
        let size = NSSize(width: 18, height: 18)
        let image = NSImage(size: size, flipped: false) { rect in
            let inset = rect.insetBy(dx: 2, dy: 2)
            let center = NSPoint(x: inset.midX, y: inset.midY)
            let radius = inset.width / 2

            let track = NSBezierPath(ovalIn: inset)
            track.lineWidth = 2
            NSColor.black.withAlphaComponent(0.3).setStroke()
            track.stroke()

            let arc = NSBezierPath()
            let end = 90 - 360 * CGFloat(max(0.02, min(1, value)))
            arc.appendArc(withCenter: center, radius: radius, startAngle: 90, endAngle: end, clockwise: true)
            arc.lineWidth = 2.5
            arc.lineCapStyle = .round
            NSColor.black.setStroke()
            arc.stroke()
            return true
        }
        image.isTemplate = true
        return image
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
