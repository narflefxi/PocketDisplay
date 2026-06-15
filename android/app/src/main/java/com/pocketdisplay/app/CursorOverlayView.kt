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

    // ── Arrow (type 0) — Windows classic, tail-notch, hotspot at tip (0,0) ────
    private val arrowPath = Path().apply {
        moveTo(  0f * s,   0f * s)   // tip
        lineTo(  0f * s,  18f * s)   // bottom of left edge
        lineTo(4.5f * s,  14f * s)   // notch
        lineTo(7.5f * s,  21f * s)   // tail bottom-left
        lineTo( 10f * s,  19.5f * s) // tail bottom-right
        lineTo(6.5f * s,  12.5f * s) // tail top (inner)
        lineTo( 11f * s,  12.5f * s) // arrowhead right
        close()
    }

    // ── Hand / pointer (type 9) — finger + fist, hotspot at fingertip (0,0) ──
    private val handPath = Path().apply {
        val fw = 2.5f * s   // finger half-width
        val fh = 10f * s    // finger length (tip → fist top)
        val fL = -5f * s    // fist left extent
        val fR =  9f * s    // fist right extent
        val fB = 22f * s    // fist bottom
        val cr =  2.5f * s  // bottom-corner rounding radius
        moveTo(-fw, 0f)
        lineTo(-fw, fh)
        lineTo(fL,  fh)
        lineTo(fL,  fB - cr)
        quadTo(fL,  fB, fL + cr, fB)
        lineTo(fR - cr, fB)
        quadTo(fR,  fB, fR, fB - cr)
        lineTo(fR,  fh)
        lineTo( fw, fh)
        lineTo( fw, 0f)
        close()
    }

    // ── Double-headed arrow (resize) — slim shaft + clean arrowheads, at (0,0)
    private val dblArrowPath = Path().apply {
        val sw = 1.5f * s; val sl = 6f * s   // shaft half-width, shaft half-len
        val hw = 4f * s;   val tl = sl + 5f * s // head half-width, total half-len
        moveTo(-tl, 0f); lineTo(-sl, -hw); lineTo(-sl, -sw)
        lineTo( sl, -sw); lineTo( sl, -hw); lineTo( tl, 0f)
        lineTo( sl,  hw); lineTo( sl,  sw); lineTo(-sl,  sw)
        lineTo(-sl,  hw); close()
    }

    // ── Hourglass (type 2 / wait) — centered at origin ───────────────────────
    private val hourglassPath = Path().apply {
        val w = 7f * s; val h = 9f * s
        moveTo(-w, -h); lineTo(w, -h); lineTo(0f, 0f); close()
        moveTo( 0f, 0f); lineTo(-w, h); lineTo(w, h);  close()
    }

    // ── Paints ────────────────────────────────────────────────────────────────
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.FILL
    }
    private val outlinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeWidth = 1.8f * s
        strokeJoin = Paint.Join.ROUND
        strokeCap = Paint.Cap.ROUND
    }
    private val whiteLine = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.STROKE
        strokeWidth = 1.2f * s
        strokeCap = Paint.Cap.ROUND
    }
    private val blackLine = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeWidth = 2.8f * s
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
            8     -> drawMove(canvas)
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
        val sw = 3.5f * s; val h = 9f * s   // serif half-width, half-height
        canvas.drawLine(cursorX - sw, cursorY - h, cursorX + sw, cursorY - h, blackLine)
        canvas.drawLine(cursorX,      cursorY - h, cursorX,      cursorY + h, blackLine)
        canvas.drawLine(cursorX - sw, cursorY + h, cursorX + sw, cursorY + h, blackLine)
        canvas.drawLine(cursorX - sw, cursorY - h, cursorX + sw, cursorY - h, whiteLine)
        canvas.drawLine(cursorX,      cursorY - h, cursorX,      cursorY + h, whiteLine)
        canvas.drawLine(cursorX - sw, cursorY + h, cursorX + sw, cursorY + h, whiteLine)
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

    private fun drawMove(canvas: Canvas) {
        canvas.save()
        canvas.translate(cursorX, cursorY)
        canvas.drawPath(dblArrowPath, outlinePaint)          // horizontal outline
        canvas.rotate(90f)
        canvas.drawPath(dblArrowPath, outlinePaint)          // vertical outline
        canvas.rotate(-90f)
        canvas.drawPath(dblArrowPath, fillPaint)             // horizontal fill
        canvas.rotate(90f)
        canvas.drawPath(dblArrowPath, fillPaint)             // vertical fill
        canvas.restore()
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
