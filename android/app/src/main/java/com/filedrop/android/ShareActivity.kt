package com.filedrop.android

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Send
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.core.content.IntentCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.filedrop.android.core.itemsDescription
import com.filedrop.android.ui.ActionZone
import com.filedrop.android.ui.FileDropTheme
import com.filedrop.android.ui.Header
import com.filedrop.android.ui.PeerGrid
import com.filedrop.android.ui.SectionLabel
import com.filedrop.android.ui.StatusLine
import kotlinx.coroutines.delay

/**
 * Xuất hiện trong bảng "Chia sẻ" của Android. Hiện danh sách máy trong mạng; bấm vào một máy là gửi ngay.
 */
class ShareActivity : ComponentActivity() {
    private var uris: List<Uri> = emptyList()
    private var sharedText: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        readIntent(intent)
        val names = uris.map { FileDrop.displayName(this, it) }
        val description = when {
            uris.isEmpty() && sharedText != null -> "1 đoạn văn bản"
            else -> itemsDescription(uris.size, names.singleOrNull() ?: "")
        }
        setContent {
            FileDropTheme {
                ShareScreen(description, onSend = { peerId -> FileDrop.send(this, peerId, uris, sharedText) },
                            onClose = { finish() })
            }
        }
    }

    private fun readIntent(intent: Intent) {
        when (intent.action) {
            Intent.ACTION_SEND -> {
                IntentCompat.getParcelableExtra(intent, Intent.EXTRA_STREAM, Uri::class.java)?.let { uris = listOf(it) }
                if (uris.isEmpty()) sharedText = intent.getStringExtra(Intent.EXTRA_TEXT)
            }
            Intent.ACTION_SEND_MULTIPLE ->
                uris = IntentCompat.getParcelableArrayListExtra(intent, Intent.EXTRA_STREAM, Uri::class.java).orEmpty()
        }
    }

    override fun onStart() {
        super.onStart()
        FileDrop.screenStarted()
        FileDropService.start(this)
    }

    override fun onStop() {
        FileDrop.screenStopped()
        super.onStop()
    }
}

@Composable
private fun ShareScreen(description: String, onSend: (String) -> Unit, onClose: () -> Unit) {
    val peers by FileDrop.peers.collectAsStateWithLifecycle()
    val selectedId by FileDrop.selectedPeerId.collectAsStateWithLifecycle()
    val transfers by FileDrop.transfers.collectAsStateWithLifecycle()
    val note by FileDrop.note.collectAsStateWithLifecycle()
    val searching by FileDrop.searching.collectAsStateWithLifecycle()

    var sent by remember { mutableStateOf(false) }
    var sawTransfer by remember { mutableStateOf(false) }
    val transfer = transfers.values.maxByOrNull { it.startedAt }
    if (transfer != null) sawTransfer = true

    // Gửi xong (hoặc lỗi) => đóng sau giây lát để người dùng kịp thấy thông báo.
    LaunchedEffect(sent, sawTransfer, transfer == null, note) {
        if (sent && sawTransfer && transfer == null) {
            delay(if (note?.isError == true) 3000 else 1500)
            onClose()
        }
    }

    // Gửi lỗi ngay từ đầu (chưa kịp tạo phiên) => cho phép chọn lại máy khác.
    LaunchedEffect(note) {
        if (sent && !sawTransfer && note?.isError == true) sent = false
    }

    // Nền mờ; bấm ra ngoài để đóng (khi chưa gửi).
    Box(
        Modifier.fillMaxSize().clickable(remember { MutableInteractionSource() }, null) { if (!sent) onClose() },
        contentAlignment = Alignment.BottomCenter,
    ) {
        Surface(
            Modifier.safeDrawingPadding().padding(12.dp).widthIn(max = 480.dp).fillMaxWidth()
                .clickable(remember { MutableInteractionSource() }, null) {},
            shape = RoundedCornerShape(24.dp),
            color = MaterialTheme.colorScheme.background,
            shadowElevation = 12.dp,
        ) {
            Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
                Header("Gửi qua FileDrop", description, onRefresh = FileDrop::refresh)
                SectionLabel("Chọn máy để gửi")
                PeerGrid(peers, selectedId, searching, onSelect = { peer ->
                    if (!sent) {
                        FileDrop.select(peer)
                        sent = true
                        onSend(peer.id)
                    }
                })
                if (sent || transfer != null) {
                    ActionZone(transfer, enabled = false, icon = Icons.AutoMirrored.Rounded.Send, title = "Đang chuẩn bị…",
                               subtitle = description, onClick = {}, onCancel = FileDrop::cancel)
                }
                StatusLine(note)
                Row {
                    Spacer(Modifier.weight(1f))
                    TextButton(onClick = onClose) { Text(if (sent) "Đóng" else "Hủy") }
                }
            }
        }
    }
}
