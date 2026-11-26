package com.filedrop.android

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.provider.MediaStore
import android.provider.OpenableColumns
import android.provider.Settings
import android.webkit.MimeTypeMap
import com.filedrop.android.core.Direction
import com.filedrop.android.core.IncomingRequest
import com.filedrop.android.core.NativeEngine
import com.filedrop.android.core.Note
import com.filedrop.android.core.Peer
import com.filedrop.android.core.Phase
import com.filedrop.android.core.Status
import com.filedrop.android.core.Transfer
import com.filedrop.android.core.WebClient
import com.filedrop.android.core.itemsDescription
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.io.File
import java.util.UUID
import java.util.concurrent.Executors

/**
 * Trạng thái dùng chung của app (máy trong mạng, máy đích đã chọn, phiên truyền, yêu cầu nhận file)
 * và cầu nối tới lõi C++. Engine chỉ chạy khi [FileDropService] đang chạy.
 */
object FileDrop : NativeEngine.Listener {

    private lateinit var app: Context
    private val main = Handler(Looper.getMainLooper())
    private val io = Executors.newSingleThreadExecutor { Thread(it, "FileDropIO") }
    private var engine: NativeEngine? = null

    private val _peers = MutableStateFlow<List<Peer>>(emptyList())
    val peers: StateFlow<List<Peer>> = _peers.asStateFlow()
    private val _selectedPeerId = MutableStateFlow<String?>(null)
    val selectedPeerId: StateFlow<String?> = _selectedPeerId.asStateFlow()
    private val _transfers = MutableStateFlow<Map<String, Transfer>>(emptyMap())
    val transfers: StateFlow<Map<String, Transfer>> = _transfers.asStateFlow()
    private val _incoming = MutableStateFlow<List<IncomingRequest>>(emptyList())
    val incoming: StateFlow<List<IncomingRequest>> = _incoming.asStateFlow()
    private val _note = MutableStateFlow<Note?>(null)
    val note: StateFlow<Note?> = _note.asStateFlow()
    private val _searching = MutableStateFlow(false)
    val searching: StateFlow<Boolean> = _searching.asStateFlow()
    private val _receiveInBackground = MutableStateFlow(true)
    val receiveInBackground: StateFlow<Boolean> = _receiveInBackground.asStateFlow()
    private val _hidden = MutableStateFlow(false)
    /** Tạm ẩn — không hiện với máy khác, không nhận file (vẫn gửi đi được). */
    val hidden: StateFlow<Boolean> = _hidden.asStateFlow()
    private val _webUrl = MutableStateFlow<String?>(null)
    /** Máy chủ nhận file qua trình duyệt: địa chỉ đang mở (null = tắt). */
    val webUrl: StateFlow<String?> = _webUrl.asStateFlow()
    private val _webClients = MutableStateFlow<List<WebClient>>(emptyList())
    val webClients: StateFlow<List<WebClient>> = _webClients.asStateFlow()
    private val _webRequests = MutableStateFlow<List<WebClient>>(emptyList())
    /** Máy đang xin kết nối, chờ người dùng đồng ý. */
    val webRequests: StateFlow<List<WebClient>> = _webRequests.asStateFlow()
    private val _visibleScreens = MutableStateFlow(0)
    val visibleScreens: StateFlow<Int> = _visibleScreens.asStateFlow()

    lateinit var deviceName: String
        private set

    private val prefs by lazy { app.getSharedPreferences("filedrop", Context.MODE_PRIVATE) }
    private val incomingDir: File by lazy { File(app.filesDir, "incoming").apply { mkdirs() } }
    private val outgoingDir: File by lazy { File(app.cacheDir, "outgoing").apply { mkdirs() } }

    fun init(context: Context) {
        app = context.applicationContext
        deviceName = Settings.Global.getString(app.contentResolver, Settings.Global.DEVICE_NAME)
            ?.takeIf { it.isNotBlank() } ?: Build.MODEL
        _selectedPeerId.value = prefs.getString("selectedPeerId", null)
        _receiveInBackground.value = prefs.getBoolean("receiveInBackground", true)
        _hidden.value = prefs.getBoolean("hidden", false)
    }

    private val deviceId: String
        get() = prefs.getString("deviceId", null) ?: UUID.randomUUID().toString().replace("-", "").take(16)
            .also { prefs.edit().putString("deviceId", it).apply() }

    // ------------------------------------------------------------------ Vòng đời engine (gọi bởi service)

    fun startEngine() {
        if (engine != null) return
        val e = NativeEngine(deviceId, deviceName, incomingDir.absolutePath, this)
        e.setHidden(_hidden.value)  // trước khi start để gói đầu tiên đã ở chế độ ẩn
        val error = e.start()
        if (error != null) {
            e.destroy()
            showNote("Không khởi động được mạng: $error", isError = true)
            return
        }
        engine = e
        refresh()
    }

    fun stopEngine() {
        val e = engine ?: return
        engine = null
        e.destroy()
        _webUrl.value = null
        _webClients.value = emptyList()
        _webRequests.value.forEach { Notifications.cancelRequest(app, it.id) }
        _webRequests.value = emptyList()
        _peers.value = emptyList()
        _transfers.value = emptyMap()
        _incoming.value = emptyList()
    }

    /**
     * Service cần chạy khi: cho phép nhận nền, có màn hình mở, đang truyền/đang chờ trả lời,
     * hoặc máy chủ web đang bật (máy khác vẫn kết nối được sau khi đóng app).
     */
    fun serviceNeeded(): Boolean =
        _receiveInBackground.value || _visibleScreens.value > 0 || _transfers.value.isNotEmpty() ||
            _incoming.value.isNotEmpty() || _webUrl.value != null

    fun screenStarted() = _visibleScreens.update { it + 1 }
    fun screenStopped() = _visibleScreens.update { maxOf(0, it - 1) }

    // ------------------------------------------------------------------ Hành động từ UI

    fun refresh() {
        engine?.discover()
        _searching.value = true
        main.removeCallbacks(stopSearching)
        main.postDelayed(stopSearching, 3000)
    }

    private val stopSearching = Runnable { _searching.value = false }

    fun select(peer: Peer) {
        _selectedPeerId.value = peer.id
        prefs.edit().putString("selectedPeerId", peer.id).putString("selectedPeerName", peer.name).apply()
    }

    val selectedPeer: Peer? get() = _peers.value.firstOrNull { it.id == _selectedPeerId.value }
    val rememberedPeerName: String? get() = selectedPeer?.name ?: prefs.getString("selectedPeerName", null)

    fun setHidden(enabled: Boolean) {
        if (_hidden.value == enabled) return
        _hidden.value = enabled
        prefs.edit().putBoolean("hidden", enabled).apply()
        engine?.setHidden(enabled)
        showNote(if (enabled) "Đã tạm ẩn — máy khác không thấy và không gửi file tới được"
                 else "Đã hiện lại — sẵn sàng nhận file", isError = false)
    }

    fun setReceiveInBackground(enabled: Boolean) {
        _receiveInBackground.value = enabled
        prefs.edit().putBoolean("receiveInBackground", enabled).apply()
    }

    /**
     * Gửi các URI (content:// hoặc file://) tới [peerId]. File thường được gửi thẳng từ file descriptor;
     * nguồn không có kích thước (pipe...) được chép tạm vào cache trước. [text] dùng khi chia sẻ văn bản.
     */
    fun send(context: Context, peerId: String, uris: List<Uri>, text: String? = null) {
        val resolver = context.contentResolver
        // Mở descriptor ngay (khi Activity còn quyền đọc URI), phần chậm chạy ở luồng IO.
        val opened = uris.mapNotNull { uri ->
            runCatching { resolver.openFileDescriptor(uri, "r")?.let { uri to it } }.getOrNull()
        }
        io.execute {
            val fds = ArrayList<Int>()
            val names = ArrayList<String>()
            val sizes = ArrayList<Long>()
            try {
                for ((uri, pfd) in opened) {
                    val name = displayName(context, uri)
                    var source = pfd
                    if (source.statSize < 0) {  // không phải file thường => chép tạm
                        val tmp = uniqueFile(outgoingDir, name)
                        ParcelFileDescriptor.AutoCloseInputStream(source).use { input ->
                            tmp.outputStream().use { input.copyTo(it, 256 * 1024) }
                        }
                        source = ParcelFileDescriptor.open(tmp, ParcelFileDescriptor.MODE_READ_ONLY)
                        tmp.deleteOnExit()
                    }
                    sizes += source.statSize
                    names += name
                    fds += source.detachFd()
                }
                if (text != null && uris.isEmpty()) {
                    val tmp = uniqueFile(outgoingDir, "Văn bản.txt").apply { writeText(text) }
                    val pfd = ParcelFileDescriptor.open(tmp, ParcelFileDescriptor.MODE_READ_ONLY)
                    sizes += pfd.statSize
                    names += tmp.name
                    fds += pfd.detachFd()
                }
                val e = engine ?: error("FileDrop chưa sẵn sàng")
                if (fds.isEmpty()) error("Không đọc được file nào để gửi")
                val handed = fds.toIntArray()
                fds.clear()  // từ đây lõi C++ sở hữu các fd (kể cả khi gửi lỗi)
                e.sendFds(peerId, handed, names.toTypedArray(), sizes.toLongArray())
            } catch (t: Throwable) {
                fds.forEach { closeFd(it) }  // các fd chưa trao cho lõi
                main.post { showNote(t.message ?: "Gửi file thất bại", isError = true) }
            }
        }
    }

    fun respond(id: String, accept: Boolean) {
        _incoming.update { list -> list.filterNot { it.id == id } }
        Notifications.cancelRequest(app, id)
        val ok = engine?.respond(id, accept) ?: false
        if (!ok && accept) showNote("Yêu cầu đã hết hạn", isError = true)
    }

    fun cancel(id: String) {
        engine?.cancel(id)
    }

    // ------------------------------------------------------------------ Máy chủ nhận file qua trình duyệt

    fun setWebServer(enabled: Boolean) {
        val e = engine
        if (enabled) {
            if (_webUrl.value != null) return
            if (e == null) {
                showNote("FileDrop chưa sẵn sàng", isError = true)
                return
            }
            // Trang web gợi ý máy khách tải FileDrop: APK của chính app + bản cài khác bỏ trong
            // Android/data/<gói>/files/packages (vd. FileDrop-Windows.exe, FileDrop-macOS.zip).
            runCatching { e.setWebDownloads(app.applicationInfo.sourceDir, app.getExternalFilesDir("packages")?.absolutePath) }
            runCatching { e.startWebServer() }
                .onSuccess {
                    _webUrl.value = it
                    showNote("Đã bật máy chủ nhận file — mở địa chỉ trên trình duyệt của máy khác", isError = false)
                }
                .onFailure { showNote("Không bật được máy chủ: ${it.message}", isError = true) }
        } else {
            if (_webUrl.value == null) return
            e?.stopWebServer()
            _webUrl.value = null
            _webClients.value = emptyList()
            _webRequests.value.forEach { Notifications.cancelRequest(app, it.id) }
            _webRequests.value = emptyList()
            showNote("Đã tắt máy chủ nhận file", isError = false)
        }
    }

    fun respondWeb(clientId: String, accept: Boolean) {
        val client = _webRequests.value.firstOrNull { it.id == clientId }
        _webRequests.update { list -> list.filterNot { it.id == clientId } }
        Notifications.cancelRequest(app, clientId)
        val ok = engine?.respondWebClient(clientId, accept) ?: false
        if (!ok && accept) showNote("${client?.name ?: "Máy đó"} đã rời đi", isError = true)
    }

    fun disconnectWeb(clientId: String) {
        engine?.disconnectWebClient(clientId)
    }

    // ------------------------------------------------------------------ Callback từ lõi (luồng nền)

    override fun onPeerUpdated(id: String, name: String, os: String, ip: String, port: Int) = main.post {
        val peer = Peer(id, name, os, ip, port)
        _peers.update { list ->
            val others = list.filterNot { it.id == id }
            (others + peer).sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name })
        }
        if (id == _selectedPeerId.value) prefs.edit().putString("selectedPeerName", name).apply()
    }.let {}

    override fun onPeerLost(id: String) = main.post {
        _peers.update { list -> list.filterNot { it.id == id } }
    }.let {}

    override fun onIncomingRequest(id: String, peerId: String, peerName: String, peerOs: String,
                                   fileCount: Int, fileName: String, totalBytes: Long) = main.post {
        val request = IncomingRequest(id, peerName, fileCount, fileName, totalBytes)
        _incoming.update { it + request }
        Notifications.showRequest(app, request)
    }.let {}

    override fun onWebClientChanged(id: String, name: String, ip: String, state: Int) = main.post {
        val client = WebClient(id, name, ip)
        _webClients.update { list -> list.filterNot { it.id == id } }
        _webRequests.update { list -> list.filterNot { it.id == id } }
        Notifications.cancelRequest(app, id)
        if (_webUrl.value == null) return@post  // máy chủ đã tắt
        when (state) {
            0 -> {
                _webRequests.update { it + client }
                Notifications.showWebRequest(app, client)
            }
            1 -> {
                _webClients.update { it + client }
                showNote("$name đã kết nối — có thể gửi file qua trình duyệt", isError = false)
            }
        }
    }.let {}

    override fun onTransferPhase(id: String, direction: Int, peerName: String, fileCount: Int,
                                 fileName: String, totalBytes: Long, phase: Int) = main.post {
        _transfers.update { map ->
            val current = map[id] ?: Transfer(
                id = id,
                direction = if (direction == 0) Direction.SEND else Direction.RECEIVE,
                peerName = peerName,
                items = itemsDescription(fileCount, fileName),
                phase = Phase.entries[phase],
                progress = 0f,
                startedAt = System.currentTimeMillis(),
            )
            map + (id to current.copy(phase = Phase.entries[phase]))
        }
    }.let {}

    override fun onProgress(id: String, direction: Int, done: Long, total: Long) = main.post {
        _transfers.update { map ->
            val t = map[id] ?: return@update map
            val p = if (total <= 0) 1f else (done.toDouble() / total).toFloat()
            map + (id to t.copy(phase = Phase.TRANSFERRING, progress = p))
        }
    }.let {}

    override fun onTransferFinished(id: String, direction: Int, peerName: String, fileCount: Int,
                                    fileName: String, status: Int, message: String,
                                    savedPaths: Array<String>) {
        val dir = if (direction == 0) Direction.SEND else Direction.RECEIVE
        val st = Status.entries[status]
        val items = itemsDescription(fileCount, fileName)
        if (dir == Direction.RECEIVE && savedPaths.isNotEmpty()) {
            // Chuyển file từ thư mục tạm của app sang Download/FileDrop (MediaStore), rồi mới báo xong.
            io.execute {
                val published = savedPaths.flatMap { publishToDownloads(File(it)) }
                main.post { finish(id, dir, st, peerName, items, message, published) }
            }
        } else {
            main.post { finish(id, dir, st, peerName, items, message, emptyList()) }
        }
    }

    private fun finish(id: String, direction: Direction, status: Status, peer: String, items: String,
                       message: String, published: List<Uri>) {
        _transfers.update { it - id }
        _incoming.update { list -> list.filterNot { it.id == id } }
        Notifications.cancelRequest(app, id)
        when (status) {
            Status.COMPLETED -> if (direction == Direction.SEND) {
                showNote("Đã gửi xong $items tới $peer", isError = false)
                Notifications.showResult(app, "Đã gửi xong", "$items đã được gửi tới $peer.", null)
            } else {
                showNote("Đã nhận $items từ $peer", isError = false)
                Notifications.showResult(app, "Đã nhận xong", "$items từ $peer đã lưu vào Download/FileDrop.",
                                         published.singleOrNull())
            }
            Status.DECLINED -> if (direction == Direction.SEND) showNote("$peer đã từ chối nhận file", isError = true)
            Status.CANCELLED -> showNote(message.ifEmpty { "Đã hủy" }, isError = true)
            Status.FAILED, Status.TIMED_OUT -> {
                showNote(message.ifEmpty { "Truyền file thất bại" }, isError = true)
                if (direction == Direction.RECEIVE || status == Status.FAILED) {
                    Notifications.showResult(app, "Truyền file thất bại", message, null)
                }
            }
        }
    }

    // ------------------------------------------------------------------ Tiện ích

    private val clearNote = Runnable { _note.value = null }

    fun showNote(text: String, isError: Boolean) {
        _note.value = Note(text, isError)
        main.removeCallbacks(clearNote)
        main.postDelayed(clearNote, 6000)
    }

    fun displayName(context: Context, uri: Uri): String {
        if (uri.scheme == "content") {
            runCatching {
                context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                    if (c.moveToFirst()) c.getString(0)?.takeIf { it.isNotBlank() }?.let { return it }
                }
            }
        }
        return uri.lastPathSegment?.substringAfterLast('/')?.takeIf { it.isNotBlank() } ?: "file"
    }

    private fun uniqueFile(dir: File, name: String): File {
        var f = File(dir, name)
        var i = 1
        while (f.exists()) {
            f = File(dir, "${name.substringBeforeLast('.', name)} ($i)" +
                if (name.contains('.')) ".${name.substringAfterLast('.')}" else "")
            i++
        }
        return f
    }

    private fun closeFd(fd: Int) {
        runCatching { ParcelFileDescriptor.adoptFd(fd).close() }
    }

    /** Đưa file/thư mục đã nhận vào Download/FileDrop qua MediaStore rồi xoá bản tạm. */
    private fun publishToDownloads(source: File): List<Uri> {
        val result = ArrayList<Uri>()
        val base = Environment.DIRECTORY_DOWNLOADS + "/FileDrop"
        val root = source.parentFile ?: return result
        source.walkTopDown().filter { it.isFile }.forEach { file ->
            val relDir = file.parentFile!!.relativeTo(root).path.let { if (it.isEmpty()) base else "$base/$it" }
            val mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(file.extension.lowercase())
                ?: "application/octet-stream"
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, file.name)
                put(MediaStore.Downloads.MIME_TYPE, mime)
                put(MediaStore.Downloads.RELATIVE_PATH, relDir)
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
            val resolver = app.contentResolver
            val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values) ?: return@forEach
            runCatching {
                resolver.openOutputStream(uri)?.use { out -> file.inputStream().use { it.copyTo(out, 256 * 1024) } }
                values.clear()
                values.put(MediaStore.Downloads.IS_PENDING, 0)
                resolver.update(uri, values, null, null)
                result += uri
            }.onFailure { resolver.delete(uri, null, null) }
        }
        source.deleteRecursively()
        return result
    }
}
