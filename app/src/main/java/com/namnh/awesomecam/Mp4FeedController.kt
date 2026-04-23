package com.namnh.awesomecam

import android.media.Image
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import java.io.File
import java.io.FileInputStream
import kotlin.concurrent.thread
import kotlin.math.max
import kotlin.math.sqrt

class Mp4FeedController(
    private val postStatus: (String) -> Unit,
) {
    @Volatile
    private var running: Boolean = false

    @Volatile
    private var worker: Thread? = null

    @Volatile
    private var lastBinderScaleNote: String? = null

    val isRunning: Boolean
        get() = running

    fun start(path: String = DEFAULT_VIDEO_PATH) {
        if (running) return
        running = true
        worker = thread(start = true, name = "mp4-feed") {
            try {
                if (!VideoBridge.connect()) {
                    postStatus("Video feed: binder service unavailable. Inject hook first.")
                    return@thread
                }
                val decodePath = prepareReadableSource(path)
                if (!File(decodePath).exists()) {
                    postStatus("Video feed: missing source $decodePath")
                    return@thread
                }
                postStatus("Video feed: decoding $decodePath")
                while (running) {
                    decodeOneLoop(decodePath)
                    if (running) {
                        postStatus("Video feed: reached EOF, looping $decodePath")
                    }
                }
            } catch (t: Throwable) {
                postStatus("Video feed error: ${t.message}")
            } finally {
                VideoBridge.clear()
                running = false
                worker = null
                postStatus("Video feed: stopped")
            }
        }
    }

    fun stop() {
        running = false
        worker?.interrupt()
    }


    private fun prepareReadableSource(path: String): String {
        val source = File(path)
        require(source.exists()) { "Missing source $path" }
        return source.absolutePath
    }

    private fun decodeOneLoop(path: String) {
        val extractor = MediaExtractor()
        var codec: MediaCodec? = null
        var inputStream: FileInputStream? = null
        try {
            inputStream = FileInputStream(path)
            extractor.setDataSource(inputStream.fd)
            val trackIndex = selectVideoTrack(extractor)
            require(trackIndex >= 0) { "No video track in $path" }
            extractor.selectTrack(trackIndex)
            val format = extractor.getTrackFormat(trackIndex)
            val mime = format.getString(MediaFormat.KEY_MIME)
                ?: error("Missing MIME for video track")
            val fps = when {
                format.containsKey(MediaFormat.KEY_FRAME_RATE) ->
                    max(1, format.getInteger(MediaFormat.KEY_FRAME_RATE))
                else -> DEFAULT_FPS
            }
            val frameDelayMs = max(1L, 1000L / fps.toLong())

            codec = MediaCodec.createDecoderByType(mime)
            codec.configure(format, null, null, 0)
            codec.start()

            val bufferInfo = MediaCodec.BufferInfo()
            var inputDone = false
            var outputDone = false
            var sentFrames = 0

            while (running && !outputDone) {
                if (!inputDone) {
                    val inputIndex = codec.dequeueInputBuffer(TIMEOUT_US)
                    if (inputIndex >= 0) {
                        val inputBuffer = codec.getInputBuffer(inputIndex)
                            ?: error("Input buffer $inputIndex is null")
                        val sampleSize = extractor.readSampleData(inputBuffer, 0)
                        if (sampleSize < 0) {
                            codec.queueInputBuffer(
                                inputIndex,
                                0,
                                0,
                                0,
                                MediaCodec.BUFFER_FLAG_END_OF_STREAM,
                            )
                            inputDone = true
                        } else {
                            val ptsUs = extractor.sampleTime.coerceAtLeast(0L)
                            codec.queueInputBuffer(inputIndex, 0, sampleSize, ptsUs, 0)
                            extractor.advance()
                        }
                    }
                }

                when (val outputIndex = codec.dequeueOutputBuffer(bufferInfo, TIMEOUT_US)) {
                    MediaCodec.INFO_TRY_AGAIN_LATER -> Unit
                    MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        postStatus("Video feed: decoder output ${codec.outputFormat}")
                    }
                    MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED -> Unit
                    else -> {
                        if (outputIndex >= 0) {
                            if ((bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                                outputDone = true
                            }

                            if (bufferInfo.size > 0) {
                                val image = codec.getOutputImage(outputIndex)
                                if (image != null) {
                                    image.use {
                                        val frame = imageToI420(it)
                                        val binderFrame = fitI420ForBinder(it.width, it.height, frame)
                                        if (!VideoBridge.pushI420(
                                                binderFrame.width,
                                                binderFrame.height,
                                                binderFrame.bytes,
                                            )
                                        ) {
                                            throw IllegalStateException("Binder pushFrame failed")
                                        }
                                        sentFrames += 1
                                        if (sentFrames <= 5 || sentFrames % 120 == 0) {
                                            postStatus(
                                                "Video feed: pushed frame #$sentFrames " +
                                                    "${binderFrame.width}x${binderFrame.height}",
                                            )
                                        }
                                    }
                                    Thread.sleep(frameDelayMs)
                                }
                            }
                            codec.releaseOutputBuffer(outputIndex, false)
                        }
                    }
                }
            }
        } finally {
            runCatching { codec?.stop() }
            runCatching { codec?.release() }
            runCatching { inputStream?.close() }
            runCatching { extractor.release() }
        }
    }

    private fun selectVideoTrack(extractor: MediaExtractor): Int {
        for (index in 0 until extractor.trackCount) {
            val format = extractor.getTrackFormat(index)
            val mime = format.getString(MediaFormat.KEY_MIME) ?: continue
            if (mime.startsWith("video/")) return index
        }
        return -1
    }

    private fun imageToI420(image: Image): ByteArray {
        val width = image.width
        val height = image.height
        val chromaWidth = (width + 1) / 2
        val chromaHeight = (height + 1) / 2
        val out = ByteArray(width * height + 2 * chromaWidth * chromaHeight)

        val yPlane = image.planes[0]
        copyPlane(
            plane = yPlane,
            width = width,
            height = height,
            out = out,
            offset = 0,
            outRowStride = width,
        )

        val uOffset = width * height
        val vOffset = uOffset + chromaWidth * chromaHeight
        copyPlane(
            plane = image.planes[1],
            width = chromaWidth,
            height = chromaHeight,
            out = out,
            offset = uOffset,
            outRowStride = chromaWidth,
        )
        copyPlane(
            plane = image.planes[2],
            width = chromaWidth,
            height = chromaHeight,
            out = out,
            offset = vOffset,
            outRowStride = chromaWidth,
        )
        return out
    }

    private data class BinderFrame(
        val width: Int,
        val height: Int,
        val bytes: ByteArray,
    )

    private fun fitI420ForBinder(width: Int, height: Int, bytes: ByteArray): BinderFrame {
        if (bytes.size <= MAX_BINDER_FRAME_BYTES) {
            maybePostBinderScaleNote(null)
            return BinderFrame(width, height, bytes)
        }

        val scale = sqrt(MAX_BINDER_FRAME_BYTES.toDouble() / bytes.size.toDouble())
        var targetWidth = makeEven(max(2, (width * scale).toInt()))
        var targetHeight = makeEven(max(2, (height * scale).toInt()))
        if (targetWidth > width) targetWidth = makeEven(width)
        if (targetHeight > height) targetHeight = makeEven(height)

        while (targetWidth > 2 && targetHeight > 2 &&
            i420Size(targetWidth, targetHeight) > MAX_BINDER_FRAME_BYTES
        ) {
            targetWidth = makeEven(max(2, targetWidth - 2))
            targetHeight = makeEven(max(2, targetHeight - 2))
        }

        val scaled = scaleI420Nearest(bytes, width, height, targetWidth, targetHeight)
        maybePostBinderScaleNote(
            "Video feed: scaled frame for binder ${width}x${height} -> " +
                "${targetWidth}x${targetHeight} (${scaled.size} bytes)",
        )
        return BinderFrame(targetWidth, targetHeight, scaled)
    }

    private fun maybePostBinderScaleNote(note: String?) {
        if (lastBinderScaleNote == note) return
        lastBinderScaleNote = note
        if (note != null) postStatus(note)
    }

    private fun scaleI420Nearest(
        src: ByteArray,
        srcWidth: Int,
        srcHeight: Int,
        dstWidth: Int,
        dstHeight: Int,
    ): ByteArray {
        val dst = ByteArray(i420Size(dstWidth, dstHeight))
        val srcYSize = srcWidth * srcHeight
        val srcChromaWidth = (srcWidth + 1) / 2
        val srcChromaHeight = (srcHeight + 1) / 2
        val srcChromaSize = srcChromaWidth * srcChromaHeight

        val dstYSize = dstWidth * dstHeight
        val dstChromaWidth = (dstWidth + 1) / 2
        val dstChromaHeight = (dstHeight + 1) / 2
        val dstChromaSize = dstChromaWidth * dstChromaHeight

        scalePlaneNearest(
            src = src,
            srcOffset = 0,
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            srcStride = srcWidth,
            dst = dst,
            dstOffset = 0,
            dstWidth = dstWidth,
            dstHeight = dstHeight,
            dstStride = dstWidth,
        )
        scalePlaneNearest(
            src = src,
            srcOffset = srcYSize,
            srcWidth = srcChromaWidth,
            srcHeight = srcChromaHeight,
            srcStride = srcChromaWidth,
            dst = dst,
            dstOffset = dstYSize,
            dstWidth = dstChromaWidth,
            dstHeight = dstChromaHeight,
            dstStride = dstChromaWidth,
        )
        scalePlaneNearest(
            src = src,
            srcOffset = srcYSize + srcChromaSize,
            srcWidth = srcChromaWidth,
            srcHeight = srcChromaHeight,
            srcStride = srcChromaWidth,
            dst = dst,
            dstOffset = dstYSize + dstChromaSize,
            dstWidth = dstChromaWidth,
            dstHeight = dstChromaHeight,
            dstStride = dstChromaWidth,
        )
        return dst
    }

    private fun scalePlaneNearest(
        src: ByteArray,
        srcOffset: Int,
        srcWidth: Int,
        srcHeight: Int,
        srcStride: Int,
        dst: ByteArray,
        dstOffset: Int,
        dstWidth: Int,
        dstHeight: Int,
        dstStride: Int,
    ) {
        for (y in 0 until dstHeight) {
            val srcY = (y * srcHeight) / dstHeight
            val srcRow = srcOffset + srcY * srcStride
            val dstRow = dstOffset + y * dstStride
            for (x in 0 until dstWidth) {
                val srcX = (x * srcWidth) / dstWidth
                dst[dstRow + x] = src[srcRow + srcX]
            }
        }
    }

    private fun i420Size(width: Int, height: Int): Int {
        val chromaWidth = (width + 1) / 2
        val chromaHeight = (height + 1) / 2
        return width * height + 2 * chromaWidth * chromaHeight
    }

    private fun makeEven(value: Int): Int = if ((value and 1) == 0) value else value - 1

    private fun copyPlane(
        plane: Image.Plane,
        width: Int,
        height: Int,
        out: ByteArray,
        offset: Int,
        outRowStride: Int,
    ) {
        val buffer = plane.buffer
        val rowStride = plane.rowStride
        val pixelStride = plane.pixelStride
        for (row in 0 until height) {
            val rowBase = row * rowStride
            val outBase = offset + row * outRowStride
            for (col in 0 until width) {
                out[outBase + col] = buffer.get(rowBase + col * pixelStride)
            }
        }
    }

    private inline fun Image.use(block: (Image) -> Unit) {
        try {
            block(this)
        } finally {
            close()
        }
    }

    companion object {
        private const val DEFAULT_FPS = 30
        private const val TIMEOUT_US = 10_000L
        private const val MAX_BINDER_FRAME_BYTES = 786_432
        const val DEFAULT_VIDEO_PATH: String = "/data/camera/input.mp4"
    }
}
