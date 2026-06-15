package com.pocketdisplay.app

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.drawable.Drawable
import android.util.AttributeSet
import android.view.View
import androidx.appcompat.content.res.AppCompatResources

class CursorOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    private val s = resources.displayMetrics.density
    private val cursorResIds = intArrayOf(
        R.drawable.cursor_arrow, R.drawable.cursor_ibeam, R.drawable.cursor_wait,
        R.drawable.cursor_cross, R.drawable.cursor_resize_h, R.drawable.cursor_resize_v,
        R.drawable.cursor_resize_nwse, R.drawable.cursor_resize_nesw, R.drawable.cursor_move,
        R.drawable.cursor_hand, R.drawable.cursor_no
    )
    private val cachedDrawables = arrayOfNulls<Drawable?>(11)
    private val fallbackCursors = BooleanArray(11)
    private var cursorX = 0f
    private var cursorY = 0f
    private var cursorType = 0
    private var cursorVisible = false

    init {
        isClickable = false
        isFocusable = false
    }

    fun moveTo(x: Float, y: Float, type: Int = 0) {
        cursorX = x; cursorY = y; cursorType = type
        cursorVisible = true
        invalidate()
    }

    fun hide() {
        cursorVisible = false
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        if (!cursorVisible) return
        val type = cursorType.coerceIn(0, 10)
        val drawable = getOrLoadDrawable(type)
        if (drawable != null && !fallbackCursors[type]) {
            drawCursorWithHotspot(canvas, drawable, type)
        } else {
            drawFallback(canvas, type)
        }
    }

    private fun getOrLoadDrawable(type: Int): Drawable? {
        cachedDrawables[type]?.let { return it }
        return try {
            val drawable = AppCompatResources.getDrawable(context, cursorResIds[type])?.mutate()
            cachedDrawables[type] = drawable
            drawable
        } catch (e: Exception) {
            fallbackCursors[type] = true
            null
        }
    }

    private fun drawCursorWithHotspot(canvas: Canvas, drawable: Drawable, type: Int) {
        val w = drawable.intrinsicWidth
        val h = drawable.intrinsicHeight
        // Per-type fractional hotspot (hx, hy): fraction of drawable size where the
        // logical click point sits. left = cursorX - hx*w, top = cursorY - hy*h.
        // Drawables now fill their viewport naturally with NO hotspot translate group.
        //   type 0 arrow:      tip at ~(384,213) in 1024x1024 viewport → (0.375, 0.208)
        //   type 9 hand:       index fingertip at ~x=90,y=22.5 in 203.1x203.1 → (0.443, 0.111)
        //   all others:        hotspot at center → (0.5, 0.5)
        val hx: Float
        val hy: Float
        when (type) {
            0 -> { hx = 0.375f; hy = 0.208f }
            9 -> { hx = 0.443f; hy = 0.111f }
            else -> { hx = 0.5f; hy = 0.5f }
        }
        val left = (cursorX - hx * w).toInt()
        val top  = (cursorY - hy * h).toInt()
        drawable.setBounds(left, top, left + w, top + h)
        drawable.draw(canvas)
    }

    private fun drawFallback(canvas: Canvas, type: Int) {
        when (type) {
            1 -> drawFallbackIBeam(canvas)
            2 -> drawFallbackWait(canvas)
            3 -> drawFallbackCross(canvas)
            4 -> drawFallbackDoubleArrow(canvas, 0f)
            5 -> drawFallbackDoubleArrow(canvas, 90f)
            6 -> drawFallbackDoubleArrow(canvas, -45f)
            7 -> drawFallbackDoubleArrow(canvas, 45f)
            8 -> drawFallbackMove(canvas)
            9 -> drawFallbackHand(canvas)
            10 -> drawFallbackNo(canvas)
            else -> drawFallbackArrow(canvas)
        }
    }

    private fun drawFallbackArrow(canvas: Canvas) {
        val outlinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLACK
            style = Paint.Style.STROKE
            strokeWidth = 1.5f * s
            strokeJoin = Paint.Join.ROUND
            strokeCap = Paint.Cap.ROUND
        }
        val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = Paint.Style.FILL
        }
        val path = Path().apply {
            moveTo(cursorX, cursorY)
            lineTo(cursorX, cursorY + 20f * s)
            lineTo(cursorX + 4f * s, cursorY + 15.5f * s)
            lineTo(cursorX + 7f * s, cursorY + 22f * s)
            cubicTo(cursorX + 8.2f * s, cursorY + 23f * s,
                    cursorX + 9.8f * s, cursorY + 22.5f * s,
                    cursorX + 10.2f * s, cursorY + 21f * s)
            lineTo(cursorX + 6.8f * s, cursorY + 13.5f * s)
            lineTo(cursorX + 12.5f * s, cursorY + 13.5f * s)
            cubicTo(cursorX + 13.2f * s, cursorY + 13.5f * s,
                    cursorX + 13.5f * s, cursorY + 12.8f * s,
                    cursorX + 13f * s, cursorY + 12f * s)
            lineTo(cursorX + 1f * s, cursorY + 0.5f * s)
            close()
        }
        canvas.drawPath(path, outlinePaint)
        canvas.drawPath(path, fillPaint)
    }

    private fun drawFallbackIBeam(canvas: Canvas) {
        val sh = 11f * s
        val sw = 4.5f * s
        val blackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLACK
            style = Paint.Style.STROKE
            strokeWidth = 2.5f * s
            strokeCap = Paint.Cap.ROUND
        }
        val whitePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = Paint.Style.STROKE
            strokeWidth = 1.5f * s
            strokeCap = Paint.Cap.ROUND
        }
        val cx = cursorX
        val cy = cursorY
        canvas.drawLine(cx, cy - sh, cx, cy + sh, blackPaint)
        canvas.drawLine(cx, cy - sh, cx, cy + sh, whitePaint)
        canvas.drawLine(cx - sw, cy - sh, cx + sw, cy - sh, blackPaint)
        canvas.drawLine(cx - sw, cy - sh, cx + sw, cy - sh, whitePaint)
        canvas.drawLine(cx - sw, cy + sh, cx + sw, cy + sh, blackPaint)
        canvas.drawLine(cx - sw, cy + sh, cx + sw, cy + sh, whitePaint)
    }

    private fun drawFallbackWait(canvas: Canvas) {
        val outlinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLACK
            style = Paint.Style.STROKE
            strokeWidth = 1.5f * s
            strokeJoin = Paint.Join.ROUND
        }
        val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = Paint.Style.FILL
        }
        val w = 6.5f * s
        val h = 10f * s
        val nw = 1.2f * s
        val cx = cursorX
        val cy = cursorY
        val path = Path().apply {
            moveTo(cx - w, cy - h)
            lineTo(cx + w, cy - h)
            lineTo(cx + nw, cy - 0.5f * s)
            lineTo(cx - nw, cy - 0.5f * s)
            close()
            moveTo(cx - nw, cy + 0.5f * s)
            lineTo(cx + nw, cy + 0.5f * s)
            lineTo(cx + w, cy + h)
            lineTo(cx - w, cy + h)
            close()
            moveTo(cx - w, cy - h - 1.5f * s)
            lineTo(cx + w, cy - h - 1.5f * s)
            lineTo(cx + w, cy - h)
            lineTo(cx - w, cy - h)
            close()
            moveTo(cx - w, cy + h)
            lineTo(cx + w, cy + h)
            lineTo(cx + w, cy + h + 1.5f * s)
            lineTo(cx - w, cy + h + 1.5f * s)
            close()
        }
        canvas.drawPath(path, outlinePaint)
        canvas.drawPath(path, fillPaint)
    }

    private fun drawFallbackCross(canvas: Canvas) {
        val a = 11f * s
        val dotR = 1.8f * s
        val blackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLACK
            style = Paint.Style.STROKE
            strokeWidth = 2.5f * s
            strokeCap = Paint.Cap.ROUND
        }
        val whitePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = Paint.Style.STROKE
            strokeWidth = 1.5f * s
            strokeCap = Paint.Cap.ROUND
        }
        val dotPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLACK
            style = Paint.Style.FILL
        }
        canvas.drawLine(cursorX - a, cursorY, cursorX + a, cursorY, blackPaint)
        canvas.drawLine(cursorX, cursorY - a, cursorX, cursorY + a, blackPaint)
        canvas.drawLine(cursorX - a, cursorY, cursorX + a, cursorY, whitePaint)
        canvas.drawLine(cursorX, cursorY - a, cursorX, cursorY + a, whitePaint)
        canvas.drawCircle(cursorX, cursorY, dotR, dotPaint)
    }

    private fun drawFallbackDoubleArrow(canvas: Canvas, angleDeg: Float) {
        val outlinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLACK
            style = Paint.Style.STROKE
            strokeWidth = 1.5f * s
            strokeJoin = Paint.Join.ROUND
        }
        val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = Paint.Style.FILL
        }
        val sw = 1.8f * s
        val sl = 5.5f * s
        val hw = 4.5f * s
        val tl = sl + 5.5f * s
        val path = Path().apply {
            moveTo(tl, 0f)
            lineTo(sl, -hw)
            lineTo(sl, -sw)
            lineTo(-sl, -sw)
            lineTo(-sl, -hw)
            lineTo(-tl, 0f)
            lineTo(-sl, hw)
            lineTo(-sl, sw)
            lineTo(sl, sw)
            lineTo(sl, hw)
            close()
        }
        canvas.save()
        canvas.translate(cursorX, cursorY)
        canvas.rotate(angleDeg)
        canvas.drawPath(path, outlinePaint)
        canvas.drawPath(path, fillPaint)
        canvas.restore()
    }

    private fun drawFallbackMove(canvas: Canvas) {
        drawFallbackDoubleArrow(canvas, 0f)
        drawFallbackDoubleArrow(canvas, 90f)
    }

    private fun drawFallbackHand(canvas: Canvas) {
        val outlinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLACK
            style = Paint.Style.STROKE
            strokeWidth = 1.5f * s
            strokeJoin = Paint.Join.ROUND
        }
        val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = Paint.Style.FILL
        }
        val ifw = 2.0f * s
        val fTop = 9.0f * s
        val fBot = 24.0f * s
        val fL = -5.5f * s
        val fR = 6.5f * s
        val mL = ifw
        val mR = 4.0f * s
        val rL = 3.5f * s
        val rR = 5.5f * s
        val pL = 5.0f * s
        val pR = fR
        val path = Path().apply {
            moveTo(cursorX + ifw, cursorY)
            lineTo(cursorX + ifw, cursorY + fTop)
            quadTo(cursorX + (mL + mR) / 2f, cursorY + fTop - 4.5f * s,
                   cursorX + mR, cursorY + fTop)
            quadTo(cursorX + (rL + rR) / 2f, cursorY + fTop - 3.5f * s,
                   cursorX + rR, cursorY + fTop)
            quadTo(cursorX + (pL + pR) / 2f, cursorY + fTop - 2.5f * s,
                   cursorX + pR, cursorY + fTop)
            lineTo(cursorX + fR, cursorY + fBot - 2.5f * s)
            quadTo(cursorX + fR, cursorY + fBot,
                   cursorX + fR - 2.5f * s, cursorY + fBot)
            lineTo(cursorX + fL + 2.5f * s, cursorY + fBot)
            quadTo(cursorX + fL, cursorY + fBot,
                   cursorX + fL, cursorY + fBot - 2.5f * s)
            lineTo(cursorX + fL, cursorY + fTop + 5.0f * s)
            quadTo(cursorX + fL - 3.5f * s, cursorY + fTop + 4.5f * s,
                   cursorX + fL - 4.0f * s, cursorY + fTop + 2.5f * s)
            quadTo(cursorX + fL - 3.5f * s, cursorY + fTop + 0.5f * s,
                   cursorX + fL, cursorY + fTop)
            lineTo(cursorX - ifw, cursorY + fTop)
            lineTo(cursorX - ifw, cursorY)
            close()
        }
        canvas.drawPath(path, outlinePaint)
        canvas.drawPath(path, fillPaint)
    }

    private fun drawFallbackNo(canvas: Canvas) {
        val r = 10f * s
        val d = r * 0.7071f
        val blackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLACK
            style = Paint.Style.STROKE
            strokeWidth = 3.0f * s
            strokeCap = Paint.Cap.ROUND
        }
        val whitePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = Paint.Style.STROKE
            strokeWidth = 1.5f * s
            strokeCap = Paint.Cap.ROUND
        }
        canvas.drawCircle(cursorX, cursorY, r, blackPaint)
        canvas.drawCircle(cursorX, cursorY, r, whitePaint)
        canvas.drawLine(cursorX - d, cursorY - d,
                        cursorX + d, cursorY + d, blackPaint)
        canvas.drawLine(cursorX - d, cursorY - d,
                        cursorX + d, cursorY + d, whitePaint)
    }
}
