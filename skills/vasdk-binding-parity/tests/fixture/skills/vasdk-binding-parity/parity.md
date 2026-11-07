# Fixture manifest: every rule broken once

## Audited C++

| Header | Class | Audit |
| --- | --- | --- |
| `VASDK/include/VASDK/Widget.h` | `Widget` | methods |

## Capabilities

| C++ | C | Python | Kotlin | Note |
| --- | --- | --- | --- | --- |
| `Widget::start` | `vasdk_widget_start` | `Widget.begin` | `Widget.start` | Python names a method it doesn't have |
| `Widget::stop` | `vasdk_widget_stop` | `Widget.stop` | — | Kotlin missing |
| `Widget::internals` | `vasdk_widget_start` | — | — | C++ only: but it names a C function |
| `Widget::gone` | — | — | — | C++ only: the method was removed |

## C functions the bindings leave out

| C function | Covered by |
| --- | --- |
| `vasdk_widget_start_json` | `vasdk_widget_start` |
| `vasdk_widget_missing` | nothing: it doesn't exist |
