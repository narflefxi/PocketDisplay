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

    private val arrowPath = Path().apply {
        moveTo(  0f * s,   0f * s)   // tip (hotspot)
        lineTo(  0f * s,  17f * s)   // left edge
        lineTo(  4f * s,  13f * s)   // notch
        lineTo(  8f * s,  21f * s)   // tail bottom-left
        lineTo( 11f * s,  19f * s)   // tail bottom-right
        lineTo(  7f * s,  11f * s)   // tail top-right
        lineTo( 12f * s,  11f * s)   // body right
        close()                       // ~45° diagonal back to tip
    }

    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.FILL
    }
    private val outlinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeWidth = 2f * resources.displayMetrics.density
        strokeJoin = Paint.Join.ROUND
        strokeCap = Paint.Cap.ROUND
    }

    private var cursorX = 0f
    private var cursorY = 0f
    private var cursorVisible = false

    init {
        isClickable  = false
        isFocusable  = false
    }

    /** Move the cursor to screen position [x], [y] and make it visible. */
    fun moveTo(x: Float, y: Float) {
        cursorX = x
        cursorY = y
        if (!cursorVisible) cursorVisible = true
        invalidate()
    }

    /** Hide the cursor (e.g. when streaming stops). */
    fun hide() {
        cursorVisible = false
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        if (!cursorVisible) return
        canvas.save()
        canvas.translate(cursorX, cursorY)
        canvas.drawPath(arrowPath, outlinePaint)
        canvas.drawPath(arrowPath, fillPaint)
        canvas.restore()
    }
}
