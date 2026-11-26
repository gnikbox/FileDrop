package com.filedrop.android.core

import android.text.format.Formatter
import android.content.Context

enum class Direction { SEND, RECEIVE }
enum class Phase { WAITING_FOR_ACCEPT, CONNECTING, TRANSFERRING }
enum class Status { COMPLETED, DECLINED, FAILED, CANCELLED, TIMED_OUT }

data class Peer(val id: String, val name: String, val os: String, val ip: String, val port: Int)

/** “ảnh.jpg” khi 1 file, ngược lại "3 file" — giống bản desktop. */
fun itemsDescription(fileCount: Int, fileName: String): String =
    if (fileCount == 1 && fileName.isNotEmpty()) "“$fileName”" else "$fileCount file"

data class Transfer(
    val id: String,
    val direction: Direction,
    val peerName: String,
    val items: String,
    val phase: Phase,
    val progress: Float,
    val startedAt: Long,
) {
    val title: String
        get() = when (direction) {
            Direction.SEND -> when (phase) {
                Phase.WAITING_FOR_ACCEPT -> "Đang chờ $peerName chấp nhận…"
                Phase.CONNECTING -> "Đang kết nối tới $peerName…"
                Phase.TRANSFERRING -> "Đang gửi tới $peerName"
            }
            Direction.RECEIVE ->
                if (phase == Phase.TRANSFERRING) "Đang nhận từ $peerName" else "Đang chờ $peerName kết nối…"
        }
}

data class IncomingRequest(
    val id: String,
    val peerName: String,
    val fileCount: Int,
    val fileName: String,
    val totalBytes: Long,
) {
    val items: String get() = itemsDescription(fileCount, fileName)
    fun sizeText(context: Context): String = Formatter.formatFileSize(context, totalBytes)
}

data class Note(val text: String, val isError: Boolean, val id: Long = System.nanoTime())

/** Máy dùng trình duyệt gửi file tới "Máy chủ nhận file". */
data class WebClient(val id: String, val name: String, val ip: String)
