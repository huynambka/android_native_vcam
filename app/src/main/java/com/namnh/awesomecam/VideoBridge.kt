package com.namnh.awesomecam

object VideoBridge {
    const val FRAME_FORMAT_I420: Int = 1

    init {
        System.loadLibrary("streamclient")
    }

    external fun nativeConnect(): Boolean
    external fun nativePushFrame(width: Int, height: Int, format: Int, frameData: ByteArray): Boolean
    external fun nativeClearFrame(): Boolean

    fun connect(): Boolean = nativeConnect()
    fun pushI420(width: Int, height: Int, frameData: ByteArray): Boolean =
        nativePushFrame(width, height, FRAME_FORMAT_I420, frameData)

    fun clear(): Boolean = nativeClearFrame()
}
