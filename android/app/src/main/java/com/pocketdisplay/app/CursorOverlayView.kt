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

    // ── Arrow (type 0) — Windows classic pointer, hotspot at tip (0,0) ────────
    // ~20° tilt: left edge is straight vertical, body leans right.
    // Tip at (0,0). Notch at ~75% of left-edge height, then angled stem/tail.
    private val arrowPath = Path().apply {
        moveTo(  0f * s,   0f * s)    // TIP — hotspot
        lineTo(  0f * s,  20f * s)    // bottom of straight left edge
        lineTo(4.0f * s,  15.5f * s)  // notch cut-in
        lineTo(7.0f * s,  22f * s)    // tail bottom-left
        cubicTo(
            8.2f * s, 23f * s,
            9.8f * s, 22.5f * s,
            10.2f * s, 21.0f * s      // tail bottom-right (rounded end)
        )
        lineTo(6.8f * s,  13.5f * s)  // back up the tail inner edge
        lineTo(12.5f * s, 13.5f * s)  // right side of arrowhead body
        cubicTo(
            13.2f * s, 13.5f * s,
            13.5f * s, 12.8f * s,
            13.0f * s, 12.0f * s      // top-right rounded corner
        )
        lineTo(  1.0f * s,  0.5f * s) // top edge back toward tip
        close()
    }

    // ── I-Beam (type 1) — text caret, hotspot at center (drawn at cursorX/Y) ──
    // Drawn directly in drawIBeam(); no pre-built path needed.

    // ── Hourglass (type 2 / wait) — two triangles pinched at center ───────────
    // Top triangle flat side up, bottom flat side down; outline at frame edges.
    private val hourglassPath = Path().apply {
        val w = 6.5f * s; val h = 10f * s; val nw = 1.2f * s
        // top triangle (apex points down toward center)
        moveTo(-w,  -h)
        lineTo( w,  -h)
        lineTo( nw, -0.5f * s)
        lineTo(-nw, -0.5f * s)
        close()
        // bottom triangle (apex points up toward center)
        moveTo(-nw,  0.5f * s)
        lineTo( nw,  0.5f * s)
        lineTo( w,   h)
        lineTo(-w,   h)
        close()
        // top bar cap
        moveTo(-w,  -h - 1.5f * s)
        lineTo( w,  -h - 1.5f * s)
        lineTo( w,  -h)
        lineTo(-w,  -h)
        close()
        // bottom bar cap
        moveTo(-w,  h)
        lineTo( w,  h)
        lineTo( w,  h + 1.5f * s)
        lineTo(-w,  h + 1.5f * s)
        close()
    }

    // ── Cross / crosshair (type 3) — thin plus with center dot ────────────────
    // Drawn directly in drawCross().

    // ── Double-headed arrow (resize types 4-7) — slim shaft, crisp arrowheads ─
    // Oriented horizontally; rotated in drawDoubleArrow().
    private val dblArrowPath = Path().apply {
        val sw = 1.8f * s  // shaft half-width
        val sl = 5.5f * s  // shaft half-length (where head begins)
        val hw = 4.5f * s  // arrowhead half-width (perpendicular)
        val tl = sl + 5.5f * s // total half-length (arrowhead tip)
        // right arrowhead
        moveTo(tl,  0f)
        lineTo(sl, -hw)
        lineTo(sl, -sw)
        // shaft
        lineTo(-sl, -sw)
        // left arrowhead
        lineTo(-sl, -hw)
        lineTo(-tl,  0f)
        lineTo(-sl,  hw)
        lineTo(-sl,  sw)
        // shaft back
        lineTo( sl,  sw)
        lineTo( sl,  hw)
        close()
    }

    // ── Hand / pointer (type 9) — pointing hand, hotspot at fingertip (0,0) ───
    // Extended index finger pointing up, three curled fingers + thumb below.
    // Single closed contour, CW winding (Android fills non-zero).
    //
    // Layout (dp units, *s applied):
    //   ifw=2   index finger half-width; tip at (0,0) is the hotspot
    //   fTop=9  y where finger meets fist top (knuckle row base)
    //   fBot=24 fist bottom y
    //   fL=-5.5 fist left edge,  fR=6.5 fist right edge
    //   Knuckle columns (right of index): middle 2..4, ring 3.5..5.5, pinky 5..6.5
    //   Thumb bump protrudes left from fL between y=fTop+5 and y=fTop
    //
    private val handPath = Path().apply {
        val ifw  =  2.0f * s   // index finger half-width
        val fTop =  9.0f * s   // fist top y (base of knuckle row)
        val fBot = 24.0f * s   // fist bottom y
        val fL   = -5.5f * s   // fist left edge x
        val fR   =  6.5f * s   // fist right edge x
        // Knuckle columns to the RIGHT of the index finger (middle, ring, pinky):
        val mL = ifw;            val mR =  4.0f * s   // middle finger
        val rL = 3.5f * s;       val rR =  5.5f * s   // ring finger
        val pL = 5.0f * s;       val pR =  fR         // pinky finger

        // Single closed contour, CW winding:
        // 1. Down right side of index
        // 2. Knuckle bumps rightward from index to fist right
        // 3. Down right fist, across bottom, up left fist
        // 4. Thumb bump on left
        // 5. Straight left-to-right along flat fist top back to index left
        // 6. Up left side of index to close

        moveTo( ifw,   0f)                             // index tip-right
        lineTo( ifw,   fTop)                           // index right edge → fist top
        // knuckle bumps going rightward
        quadTo((mL + mR) / 2f, fTop - 4.5f * s,  mR,  fTop)  // middle
        quadTo((rL + rR) / 2f, fTop - 3.5f * s,  rR,  fTop)  // ring
        quadTo((pL + pR) / 2f, fTop - 2.5f * s,  fR,  fTop)  // pinky → fist right
        // right side + bottom of fist
        lineTo( fR,    fBot - 2.5f * s)
        quadTo( fR,    fBot,  fR - 2.5f * s, fBot)    // bottom-right corner
        lineTo( fL + 2.5f * s, fBot)
        quadTo( fL,    fBot,  fL, fBot - 2.5f * s)    // bottom-left corner
        // left side up to thumb
        lineTo( fL,    fTop + 5.0f * s)
        // thumb knuckle bump (protrudes left of fist)
        quadTo( fL - 3.5f * s, fTop + 4.5f * s,  fL - 4.0f * s, fTop + 2.5f * s)
        quadTo( fL - 3.5f * s, fTop + 0.5f * s,  fL, fTop)
        // flat fist top going right to index left base
        lineTo(-ifw,   fTop)
        // up left edge of index to tip
        lineTo(-ifw,   0f)
        close()                                         // back to (ifw, 0)
    }

    // ── No / not-allowed (type 10) — circle + diagonal slash ─────────────────
    // Drawn directly in drawNo().

    // ── Paints ────────────────────────────────────────────────────────────────
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.FILL
    }
    private val outlinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * s
        strokeJoin = Paint.Join.ROUND
        strokeCap = Paint.Cap.ROUND
    }
    private val thinOutline = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeWidth = 1.2f * s
        strokeJoin = Paint.Join.ROUND
        strokeCap = Paint.Cap.ROUND
    }
    private val whiteLine = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.STROKE
        strokeWidth = 1.0f * s
        strokeCap = Paint.Cap.ROUND
    }
    private val blackLine = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeWidth = 2.5f * s
        strokeCap = Paint.Cap.ROUND
    }
    private val blackDot = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.FILL
    }
    private val noPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeWidth = 3.0f * s
        strokeCap = Paint.Cap.ROUND
    }
    private val noWhite = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * s
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
        // Proportions: total height ~22dp, serif width ~5dp, stem width ~1.5dp
        val sh = 11f * s   // half-height
        val sw = 4.5f * s  // serif half-width (top & bottom caps)
        val cx = cursorX;  val cy = cursorY
        // vertical stem
        canvas.drawLine(cx, cy - sh, cx, cy + sh, blackLine)
        canvas.drawLine(cx, cy - sh, cx, cy + sh, whiteLine)
        // top serif
        canvas.drawLine(cx - sw, cy - sh, cx + sw, cy - sh, blackLine)
        canvas.drawLine(cx - sw, cy - sh, cx + sw, cy - sh, whiteLine)
        // bottom serif
        canvas.drawLine(cx - sw, cy + sh, cx + sw, cy + sh, blackLine)
        canvas.drawLine(cx - sw, cy + sh, cx + sw, cy + sh, whiteLine)
    }

    private fun drawWait(canvas: Canvas) {
        canvas.save()
        canvas.translate(cursorX, cursorY)
        canvas.drawPath(hourglassPath, outlinePaint)
        canvas.drawPath(hourglassPath, fillPaint)
        canvas.restore()
    }

    private fun drawCross(canvas: Canvas) {
        val a = 11f * s   // arm half-length
        val dotR = 1.8f * s
        canvas.drawLine(cursorX - a, cursorY, cursorX + a, cursorY, blackLine)
        canvas.drawLine(cursorX, cursorY - a, cursorX, cursorY + a, blackLine)
        canvas.drawLine(cursorX - a, cursorY, cursorX + a, cursorY, whiteLine)
        canvas.drawLine(cursorX, cursorY - a, cursorX, cursorY + a, whiteLine)
        canvas.drawCircle(cursorX, cursorY, dotR, blackDot)
    }

    private fun drawMove(canvas: Canvas) {
        canvas.save()
        canvas.translate(cursorX, cursorY)
        // Draw all four outlines first, then all four fills
        for (angle in listOf(0f, 90f)) {
            canvas.save(); canvas.rotate(angle)
            canvas.drawPath(dblArrowPath, outlinePaint)
            canvas.restore()
        }
        for (angle in listOf(0f, 90f)) {
            canvas.save(); canvas.rotate(angle)
            canvas.drawPath(dblArrowPath, fillPaint)
            canvas.restore()
        }
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
        // Draw subtle finger-separation lines between index and middle finger
        val ifw = 2.0f * s
        val fTop = 9.0f * s
        thinOutline.strokeWidth = 0.8f * s
        canvas.drawLine(-ifw, fTop, -ifw, fTop + 6f * s, thinOutline)
        canvas.drawLine( ifw, fTop,  ifw, fTop + 6f * s, thinOutline)
        canvas.restore()
    }

    private fun drawNo(canvas: Canvas) {
        val r  = 10f * s
        val d  = (r * 0.7071f)   // r * cos(45°)
        canvas.drawCircle(cursorX, cursorY, r, noPaint)
        canvas.drawCircle(cursorX, cursorY, r, noWhite)
        canvas.drawLine(cursorX - d, cursorY - d, cursorX + d, cursorY + d, noPaint)
        canvas.drawLine(cursorX - d, cursorY - d, cursorX + d, cursorY + d, noWhite)
    }
}
