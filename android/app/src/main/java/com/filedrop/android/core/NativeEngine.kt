package com.filedrop.android.core

/**
 * Bọc lõi C++ (shared/) qua JNI — xem app/src/main/cpp/filedrop_jni.cpp.
 * Các hàm của [Listener] được gọi từ luồng nền của lõi.
 */
class NativeEngine(deviceId: String, deviceName: String, downloadDir: String, listener: Listener) {

    interface Listener {
        fun onPeerUpdated(id: String, name: String, os: String, ip: String, port: Int)
        fun onPeerLost(id: String)
        fun onIncomingRequest(id: String, peerId: String, peerName: String, peerOs: String,
                              fileCount: Int, fileName: String, totalBytes: Long)
        fun onTransferPhase(id: String, direction: Int, peerName: String, fileCount: Int,
                            fileName: String, totalBytes: Long, phase: Int)
        fun onProgress(id: String, direction: Int, done: Long, total: Long)
        fun onTransferFinished(id: String, direction: Int, peerName: String, fileCount: Int,
                               fileName: String, status: Int, message: String, savedPaths: Array<String>)
        /** Máy khách web đổi trạng thái: 0 chờ duyệt, 1 đã kết nối, 2 bị từ chối, 3 đã ngắt. */
        fun onWebClientChanged(id: String, name: String, ip: String, state: Int)
    }

    private var handle: Long = nativeCreate(deviceId, deviceName, downloadDir, listener)

    /** Trả về null nếu thành công, ngược lại là thông báo lỗi. */
    fun start(): String? = nativeStart(handle)
    fun stop() = nativeStop(handle)
    fun discover() = nativeDiscover(handle)

    /** fds phải đã detach (lõi C++ sở hữu và tự đóng). Ném IllegalStateException nếu lỗi. */
    fun sendFds(peerId: String, fds: IntArray, names: Array<String>, sizes: LongArray): String =
        nativeSendFds(handle, peerId, fds, names, sizes)

    fun respond(transferId: String, accept: Boolean): Boolean = nativeRespond(handle, transferId, accept)
    fun cancel(transferId: String) = nativeCancel(handle, transferId)
    fun setDeviceName(name: String) = nativeSetDeviceName(handle, name)

    /** Tạm ẩn: máy khác không thấy, mọi yêu cầu gửi file tới đều tự động bị từ chối. */
    fun setHidden(hidden: Boolean) = nativeSetHidden(handle, hidden)

    /** Bật máy chủ nhận file qua trình duyệt, trả về địa chỉ. Ném IllegalStateException nếu lỗi. */
    fun startWebServer(): String = nativeWebStart(handle)
    fun stopWebServer() = nativeWebStop(handle)
    fun setWebDownloads(apkPath: String?, packagesDir: String?) = nativeWebSetDownloads(handle, apkPath, packagesDir)
    fun respondWebClient(clientId: String, accept: Boolean): Boolean = nativeWebRespond(handle, clientId, accept)
    fun disconnectWebClient(clientId: String) = nativeWebDisconnect(handle, clientId)

    fun destroy() {
        if (handle != 0L) {
            nativeDestroy(handle)
            handle = 0L
        }
    }

    private external fun nativeCreate(deviceId: String, deviceName: String, downloadDir: String, listener: Listener): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeStart(handle: Long): String?
    private external fun nativeStop(handle: Long)
    private external fun nativeDiscover(handle: Long)
    private external fun nativeSendFds(handle: Long, peerId: String, fds: IntArray, names: Array<String>, sizes: LongArray): String
    private external fun nativeRespond(handle: Long, transferId: String, accept: Boolean): Boolean
    private external fun nativeCancel(handle: Long, transferId: String)
    private external fun nativeSetDeviceName(handle: Long, name: String)
    private external fun nativeSetHidden(handle: Long, hidden: Boolean)
    private external fun nativeWebStart(handle: Long): String
    private external fun nativeWebStop(handle: Long)
    private external fun nativeWebSetDownloads(handle: Long, apkPath: String?, packagesDir: String?)
    private external fun nativeWebRespond(handle: Long, clientId: String, accept: Boolean): Boolean
    private external fun nativeWebDisconnect(handle: Long, clientId: String)

    companion object {
        init {
            System.loadLibrary("filedrop_jni")
        }
    }
}
