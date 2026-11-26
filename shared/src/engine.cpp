#include "filedrop/engine.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "filedrop/net.h"
#include "filedrop/transfer.h"
#include "filedrop/util.h"
#include "filedrop/webserver.h"

namespace filedrop {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kRediscoverInterval = std::chrono::seconds(30);
constexpr auto kPeerTimeout = std::chrono::seconds(95);
constexpr auto kRequestTimeout = std::chrono::seconds(120);
constexpr int kAcceptTimeoutMs = 60000;
constexpr int kPollIntervalMs = 300;
constexpr size_t kMaxRememberedRequests = 512;
constexpr int kControlRepeat = 2;  // gửi lặp gói điều khiển quan trọng phòng mất gói UDP

std::string baseName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

}  // namespace

struct Engine::Impl {
    using Event = std::function<void()>;

    struct PeerEntry {
        Peer peer;
        Clock::time_point lastSeen;
    };
    struct Outgoing {
        TransferInfo info;
        std::vector<FileEntry> files;
        Clock::time_point created;
    };
    struct Incoming {
        TransferInfo info;
        Clock::time_point created;
    };
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<std::atomic<bool>> done;
    };

    EngineConfig cfg;
    EngineCallbacks cb;

    mutable std::mutex mu;  // bảo vệ trạng thái bên dưới + cfg.deviceName/downloadDir
    std::map<std::string, PeerEntry> peers;
    std::map<std::string, Outgoing> outgoing;
    std::map<std::string, Incoming> incoming;
    std::map<std::string, Worker> workers;
    std::set<std::string> seenRequests;
    std::deque<std::string> seenOrder;
    Clock::time_point lastDiscover{};

    std::mutex sockMu;  // bảo vệ gửi/đóng socket UDP
    net::Socket discSock;  // nghe broadcast trên port discovery
    net::Socket msgSock;   // gửi/nhận gói điều khiển (port riêng)
    uint16_t msgPort = 0;

    std::atomic<bool> running{false};
    std::atomic<bool> hidden{false};
    std::thread netThread;
    std::unique_ptr<WebServer> web;

    Impl(EngineConfig c, EngineCallbacks callbacks) : cfg(std::move(c)), cb(std::move(callbacks)) {
        WebServerCallbacks wcb;
        wcb.onClientChanged = [this](const WebClient& c) {
            if (cb.onWebClientChanged) cb.onWebClientChanged(c);
        };
        wcb.onTransferPhase = [this](const TransferInfo& i, TransferPhase p) {
            if (cb.onTransferPhase) cb.onTransferPhase(i, p);
        };
        wcb.onProgress = [this](const TransferInfo& i, uint64_t d, uint64_t t) {
            if (cb.onProgress) cb.onProgress(i, d, t);
        };
        wcb.onTransferFinished = [this](const TransferInfo& i, const TransferResult& r) {
            if (cb.onTransferFinished) cb.onTransferFinished(i, r);
        };
        web = std::make_unique<WebServer>(std::move(wcb));
        if (cfg.deviceId.empty()) cfg.deviceId = randomHex(8);
        if (cfg.deviceName.empty()) cfg.deviceName = defaultDeviceName();
        if (cfg.os.empty()) cfg.os = currentOs();
        if (cfg.downloadDir.empty()) cfg.downloadDir = defaultDownloadDir();
        hidden = cfg.hidden;
    }

    static void fire(std::vector<Event>& events) {
        for (auto& e : events) {
            if (e) e();
        }
        events.clear();
    }

    Event finishedEvent(const TransferInfo& info, TransferStatus status, std::string message) {
        TransferResult r;
        r.status = status;
        r.message = std::move(message);
        return [this, info, r] {
            if (cb.onTransferFinished) cb.onTransferFinished(info, r);
        };
    }

    void emitPhase(const TransferInfo& info, TransferPhase phase) {
        if (cb.onTransferPhase) cb.onTransferPhase(info, phase);
    }

    // ---- Gửi gói UDP ----

    Message identity(MessageType type, const std::string& towardIp = {}) {
        Message m(type);
        std::string name;
        {
            std::lock_guard<std::mutex> l(mu);
            name = cfg.deviceName;
        }
        std::string ip = towardIp.empty() ? std::string() : net::localIpFor(towardIp);
        if (ip.empty()) ip = net::primaryLocalIp();
        m.set(field::kId, cfg.deviceId);
        m.set(field::kName, name);
        m.set(field::kOs, cfg.os);
        m.set(field::kIp, ip);
        m.setUInt(field::kPort, msgPort);
        if (hidden) m.setInt(field::kHidden, 1);
        return m;
    }

    void sendTo(const std::string& ip, uint16_t port, const Message& m, int repeat = 1) {
        const std::string data = m.encode();
        std::lock_guard<std::mutex> l(sockMu);
        if (!msgSock.valid()) return;
        for (int i = 0; i < repeat; ++i) net::sendTo(msgSock, ip, port, data.data(), data.size());
    }

    void broadcast(const Message& m) {
        for (const auto& addr : net::broadcastAddresses()) sendTo(addr, cfg.discoveryPort, m);
    }

    // ---- Vòng lặp mạng ----

    void netLoop() {
        std::vector<char> buf(kMaxDatagramSize + 1);
        const std::vector<net::socket_t> fds{discSock.fd(), msgSock.fd()};
        while (running) {
            std::vector<bool> ready;
            if (net::waitReadable(fds, kPollIntervalMs, &ready) > 0) {
                for (size_t i = 0; i < fds.size(); ++i) {
                    if (!ready[i]) continue;
                    net::Endpoint from;
                    int n = net::recvFrom(i == 0 ? discSock : msgSock, buf.data(), buf.size(), &from);
                    if (n > 0) handlePacket(std::string_view(buf.data(), static_cast<size_t>(n)), from);
                }
            }
            housekeeping();
        }
    }

    void handlePacket(std::string_view data, const net::Endpoint& from) {
        auto msg = Message::decode(data);
        if (!msg) return;
        const std::string senderId = msg->get(field::kId);
        if (senderId.empty() || senderId == cfg.deviceId) return;  // bỏ qua gói của chính mình

        std::vector<Event> events;
        switch (msg->type()) {
            case MessageType::Discover: {
                // Máy hỏi đang ẩn: vẫn trả lời (để nó thấy mình) nhưng không đưa nó vào danh sách.
                Peer p = senderHidden(*msg) ? peerFrom(*msg, from) : upsertPeer(*msg, from, events);
                if (!hidden) sendTo(p.ip, p.port, identity(MessageType::Announce, p.ip));
                break;
            }
            case MessageType::Announce:
                if (!senderHidden(*msg)) upsertPeer(*msg, from, events);
                break;
            case MessageType::Bye: {
                std::lock_guard<std::mutex> l(mu);
                if (peers.erase(senderId)) {
                    events.push_back([this, senderId] {
                        if (cb.onPeerLost) cb.onPeerLost(senderId);
                    });
                }
                break;
            }
            case MessageType::SendRequest:
                handleSendRequest(*msg, from, events);
                break;
            case MessageType::SendResponse:
                handleSendResponse(*msg, from, events);
                break;
            case MessageType::Cancel:
                handleCancel(*msg, events);
                break;
        }
        fire(events);
    }

    static bool senderHidden(const Message& msg) { return msg.getInt(field::kHidden) == 1; }

    static Peer peerFrom(const Message& msg, const net::Endpoint& from) {
        Peer p;
        p.id = msg.get(field::kId);
        p.name = msg.get(field::kName, from.ip);
        p.os = msg.get(field::kOs, "unknown");
        p.ip = from.ip;  // IP nguồn của gói tin là đáng tin nhất
        p.port = static_cast<uint16_t>(msg.getUInt(field::kPort, from.port));
        if (p.port == 0) p.port = from.port;
        return p;
    }

    Peer upsertPeer(const Message& msg, const net::Endpoint& from, std::vector<Event>& events) {
        Peer p = peerFrom(msg, from);
        std::lock_guard<std::mutex> l(mu);
        auto it = peers.find(p.id);
        bool changed = it == peers.end() || it->second.peer != p;
        peers[p.id] = PeerEntry{p, Clock::now()};
        if (changed) {
            events.push_back([this, p] {
                if (cb.onPeerUpdated) cb.onPeerUpdated(p);
            });
        }
        return p;
    }

    void rememberRequest(const std::string& id) {
        seenRequests.insert(id);
        seenOrder.push_back(id);
        while (seenOrder.size() > kMaxRememberedRequests) {
            seenRequests.erase(seenOrder.front());
            seenOrder.pop_front();
        }
    }

    void handleSendRequest(const Message& msg, const net::Endpoint& from, std::vector<Event>& events) {
        Peer p = senderHidden(msg) ? peerFrom(msg, from) : upsertPeer(msg, from, events);
        const std::string req = msg.get(field::kRequest);
        if (req.empty()) return;

        if (hidden) {  // Đang tạm ẩn => tự động từ chối, không hỏi người dùng.
            {
                std::lock_guard<std::mutex> l(mu);
                if (seenRequests.count(req)) return;
                rememberRequest(req);
            }
            Message m = identity(MessageType::SendResponse, p.ip);
            m.set(field::kRequest, req);
            m.setInt(field::kAccept, 0);
            m.set(field::kReason, "hidden");
            sendTo(p.ip, p.port, m, kControlRepeat);
            return;
        }

        TransferInfo info;
        info.id = req;
        info.direction = Direction::Receive;
        info.peer = p;
        info.fileCount = static_cast<int>(msg.getInt(field::kCount, 1));
        info.fileName = msg.get(field::kFile);
        info.totalBytes = msg.getUInt(field::kSize);
        {
            std::lock_guard<std::mutex> l(mu);
            if (seenRequests.count(req)) return;  // gói lặp
            rememberRequest(req);
            incoming[req] = Incoming{info, Clock::now()};
        }
        events.push_back([this, info] {
            if (cb.onIncomingRequest) cb.onIncomingRequest(info);
        });
    }

    void handleSendResponse(const Message& msg, const net::Endpoint& from, std::vector<Event>& events) {
        const std::string req = msg.get(field::kRequest);
        Outgoing out;
        {
            std::lock_guard<std::mutex> l(mu);
            auto it = outgoing.find(req);
            if (it == outgoing.end() || it->second.info.peer.id != msg.get(field::kId)) return;
            out = std::move(it->second);
            outgoing.erase(it);
        }
        if (msg.getInt(field::kAccept) != 1) {
            const bool wasHidden = msg.get(field::kReason) == "hidden";
            events.push_back(finishedEvent(out.info, TransferStatus::Declined,
                                           out.info.peer.name + (wasHidden ? " đang tạm ẩn, không nhận file"
                                                                           : " đã từ chối nhận file")));
            return;
        }
        const std::string ip = msg.get(field::kIp, from.ip);
        const auto port = static_cast<uint16_t>(msg.getUInt(field::kTcpPort));
        const std::string password = msg.get(field::kPassword);
        if (port == 0 || password.size() != kPasswordLength) {
            events.push_back(finishedEvent(out.info, TransferStatus::Failed, "Phản hồi không hợp lệ từ máy nhận"));
            return;
        }
        startSender(std::move(out), ip.empty() ? from.ip : ip, port, password);
    }

    void handleCancel(const Message& msg, std::vector<Event>& events) {
        const std::string req = msg.get(field::kRequest);
        const std::string sender = msg.get(field::kId);
        std::lock_guard<std::mutex> l(mu);
        if (auto it = incoming.find(req); it != incoming.end() && it->second.info.peer.id == sender) {
            events.push_back(finishedEvent(it->second.info, TransferStatus::Cancelled, "Máy gửi đã hủy yêu cầu"));
            incoming.erase(it);
        } else if (auto ot = outgoing.find(req); ot != outgoing.end() && ot->second.info.peer.id == sender) {
            events.push_back(finishedEvent(ot->second.info, TransferStatus::Cancelled, "Máy nhận đã hủy"));
            outgoing.erase(ot);
        } else if (auto wt = workers.find(req); wt != workers.end()) {
            wt->second.cancel->store(true);
        }
    }

    void housekeeping() {
        const auto now = Clock::now();
        std::vector<Event> events;
        bool doDiscover = false;
        {
            std::lock_guard<std::mutex> l(mu);
            if (now - lastDiscover >= kRediscoverInterval) {
                lastDiscover = now;
                doDiscover = true;
            }
            for (auto it = peers.begin(); it != peers.end();) {
                if (now - it->second.lastSeen > kPeerTimeout) {
                    std::string id = it->first;
                    events.push_back([this, id] {
                        if (cb.onPeerLost) cb.onPeerLost(id);
                    });
                    it = peers.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = outgoing.begin(); it != outgoing.end();) {
                if (now - it->second.created > kRequestTimeout) {
                    events.push_back(finishedEvent(it->second.info, TransferStatus::TimedOut,
                                                   it->second.info.peer.name + " không phản hồi"));
                    it = outgoing.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = incoming.begin(); it != incoming.end();) {
                if (now - it->second.created > kRequestTimeout) {
                    events.push_back(finishedEvent(it->second.info, TransferStatus::TimedOut, "Yêu cầu đã hết hạn"));
                    it = incoming.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = workers.begin(); it != workers.end();) {
                if (it->second.done->load()) {
                    if (it->second.thread.joinable()) it->second.thread.join();
                    it = workers.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (doDiscover) broadcast(identity(MessageType::Discover));
        fire(events);
    }

    // ---- Luồng truyền file ----

    void addWorker(const std::string& id, std::shared_ptr<std::atomic<bool>> cancel,
                   std::shared_ptr<std::atomic<bool>> done, std::thread thread) {
        std::lock_guard<std::mutex> l(mu);
        auto& w = workers[id];
        if (w.thread.joinable()) w.thread.detach();  // không nên xảy ra (ID trùng)
        w = Worker{std::move(thread), std::move(cancel), std::move(done)};
    }

    static TransferResult toResult(const TransferOutcome& outcome, bool cancelled) {
        TransferResult r;
        if (outcome.ok) {
            r.status = TransferStatus::Completed;
        } else {
            r.status = cancelled ? TransferStatus::Cancelled : TransferStatus::Failed;
            r.message = outcome.error;
        }
        r.savedPaths = outcome.savedPaths;
        return r;
    }

    void startSender(Outgoing out, std::string ip, uint16_t port, std::string password) {
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto files = std::make_shared<std::vector<FileEntry>>(std::move(out.files));
        TransferInfo info = out.info;
        std::thread t([this, info, files, ip, port, password, cancel, done] {
            emitPhase(info, TransferPhase::Connecting);
            bool started = false;
            auto outcome = runSender(
                ip, port, password, *files,
                [&](uint64_t d, uint64_t total) {
                    if (!started) {
                        started = true;
                        emitPhase(info, TransferPhase::Transferring);
                    }
                    if (cb.onProgress) cb.onProgress(info, d, total);
                },
                *cancel);
            TransferResult r = toResult(outcome, *cancel);
            if (cb.onTransferFinished) cb.onTransferFinished(info, r);
            done->store(true);
        });
        addWorker(info.id, cancel, done, std::move(t));
    }

    void startReceiver(const TransferInfo& info, net::Socket listener, std::string password) {
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto listenerPtr = std::make_shared<net::Socket>(std::move(listener));
        std::string dir;
        {
            std::lock_guard<std::mutex> l(mu);
            dir = cfg.downloadDir;
        }
        std::thread t([this, info, listenerPtr, password, dir, cancel, done] {
            emitPhase(info, TransferPhase::Connecting);
            auto outcome = runReceiver(
                *listenerPtr, password, dir, kAcceptTimeoutMs,
                [&] { emitPhase(info, TransferPhase::Transferring); },
                [&](uint64_t d, uint64_t total) {
                    if (cb.onProgress) cb.onProgress(info, d, total);
                },
                *cancel);
            TransferResult r = toResult(outcome, *cancel);
            r.saveDir = dir;
            if (cb.onTransferFinished) cb.onTransferFinished(info, r);
            done->store(true);
        });
        addWorker(info.id, cancel, done, std::move(t));
    }
};

// ---------------------------------------------------------------------------

Engine::Engine(EngineConfig config, EngineCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(callbacks))) {}

Engine::~Engine() {
    stop();
    stopWebServer();
}

bool Engine::start(std::string* error) {
    if (impl_->running) return true;
    net::initialize();
    {
        std::lock_guard<std::mutex> l(impl_->sockMu);
        impl_->discSock = net::openUdp(impl_->cfg.discoveryPort, true, error);
        if (!impl_->discSock.valid()) return false;
        impl_->msgSock = net::openUdp(0, false, error);
        if (!impl_->msgSock.valid()) {
            impl_->discSock.close();
            return false;
        }
        impl_->msgPort = net::localPort(impl_->msgSock);
    }
    impl_->lastDiscover = Clock::now();
    impl_->running = true;
    impl_->netThread = std::thread([this] { impl_->netLoop(); });
    discover();
    return true;
}

void Engine::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->broadcast(impl_->identity(MessageType::Bye));
    if (impl_->netThread.joinable()) impl_->netThread.join();

    std::map<std::string, Impl::Worker> workers;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        for (auto& [id, w] : impl_->workers) w.cancel->store(true);
        workers.swap(impl_->workers);
        impl_->outgoing.clear();
        impl_->incoming.clear();
        impl_->peers.clear();
    }
    for (auto& [id, w] : workers) {
        if (w.thread.joinable()) w.thread.join();
    }
    std::lock_guard<std::mutex> l(impl_->sockMu);
    impl_->discSock.close();
    impl_->msgSock.close();
}

bool Engine::isRunning() const { return impl_->running; }

void Engine::discover() {
    if (!impl_->running) return;
    impl_->broadcast(impl_->identity(MessageType::Discover));
}

std::vector<Peer> Engine::peers() const {
    std::lock_guard<std::mutex> l(impl_->mu);
    std::vector<Peer> out;
    for (const auto& [id, entry] : impl_->peers) out.push_back(entry.peer);
    return out;
}

std::string Engine::sendFiles(const std::string& peerId, const std::vector<std::string>& paths, std::string* error) {
    auto setError = [&](const std::string& e) {
        if (error) *error = e;
        return std::string();
    };
    auto files = collectFiles(paths);
    if (files.empty()) return setError("Không có file nào hợp lệ để gửi");
    return sendEntries(peerId, std::move(files), error);
}

std::string Engine::sendEntries(const std::string& peerId, std::vector<FileEntry> files, std::string* error) {
    auto setError = [&](const std::string& e) {
        if (error) *error = e;
        return std::string();
    };
    if (!impl_->running) return setError("FileDrop chưa chạy");
    if (files.empty()) return setError("Không có file nào hợp lệ để gửi");

    Peer peer;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        auto it = impl_->peers.find(peerId);
        if (it == impl_->peers.end()) return setError("Không tìm thấy thiết bị nhận trong mạng");
        peer = it->second.peer;
    }

    TransferInfo info;
    info.id = randomHex(8);
    info.direction = Direction::Send;
    info.peer = peer;
    info.fileCount = static_cast<int>(files.size());
    if (files.size() == 1) info.fileName = baseName(files.front().relPath);
    for (const auto& f : files) info.totalBytes += f.size;

    {
        std::lock_guard<std::mutex> l(impl_->mu);
        impl_->outgoing[info.id] = Impl::Outgoing{info, std::move(files), Clock::now()};
    }

    Message m = impl_->identity(MessageType::SendRequest, peer.ip);
    m.set(field::kRequest, info.id);
    m.setInt(field::kCount, info.fileCount);
    if (!info.fileName.empty()) m.set(field::kFile, info.fileName);
    m.setUInt(field::kSize, info.totalBytes);
    impl_->sendTo(peer.ip, peer.port, m, kControlRepeat);

    impl_->emitPhase(info, TransferPhase::WaitingForAccept);
    return info.id;
}

bool Engine::respond(const std::string& transferId, bool accept) {
    TransferInfo info;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        auto it = impl_->incoming.find(transferId);
        if (it == impl_->incoming.end()) return false;
        info = it->second.info;
        impl_->incoming.erase(it);
    }

    Message m = impl_->identity(MessageType::SendResponse, info.peer.ip);
    m.set(field::kRequest, transferId);

    std::vector<Impl::Event> events;
    if (accept) {
        std::string err;
        net::Socket listener = net::listenTcp(0, &err);
        if (listener.valid()) {
            const uint16_t tcpPort = net::localPort(listener);
            const std::string password = randomToken(kPasswordLength);
            std::string ip = net::localIpFor(info.peer.ip);
            if (ip.empty()) ip = net::primaryLocalIp();

            impl_->startReceiver(info, std::move(listener), password);

            m.setInt(field::kAccept, 1);
            m.set(field::kIp, ip);
            m.setUInt(field::kTcpPort, tcpPort);
            m.set(field::kPassword, password);
            impl_->sendTo(info.peer.ip, info.peer.port, m, kControlRepeat);
            return true;
        }
        events.push_back(impl_->finishedEvent(info, TransferStatus::Failed, "Không mở được cổng TCP: " + err));
    } else {
        events.push_back(impl_->finishedEvent(info, TransferStatus::Declined, "Đã từ chối nhận file"));
    }
    m.setInt(field::kAccept, 0);
    impl_->sendTo(info.peer.ip, info.peer.port, m, kControlRepeat);
    Impl::fire(events);
    return true;
}

void Engine::cancel(const std::string& transferId) {
    if (impl_->web->cancel(transferId)) return;
    std::vector<Impl::Event> events;
    Peer notify;
    bool declineIncoming = false;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        if (auto it = impl_->outgoing.find(transferId); it != impl_->outgoing.end()) {
            notify = it->second.info.peer;
            events.push_back(impl_->finishedEvent(it->second.info, TransferStatus::Cancelled, "Đã hủy"));
            impl_->outgoing.erase(it);
        } else if (auto in = impl_->incoming.find(transferId); in != impl_->incoming.end()) {
            declineIncoming = true;
        } else if (auto wt = impl_->workers.find(transferId); wt != impl_->workers.end()) {
            wt->second.cancel->store(true);
        }
    }
    if (declineIncoming) {
        respond(transferId, false);
        return;
    }
    if (!notify.id.empty()) {
        Message m = impl_->identity(MessageType::Cancel, notify.ip);
        m.set(field::kRequest, transferId);
        impl_->sendTo(notify.ip, notify.port, m, kControlRepeat);
    }
    Impl::fire(events);
}

void Engine::setHidden(bool hidden) {
    if (impl_->hidden.exchange(hidden) == hidden || !impl_->running) return;
    // Ẩn: báo BYE để các máy khác xoá mình. Hiện lại: hỏi danh tính để các máy khác thêm mình vào.
    impl_->broadcast(impl_->identity(hidden ? MessageType::Bye : MessageType::Discover));
}

bool Engine::isHidden() const { return impl_->hidden; }

void Engine::setDeviceName(const std::string& name) {
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        impl_->cfg.deviceName = name.empty() ? defaultDeviceName() : name;
    }
    impl_->web->setHostName(name);
    discover();  // thông báo tên mới cho các máy khác
}

void Engine::setDownloadDir(const std::string& dir) {
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        impl_->cfg.downloadDir = dir.empty() ? defaultDownloadDir() : dir;
    }
    impl_->web->setDownloadDir(dir);
}

bool Engine::startWebServer(std::string* error) {
    std::string name, dir;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        name = impl_->cfg.deviceName;
        dir = impl_->cfg.downloadDir;
    }
    return impl_->web->start(name, dir, error);
}

void Engine::stopWebServer() { impl_->web->stop(); }
bool Engine::isWebServerRunning() const { return impl_->web->isRunning(); }
std::string Engine::webServerUrl() const { return impl_->web->url(); }
bool Engine::respondWebClient(const std::string& clientId, bool accept) { return impl_->web->respond(clientId, accept); }
void Engine::disconnectWebClient(const std::string& clientId) { impl_->web->disconnect(clientId); }
std::vector<WebClient> Engine::webClients() const { return impl_->web->clients(); }
void Engine::setWebDownloads(std::vector<WebDownload> downloads, std::vector<std::string> packageDirs) {
    impl_->web->setDownloads(std::move(downloads), std::move(packageDirs));
}

std::string Engine::deviceId() const { return impl_->cfg.deviceId; }

std::string Engine::deviceName() const {
    std::lock_guard<std::mutex> l(impl_->mu);
    return impl_->cfg.deviceName;
}

uint16_t Engine::controlPort() const { return impl_->msgPort; }

}  // namespace filedrop
