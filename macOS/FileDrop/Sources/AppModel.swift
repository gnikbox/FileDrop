import AppKit
import Combine
import Foundation

/// Trạng thái một phiên truyền đang hiển thị trên UI.
struct ActiveTransfer: Identifiable {
    let id: String
    let direction: TransferDirection
    let peerName: String
    let itemsDescription: String
    let startedAt = Date()
    var phase: TransferPhase
    var progress: Double  // 0...1

    var title: String {
        switch (direction, phase) {
        case (.send, .waitingForAccept): return "Đang chờ \(peerName) chấp nhận…"
        case (.send, .connecting): return "Đang kết nối tới \(peerName)…"
        case (.send, .transferring): return "Đang gửi tới \(peerName)"
        case (.receive, .transferring): return "Đang nhận từ \(peerName)"
        case (.receive, _): return "Đang chờ \(peerName) kết nối…"
        }
    }
}

struct StatusNote: Equatable {
    let text: String
    let isError: Bool
}

/// Model trung tâm của app: giữ danh sách máy, máy đích đã chọn, tiến trình truyền.
final class AppModel: ObservableObject, FileDropEngineDelegate {
    @Published private(set) var peers: [PeerDevice] = []
    @Published private(set) var selectedPeerId: String?
    @Published private(set) var transfers: [String: ActiveTransfer] = [:]
    @Published private(set) var note: StatusNote?
    @Published private(set) var isSearching = false
    @Published var launchAtLogin: Bool = LaunchAtLogin.isEnabled {
        didSet {
            guard launchAtLogin != oldValue else { return }
            if let error = LaunchAtLogin.set(launchAtLogin) {
                show(note: "Không đổi được chế độ khởi động: \(error.localizedDescription)", isError: true)
                launchAtLogin = LaunchAtLogin.isEnabled
            }
        }
    }

    /// Tạm ẩn — không hiện với máy khác, không nhận file (vẫn gửi đi được). Ghi nhớ qua các lần mở app.
    @Published var isHidden: Bool = UserDefaults.standard.bool(forKey: "hidden") {
        didSet {
            guard isHidden != oldValue else { return }
            engine.isHidden = isHidden
            defaults.set(isHidden, forKey: Keys.hidden)
            onHiddenChanged?(isHidden)
            show(note: isHidden ? "Đã tạm ẩn — máy khác không thấy và không gửi file tới được"
                                : "Đã hiện lại — sẵn sàng nhận file", isError: false)
        }
    }

    /// Máy chủ nhận file qua web: địa chỉ đang mở (nil = đang tắt) và các máy đã kết nối.
    @Published private(set) var webServerURL: String?
    @Published private(set) var webClients: [WebClientInfo] = []

    /// Được gọi khi tiến trình thay đổi để cập nhật icon trên thanh menu (nil = không truyền).
    var onTransferProgressChanged: ((Double?) -> Void)?
    var onHiddenChanged: ((Bool) -> Void)?

    let deviceName: String
    private let engine: FileDropEngine
    private let defaults = UserDefaults.standard
    private var incomingQueue: [TransferDescriptor] = []
    private var isPresentingIncoming = false
    private var webRequestQueue: [WebClientInfo] = []
    private var noteTimer: Timer?
    private var searchTimer: Timer?

    private enum Keys {
        static let deviceId = "deviceId"
        static let selectedPeerId = "selectedPeerId"
        static let selectedPeerName = "selectedPeerName"
        static let hidden = "hidden"
    }

    init() {
        let deviceId: String
        if let saved = defaults.string(forKey: Keys.deviceId) {
            deviceId = saved
        } else {
            deviceId = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased().prefix(16).description
            defaults.set(deviceId, forKey: Keys.deviceId)
        }
        deviceName = Host.current().localizedName ?? ProcessInfo.processInfo.hostName
        let downloads = FileManager.default.urls(for: .downloadsDirectory, in: .userDomainMask).first?.path
            ?? NSHomeDirectory() + "/Downloads"
        engine = FileDropEngine(deviceId: deviceId, deviceName: deviceName, downloadDir: downloads)
        selectedPeerId = defaults.string(forKey: Keys.selectedPeerId)
        engine.isHidden = isHidden  // đặt trước khi start để ngay gói đầu tiên đã ở chế độ ẩn
        engine.delegate = self
    }

    func start() {
        do {
            try engine.start()
            markSearching()
        } catch {
            show(note: "Không khởi động được mạng: \(error.localizedDescription)", isError: true, sticky: true)
        }
    }

    func stop() {
        engine.stopWebServer()
        engine.stop()
    }

    // MARK: - Hành động từ UI

    func refresh() {
        engine.discover()
        markSearching()
    }

    func select(_ peer: PeerDevice) {
        selectedPeerId = peer.id
        defaults.set(peer.id, forKey: Keys.selectedPeerId)
        defaults.set(peer.name, forKey: Keys.selectedPeerName)
    }

    var selectedPeer: PeerDevice? { peers.first { $0.id == selectedPeerId } }

    /// Tên máy đích đã ghi nhớ (kể cả khi máy đó đang offline).
    var rememberedPeerName: String? { selectedPeer?.name ?? defaults.string(forKey: Keys.selectedPeerName) }

    /// Phiên truyền hiển thị trên vùng thả file (phiên mới nhất).
    var currentTransfer: ActiveTransfer? { transfers.values.max { $0.startedAt < $1.startedAt } }

    var canDrop: Bool { selectedPeer != nil }

    func send(urls: [URL]) {
        guard let peer = selectedPeer else {
            show(note: "Hãy chọn một máy để gửi trước", isError: true)
            return
        }
        let paths = urls.filter { $0.isFileURL }.map { $0.path }
        guard !paths.isEmpty else { return }
        do {
            try engine.send(paths: paths, to: peer.id)
        } catch {
            show(note: error.localizedDescription, isError: true)
        }
    }

    func cancel(_ transferId: String) { engine.cancel(transferId) }

    // MARK: - Máy chủ nhận file qua web

    func setWebServer(_ on: Bool) {
        if on {
            guard webServerURL == nil else { return }
            do {
                webServerURL = try engine.startWebServer()
                show(note: "Đã bật máy chủ nhận file — mở địa chỉ trên trình duyệt của máy khác", isError: false)
                // Trang web gợi ý máy khách tải FileDrop: bản cho nền tảng khác có ngay, bản macOS sau khi nén xong.
                let dirs = AppPackages.packageDirs
                engine.setWebDownloads([], packageDirs: dirs)
                AppPackages.prepareMacZip { [weak self] zip in
                    guard let self, let zip, self.webServerURL != nil else { return }
                    self.engine.setWebDownloads(
                        [WebDownloadItem(platform: "macos", path: zip.path, fileName: AppPackages.macZipName)],
                        packageDirs: dirs)
                }
            } catch {
                show(note: "Không bật được máy chủ: \(error.localizedDescription)", isError: true)
            }
        } else {
            guard webServerURL != nil else { return }
            engine.stopWebServer()
            webServerURL = nil
            webClients.removeAll()
            webRequestQueue.removeAll()
            show(note: "Đã tắt máy chủ nhận file", isError: false)
        }
    }

    func copyWebServerURL() {
        guard let url = webServerURL else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(url, forType: .string)
        show(note: "Đã chép địa chỉ", isError: false)
    }

    func openWebServerURL() {
        if let s = webServerURL, let url = URL(string: s) { NSWorkspace.shared.open(url) }
    }

    func disconnectWebClient(_ id: String) { engine.disconnectWebClient(id) }

    // MARK: - FileDropEngineDelegate

    func engine(peerUpdated peer: PeerDevice) {
        if let i = peers.firstIndex(where: { $0.id == peer.id }) {
            peers[i] = peer
        } else {
            peers.append(peer)
            peers.sort { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
        }
        if selectedPeerId == peer.id { defaults.set(peer.name, forKey: Keys.selectedPeerName) }
    }

    func engine(peerLost peerId: String) {
        peers.removeAll { $0.id == peerId }
    }

    func engine(incomingRequest info: TransferDescriptor) {
        incomingQueue.append(info)
        presentNextIncoming()
    }

    func engine(transfer info: TransferDescriptor, phase: TransferPhase) {
        var t = transfers[info.id] ?? ActiveTransfer(id: info.id, direction: info.direction, peerName: info.peer.name,
                                                     itemsDescription: info.itemsDescription, phase: phase, progress: 0)
        t.phase = phase
        transfers[info.id] = t
        publishProgress()
    }

    func engine(transfer info: TransferDescriptor, progress done: UInt64, total: UInt64) {
        guard var t = transfers[info.id] else { return }
        t.phase = .transferring
        t.progress = total == 0 ? 1 : Double(done) / Double(total)
        transfers[info.id] = t
        publishProgress()
    }

    func engine(transfer info: TransferDescriptor, finished outcome: TransferOutcome) {
        transfers.removeValue(forKey: info.id)
        incomingQueue.removeAll { $0.id == info.id }
        publishProgress()

        let peer = info.peer.name
        switch (outcome.status, info.direction) {
        case (.completed, .send):
            show(note: "Đã gửi xong \(info.itemsDescription) tới \(peer)", isError: false)
            Notifier.post(title: "Đã gửi xong", body: "\(info.itemsDescription) đã được gửi tới \(peer).")
        case (.completed, .receive):
            show(note: "Đã nhận \(info.itemsDescription) từ \(peer)", isError: false)
            Notifier.post(title: "Đã nhận xong", body: "\(info.itemsDescription) từ \(peer) đã lưu vào Downloads.",
                          revealPath: outcome.firstSavedPath.isEmpty ? outcome.saveDir : outcome.firstSavedPath)
        case (.declined, .send):
            show(note: "\(peer) đã từ chối nhận file", isError: true)
        case (.declined, .receive):
            break
        case (.cancelled, _):
            show(note: outcome.message.isEmpty ? "Đã hủy" : outcome.message, isError: true)
        case (.timedOut, _), (.failed, _):
            show(note: outcome.message.isEmpty ? "Truyền file thất bại" : outcome.message, isError: true)
            if info.direction == .receive || outcome.status == .failed {
                Notifier.post(title: "Truyền file thất bại", body: outcome.message)
            }
        }
    }

    func engine(webClientChanged client: WebClientInfo) {
        webClients.removeAll { $0.id == client.id }
        switch client.state {
        case .pending:
            webRequestQueue.append(client)
            presentNextIncoming()
        case .connected:
            webClients.append(client)
            show(note: "\(client.name) đã kết nối — có thể gửi file qua trình duyệt", isError: false)
        case .declined:
            webRequestQueue.removeAll { $0.id == client.id }
        case .disconnected:
            webRequestQueue.removeAll { $0.id == client.id }
        }
    }

    // MARK: - Hỏi người dùng khi có yêu cầu nhận file / máy khách web xin kết nối

    private func presentNextIncoming() {
        guard !isPresentingIncoming else { return }
        if incomingQueue.isEmpty, !webRequestQueue.isEmpty {
            presentWebRequest(webRequestQueue.removeFirst())
            return
        }
        guard !incomingQueue.isEmpty else { return }
        let info = incomingQueue.removeFirst()
        isPresentingIncoming = true

        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.icon = NSImage(systemSymbolName: "arrow.down.circle.fill", accessibilityDescription: nil)
        alert.messageText = "Nhận file từ \(info.peer.name)?"
        let size = ByteCountFormatter.string(fromByteCount: Int64(info.totalBytes), countStyle: .file)
        alert.informativeText = "\(info.peer.name) muốn gửi cho bạn \(info.itemsDescription) (\(size))."
        alert.addButton(withTitle: "Chấp nhận")
        alert.addButton(withTitle: "Từ chối")
        let accepted = alert.runModal() == .alertFirstButtonReturn

        if !engine.respond(to: info.id, accept: accepted), accepted {
            show(note: "Yêu cầu từ \(info.peer.name) đã hết hạn", isError: true)
        }
        isPresentingIncoming = false
        DispatchQueue.main.async { [weak self] in self?.presentNextIncoming() }
    }

    private func presentWebRequest(_ client: WebClientInfo) {
        isPresentingIncoming = true
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.icon = NSImage(systemSymbolName: "globe", accessibilityDescription: nil)
        alert.messageText = "Cho phép “\(client.name)” kết nối?"
        alert.informativeText = "Máy \(client.ip) muốn gửi file cho bạn qua trình duyệt web. "
            + "Nếu đồng ý, máy đó có thể gửi file nhiều lần cho tới khi bạn ngắt kết nối hoặc tắt máy chủ."
        alert.addButton(withTitle: "Chấp nhận")
        alert.addButton(withTitle: "Từ chối")
        let accepted = alert.runModal() == .alertFirstButtonReturn

        if !engine.respondWebClient(client.id, accept: accepted), accepted {
            show(note: "\(client.name) đã rời đi", isError: true)
        }
        isPresentingIncoming = false
        DispatchQueue.main.async { [weak self] in self?.presentNextIncoming() }
    }

    // MARK: - Tiện ích

    private func publishProgress() {
        onTransferProgressChanged?(currentTransfer.map { $0.phase == .transferring ? $0.progress : 0 })
    }

    private func markSearching() {
        isSearching = true
        searchTimer?.invalidate()
        searchTimer = Timer.scheduledTimer(withTimeInterval: 3, repeats: false) { [weak self] _ in
            self?.isSearching = false
        }
    }

    private func show(note text: String, isError: Bool, sticky: Bool = false) {
        note = StatusNote(text: text, isError: isError)
        noteTimer?.invalidate()
        guard !sticky else { return }
        noteTimer = Timer.scheduledTimer(withTimeInterval: 6, repeats: false) { [weak self] _ in
            self?.note = nil
        }
    }
}
