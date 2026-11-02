package ownapp.esp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.PorterDuff
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import kotlin.math.hypot

class OverlayService : Service() {
    private var wm: WindowManager? = null
    private var view: EspView? = null
    private val ui = Handler(Looper.getMainLooper())
    private val poll = object : Runnable {
        override fun run() {
            view?.invalidate()
            ui.postDelayed(this, 33)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(NotificationChannel("esp", "ESP", NotificationManager.IMPORTANCE_LOW))
            startForeground(3, Notification.Builder(this, "esp")
                .setContentTitle("LibTool ESP")
                .setSmallIcon(android.R.drawable.ic_menu_compass)
                .build())
        } else {
            @Suppress("DEPRECATION")
            startForeground(3, Notification.Builder(this)
                .setContentTitle("LibTool ESP")
                .setSmallIcon(android.R.drawable.ic_menu_compass)
                .build())
        }
        wm = getSystemService(WINDOW_SERVICE) as WindowManager
        view = EspView(this)
        val lp = WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            if (Build.VERSION.SDK_INT >= 26)
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
            else @Suppress("DEPRECATION") WindowManager.LayoutParams.TYPE_PHONE,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        )
        lp.gravity = Gravity.TOP or Gravity.START
        wm?.addView(view, lp)
        ui.post(poll)
    }

    override fun onDestroy() {
        ui.removeCallbacks(poll)
        try { if (view != null) wm?.removeView(view) } catch (_: Throwable) {}
        super.onDestroy()
    }
}

class EspView(ctx: android.content.Context) : View(ctx) {
    private val pBox = Paint().apply { color = Color.WHITE; style = Paint.Style.STROKE; strokeWidth = 2.2f; isAntiAlias = true }
    private val pLine = Paint().apply { color = 0xB3FFFFFF.toInt(); style = Paint.Style.STROKE; strokeWidth = 1.6f; isAntiAlias = true }
    private val pName = Paint().apply { color = Color.WHITE; textSize = 22f; isAntiAlias = true; textAlign = Paint.Align.CENTER }
    private val pBarBg = Paint().apply { color = 0xDC1E1E1E.toInt(); style = Paint.Style.FILL }
    private val pBar = Paint().apply { style = Paint.Style.FILL }
    private val pInfo = Paint().apply { color = 0xFFCCCCCC.toInt(); textSize = 20f; isAntiAlias = true }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR)
        val age = if (EspState.updatedNanos == 0L) -1L else (System.nanoTime() - EspState.updatedNanos) / 1_000_000L
        val stale = age < 0 || age > 4000L || !EspState.ok
        val enemies = if (stale) emptyArray() else EspState.enemies
        canvas.drawText(if (stale) "LibTool ESP  waiting" else "LibTool ESP  ${enemies.size}", 16f, 36f, pInfo)
        if (stale || enemies.isEmpty()) return

        val lx = if (EspState.hasLp) EspState.lpX else enemies.map { it.x }.average().toFloat()
        val lz = if (EspState.hasLp) EspState.lpZ else enemies.map { it.z }.average().toFloat()
        val ax = width * 0.5f
        val ay = height * 0.82f
        val cx = width * 0.5f
        val cy = height * 0.48f
        val range = 45f
        val sx = width * 0.40f / range
        val sy = height * 0.28f / range
        val boxH = height / 6.5f
        val boxW = boxH * 0.48f

        for (e in enemies) {
            val dx = e.x - lx
            val dz = e.z - lz
            val dist = hypot(dx, dz)
            val clamp = if (dist > range) range / dist else 1f
            val x = (cx + dx * clamp * sx).coerceIn(16f, width - 16f)
            val y = (cy - dz * clamp * sy).coerceIn(90f, height - 28f)
            val left = x - boxW / 2f
            val top = y - boxH
            if (EspFeatures.line) canvas.drawLine(ax, ay, x, y, pLine)
            if (EspFeatures.box) canvas.drawRoundRect(left, top, left + boxW, top + boxH, 8f, 8f, pBox)
            if (EspFeatures.name) canvas.drawText(e.name, x, top - 8f, pName)
            if (EspFeatures.health && e.hpMax > 0) {
                val pct = (e.hp.toFloat() / e.hpMax).coerceIn(0f, 1f)
                pBar.color = if (pct < 0.3f) 0xFFB42D2D.toInt() else if (pct <= 0.6f) 0xFFB4B42D.toInt() else 0xFF00FF00.toInt()
                canvas.drawRoundRect(left, top + boxH + 3f, left + boxW, top + boxH + 8f, 2f, 2f, pBarBg)
                canvas.drawRoundRect(left, top + boxH + 3f, left + boxW * pct, top + boxH + 8f, 2f, 2f, pBar)
            }
            if (EspFeatures.distance) canvas.drawText("${e.meters}m", x, top + boxH + 24f, pName)
        }
    }
}
