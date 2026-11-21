package com.filedrop.android.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.CheckCircle
import androidx.compose.material.icons.rounded.Computer
import androidx.compose.material.icons.rounded.DesktopWindows
import androidx.compose.material.icons.rounded.Error
import androidx.compose.material.icons.rounded.Laptop
import androidx.compose.material.icons.rounded.PhoneAndroid
import androidx.compose.material.icons.rounded.PhoneIphone
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.WifiFind
import androidx.compose.material.icons.rounded.WifiTethering
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.filedrop.android.core.Note
import com.filedrop.android.core.Peer
import com.filedrop.android.core.Phase
import com.filedrop.android.core.Transfer

/** Màu accent chung với macOS/Windows (systemBlue). */
val Accent = Color(0xFF0A84FF)

@Composable
fun FileDropTheme(content: @Composable () -> Unit) {
    val scheme = if (isSystemInDarkTheme()) {
        darkColorScheme(primary = Accent, secondary = Accent, background = Color(0xFF2C2C2E), surface = Color(0xFF2C2C2E))
    } else {
        lightColorScheme(primary = Accent, secondary = Accent, background = Color(0xFFF6F6F6), surface = Color(0xFFF6F6F6))
    }
    MaterialTheme(colorScheme = scheme, content = content)
}

private val secondaryText @Composable get() = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.55f)

@Composable
fun Header(title: String, subtitle: String, onRefresh: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(Icons.Rounded.WifiTethering, contentDescription = null, tint = Accent, modifier = Modifier.size(24.dp))
        Spacer(Modifier.width(8.dp))
        Column(Modifier.weight(1f)) {
            Text(title, fontSize = 17.sp, fontWeight = FontWeight.Bold)
            Text(subtitle, fontSize = 12.sp, color = secondaryText, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        FilledTonalIconButton(onClick = onRefresh, modifier = Modifier.size(36.dp)) {
            Icon(Icons.Rounded.Refresh, contentDescription = "Tìm lại thiết bị", modifier = Modifier.size(18.dp))
        }
    }
}

@Composable
fun SectionLabel(text: String) {
    Text(text, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, color = secondaryText)
}

/** Lưới thiết bị 3 cột: icon Mac/PC/điện thoại + tên máy. */
@Composable
fun PeerGrid(peers: List<Peer>, selectedId: String?, searching: Boolean, onSelect: (Peer) -> Unit) {
    if (peers.isEmpty()) {
        Column(
            Modifier.fillMaxWidth().heightIn(min = 100.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            if (searching) {
                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                Spacer(Modifier.height(8.dp))
                Text("Đang tìm thiết bị…", fontSize = 13.sp, color = secondaryText)
            } else {
                Icon(Icons.Rounded.WifiFind, contentDescription = null, tint = secondaryText)
                Spacer(Modifier.height(8.dp))
                Text("Chưa thấy máy nào cùng mạng Wi-Fi", fontSize = 13.sp, color = secondaryText)
            }
        }
        return
    }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        peers.chunked(3).forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                row.forEach { peer ->
                    PeerTile(peer, peer.id == selectedId, Modifier.weight(1f)) { onSelect(peer) }
                }
                repeat(3 - row.size) { Spacer(Modifier.weight(1f)) }
            }
        }
    }
}

fun iconFor(os: String): ImageVector = when (os) {
    "macos" -> Icons.Rounded.Laptop
    "windows" -> Icons.Rounded.DesktopWindows
    "ios" -> Icons.Rounded.PhoneIphone
    "android" -> Icons.Rounded.PhoneAndroid
    else -> Icons.Rounded.Computer
}

@Composable
fun PeerTile(peer: Peer, selected: Boolean, modifier: Modifier = Modifier, onClick: () -> Unit) {
    val shape = RoundedCornerShape(12.dp)
    val onSurface = MaterialTheme.colorScheme.onSurface
    Column(
        modifier
            .clip(shape)
            .background(if (selected) Accent.copy(alpha = 0.15f) else onSurface.copy(alpha = 0.05f))
            .border(1.5.dp, if (selected) Accent else Color.Transparent, shape)
            .clickable(onClick = onClick)
            .heightIn(min = 92.dp)
            .padding(6.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Icon(iconFor(peer.os), contentDescription = peer.os, tint = if (selected) Accent else onSurface,
             modifier = Modifier.size(34.dp))
        Spacer(Modifier.height(6.dp))
        Text(peer.name, fontSize = 12.sp, fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
             maxLines = 2, overflow = TextOverflow.Ellipsis, textAlign = TextAlign.Center)
    }
}

/**
 * Vùng hành động (tương ứng vùng "Thả file vào đây" của desktop): viền nét đứt, bo tròn.
 * Khi đang truyền hiện progress tròn + nút Hủy.
 */
@Composable
fun ActionZone(
    transfer: Transfer?,
    enabled: Boolean,
    icon: ImageVector,
    title: String,
    subtitle: String,
    onClick: () -> Unit,
    onCancel: (String) -> Unit,
) {
    val dashColor = if (enabled) Accent else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.4f)
    val fill = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.04f)
    Box(
        Modifier
            .fillMaxWidth()
            .height(140.dp)
            .clip(RoundedCornerShape(14.dp))
            .clickable(enabled = transfer == null, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.matchParentSize()) {
            val r = CornerRadius(14.dp.toPx())
            drawRoundRect(fill, cornerRadius = r)
            drawRoundRect(dashColor, cornerRadius = r, style = Stroke(
                width = 1.5.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(6.dp.toPx(), 4.dp.toPx()))))
        }
        if (transfer != null) {
            Row(Modifier.fillMaxWidth().padding(horizontal = 18.dp), verticalAlignment = Alignment.CenterVertically) {
                ProgressRing(if (transfer.phase == Phase.TRANSFERRING) transfer.progress else null, Modifier.size(64.dp))
                Spacer(Modifier.width(16.dp))
                Column(Modifier.weight(1f)) {
                    Text(transfer.title, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, maxLines = 2)
                    Text(transfer.items, fontSize = 12.sp, color = secondaryText, maxLines = 1,
                         overflow = TextOverflow.Ellipsis)
                    TextButton(onClick = { onCancel(transfer.id) }, contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp)) {
                        Text("Hủy", fontSize = 13.sp)
                    }
                }
            }
        } else {
            Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.padding(horizontal = 12.dp)) {
                Icon(icon, contentDescription = null, tint = dashColor, modifier = Modifier.size(32.dp))
                Spacer(Modifier.height(6.dp))
                Text(title, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                Text(subtitle, fontSize = 12.sp, color = secondaryText, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
    }
}

/** Progress bar hình tròn; progress = null => vòng xoay chờ. */
@Composable
fun ProgressRing(progress: Float?, modifier: Modifier = Modifier) {
    val track = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.1f)
    Box(modifier, contentAlignment = Alignment.Center) {
        if (progress != null) {
            val animated by animateFloatAsState(progress, tween(150), label = "progress")
            Canvas(Modifier.matchParentSize()) {
                val stroke = Stroke(width = 6.dp.toPx(), cap = StrokeCap.Round)
                val inset = 3.dp.toPx()
                val size = androidx.compose.ui.geometry.Size(size.width - 2 * inset, size.height - 2 * inset)
                val topLeft = androidx.compose.ui.geometry.Offset(inset, inset)
                drawArc(track, 0f, 360f, false, topLeft, size, style = stroke)
                drawArc(Accent, -90f, 360f * animated.coerceIn(0.001f, 1f), false, topLeft, size, style = stroke)
            }
            Text("${(progress * 100).toInt()}%", fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        } else {
            val angle by rememberInfiniteTransition(label = "spin").animateFloat(
                0f, 360f, infiniteRepeatable(tween(1000, easing = LinearEasing), RepeatMode.Restart), label = "angle")
            Canvas(Modifier.matchParentSize().rotate(angle)) {
                val stroke = Stroke(width = 6.dp.toPx(), cap = StrokeCap.Round)
                val inset = 3.dp.toPx()
                val size = androidx.compose.ui.geometry.Size(size.width - 2 * inset, size.height - 2 * inset)
                val topLeft = androidx.compose.ui.geometry.Offset(inset, inset)
                drawArc(track, 0f, 360f, false, topLeft, size, style = stroke)
                drawArc(Accent, -90f, 90f, false, topLeft, size, style = stroke)
            }
        }
    }
}

@Composable
fun StatusLine(note: Note?) {
    Box(Modifier.fillMaxWidth().heightIn(min = 20.dp)) {
        AnimatedVisibility(visible = note != null) {
            note?.let {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(if (it.isError) Icons.Rounded.Error else Icons.Rounded.CheckCircle, contentDescription = null,
                         tint = if (it.isError) Color(0xFFFF9500) else Color(0xFF34C759), modifier = Modifier.size(16.dp))
                    Spacer(Modifier.width(6.dp))
                    Text(it.text, fontSize = 12.sp, maxLines = 2)
                }
            }
        }
    }
}
