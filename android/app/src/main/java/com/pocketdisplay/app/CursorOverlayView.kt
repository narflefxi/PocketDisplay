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

    // Arrow defined in a 26-unit grid; scaled to 24 dp on screen.
    private val k = 24f * resources.displayMetrics.density / 26f

    private val arrowPath = Path().apply {
        // Hotspot at origin (0,0) = tip of the arrow (top-left corner)
        moveTo(0f,      0f     )   // tip
        lineTo(0f,      k*20f  )   // down the left edge
        lineTo(k*6f,    k*15f  )   // left shoulder of tail
        lineTo(k*11f,   k*26f  )   // tail tip (extends below body)
        lineTo(k*14f,   k*23f  )   // right of tail tip
        lineTo(k*8f,    k*12f  )   // right shoulder of tail
        lineTo(k*14f,   k*12f  )   // top-right of arrow body
        close()                    // diagonal back to tip
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
        // Draw outline first so it sits behind the white fill
        canvas.drawPath(arrowPath, outlinePaint)
        canvas.drawPath(arrowPath, fillPaint)
        canvas.restore()
    }
}
