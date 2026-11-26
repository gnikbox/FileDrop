package com.filedrop.android

import android.app.DownloadManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.core.app.NotificationCompat
import com.filedrop.android.core.IncomingRequest
import com.filedrop.android.core.Transfer
import com.filedrop.android.core.WebClient

/** Thông báo: trạng thái service, hỏi nhận file (Chấp nhận/Từ chối), kết quả gửi/nhận. */
object Notifications {
    private const val CHANNEL_STATUS = "status"
    private const val CHANNEL_REQUEST = "request"
    private const val CHANNEL_RESULT = "result"
    const val STATUS_ID = 1
    private const val ACTION_ACCEPT = "com.filedrop.android.ACCEPT"
    private const val ACTION_DECLINE = "com.filedrop.android.DECLINE"
    private const val EXTRA_ID = "transferId"
    private const val EXTRA_WEB = "web"

    fun createChannels(context: Context) {
        val nm = context.getSystemService(NotificationManager::class.java)
        nm.createNotificationChannels(listOf(
            NotificationChannel(CHANNEL_STATUS, "Trạng thái FileDrop", NotificationManager.IMPORTANCE_LOW),
            NotificationChannel(CHANNEL_REQUEST, "Yêu cầu nhận file", NotificationManager.IMPORTANCE_HIGH),
            NotificationChannel(CHANNEL_RESULT, "Gửi/nhận xong", NotificationManager.IMPORTANCE_DEFAULT),
        ))
    }

    private fun openApp(context: Context): PendingIntent = PendingIntent.getActivity(
        context, 0, Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

    fun status(context: Context, transfer: Transfer?): Notification {
        val b = NotificationCompat.Builder(context, CHANNEL_STATUS)
            .setSmallIcon(R.drawable.ic_stat_filedrop)
            .setContentIntent(openApp(context))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
        if (transfer == null) {
            val web = FileDrop.webUrl.value
            b.setContentTitle("FileDrop").setContentText(when {
                web != null -> "Máy chủ nhận file: $web"
                FileDrop.hidden.value -> "Đang tạm ẩn — không nhận file"
                else -> "Sẵn sàng nhận file với tên ${FileDrop.deviceName}"
            })
        } else {
            b.setContentTitle(transfer.title).setContentText(transfer.items)
            if (transfer.phase == com.filedrop.android.core.Phase.TRANSFERRING) {
                b.setProgress(100, (transfer.progress * 100).toInt(), false)
            } else {
                b.setProgress(0, 0, true)
            }
        }
        return b.build()
    }

    fun showRequest(context: Context, request: IncomingRequest) {
        fun action(name: String) = PendingIntent.getBroadcast(
            context, request.id.hashCode() + name.hashCode(),
            Intent(context, RequestActionReceiver::class.java).setAction(name).putExtra(EXTRA_ID, request.id),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

        val n = NotificationCompat.Builder(context, CHANNEL_REQUEST)
            .setSmallIcon(R.drawable.ic_stat_filedrop)
            .setContentTitle("Nhận file từ ${request.peerName}?")
            .setContentText("${request.peerName} muốn gửi cho bạn ${request.items} (${request.sizeText(context)}).")
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setCategory(NotificationCompat.CATEGORY_MESSAGE)
            .setContentIntent(openApp(context))
            .setAutoCancel(true)
            .setTimeoutAfter(120_000)
            .addAction(0, "Chấp nhận", action(ACTION_ACCEPT))
            .addAction(0, "Từ chối", action(ACTION_DECLINE))
            .build()
        notify(context, request.id.hashCode(), n)
    }

    fun showWebRequest(context: Context, client: WebClient) {
        fun action(name: String) = PendingIntent.getBroadcast(
            context, client.id.hashCode() + name.hashCode(),
            Intent(context, RequestActionReceiver::class.java).setAction(name)
                .putExtra(EXTRA_ID, client.id).putExtra(EXTRA_WEB, true),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

        val n = NotificationCompat.Builder(context, CHANNEL_REQUEST)
            .setSmallIcon(R.drawable.ic_stat_filedrop)
            .setContentTitle("Cho phép “${client.name}” kết nối?")
            .setContentText("Máy ${client.ip} muốn gửi file cho bạn qua trình duyệt web.")
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setCategory(NotificationCompat.CATEGORY_MESSAGE)
            .setContentIntent(openApp(context))
            .setAutoCancel(true)
            .addAction(0, "Chấp nhận", action(ACTION_ACCEPT))
            .addAction(0, "Từ chối", action(ACTION_DECLINE))
            .build()
        notify(context, client.id.hashCode(), n)
    }

    fun cancelRequest(context: Context, id: String) {
        context.getSystemService(NotificationManager::class.java).cancel(id.hashCode())
    }

    fun showResult(context: Context, title: String, text: String, open: Uri?) {
        val intent = if (open != null) {
            Intent(Intent.ACTION_VIEW).setDataAndType(open, context.contentResolver.getType(open))
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        } else {
            Intent(DownloadManager.ACTION_VIEW_DOWNLOADS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        val n = NotificationCompat.Builder(context, CHANNEL_RESULT)
            .setSmallIcon(R.drawable.ic_stat_filedrop)
            .setContentTitle(title)
            .setContentText(text)
            .setAutoCancel(true)
            .setContentIntent(PendingIntent.getActivity(context, text.hashCode(), intent,
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
            .build()
        notify(context, (System.currentTimeMillis() and 0x7fffffff).toInt(), n)
    }

    private fun notify(context: Context, id: Int, n: Notification) {
        runCatching { context.getSystemService(NotificationManager::class.java).notify(id, n) }
    }

    /** Xử lý nút Chấp nhận/Từ chối trên thông báo. */
    class RequestActionReceiver : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val id = intent.getStringExtra(EXTRA_ID) ?: return
            val accept = intent.action == ACTION_ACCEPT
            if (intent.getBooleanExtra(EXTRA_WEB, false)) FileDrop.respondWeb(id, accept)
            else FileDrop.respond(id, accept)
        }
    }
}
