package com.filedrop.android

import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.IBinder
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.FlowPreview
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onEach

/**
 * Foreground service giữ lõi FileDrop chạy (UDP discovery + TCP) và giữ MulticastLock để
 * Android không lọc gói broadcast. Tự dừng khi không còn cần (xem [FileDrop.serviceNeeded]).
 */
class FileDropService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var multicastLock: WifiManager.MulticastLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    @OptIn(FlowPreview::class)
    override fun onCreate() {
        super.onCreate()
        ServiceCompat.startForeground(this, Notifications.STATUS_ID, Notifications.status(this, null),
                                      ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)

        val wifi = applicationContext.getSystemService(WifiManager::class.java)
        multicastLock = wifi.createMulticastLock("FileDrop").apply {
            setReferenceCounted(false)
            acquire()
        }
        FileDrop.startEngine()

        // Cập nhật thông báo trạng thái theo phiên truyền hiện tại.
        FileDrop.transfers
            .map { map -> map.values.maxByOrNull { it.startedAt } }
            .distinctUntilChanged { a, b -> a?.id == b?.id && a?.phase == b?.phase &&
                ((a?.progress ?: 0f) * 50).toInt() == ((b?.progress ?: 0f) * 50).toInt() }
            .onEach { t ->
                ContextCompat.getSystemService(this, android.app.NotificationManager::class.java)
                    ?.notify(Notifications.STATUS_ID, Notifications.status(this, t))
            }
            .launchIn(scope)

        // Bật/tắt tạm ẩn hoặc máy chủ web => cập nhật dòng trạng thái của thông báo.
        combine(FileDrop.hidden, FileDrop.webUrl) { h, w -> h to w }
            .onEach {
                val t = FileDrop.transfers.value.values.maxByOrNull { it.startedAt }
                ContextCompat.getSystemService(this, android.app.NotificationManager::class.java)
                    ?.notify(Notifications.STATUS_ID, Notifications.status(this, t))
            }
            .launchIn(scope)

        // Dừng service khi không còn cần (trễ vài giây để chuyển màn hình không làm tắt engine).
        combine(FileDrop.receiveInBackground, FileDrop.visibleScreens, FileDrop.transfers, FileDrop.incoming,
                FileDrop.webUrl) { _, _, _, _, _ ->
            FileDrop.serviceNeeded()
        }
            .debounce(3000)
            .onEach { needed -> if (!needed) stopSelf() }
            .launchIn(scope)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_REFRESH) FileDrop.refresh()
        return START_STICKY
    }

    override fun onTimeout(startId: Int, fgsType: Int) {
        // Android 15 giới hạn thời gian chạy của dataSync — dừng an toàn.
        stopSelf()
    }

    override fun onDestroy() {
        scope.cancel()
        FileDrop.stopEngine()
        multicastLock?.release()
        super.onDestroy()
    }

    companion object {
        const val ACTION_REFRESH = "com.filedrop.android.REFRESH"

        fun start(context: Context) {
            runCatching {
                ContextCompat.startForegroundService(context, Intent(context, FileDropService::class.java))
            }
        }
    }
}
