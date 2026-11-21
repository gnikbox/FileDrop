import Foundation

// Cầu nối Swift <-> C API của lõi C++ (shared/include/filedrop/filedrop_c.h).

enum TransferDirection: Int32 { case send = 0, receive = 1 }
enum TransferPhase: Int32 { case waitingForAccept = 0, connecting = 1, transferring = 2 }
enum TransferStatus: Int32 { case completed = 0, declined = 1, failed = 2, cancelled = 3, timedOut = 4 }

struct PeerDevice: Identifiable, Hashable {
    let id: String
    let name: String
    let os: String
    let ip: String
    let port: UInt16

    init(_ c: fd_peer) {
        id = String(cString: c.id)
        name = String(cString: c.name)
        os = String(cString: c.os)
        ip = String(cString: c.ip)
        port = c.port
    }
}

struct TransferDescriptor {
    let id: String
    let direction: TransferDirection
    let peer: PeerDevice
    let fileCount: Int
    let fileName: String
    let totalBytes: UInt64

    init(_ c: fd_transfer_info) {
        id = String(cString: c.transfer_id)
        direction = TransferDirection(rawValue: c.direction) ?? .send
        peer = PeerDevice(c.peer)
        fileCount = Int(c.file_count)
        fileName = String(cString: c.file_name)
        totalBytes = c.total_bytes
    }

    /// "“ảnh.jpg”" hoặc "3 file"
    var itemsDescription: String {
        fileCount == 1 && !fileName.isEmpty ? "“\(fileName)”" : "\(fileCount) file"
    }
}

struct TransferOutcome {
    let status: TransferStatus
    let message: String
    let saveDir: String
    let firstSavedPath: String
}

protocol FileDropEngineDelegate: AnyObject {
    func engine(peerUpdated peer: PeerDevice)
    func engine(peerLost peerId: String)
    func engine(incomingRequest info: TransferDescriptor)
    func engine(transfer info: TransferDescriptor, phase: TransferPhase)
    func engine(transfer info: TransferDescriptor, progress done: UInt64, total: UInt64)
    func engine(transfer info: TransferDescriptor, finished outcome: TransferOutcome)
}

/// Bọc fd_engine. Delegate luôn được gọi trên main thread.
final class FileDropEngine {
    weak var delegate: FileDropEngineDelegate?
    private var handle: OpaquePointer?

    init(deviceId: String, deviceName: String, downloadDir: String) {
        var cb = fd_callbacks()
        cb.context = Unmanaged.passUnretained(self).toOpaque()
        cb.on_peer_updated = { ctx, peer in
            guard let ctx, let peer else { return }
            let p = PeerDevice(peer.pointee)
            FileDropEngine.from(ctx).onMain { $0.delegate?.engine(peerUpdated: p) }
        }
        cb.on_peer_lost = { ctx, id in
            guard let ctx, let id else { return }
            let peerId = String(cString: id)
            FileDropEngine.from(ctx).onMain { $0.delegate?.engine(peerLost: peerId) }
        }
        cb.on_incoming_request = { ctx, info in
            guard let ctx, let info else { return }
            let t = TransferDescriptor(info.pointee)
            FileDropEngine.from(ctx).onMain { $0.delegate?.engine(incomingRequest: t) }
        }
        cb.on_transfer_phase = { ctx, info, phase in
            guard let ctx, let info else { return }
            let t = TransferDescriptor(info.pointee)
            let ph = TransferPhase(rawValue: phase) ?? .connecting
            FileDropEngine.from(ctx).onMain { $0.delegate?.engine(transfer: t, phase: ph) }
        }
        cb.on_progress = { ctx, info, done, total in
            guard let ctx, let info else { return }
            let t = TransferDescriptor(info.pointee)
            FileDropEngine.from(ctx).onMain { $0.delegate?.engine(transfer: t, progress: done, total: total) }
        }
        cb.on_transfer_finished = { ctx, info, result in
            guard let ctx, let info, let result else { return }
            let t = TransferDescriptor(info.pointee)
            let r = result.pointee
            let outcome = TransferOutcome(status: TransferStatus(rawValue: r.status) ?? .failed,
                                          message: String(cString: r.message),
                                          saveDir: String(cString: r.save_dir),
                                          firstSavedPath: String(cString: r.first_saved_path))
            FileDropEngine.from(ctx).onMain { $0.delegate?.engine(transfer: t, finished: outcome) }
        }
        handle = fd_engine_create(deviceId, deviceName, "macos", downloadDir, cb)
    }

    deinit {
        if let handle { fd_engine_destroy(handle) }
    }

    private static func from(_ ctx: UnsafeMutableRawPointer) -> FileDropEngine {
        Unmanaged<FileDropEngine>.fromOpaque(ctx).takeUnretainedValue()
    }

    private func onMain(_ body: @escaping (FileDropEngine) -> Void) {
        DispatchQueue.main.async { [weak self] in
            if let self { body(self) }
        }
    }

    func start() throws {
        var err = [CChar](repeating: 0, count: 512)
        if fd_engine_start(handle, &err, err.count) == 0 {
            throw NSError(domain: "FileDrop", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: String(cString: err)])
        }
    }

    func stop() { fd_engine_stop(handle) }
    func discover() { fd_engine_discover(handle) }

    @discardableResult
    func send(paths: [String], to peerId: String) throws -> String {
        let cStrings: [UnsafeMutablePointer<CChar>?] = paths.map { strdup($0) }
        defer { cStrings.forEach { free($0) } }
        let pointers: [UnsafePointer<CChar>?] = cStrings.map { UnsafePointer($0) }
        var idBuf = [CChar](repeating: 0, count: 64)
        var errBuf = [CChar](repeating: 0, count: 512)
        let ok = pointers.withUnsafeBufferPointer { buf in
            fd_engine_send_files(handle, peerId, buf.baseAddress, buf.count, &idBuf, idBuf.count, &errBuf, errBuf.count)
        }
        _ = cStrings  // giữ bộ nhớ tới hết lời gọi
        if ok == 0 {
            throw NSError(domain: "FileDrop", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: String(cString: errBuf)])
        }
        return String(cString: idBuf)
    }

    @discardableResult
    func respond(to transferId: String, accept: Bool) -> Bool {
        fd_engine_respond(handle, transferId, accept ? 1 : 0) != 0
    }

    func cancel(_ transferId: String) { fd_engine_cancel(handle, transferId) }
}
