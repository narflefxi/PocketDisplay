package com.pocketdisplay.app

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.util.AttributeSet
import android.view.View

/**
 * Transparent full-screen overlay that draws a Windows-style arrow cursor.
 *
 * Sits above the TextureView in z-order. Touch events pass through because
 * isClickable and isFocusable are both false.
 *
 * Call [moveTo] from the touch handler (raw screen coordinates — no transform
 * needed because this view and the finger occupy the same screen space).
 * Call [hide] when streaming stops.
 */
class CursorOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    private val s = resources.displayMetrics.density

    // ── Arrow (type 0 / default) — hotspot at tip (0,0) ──────────────────────
    private val arrowPath = Path().apply {
        moveTo(  0f * s,   0f * s)
        lineTo(  0f * s,  17f * s)
        lineTo(  4f * s,  13f * s)
        lineTo(  8f * s,  21f * s)
        lineTo( 11f * s,  19f * s)
        lineTo(  7f * s,  11f * s)
        lineTo( 12f * s,  11f * s)
        close()
    }

    // ── Double-headed arrow (resize) — centered at origin, horizontal ─────────
    private val dblArrowPath = Path().apply {
        val shaft = 7f * s; val hw = 3.5f * s; val ht = 5f * s
        moveTo(-(shaft + ht), 0f); lineTo(-shaft, -hw); lineTo( shaft, -hw)
        lineTo( shaft + ht,  0f); lineTo( shaft,  hw); lineTo(-shaft,  hw)
        close()
    }

    // ── Hourglass (type 2 / wait) — centered at origin ───────────────────────
    private val hourglassPath = Path().apply {
        val w = 7f * s; val h = 9f * s
        moveTo(-w, -h); lineTo(w, -h); lineTo(0f, 0f); close()
        moveTo( 0f, 0f); lineTo(-w, h); lineTo(w, h);  close()
    }

    // ── Hand / pointer (type 9) — upward arrow, hotspot at tip (0,0) ─────────
    private val handPath = Path().apply {
        val hw = 5f * s; val hh = 7f * s; val sw = 2.5f * s; val sh = 11f * s
        moveTo(0f, 0f)
        lineTo(-hw, hh); lineTo(-sw, hh); lineTo(-sw, hh + sh)
        lineTo( sw, hh + sh); lineTo(sw, hh); lineTo(hw, hh)
        close()
    }

    // ── Paints ────────────────────────────────────────────────────────────────
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.FILL
    }
    private val outlinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeWidth = 2f * s
        strokeJoin = Paint.Join.ROUND
        strokeCap = Paint.Cap.ROUND
    }
    private val whiteLine = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * s
        strokeCap = Paint.Cap.ROUND
    }
    private val blackLine = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeWidth = 3f * s
        strokeCap = Paint.Cap.ROUND
    }

    private var cursorX = 0f
    private var cursorY = 0f
    private var cursorType = 0
    private var cursorVisible = false

    init {
        isClickable  = false
        isFocusable  = false
    }

    fun moveTo(x: Float, y: Float, type: Int = 0) {
        cursorX = x; cursorY = y; cursorType = type
        cursorVisible = true
        invalidate()
    }

    /** Hide the cursor (e.g. when streaming stops). */
    fun hide() {
        cursorVisible = false
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        if (!cursorVisible) return
        when (cursorType) {
            1     -> drawIBeam(canvas)
            2     -> drawWait(canvas)
            3     -> drawCross(canvas)
            4     -> drawDoubleArrow(canvas, 0f)
            5     -> drawDoubleArrow(canvas, 90f)
            6     -> drawDoubleArrow(canvas, -45f)
            7     -> drawDoubleArrow(canvas, 45f)
            8     -> { drawDoubleArrow(canvas, 0f); drawDoubleArrow(canvas, 90f) }
            9     -> drawHand(canvas)
            10    -> drawNo(canvas)
            else  -> drawArrow(canvas)
        }
    }

    private fun drawArrow(canvas: Canvas) {
        canvas.save()
        canvas.translate(cursorX, cursorY)
        canvas.drawPath(arrowPath, outlinePaint)
        canvas.drawPath(arrowPath, fillPaint)
        canvas.restore()
    }

    private fun drawIBeam(canvas: Canvas) {
        val cw = 4f * s; val ch = 9f * s
        canvas.drawLine(cursorX - cw, cursorY - ch, cursorX + cw, cursorY - ch, blackLine)
        canvas.drawLine(cursorX,      cursorY - ch, cursorX,      cursorY + ch, blackLine)
        canvas.drawLine(cursorX - cw, cursorY + ch, cursorX + cw, cursorY + ch, blackLine)
        canvas.drawLine(cursorX - cw, cursorY - ch, cursorX + cw, cursorY - ch, whiteLine)
        canvas.drawLine(cursorX,      cursorY - ch, cursorX,      cursorY + ch, whiteLine)
        canvas.drawLine(cursorX - cw, cursorY + ch, cursorX + cw, cursorY + ch, whiteLine)
    }

    private fun drawWait(canvas: Canvas) {
        canvas.save()
        canvas.translate(cursorX, cursorY)
        canvas.drawPath(hourglassPath, outlinePaint)
        canvas.drawPath(hourglassPath, fillPaint)
        canvas.restore()
    }

    private fun drawCross(canvas: Canvas) {
        val a = 10f * s
        canvas.drawLine(cursorX - a, cursorY, cursorX + a, cursorY, blackLine)
        canvas.drawLine(cursorX, cursorY - a, cursorX, cursorY + a, blackLine)
        canvas.drawLine(cursorX - a, cursorY, cursorX + a, cursorY, whiteLine)
        canvas.drawLine(cursorX, cursorY - a, cursorX, cursorY + a, whiteLine)
        canvas.drawCircle(cursorX, cursorY, 2f * s, fillPaint)
    }

    private fun drawDoubleArrow(canvas: Canvas, angleDeg: Float) {
        canvas.save()
        canvas.translate(cursorX, cursorY)
        canvas.rotate(angleDeg)
        canvas.drawPath(dblArrowPath, outlinePaint)
        canvas.drawPath(dblArrowPath, fillPaint)
        canvas.restore()
    }

    private fun drawHand(canvas: Canvas) {
        canvas.save()
        canvas.translate(cursorX, cursorY)
        canvas.drawPath(handPath, outlinePaint)
        canvas.drawPath(handPath, fillPaint)
        canvas.restore()
    }

    private fun drawNo(canvas: Canvas) {
        val r = 9f * s; val d = r * 0.707f
        canvas.drawCircle(cursorX, cursorY, r, blackLine)
        canvas.drawCircle(cursorX, cursorY, r, whiteLine)
        canvas.drawLine(cursorX - d, cursorY - d, cursorX + d, cursorY + d, blackLine)
        canvas.drawLine(cursorX - d, cursorY - d, cursorX + d, cursorY + d, whiteLine)
    }
}
