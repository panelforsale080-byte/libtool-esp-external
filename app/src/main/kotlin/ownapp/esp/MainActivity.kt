package ownapp.esp

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : AppCompatActivity() {
    external fun nativeSetPid(pid: Int)
    external fun nativeResolve(): String
    external fun nativeSnapshot(): String
    external fun nativeLog(): String
    external fun nativeProbeRead(pid: Int): Boolean

    private lateinit var status: TextView
    private lateinit var log: TextView
    private lateinit var pidInput: EditText
    private var pid = 0
    private var nativeOk = false
    private var auto = false
    private val ui = Handler(Looper.getMainLooper())

    private fun parse(s: String): Array<EspTarget> {
        if (!s.startsWith("OK|")) return emptyArray()
        val parts = s.split('|')
        val n = parts.firstOrNull { it.startsWith("count=") }?.substringAfter('=')?.toIntOrNull() ?: 0
        val lp = parts.firstOrNull { it.startsWith("lp=") }?.substringAfter("lp=")?.split(',')
        if (lp != null && lp.size >= 2) {
            EspState.lpX = lp[0].toFloatOrNull() ?: 0f
            EspState.lpZ = lp[1].toFloatOrNull() ?: 0f
            EspState.hasLp = EspState.lpX != 0f || EspState.lpZ != 0f
        }
        if (n <= 0) return emptyArray()
        val payload = parts.last()
        if (payload.contains('=')) return emptyArray()
        val out = ArrayList<EspTarget>()
        for (tok in payload.split(';')) {
            val p = tok.split(',')
            if (p.size < 4) continue
            val x = p[0].toFloatOrNull() ?: continue
            val z = p[1].toFloatOrNull() ?: continue
            val name = p[2]
            val meters = p[3].toIntOrNull() ?: 0
            val hp = p.getOrNull(4)?.toIntOrNull() ?: 0
            val hpMax = p.getOrNull(5)?.toIntOrNull() ?: 0
            out.add(EspTarget(x, z, name, meters, hp, hpMax, p.getOrNull(6) == "M"))
        }
        return out.toTypedArray()
    }

    private val tick = object : Runnable {
        override fun run() {
            if (!auto || !nativeOk) return
            try {
                val r = nativeSnapshot()
                append(r)
                val enemies = parse(r)
                val ok = r.startsWith("OK|")
                EspState.update(ok && enemies.isNotEmpty(), r, enemies, EspState.lpX, EspState.lpZ, EspState.hasLp)
                status.text = if (enemies.isEmpty()) r.take(90) else "OK · targets=${enemies.size}"
            } catch (t: Throwable) {
                append("ERR|${t.message}")
            }
            ui.postDelayed(this, 200)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        status = findViewById(R.id.statusText)
        log = findViewById(R.id.logText)
        pidInput = findViewById(R.id.pidInput)

        bind(R.id.chkBox) { EspFeatures.box = it }
        bind(R.id.chkLine) { EspFeatures.line = it }
        bind(R.id.chkHp) { EspFeatures.health = it }
        bind(R.id.chkName) { EspFeatures.name = it }
        bind(R.id.chkDist) { EspFeatures.distance = it }

        try {
            System.loadLibrary("LibToolEsp")
            nativeOk = true
            status.text = "Find MLBB UnityKillsMe → Connect → Resolve in match"
        } catch (t: Throwable) {
            status.text = "Native failed: ${t.message}"
        }

        findViewById<Button>(R.id.btnFindMlbb).setOnClickListener { findAndConnectMlbb() }

        findViewById<Button>(R.id.btnApply).setOnClickListener {
            val v = pidInput.text.toString().trim().toIntOrNull()
            if (v == null || v <= 0) {
                pidInput.error = "Invalid PID"
                return@setOnClickListener
            }
            if (v == android.os.Process.myPid()) {
                pidInput.error = "Use MLBB Unity PID, not this app"
                return@setOnClickListener
            }
            connectPid(v, manual = true)
        }

        findViewById<Button>(R.id.btnResolve).setOnClickListener {
            if (pid <= 0) {
                toast("Connect to MLBB first")
                return@setOnClickListener
            }
            val r = nativeResolve()
            EspState.resolveLog = r
            append(r)
            status.text = when {
                r.contains("READY=1") && r.contains("Instance NULL") ->
                    "Klass OK · Instance NULL (enter a match)"
                r.contains("READY=1") -> "Resolved — ESP / Overlay"
                else -> "Resolve failed — see log"
            }
        }

        findViewById<Button>(R.id.btnEsp).setOnClickListener {
            if (pid <= 0) {
                toast("Connect to MLBB first")
                return@setOnClickListener
            }
            auto = !auto
            findViewById<Button>(R.id.btnEsp).text = if (auto) "ESP OFF" else "ESP ON"
            if (auto) ui.post(tick) else ui.removeCallbacks(tick)
        }

        findViewById<Button>(R.id.btnCopy).setOnClickListener {
            val cm = getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("esp", EspState.resolveLog.ifBlank { log.text.toString() }))
            toast("Copied")
        }

        findViewById<Button>(R.id.btnOverlay).setOnClickListener {
            if (pid <= 0) {
                toast("Connect to MLBB first")
                return@setOnClickListener
            }
            if (Build.VERSION.SDK_INT >= 23 && !Settings.canDrawOverlays(this)) {
                startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName")))
                toast("Allow overlay")
                return@setOnClickListener
            }
            startService(Intent(this, OverlayService::class.java))
            toast("Overlay started")
        }
    }

    private fun findAndConnectMlbb() {
        lifecycleScope.launch {
            val target = withContext(Dispatchers.IO) { MlbbProcessFinder.findUnityKills() }
            if (target == null) {
                val list = withContext(Dispatchers.IO) { MlbbProcessFinder.listCandidates() }
                append("No UnityKillsMe — open MLBB lobby")
                list.take(5).forEach { append("  pid=${it.pid} ${it.cmdline}") }
                status.text = "MLBB Unity process not found"
                return@launch
            }
            pidInput.setText(target.pid.toString())
            append("Found: pid=${target.pid} ${target.cmdline}")
            connectPid(target.pid, manual = false)
        }
    }

    private fun connectPid(v: Int, manual: Boolean) {
        pid = v
        nativeSetPid(pid)
        auto = false
        findViewById<Button>(R.id.btnEsp).text = "ESP ON"
        append("Target PID=$pid (MLBB game process)")
        val canRead = nativeProbeRead(pid)
        append(if (canRead) "process_vm_readv: OK (root / permissive)" else "process_vm_readv: FAIL — need root or GSpace pair")
        status.text = if (canRead) {
            "Connected pid $pid — tap Resolve in match"
        } else {
            "PID set but cannot read memory — root required for external ESP"
        }
        if (!canRead && !manual) {
            toast("Cannot read game memory — root?")
        }
    }

    private fun bind(id: Int, fn: (Boolean) -> Unit) {
        val box = findViewById<CheckBox>(id)
        fn(box.isChecked)
        box.setOnCheckedChangeListener { _, c -> fn(c) }
    }

    private fun append(s: String) {
        ui.post {
            val cur = log.text?.toString() ?: ""
            log.text = (if (cur.length > 6000) cur.takeLast(4000) else cur) + "\n" + s
        }
    }

    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()

    override fun onDestroy() {
        auto = false
        ui.removeCallbacks(tick)
        super.onDestroy()
    }
}
