package com.vasdk

internal interface Native {
    fun vasdk_widget_start(widget: Pointer?): Int
    fun vasdk_widget_stop(widget: Pointer?)
}

class Widget {
    fun start() = Unit
}
