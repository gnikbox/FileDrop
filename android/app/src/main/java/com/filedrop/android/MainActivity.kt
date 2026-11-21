package com.filedrop.android

import android.Manifest
import android.app.Application
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

    val selected = peers.firstOrNull { it.id == selectedId }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty() && selected != null) FileDrop.send(context, selected.id, uris)
    }

    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Column(
            Modifier.safeDrawingPadding().verticalScroll(rememberScrollState()).padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Header("FileDrop", FileDrop.deviceName, onRefresh = FileDrop::refresh)
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
                    Text("Nhận file khi chạy nền", fontSize = 14.sp)
                    Text("Giữ FileDrop hiển thị với các máy khác khi đóng app", fontSize = 12.sp,
                         color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.55f))
                }
                Switch(checked = background, onCheckedChange = FileDrop::setReceiveInBackground)
            }
        }
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
