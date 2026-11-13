---
name: vasdk-transport
description: Transports in VASDK - how frames enter and leave the process (LocalTransport for mic and speaker, WebSocket, HTTP, WebRTC browser calls over libdatachannel, MockTransport for tests, the FFmpeg file transport), the network provider seam, and writing a new transport. Use when connecting VASDK to a phone line, a browser, a server, or a test harness.
---

# Transports

**What it does:** carries frames between the pipeline and the outside world. `TransportInputProcessor` and `TransportOutputProcessor` sit at the two ends of every pipeline; the transport behind them can be the local sound card, a socket, a browser call, or a scripted test.

| | |
| --- | --- |
| Interface | `VASDK/include/VASDK/Transport.h`, [reference](../../docs/reference/transports.md#the-transport-interface) |
| Factory | `VASDK/include/VASDK/TransportFactory.h`, [notes](../../docs/reference/transports.md#transportfactory) |
| Network seam | `VASDK/include/VASDK/network/` (`HttpInterface.h`, `WebSocketInterface.h`, `WebRtcInterface.h`), [notes](../../docs/reference/transports.md#the-network-provider-seam) |
| Writing one | [writing a custom transport](../../docs/reference/transports.md#writing-a-custom-transport) |

## Built in

| Transport | Where | Use |
| --- | --- | --- |
| `LocalTransport` | `VASDK/include/VASDK/LocalTransport.h` | mic and speaker through a source and sink ([vasdk-audio-io](../vasdk-audio-io/SKILL.md)) |
| `WebSocketTransport` | `VASDK/include/VASDK/WebSocketTransport.h` | audio frames over a socket |
| `HttpTransport` | `VASDK/include/VASDK/HttpTransport.h` | request/response |
| `WebRtcTransport` | `VASDK/include/VASDK/WebRtcTransport.h` + `VASDK_3P_IMPL/LibDataChannelWebRtc/` (`-DVASDK_BUILD_LIBDATACHANNEL_WEBRTC=ON`) | browser calls with Opus; [notes](../../docs/reference/transports.md#webrtctransport) |
| `MockTransport` | `VASDK/include/VASDK/MockTransport.h` | tests: inject frames, assert on what comes out ([example](../../docs/examples/testing-with-mock-transport.md)) |
| `FileTransport` | `VASDK_3P_IMPL/FFmpegAudio/FileTransport.h` | batch audio from files |

## The pattern to follow

The core transport holds the protocol-independent logic: frames, state, threading, barge-in. A third-party library sits behind a **network interface** in `VASDK/include/VASDK/network/`, implemented in `VASDK_3P_IMPL/`. `WebRtcTransport` (core) over `WebRtcInterface` (seam) with `LibDataChannelWebRtcProvider` (3P) is the model. A new library for an existing protocol is only a new seam implementation.

## Contract pitfalls

- `receiveFrame(timeoutMs)` blocks at most `timeoutMs`; `-1` waits. `stop()` must unblock it.
- Audio frames carry offsets into the shared ring buffer, never samples: write input audio into the ring buffer, then send the frame.
- Upstream frames (`ErrorFrame`, `ControlFrame::INTERRUPT`, `SynthesisStateFrame`) must reach the remote side or be handled; a transport that drops them breaks barge-in.
- Network transports: reconnect or report a `CRITICAL` error; never fail silently.

## Adding one

- **A new protocol:** a core transport in `VASDK/include/VASDK/` + `VASDK/src/`, following [vasdk-core-component](../vasdk-core-component/SKILL.md) (generic, a doc section in `docs/reference/transports.md` with an example and a mermaid flow, bindings). Its library, if any, behind a new interface in `VASDK/include/VASDK/network/`.
- **A new library for an existing seam:** a 3P integration following [vasdk-provider](../vasdk-provider/SKILL.md) (`va::thirdparty::transport`, options with defaults, every platform or warn, its own page, licenses). Template: `VASDK_3P_IMPL/LibDataChannelWebRtc/` (`LibDataChannelWebRtcOptions`).

## Tests

How to write, label and register them: [vasdk-testing](../vasdk-testing/SKILL.md).

- Unit: in `VASDK/tests/`, like `vasdk_webrtc_transport` (`VASDK/tests/webrtc_transport_test.cpp`), with a fake seam: frames in, frames out, stop unblocks, interrupt goes upstream.
- Integration: the real library against itself or a local peer, like `vasdk_webrtc_provider` (`VASDK_3P_IMPL/LibDataChannelWebRtc/tests/`); a full session over it, skipping when the network isn't available.

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) holds, plus the [vasdk-provider](../vasdk-provider/SKILL.md#done-when) checks for a 3P library.
