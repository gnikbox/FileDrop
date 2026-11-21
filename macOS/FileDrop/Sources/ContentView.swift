import SwiftUI
import UniformTypeIdentifiers

/// Giao diện trong popover. Bố cục giữ đồng nhất với bản Windows (Windows/src/PopupWindow.cpp):
/// Header → "Thiết bị trong mạng" + lưới máy → vùng thả file → dòng trạng thái → footer.
struct ContentView: View {
    @ObservedObject var model: AppModel
    var onQuit: () -> Void

    @State private var isDropTargeted = false

    private let columns = Array(repeating: GridItem(.flexible(), spacing: 8), count: 3)

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            Text("Thiết bị trong mạng")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(.secondary)
            peerGrid
            dropZone
            statusLine
            Divider()
            footer
        }
        .padding(16)
        .frame(width: 340)
    }

    // MARK: Header

    private var header: some View {
        HStack(spacing: 8) {
            Image(systemName: "dot.radiowaves.left.and.right")
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(Color.accentColor)
            VStack(alignment: .leading, spacing: 0) {
                Text("FileDrop").font(.system(size: 15, weight: .bold))
                Text(model.deviceName).font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer()
            Button(action: model.refresh) {
                Image(systemName: "arrow.clockwise")
                    .font(.system(size: 13, weight: .medium))
                    .frame(width: 28, height: 28)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .background(RoundedRectangle(cornerRadius: 7).fill(Color.primary.opacity(0.06)))
            .help("Tìm lại thiết bị")
        }
    }

    // MARK: Lưới thiết bị

    @ViewBuilder
    private var peerGrid: some View {
        if model.peers.isEmpty {
            VStack(spacing: 8) {
                if model.isSearching {
                    ProgressView().controlSize(.small)
                    Text("Đang tìm thiết bị…").font(.system(size: 12)).foregroundStyle(.secondary)
                } else {
                    Image(systemName: "wifi.exclamationmark").font(.system(size: 20)).foregroundStyle(.secondary)
                    Text("Chưa thấy máy nào cùng mạng Wi-Fi").font(.system(size: 12)).foregroundStyle(.secondary)
                }
            }
            .frame(maxWidth: .infinity, minHeight: 92)
        } else {
            ScrollView {
                LazyVGrid(columns: columns, spacing: 8) {
                    ForEach(model.peers) { peer in
                        PeerTile(peer: peer, selected: peer.id == model.selectedPeerId)
                            .onTapGesture { model.select(peer) }
                    }
                }
            }
            .frame(height: model.peers.count > 3 ? 192 : 92)
        }
    }

    // MARK: Vùng thả file

    private var dropZone: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 12)
                .fill(isDropTargeted ? Color.accentColor.opacity(0.12) : Color.primary.opacity(0.04))
            RoundedRectangle(cornerRadius: 12)
                .strokeBorder(isDropTargeted ? Color.accentColor : Color.secondary.opacity(0.45),
                              style: StrokeStyle(lineWidth: 1.5, dash: [6, 4]))
            if let t = model.currentTransfer {
                transferView(t)
            } else {
                VStack(spacing: 6) {
                    Image(systemName: "arrow.down.doc")
                        .font(.system(size: 26, weight: .regular))
                        .foregroundStyle(model.canDrop ? Color.accentColor : .secondary)
                    Text("Thả file vào đây để gửi").font(.system(size: 13, weight: .semibold))
                    Text(dropSubtitle).font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(1)
                }
                .padding(.horizontal, 12)
            }
        }
        .frame(height: 130)
        .dropDestination(for: URL.self) { urls, _ in
            guard model.canDrop else { return false }
            model.send(urls: urls)
            return true
        } isTargeted: { isDropTargeted = $0 && model.canDrop }
    }

    private var dropSubtitle: String {
        if let peer = model.selectedPeer { return "tới \(peer.name)" }
        if let name = model.rememberedPeerName { return "\(name) hiện không trực tuyến" }
        return "Chọn một máy ở trên trước"
    }

    private func transferView(_ t: ActiveTransfer) -> some View {
        HStack(spacing: 14) {
            ProgressRing(progress: t.phase == .transferring ? t.progress : nil)
                .frame(width: 56, height: 56)
            VStack(alignment: .leading, spacing: 4) {
                Text(t.title).font(.system(size: 12, weight: .semibold)).lineLimit(2)
                Text(t.itemsDescription).font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(1)
                Button("Hủy") { model.cancel(t.id) }
                    .buttonStyle(.link)
                    .font(.system(size: 11))
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16)
    }

    // MARK: Trạng thái + footer

    private var statusLine: some View {
        HStack(spacing: 6) {
            if let note = model.note {
                Image(systemName: note.isError ? "exclamationmark.circle.fill" : "checkmark.circle.fill")
                    .foregroundStyle(note.isError ? Color.orange : Color.green)
                Text(note.text).lineLimit(2)
            } else {
                Text(" ")
            }
        }
        .font(.system(size: 11))
        .frame(maxWidth: .infinity, minHeight: 16, alignment: .leading)
        .animation(.easeInOut(duration: 0.2), value: model.note)
    }

    private var footer: some View {
        HStack {
            Toggle("Khởi động cùng macOS", isOn: $model.launchAtLogin)
                .toggleStyle(.checkbox)
                .font(.system(size: 12))
            Spacer()
            Button("Thoát", action: onQuit)
                .font(.system(size: 12))
        }
    }
}

/// Ô thiết bị trong lưới: icon Mac/PC + tên máy.
struct PeerTile: View {
    let peer: PeerDevice
    let selected: Bool
    @State private var hovering = false

    var body: some View {
        VStack(spacing: 6) {
            Image(systemName: Self.symbol(for: peer.os))
                .font(.system(size: 30, weight: .light))
                .foregroundStyle(selected ? Color.accentColor : .primary)
                .frame(height: 36)
            Text(peer.name)
                .font(.system(size: 11, weight: selected ? .semibold : .regular))
                .lineLimit(2)
                .multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity, minHeight: 84)
        .padding(4)
        .background(
            RoundedRectangle(cornerRadius: 10)
                .fill(selected ? Color.accentColor.opacity(0.15) : Color.primary.opacity(hovering ? 0.07 : 0.035))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 10)
                .strokeBorder(selected ? Color.accentColor : .clear, lineWidth: 1.5)
        )
        .contentShape(RoundedRectangle(cornerRadius: 10))
        .onHover { hovering = $0 }
        .help(peer.os == "macos" ? "Mac" : peer.os == "windows" ? "PC Windows" : peer.os)
    }

    static func symbol(for os: String) -> String {
        switch os {
        case "macos": return "laptopcomputer"
        case "windows": return "pc"
        case "ios": return "iphone"
        case "android": return "candybarphone"
        default: return "desktopcomputer"
        }
    }
}

/// Progress bar hình tròn. progress = nil => vòng xoay chờ.
struct ProgressRing: View {
    let progress: Double?
    @State private var spin = false

    var body: some View {
        ZStack {
            Circle().stroke(Color.primary.opacity(0.1), lineWidth: 6)
            if let progress {
                Circle()
                    .trim(from: 0, to: max(0.001, progress))
                    .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 6, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                    .animation(.linear(duration: 0.15), value: progress)
                Text("\(Int(progress * 100))%")
                    .font(.system(size: 12, weight: .semibold).monospacedDigit())
            } else {
                Circle()
                    .trim(from: 0, to: 0.25)
                    .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 6, lineCap: .round))
                    .rotationEffect(.degrees(spin ? 360 : 0))
                    .animation(.linear(duration: 1).repeatForever(autoreverses: false), value: spin)
                    .onAppear { spin = true }
            }
        }
    }
}
