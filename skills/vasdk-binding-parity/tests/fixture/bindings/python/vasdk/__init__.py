_start = _declare("vasdk_widget_start", _i, _p)
_stop = _declare("vasdk_widget_stop", None, _p)
_old = _declare("vasdk_widget_old", None, _p)   # gone from the C API


class Widget:
    def start(self):
        _start(None)

    def stop(self):
        _stop(None)
