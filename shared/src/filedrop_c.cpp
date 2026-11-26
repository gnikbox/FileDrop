#include "filedrop/filedrop_c.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "filedrop/engine.h"

using namespace filedrop;

struct fd_engine {
    fd_callbacks callbacks{};
    std::unique_ptr<Engine> engine;
};

namespace {

std::string str(const char* s) { return s ? std::string(s) : std::string(); }

void copyTo(const std::string& s, char* buf, size_t len) {
    if (!buf || len == 0) return;
    size_t n = s.size() < len - 1 ? s.size() : len - 1;
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
}

fd_peer toC(const Peer& p) {
    fd_peer c{};
    c.id = p.id.c_str();
    c.name = p.name.c_str();
    c.os = p.os.c_str();
    c.ip = p.ip.c_str();
    c.port = p.port;
    return c;
}

fd_transfer_info toC(const TransferInfo& i) {
    fd_transfer_info c{};
    c.transfer_id = i.id.c_str();
    c.direction = static_cast<int32_t>(i.direction);
    c.peer = toC(i.peer);
    c.file_count = i.fileCount;
    c.file_name = i.fileName.c_str();
    c.total_bytes = i.totalBytes;
    return c;
}

}  // namespace

extern "C" {

fd_engine* fd_engine_create(const char* device_id, const char* device_name, const char* os,
                            const char* download_dir, fd_callbacks callbacks) {
    auto* h = new fd_engine();
    h->callbacks = callbacks;

    EngineConfig cfg;
    cfg.deviceId = str(device_id);
    cfg.deviceName = str(device_name);
    cfg.os = str(os);
    cfg.downloadDir = str(download_dir);

    EngineCallbacks cb;
    cb.onPeerUpdated = [h](const Peer& p) {
        if (!h->callbacks.on_peer_updated) return;
        fd_peer c = toC(p);
        h->callbacks.on_peer_updated(h->callbacks.context, &c);
    };
    cb.onPeerLost = [h](const std::string& id) {
        if (h->callbacks.on_peer_lost) h->callbacks.on_peer_lost(h->callbacks.context, id.c_str());
    };
    cb.onIncomingRequest = [h](const TransferInfo& info) {
        if (!h->callbacks.on_incoming_request) return;
        fd_transfer_info c = toC(info);
        h->callbacks.on_incoming_request(h->callbacks.context, &c);
    };
    cb.onTransferPhase = [h](const TransferInfo& info, TransferPhase phase) {
        if (!h->callbacks.on_transfer_phase) return;
        fd_transfer_info c = toC(info);
        h->callbacks.on_transfer_phase(h->callbacks.context, &c, static_cast<int32_t>(phase));
    };
    cb.onProgress = [h](const TransferInfo& info, uint64_t done, uint64_t total) {
        if (!h->callbacks.on_progress) return;
        fd_transfer_info c = toC(info);
        h->callbacks.on_progress(h->callbacks.context, &c, done, total);
    };
    cb.onTransferFinished = [h](const TransferInfo& info, const TransferResult& result) {
        if (!h->callbacks.on_transfer_finished) return;
        fd_transfer_info c = toC(info);
        std::string first = result.savedPaths.empty() ? std::string() : result.savedPaths.front();
        fd_transfer_result r{};
        r.status = static_cast<int32_t>(result.status);
        r.message = result.message.c_str();
        r.save_dir = result.saveDir.c_str();
        r.first_saved_path = first.c_str();
        h->callbacks.on_transfer_finished(h->callbacks.context, &c, &r);
    };

    cb.onWebClientChanged = [h](const WebClient& client) {
        if (!h->callbacks.on_web_client_changed) return;
        fd_web_client c{};
        c.id = client.id.c_str();
        c.name = client.name.c_str();
        c.ip = client.ip.c_str();
        c.state = static_cast<int32_t>(client.state);
        h->callbacks.on_web_client_changed(h->callbacks.context, &c);
    };

    h->engine = std::make_unique<Engine>(std::move(cfg), std::move(cb));
    return h;
}

void fd_engine_destroy(fd_engine* engine) {
    if (!engine) return;
    engine->engine->stop();
    delete engine;
}

int fd_engine_start(fd_engine* engine, char* error_buf, size_t error_len) {
    std::string err;
    bool ok = engine->engine->start(&err);
    if (!ok) copyTo(err, error_buf, error_len);
    return ok ? 1 : 0;
}

void fd_engine_stop(fd_engine* engine) { engine->engine->stop(); }

void fd_engine_discover(fd_engine* engine) { engine->engine->discover(); }

int fd_engine_send_files(fd_engine* engine, const char* peer_id, const char* const* paths, size_t path_count,
                         char* out_id, size_t out_id_len, char* error_buf, size_t error_len) {
    std::vector<std::string> list;
    for (size_t i = 0; i < path_count; ++i) {
        if (paths[i]) list.emplace_back(paths[i]);
    }
    std::string err;
    std::string id = engine->engine->sendFiles(str(peer_id), list, &err);
    if (id.empty()) {
        copyTo(err, error_buf, error_len);
        return 0;
    }
    copyTo(id, out_id, out_id_len);
    return 1;
}

int fd_engine_respond(fd_engine* engine, const char* transfer_id, int accept) {
    return engine->engine->respond(str(transfer_id), accept != 0) ? 1 : 0;
}

void fd_engine_cancel(fd_engine* engine, const char* transfer_id) { engine->engine->cancel(str(transfer_id)); }

void fd_engine_set_device_name(fd_engine* engine, const char* name) { engine->engine->setDeviceName(str(name)); }

void fd_engine_set_download_dir(fd_engine* engine, const char* dir) { engine->engine->setDownloadDir(str(dir)); }

void fd_engine_set_hidden(fd_engine* engine, int hidden) { engine->engine->setHidden(hidden != 0); }

int fd_engine_is_hidden(fd_engine* engine) { return engine->engine->isHidden() ? 1 : 0; }

int fd_engine_web_start(fd_engine* engine, char* url_buf, size_t url_len, char* error_buf, size_t error_len) {
    std::string err;
    if (!engine->engine->startWebServer(&err)) {
        copyTo(err, error_buf, error_len);
        return 0;
    }
    copyTo(engine->engine->webServerUrl(), url_buf, url_len);
    return 1;
}

void fd_engine_web_stop(fd_engine* engine) { engine->engine->stopWebServer(); }

int fd_engine_web_is_running(fd_engine* engine) { return engine->engine->isWebServerRunning() ? 1 : 0; }

int fd_engine_web_respond(fd_engine* engine, const char* client_id, int accept) {
    return engine->engine->respondWebClient(str(client_id), accept != 0) ? 1 : 0;
}

void fd_engine_web_disconnect(fd_engine* engine, const char* client_id) {
    engine->engine->disconnectWebClient(str(client_id));
}

void fd_engine_web_set_downloads(fd_engine* engine, const fd_web_download* items, int count,
                                 const char* const* package_dirs, int dir_count) {
    std::vector<WebDownload> list;
    for (int i = 0; items && i < count; ++i) list.push_back({str(items[i].platform), str(items[i].path), str(items[i].file_name)});
    std::vector<std::string> dirs;
    for (int i = 0; package_dirs && i < dir_count; ++i) dirs.push_back(str(package_dirs[i]));
    engine->engine->setWebDownloads(std::move(list), std::move(dirs));
}

}  // extern "C"
