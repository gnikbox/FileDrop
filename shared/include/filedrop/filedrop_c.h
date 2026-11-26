/*
 * FileDrop — C API cho lõi C++.
 * Dùng cho Swift (macOS/iOS) và JNI (Android). Windows dùng trực tiếp C++ (engine.h).
 *
 * Mọi chuỗi là UTF-8. Con trỏ chuỗi trong callback chỉ hợp lệ trong lúc callback chạy.
 * Callback được gọi từ luồng nền.
 */
#ifndef FILEDROP_C_H
#define FILEDROP_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fd_engine fd_engine;

enum {
    FD_DIRECTION_SEND = 0,
    FD_DIRECTION_RECEIVE = 1,
};

enum {
    FD_PHASE_WAITING_FOR_ACCEPT = 0,
    FD_PHASE_CONNECTING = 1,
    FD_PHASE_TRANSFERRING = 2,
};

enum {
    FD_STATUS_COMPLETED = 0,
    FD_STATUS_DECLINED = 1,
    FD_STATUS_FAILED = 2,
    FD_STATUS_CANCELLED = 3,
    FD_STATUS_TIMED_OUT = 4,
};

/* Trạng thái máy khách của máy chủ nhận file qua web. */
enum {
    FD_WEB_PENDING = 0,
    FD_WEB_CONNECTED = 1,
    FD_WEB_DECLINED = 2,
    FD_WEB_DISCONNECTED = 3,
};

typedef struct fd_web_client {
    const char* id;
    const char* name;
    const char* ip;
    int32_t state;
} fd_web_client;

typedef struct fd_peer {
    const char* id;
    const char* name;
    const char* os;
    const char* ip;
    uint16_t port;
} fd_peer;

typedef struct fd_transfer_info {
    const char* transfer_id;
    int32_t direction;
    fd_peer peer;
    int32_t file_count;
    const char* file_name; /* "" nếu nhiều file */
    uint64_t total_bytes;
} fd_transfer_info;

typedef struct fd_transfer_result {
    int32_t status;
    const char* message;
    const char* save_dir;        /* "" với chiều gửi */
    const char* first_saved_path; /* "" nếu không có */
} fd_transfer_result;

typedef struct fd_callbacks {
    void* context;
    void (*on_peer_updated)(void* context, const fd_peer* peer);
    void (*on_peer_lost)(void* context, const char* peer_id);
    void (*on_incoming_request)(void* context, const fd_transfer_info* info);
    void (*on_transfer_phase)(void* context, const fd_transfer_info* info, int32_t phase);
    void (*on_progress)(void* context, const fd_transfer_info* info, uint64_t done, uint64_t total);
    void (*on_transfer_finished)(void* context, const fd_transfer_info* info, const fd_transfer_result* result);
    /* Máy khách web xin kết nối (FD_WEB_PENDING) hoặc đổi trạng thái. Trả lời bằng fd_engine_web_respond. */
    void (*on_web_client_changed)(void* context, const fd_web_client* client);
} fd_callbacks;

/* Tham số chuỗi có thể NULL/"" để dùng mặc định. */
fd_engine* fd_engine_create(const char* device_id, const char* device_name, const char* os,
                            const char* download_dir, fd_callbacks callbacks);
void fd_engine_destroy(fd_engine* engine);

/* Trả về 1 nếu thành công; lỗi được ghi vào error_buf. */
int fd_engine_start(fd_engine* engine, char* error_buf, size_t error_len);
void fd_engine_stop(fd_engine* engine);
void fd_engine_discover(fd_engine* engine);

/* Trả về 1 nếu đã gửi yêu cầu; ID phiên ghi vào out_id. */
int fd_engine_send_files(fd_engine* engine, const char* peer_id, const char* const* paths, size_t path_count,
                         char* out_id, size_t out_id_len, char* error_buf, size_t error_len);
int fd_engine_respond(fd_engine* engine, const char* transfer_id, int accept);
void fd_engine_cancel(fd_engine* engine, const char* transfer_id);

void fd_engine_set_device_name(fd_engine* engine, const char* name);
void fd_engine_set_download_dir(fd_engine* engine, const char* dir);

/* Tạm ẩn: không hiện với máy khác, tự động từ chối mọi yêu cầu nhận file. */
void fd_engine_set_hidden(fd_engine* engine, int hidden);
int fd_engine_is_hidden(fd_engine* engine);

/* Máy chủ nhận file qua trình duyệt: HTTP trên cổng ngẫu nhiên.
 * Trả về 1 nếu đã bật; địa chỉ (vd. "http://192.168.1.5:51234/") ghi vào url_buf. */
int fd_engine_web_start(fd_engine* engine, char* url_buf, size_t url_len, char* error_buf, size_t error_len);
void fd_engine_web_stop(fd_engine* engine);
int fd_engine_web_is_running(fd_engine* engine);
int fd_engine_web_respond(fd_engine* engine, const char* client_id, int accept);
void fd_engine_web_disconnect(fd_engine* engine, const char* client_id);

/* Bản cài FileDrop cho máy khách web tải về. platform: "macos" | "windows" | "android" | "ios" | "linux". */
typedef struct fd_web_download {
    const char* platform;
    const char* path;
    const char* file_name;
} fd_web_download;
/* package_dirs: thư mục chứa thêm bản cài (.apk, .exe, .dmg, *mac*.zip...), quét mỗi lần máy khách hỏi. */
void fd_engine_web_set_downloads(fd_engine* engine, const fd_web_download* items, int count,
                                 const char* const* package_dirs, int dir_count);

#ifdef __cplusplus
}
#endif

#endif /* FILEDROP_C_H */
