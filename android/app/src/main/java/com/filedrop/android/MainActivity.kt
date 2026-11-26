package com.filedrop.android

import android.Manifest
import android.app.Application
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.UploadFile
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.filedrop.android.ui.ActionZone
import com.filedrop.android.ui.FileDropTheme
import com.filedrop.android.ui.Header
import com.filedrop.android.ui.PeerGrid
import com.filedrop.android.ui.SectionLabel
import com.filedrop.android.ui.StatusLine
import com.filedrop.android.ui.WebServerPanel

class FileDropApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        FileDrop.init(this)
        Notifications.createChannels(this)
    }
}

/** Màn hình chính: danh sách máy trong mạng, chọn file để gửi, hỏi nhận file. */
class MainActivity : ComponentActivity() {
    private val notificationPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) {}

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
        setContent { FileDropTheme { MainScreen() } }
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
private fun MainScreen() {
    val context = LocalContext.current
    val peers by FileDrop.peers.collectAsStateWithLifecycle()
    val selectedId by FileDrop.selectedPeerId.collectAsStateWithLifecycle()
    val transfers by FileDrop.transfers.collectAsStateWithLifecycle()
    val incoming by FileDrop.incoming.collectAsStateWithLifecycle()
    val note by FileDrop.note.collectAsStateWithLifecycle()
    val searching by FileDrop.searching.collectAsStateWithLifecycle()
    val background by FileDrop.receiveInBackground.collectAsStateWithLifecycle()
    val hidden by FileDrop.hidden.collectAsStateWithLifecycle()
    val webUrl by FileDrop.webUrl.collectAsStateWithLifecycle()
    val webClients by FileDrop.webClients.collectAsStateWithLifecycle()
    val webRequests by FileDrop.webRequests.collectAsStateWithLifecycle()

    val selected = peers.firstOrNull { it.id == selectedId }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty() && selected != null) FileDrop.send(context, selected.id, uris)
    }

    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Column(
            Modifier.safeDrawingPadding().verticalScroll(rememberScrollState()).padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Header("FileDrop", if (hidden) "Đang tạm ẩn · không nhận file" else FileDrop.deviceName,
                   onRefresh = FileDrop::refresh, warning = hidden)
            SectionLabel("Thiết bị trong mạng")
            PeerGrid(peers, selectedId, searching, onSelect = FileDrop::select)
            ActionZone(
                transfer = transfers.values.maxByOrNull { it.startedAt },
                enabled = selected != null,
                icon = Icons.Rounded.UploadFile,
                title = "Chọn file để gửi",
                subtitle = when {
                    selected != null -> "tới ${selected.name}"
                    FileDrop.rememberedPeerName != null -> "${FileDrop.rememberedPeerName} hiện không trực tuyến"
                    else -> "Chọn một máy ở trên trước"
                },
                onClick = {
                    if (selected != null) picker.launch(arrayOf("*/*"))
                    else FileDrop.showNote("Hãy chọn một máy để gửi trước", isError = true)
                },
                onCancel = FileDrop::cancel,
            )
            StatusLine(note)
            HorizontalDivider()
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Máy chủ nhận file qua trình duyệt", fontSize = 14.sp)
                    Text("Máy khác cùng mạng không cần cài app: mở địa chỉ bằng trình duyệt để gửi file cho bạn",
                         fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.55f))
                }
                Switch(checked = webUrl != null, onCheckedChange = FileDrop::setWebServer)
            }
            webUrl?.let { url ->
                WebServerPanel(
                    url = url,
                    clients = webClients,
                    onCopy = {
                        context.getSystemService(ClipboardManager::class.java)
                            .setPrimaryClip(ClipData.newPlainText("FileDrop", url))
                        FileDrop.showNote("Đã chép địa chỉ", isError = false)
                    },
                    onShare = {
                        val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, url)
                        context.startActivity(Intent.createChooser(send, "Chia sẻ địa chỉ máy chủ"))
                    },
                    onDisconnect = FileDrop::disconnectWeb,
                )
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Tạm ẩn — không nhận file", fontSize = 14.sp)
                    Text("Máy khác không thấy máy này; bạn vẫn gửi file đi được", fontSize = 12.sp,
                         color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.55f))
                }
                Switch(checked = hidden, onCheckedChange = FileDrop::setHidden)
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Nhận file khi chạy nền", fontSize = 14.sp)
                    Text("Giữ FileDrop hiển thị với các máy khác khi đóng app", fontSize = 12.sp,
                         color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.55f))
                }
                Switch(checked = background, onCheckedChange = FileDrop::setReceiveInBackground)
            }
        }
    }

    val webReq = webRequests.firstOrNull()
    if (incoming.isEmpty() && webReq != null) {
        AlertDialog(
            onDismissRequest = {},
            title = { Text("Cho phép “${webReq.name}” kết nối?") },
            text = { Text("Máy ${webReq.ip} muốn gửi file cho bạn qua trình duyệt web. Nếu đồng ý, máy đó có thể " +
                          "gửi file nhiều lần cho tới khi bạn ngắt kết nối hoặc tắt máy chủ.") },
            confirmButton = { TextButton(onClick = { FileDrop.respondWeb(webReq.id, true) }) { Text("Chấp nhận") } },
            dismissButton = { TextButton(onClick = { FileDrop.respondWeb(webReq.id, false) }) { Text("Từ chối") } },
        )
    }

    incoming.firstOrNull()?.let { req ->
        AlertDialog(
            onDismissRequest = {},
            title = { Text("Nhận file từ ${req.peerName}?") },
            text = { Text("${req.peerName} muốn gửi cho bạn ${req.items} (${req.sizeText(context)}).") },
            confirmButton = { TextButton(onClick = { FileDrop.respond(req.id, true) }) { Text("Chấp nhận") } },
            dismissButton = { TextButton(onClick = { FileDrop.respond(req.id, false) }) { Text("Từ chối") } },
        )
    }
}
