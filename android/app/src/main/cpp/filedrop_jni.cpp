// FileDrop (Android) — cầu nối JNI giữa Kotlin (com.filedrop.android.core.NativeEngine) và filedrop::Engine.
#include <android/log.h>
#include <jni.h>

#include <memory>
#include <string>
#include <vector>

#include "filedrop/engine.h"
#include "filedrop/transfer.h"

#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "FileDropJNI", __VA_ARGS__)

using namespace filedrop;

namespace {

JavaVM* gVm = nullptr;
jclass gStringClass = nullptr;
jmethodID gStringFromBytes = nullptr;  // String(byte[], String charset)
jmethodID gStringGetBytes = nullptr;   // byte[] String.getBytes(String charset)
jobject gUtf8 = nullptr;               // "UTF-8"

// Gắn thread nền của Engine vào JVM (một lần), tự tách khi thread kết thúc.
struct ThreadEnv {
    JNIEnv* env = nullptr;
    bool attached = false;
    ~ThreadEnv() {
        if (attached && gVm) gVm->DetachCurrentThread();
    }
};

JNIEnv* currentEnv() {
    thread_local ThreadEnv te;
    if (te.env) return te.env;
    JNIEnv* env = nullptr;
    if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        te.env = env;
        return env;
    }
    JavaVMAttachArgs args{JNI_VERSION_1_6, "FileDropCore", nullptr};
    if (gVm->AttachCurrentThread(&env, &args) != JNI_OK) return nullptr;
    te.env = env;
    te.attached = true;
    return env;
}

// JNI NewStringUTF/GetStringUTFChars dùng "Modified UTF-8" (sai với emoji) => đi qua byte[].
jstring toJ(JNIEnv* env, const std::string& s) {
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(s.size()));
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(s.size()), reinterpret_cast<const jbyte*>(s.data()));
    auto str = static_cast<jstring>(env->NewObject(gStringClass, gStringFromBytes, bytes, gUtf8));
    env->DeleteLocalRef(bytes);
    return str;
}

std::string fromJ(JNIEnv* env, jstring s) {
    if (!s) return {};
    auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(s, gStringGetBytes, gUtf8));
    if (!bytes) return {};
    jsize len = env->GetArrayLength(bytes);
    std::string out(static_cast<size_t>(len), '\0');
    env->GetByteArrayRegion(bytes, 0, len, reinterpret_cast<jbyte*>(out.data()));
    env->DeleteLocalRef(bytes);
    return out;
}

struct Bridge {
    jobject listener = nullptr;
    jmethodID onPeerUpdated = nullptr;
    jmethodID onPeerLost = nullptr;
    jmethodID onIncomingRequest = nullptr;
    jmethodID onTransferPhase = nullptr;
    jmethodID onProgress = nullptr;
    jmethodID onTransferFinished = nullptr;
    jmethodID onWebClientChanged = nullptr;
    std::unique_ptr<Engine> engine;
};

Bridge* bridgeOf(jlong handle) { return reinterpret_cast<Bridge*>(static_cast<intptr_t>(handle)); }

// Gọi listener Kotlin từ thread bất kỳ, tự dọn local ref và exception.
template <typename Fn>
void callListener(Fn&& fn) {
    JNIEnv* env = currentEnv();
    if (!env) return;
    if (env->PushLocalFrame(32) != JNI_OK) return;
    fn(env);
    if (env->ExceptionCheck()) {
        LOGW("Exception trong listener Kotlin");
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->PopLocalFrame(nullptr);
}

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    gVm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass cls = env->FindClass("java/lang/String");
    gStringClass = static_cast<jclass>(env->NewGlobalRef(cls));
    gStringFromBytes = env->GetMethodID(gStringClass, "<init>", "([BLjava/lang/String;)V");
    gStringGetBytes = env->GetMethodID(gStringClass, "getBytes", "(Ljava/lang/String;)[B");
    jstring utf8 = env->NewStringUTF("UTF-8");
    gUtf8 = env->NewGlobalRef(utf8);
    env->DeleteLocalRef(utf8);
    env->DeleteLocalRef(cls);
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jlong JNICALL Java_com_filedrop_android_core_NativeEngine_nativeCreate(
    JNIEnv* env, jobject, jstring deviceId, jstring deviceName, jstring downloadDir, jobject listener) {
    auto* b = new Bridge();
    b->listener = env->NewGlobalRef(listener);
    jclass cls = env->GetObjectClass(listener);
    b->onPeerUpdated = env->GetMethodID(cls, "onPeerUpdated",
                                        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");
    b->onPeerLost = env->GetMethodID(cls, "onPeerLost", "(Ljava/lang/String;)V");
    b->onIncomingRequest = env->GetMethodID(
        cls, "onIncomingRequest",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;J)V");
    b->onTransferPhase =
        env->GetMethodID(cls, "onTransferPhase", "(Ljava/lang/String;ILjava/lang/String;ILjava/lang/String;JI)V");
    b->onProgress = env->GetMethodID(cls, "onProgress", "(Ljava/lang/String;IJJ)V");
    b->onTransferFinished = env->GetMethodID(
        cls, "onTransferFinished",
        "(Ljava/lang/String;ILjava/lang/String;ILjava/lang/String;ILjava/lang/String;[Ljava/lang/String;)V");
    b->onWebClientChanged = env->GetMethodID(cls, "onWebClientChanged",
                                             "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");
    env->DeleteLocalRef(cls);

    EngineConfig cfg;
    cfg.deviceId = fromJ(env, deviceId);
    cfg.deviceName = fromJ(env, deviceName);
    cfg.os = "android";
    cfg.downloadDir = fromJ(env, downloadDir);

    EngineCallbacks cb;
    cb.onPeerUpdated = [b](const Peer& p) {
        callListener([&](JNIEnv* e) {
            e->CallVoidMethod(b->listener, b->onPeerUpdated, toJ(e, p.id), toJ(e, p.name), toJ(e, p.os),
                              toJ(e, p.ip), static_cast<jint>(p.port));
        });
    };
    cb.onPeerLost = [b](const std::string& id) {
        callListener([&](JNIEnv* e) { e->CallVoidMethod(b->listener, b->onPeerLost, toJ(e, id)); });
    };
    cb.onIncomingRequest = [b](const TransferInfo& i) {
        callListener([&](JNIEnv* e) {
            e->CallVoidMethod(b->listener, b->onIncomingRequest, toJ(e, i.id), toJ(e, i.peer.id), toJ(e, i.peer.name),
                              toJ(e, i.peer.os), static_cast<jint>(i.fileCount), toJ(e, i.fileName),
                              static_cast<jlong>(i.totalBytes));
        });
    };
    cb.onTransferPhase = [b](const TransferInfo& i, TransferPhase phase) {
        callListener([&](JNIEnv* e) {
            e->CallVoidMethod(b->listener, b->onTransferPhase, toJ(e, i.id), static_cast<jint>(i.direction),
                              toJ(e, i.peer.name), static_cast<jint>(i.fileCount), toJ(e, i.fileName),
                              static_cast<jlong>(i.totalBytes), static_cast<jint>(phase));
        });
    };
    cb.onProgress = [b](const TransferInfo& i, uint64_t done, uint64_t total) {
        callListener([&](JNIEnv* e) {
            e->CallVoidMethod(b->listener, b->onProgress, toJ(e, i.id), static_cast<jint>(i.direction),
                              static_cast<jlong>(done), static_cast<jlong>(total));
        });
    };
    cb.onTransferFinished = [b](const TransferInfo& i, const TransferResult& r) {
        callListener([&](JNIEnv* e) {
            jobjectArray paths = e->NewObjectArray(static_cast<jsize>(r.savedPaths.size()), gStringClass, nullptr);
            for (size_t k = 0; k < r.savedPaths.size(); ++k) {
                e->SetObjectArrayElement(paths, static_cast<jsize>(k), toJ(e, r.savedPaths[k]));
            }
            e->CallVoidMethod(b->listener, b->onTransferFinished, toJ(e, i.id), static_cast<jint>(i.direction),
                              toJ(e, i.peer.name), static_cast<jint>(i.fileCount), toJ(e, i.fileName),
                              static_cast<jint>(r.status), toJ(e, r.message), paths);
        });
    };

    cb.onWebClientChanged = [b](const WebClient& c) {
        callListener([&](JNIEnv* e) {
            e->CallVoidMethod(b->listener, b->onWebClientChanged, toJ(e, c.id), toJ(e, c.name), toJ(e, c.ip),
                              static_cast<jint>(c.state));
        });
    };

    b->engine = std::make_unique<Engine>(std::move(cfg), std::move(cb));
    return static_cast<jlong>(reinterpret_cast<intptr_t>(b));
}

extern "C" JNIEXPORT void JNICALL Java_com_filedrop_android_core_NativeEngine_nativeDestroy(JNIEnv* env, jobject,
                                                                                            jlong handle) {
    Bridge* b = bridgeOf(handle);
    if (!b) return;
    b->engine->stopWebServer();
    b->engine->stop();
    b->engine.reset();
    env->DeleteGlobalRef(b->listener);
    delete b;
}

// Trả về null nếu thành công, ngược lại là thông báo lỗi.
extern "C" JNIEXPORT jstring JNICALL Java_com_filedrop_android_core_NativeEngine_nativeStart(JNIEnv* env, jobject,
                                                                                             jlong handle) {
    std::string err;
    if (bridgeOf(handle)->engine->start(&err)) return nullptr;
    return toJ(env, err.empty() ? std::string("Không khởi động được") : err);
}

extern "C" JNIEXPORT void JNICALL Java_com_filedrop_android_core_NativeEngine_nativeStop(JNIEnv*, jobject,
                                                                                         jlong handle) {
    bridgeOf(handle)->engine->stop();
}

extern "C" JNIEXPORT void JNICALL Java_com_filedrop_android_core_NativeEngine_nativeDiscover(JNIEnv*, jobject,
                                                                                             jlong handle) {
    bridgeOf(handle)->engine->discover();
}

// fds: file descriptor đã detach từ ParcelFileDescriptor — lõi C++ nhận quyền sở hữu và tự đóng.
// Ném IllegalStateException nếu lỗi; trả về ID phiên nếu thành công.
extern "C" JNIEXPORT jstring JNICALL Java_com_filedrop_android_core_NativeEngine_nativeSendFds(
    JNIEnv* env, jobject, jlong handle, jstring peerId, jintArray fds, jobjectArray names, jlongArray sizes) {
    const jsize n = env->GetArrayLength(fds);
    std::vector<jint> fdList(static_cast<size_t>(n));
    std::vector<jlong> sizeList(static_cast<size_t>(n));
    env->GetIntArrayRegion(fds, 0, n, fdList.data());
    env->GetLongArrayRegion(sizes, 0, n, sizeList.data());

    std::vector<FileEntry> entries;
    for (jsize i = 0; i < n; ++i) {
        auto name = static_cast<jstring>(env->GetObjectArrayElement(names, i));
        entries.push_back(fileEntryFromFd(fdList[static_cast<size_t>(i)], fromJ(env, name),
                                          static_cast<uint64_t>(sizeList[static_cast<size_t>(i)])));
        env->DeleteLocalRef(name);
    }

    std::string err;
    std::string id = bridgeOf(handle)->engine->sendEntries(fromJ(env, peerId), std::move(entries), &err);
    if (id.empty()) {
        jclass ex = env->FindClass("java/lang/IllegalStateException");
        env->ThrowNew(ex, err.c_str());
        return nullptr;
    }
    return toJ(env, id);
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_filedrop_android_core_NativeEngine_nativeRespond(
    JNIEnv* env, jobject, jlong handle, jstring transferId, jboolean accept) {
    return bridgeOf(handle)->engine->respond(fromJ(env, transferId), accept == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_filedrop_android_core_NativeEngine_nativeCancel(JNIEnv* env, jobject,
                                                                                           jlong handle,
                                                                                           jstring transferId) {
    bridgeOf(handle)->engine->cancel(fromJ(env, transferId));
}

extern "C" JNIEXPORT void JNICALL Java_com_filedrop_android_core_NativeEngine_nativeSetHidden(JNIEnv*, jobject,
                                                                                              jlong handle,
                                                                                              jboolean hidden) {
    bridgeOf(handle)->engine->setHidden(hidden == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL Java_com_filedrop_android_core_NativeEngine_nativeSetDeviceName(JNIEnv* env, jobject,
                                                                                                  jlong handle,
                                                                                                  jstring name) {
    bridgeOf(handle)->engine->setDeviceName(fromJ(env, name));
}

// ---- Máy chủ nhận file qua trình duyệt ----

// Trả về địa chỉ http://...; ném IllegalStateException nếu không bật được.
extern "C" JNIEXPORT jstring JNICALL Java_com_filedrop_android_core_NativeEngine_nativeWebStart(JNIEnv* env, jobject,
                                                                                                jlong handle) {
    Engine& engine = *bridgeOf(handle)->engine;
    std::string err;
    if (!engine.startWebServer(&err)) {
        jclass ex = env->FindClass("java/lang/IllegalStateException");
        env->ThrowNew(ex, err.empty() ? "Không bật được máy chủ" : err.c_str());
        return nullptr;
    }
    return toJ(env, engine.webServerUrl());
}

// Bản cài cho máy khách web tải: APK của chính app + thư mục chứa thêm bản cài nền tảng khác.
extern "C" JNIEXPORT void JNICALL Java_com_filedrop_android_core_NativeEngine_nativeWebSetDownloads(
    JNIEnv* env, jobject, jlong handle, jstring apkPath, jstring packagesDir) {
    std::vector<WebDownload> list;
    std::string apk = fromJ(env, apkPath);
    if (!apk.empty()) list.push_back({"android", apk, "FileDrop-Android.apk"});
    std::vector<std::string> dirs;
    std::string dir = fromJ(env, packagesDir);
    if (!dir.empty()) dirs.push_back(dir);
    bridgeOf(handle)->engine->setWebDownloads(std::move(list), std::move(dirs));
}

extern "C" JNIEXPORT void JNICALL Java_com_filedrop_android_core_NativeEngine_nativeWebStop(JNIEnv*, jobject,
                                                                                            jlong handle) {
    bridgeOf(handle)->engine->stopWebServer();
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_filedrop_android_core_NativeEngine_nativeWebRespond(
    JNIEnv* env, jobject, jlong handle, jstring clientId, jboolean accept) {
    return bridgeOf(handle)->engine->respondWebClient(fromJ(env, clientId), accept == JNI_TRUE) ? JNI_TRUE
                                                                                                : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_filedrop_android_core_NativeEngine_nativeWebDisconnect(JNIEnv* env, jobject,
                                                                                                  jlong handle,
                                                                                                  jstring clientId) {
    bridgeOf(handle)->engine->disconnectWebClient(fromJ(env, clientId));
}
