// fdcli — công cụ dòng lệnh để thử nghiệm lõi FileDrop (không có UI).
//
//   fdcli list                         liệt kê thiết bị trong mạng
//   fdcli listen [--yes|--no]          chờ nhận file (mặc định hỏi y/n trên terminal)
//   fdcli send <tên máy|IP> <file...>  gửi file
// Tuỳ chọn chung: --name <tên>  --id <id>  --dir <thư mục nhận>  --port <port discovery>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "filedrop/engine.h"

using namespace filedrop;

namespace {

std::mutex gMu;
std::condition_variable gCv;
bool gDone = false;
bool gOk = false;

void usage() {
    std::fprintf(stderr,
                 "Cách dùng:\n"
                 "  fdcli [tuỳ chọn] list\n"
                 "  fdcli [tuỳ chọn] listen [--yes|--no]\n"
                 "  fdcli [tuỳ chọn] send <tên máy|IP> <file...>\n"
                 "Tuỳ chọn: --name <tên> --id <id> --dir <thư mục nhận> --port <port discovery>\n");
}

const char* statusText(TransferStatus s) {
    switch (s) {
        case TransferStatus::Completed: return "Hoàn tất";
        case TransferStatus::Declined: return "Bị từ chối";
        case TransferStatus::Failed: return "Lỗi";
        case TransferStatus::Cancelled: return "Đã hủy";
        case TransferStatus::TimedOut: return "Hết thời gian";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    EngineConfig cfg;
    std::vector<std::string> args;
    std::string autoAnswer;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--name" && i + 1 < argc) cfg.deviceName = argv[++i];
        else if (a == "--id" && i + 1 < argc) cfg.deviceId = argv[++i];
        else if (a == "--dir" && i + 1 < argc) cfg.downloadDir = argv[++i];
        else if (a == "--port" && i + 1 < argc) cfg.discoveryPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--yes" || a == "--no") autoAnswer = a;
        else args.push_back(a);
    }
    if (args.empty()) {
        usage();
        return 2;
    }
    const std::string cmd = args[0];

    Engine* enginePtr = nullptr;
    EngineCallbacks cb;
    cb.onPeerUpdated = [](const Peer& p) {
        std::printf("[+] %-24s %-8s %s:%u (id %s)\n", p.name.c_str(), p.os.c_str(), p.ip.c_str(), p.port, p.id.c_str());
        std::fflush(stdout);
    };
    cb.onPeerLost = [](const std::string& id) { std::printf("[-] %s\n", id.c_str()); };
    cb.onIncomingRequest = [&](const TransferInfo& info) {
        std::printf("\n%s muốn gửi %s (%llu byte).\n", info.peer.name.c_str(),
                    info.fileCount == 1 ? ("file \"" + info.fileName + "\"").c_str()
                                        : (std::to_string(info.fileCount) + " file").c_str(),
                    static_cast<unsigned long long>(info.totalBytes));
        bool accept = autoAnswer == "--yes";
        if (autoAnswer.empty()) {
            std::printf("Chấp nhận? [y/N] ");
            std::fflush(stdout);
            std::string line;
            std::getline(std::cin, line);
            accept = !line.empty() && (line[0] == 'y' || line[0] == 'Y');
        }
        std::thread([enginePtr, id = info.id, accept] { enginePtr->respond(id, accept); }).detach();
    };
    cb.onTransferPhase = [](const TransferInfo& info, TransferPhase phase) {
        const char* text = phase == TransferPhase::WaitingForAccept ? "Đang chờ chấp nhận..."
                           : phase == TransferPhase::Connecting      ? "Đang kết nối..."
                                                                     : "Đang truyền...";
        std::printf("[%s] %s\n", info.id.c_str(), text);
        std::fflush(stdout);
    };
    cb.onProgress = [](const TransferInfo&, uint64_t done, uint64_t total) {
        int pct = total ? static_cast<int>(done * 100 / total) : 100;
        std::printf("\r  %3d%%  %llu/%llu", pct, static_cast<unsigned long long>(done),
                    static_cast<unsigned long long>(total));
        std::fflush(stdout);
    };
    cb.onTransferFinished = [&](const TransferInfo& info, const TransferResult& r) {
        std::printf("\n[%s] %s %s\n", info.id.c_str(), statusText(r.status), r.message.c_str());
        for (const auto& p : r.savedPaths) std::printf("  -> %s\n", p.c_str());
        std::fflush(stdout);
        if (info.direction == Direction::Send) {
            std::lock_guard<std::mutex> l(gMu);
            gDone = true;
            gOk = r.status == TransferStatus::Completed;
            gCv.notify_all();
        }
    };

    Engine engine(cfg, cb);
    enginePtr = &engine;
    std::string err;
    if (!engine.start(&err)) {
        std::fprintf(stderr, "Không khởi động được: %s\n", err.c_str());
        return 1;
    }
    std::printf("FileDrop: %s (id %s, UDP %u)\n", engine.deviceName().c_str(), engine.deviceId().c_str(),
                engine.controlPort());

    if (cmd == "list") {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return 0;
    }
    if (cmd == "listen") {
        std::printf("Đang chờ nhận file... (Ctrl+C để thoát)\n");
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (cmd == "send" && args.size() >= 3) {
        const std::string target = args[1];
        std::vector<std::string> files(args.begin() + 2, args.end());
        std::string peerId;
        for (int i = 0; i < 50 && peerId.empty(); ++i) {
            for (const auto& p : engine.peers()) {
                if (p.name == target || p.ip == target || p.id == target) peerId = p.id;
            }
            if (peerId.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (peerId.empty()) {
            std::fprintf(stderr, "Không tìm thấy thiết bị \"%s\"\n", target.c_str());
            return 1;
        }
        if (engine.sendFiles(peerId, files, &err).empty()) {
            std::fprintf(stderr, "Lỗi: %s\n", err.c_str());
            return 1;
        }
        std::unique_lock<std::mutex> l(gMu);
        gCv.wait(l, [] { return gDone; });
        return gOk ? 0 : 1;
    }
    usage();
    return 2;
}
