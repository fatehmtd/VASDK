# Transports

*Reference for `va::transport::Transport` and its implementations. Header: [`Transport.h`](../../VASDK/include/VASDK/Transport.h) - the source of truth if this page and the code ever disagree.*

A transport answers *how frames get in and out of the process*: local hardware, HTTP, WebSocket, or nothing at all (tests). The pipeline itself is transport-agnostic - swap the transport and the same processor chain serves a desktop mic, a network stream, or a unit test.

```mermaid
flowchart TB
    T["Transport (abstract)"] --> BT["BaseTransport<br/><i>+ receive queue, stats, state machine</i>"]
    BT --> LT["LocalTransport<br/>hardware audio via<br/>GenericAudioSource/Sink"]
    BT --> HT["HttpTransport<br/>request/response over<br/>HttpInterface (libcurl)"]
    BT --> WT["WebSocketTransport<br/>server, via WebSocketInterface<br/>(cpp-httplib)"]
    BT --> RT["WebRtcTransport<br/>one remote peer, via WebRtcInterface<br/>(libdatachannel provider)"]
    BT --> MT["MockTransport<br/>no I/O - test double"]
    HT --> LLMT["LLMTransport<br/>(see llm.md)"]
```

## The Transport interface

| Group | Members | Notes |
| --- | --- | --- |
| Identity | `getName()`, `getDescription()` | Debugging/logs. |
| Lifecycle | `initialize()`, `start()`, `stop()`, `release()` | Same shape as processors and providers - the whole SDK uses this four-step lifecycle. |
| Connection | `isConnected()`, `getState()` | `TransportState`: `DISCONNECTED → CONNECTING → CONNECTED → DISCONNECTING`, plus `ERROR`. |
| Frame I/O | `receiveFrame(timeoutMs)`, `sendFrame(frame)`, `hasFrames()` | `receiveFrame`: blocking pull (`0` = no wait, `-1` = infinite). In pipelines you'll rarely call it - see the callback below. |
| Callbacks | `onConnected`, `onDisconnected`, `onError(code, msg)`, `onFrameReceived(frame)` | Public `std::function` members. `onFrameReceived` is the push-mode alternative to `receiveFrame()` - `TransportInputProcessor` wires it so frames enqueue straight into the pipeline with no polling thread. |
| Stats | `getStats()`, `resetStats()` | `TransportStats`: frames/bytes sent+received, errors, dropped frames. |

**`BaseTransport`** is the class to actually derive from: it implements the receive queue (bounded, thread-safe, condition-variable signaled), state tracking, stats counters, and the metrics hookup (`setMetrics`/`setMetricsConfig`). Your subclass calls the protected helpers - `enqueueReceivedFrame()`, `setState()`, `updateSentStats()`, `incrementErrorCount()` - and implements `sendFrame()` plus the lifecycle.

## LocalTransport

([`LocalTransport.h`](../../VASDK/include/VASDK/LocalTransport.h)) The hardware transport: a capture thread pulls from a [`GenericAudioSource`](providers.md#genericaudiosource), writes samples into the shared [`AudioRingBuffer`](../../README.md#the-audio-ring-buffer), and emits lightweight `AudioFrame`s (offsets, not samples). Outbound `SynthesisFrame`s are written to the [`GenericAudioSink`](providers.md#genericaudiosink).

Configuration before `initialize()`:

- `setInputAudioConfig` / `setOutputAudioConfig` - capture and playback formats (they can differ; e.g. 16 kHz mono in, TTS-native rate out).
- `setFrameSize(samples)` - capture granularity (default 2000 samples = 125 ms at 16 kHz). Smaller = lower latency, more per-frame overhead.
- `setRingBuffer(buffer)` - the shared ring buffer that VAD/STT will read; without it an internal buffer is used and nothing else can share the audio.

Barge-in support: `getPendingOutputAudioBytes()` reports unplayed audio and `flushOutputAudio()` fades (~50 ms ramp) and discards it - `TTSFrameProcessor::setPlaybackFlush` is typically wired to this. Capture-side metrics (callback jitter, underruns) are built in.

## HttpTransport

([`HttpTransport.h`](../../VASDK/include/VASDK/HttpTransport.h)) Request/response networking over a pluggable [`HttpInterface`](#the-network-provider-seam) (libcurl by default, `CurlHttpProvider`). Mostly used through its subclass [`LLMTransport`](llm.md#llmtransport), which adds conversation history, prompt templating, and streaming-response parsing on top.

## WebSocketTransport

([`WebSocketTransport.h`](../../VASDK/include/VASDK/WebSocketTransport.h)) A WebSocket **server** transport backed by a `WebSocketInterface` provider (cpp-httplib implementation ships in `network/providers`; built when `VASDK_ENABLE_WEBSOCKET_SERVER=ON`, the default). Lets external clients stream frames in and out - remote UIs, browser demos, integration harnesses. Frame serialization uses nlohmann/json when built with `VASDK_HAS_JSON`.

## WebRtcTransport

([`WebRtcTransport.h`](../../VASDK/include/VASDK/WebRtcTransport.h)) Real-time audio with one remote peer - a browser tab, or later a room on a WebRTC platform. It keeps `LocalTransport`'s contract, so an app swaps one for the other without touching the pipeline:

- Same setters before `initialize()`: `setInputAudioConfig`, `setOutputAudioConfig`, `setFrameSize`, `setRingBuffer`. Mono 16-bit PCM only.
- Inbound peer audio is written into the shared ring buffer and surfaces as offset-only `AudioFrame`s, exactly like captured mic audio.
- Outbound `SynthesisFrame`s are queued and sent in real-time paced 20 ms chunks (silence when idle, so the RTP clock never stalls). `getPendingOutputAudioBytes()` reports the queue; `flushOutputAudio()` fades and drops it for barge-in - wire it to `TTSFrameProcessor::setPlaybackFlush` as with `LocalTransport`.
- State: `CONNECTING` while waiting for a peer, `CONNECTED` while one is attached (`onConnected` fires), back to `CONNECTING` when it leaves (`onDisconnected` fires, queued playback is dropped). Synthesis sent with no peer attached is dropped.
- Pipeline state goes to the peer as one JSON object per message via `WebRtcInterface::sendEvent` (needs `VASDK_HAS_JSON`). Each has `type` and the frame `t`: `agent_state` (`state`, `from`, `reason`), `vad` (`state`, `confidence`), `turn` (`actor`, `state`, `kind`), `transcript` (`text`, `final`, `confidence`), `reply` (streamed LLM `text`, `final`), `tts` (`state`). Register the transport on `TransportOutputProcessor` with no frame filter so it sees these frames; other frame types are ignored.

The media stack sits behind [`WebRtcInterface`](../../VASDK/include/VASDK/network/WebRtcInterface.h), which carries only PCM plus connect/disconnect/error callbacks. Codecs, RTP and **signaling** are the provider's business, so the core library takes no WebRTC dependency and the same transport can front a direct PeerConnection or a platform SDK (LiveKit, Daily) that hands out decoded PCM.

**`LibDataChannelWebRtcProvider`** ([`VASDK_3P_IMPL/LibDataChannelWebRtc`](../../VASDK_3P_IMPL/LibDataChannelWebRtc/LibDataChannelWebRtcProvider.h), `-DVASDK_BUILD_LIBDATACHANNEL_WEBRTC=ON`) is the shipped provider: a direct PeerConnection via [libdatachannel](https://github.com/paullouisageneau/libdatachannel) with one sendrecv Opus track.

- Signaling is one non-trickle SDP exchange: `handleOffer(offerSdp)` returns the answer once ICE gathering completes. `startSignalingServer(port, host, pagePath)` exposes it as `POST /offer` (WHIP-shaped; `409` while a peer is connected) and serves a page at `GET /`. **No authentication** - it binds `127.0.0.1` by default; put it behind your own auth before exposing it.
- Events go out on the data channel the browser creates with its offer (the demo page calls it `events`). `sendEvent` returns false until that channel is open, so events from before then are lost.
- Opus runs directly at the transport's rates, which must be 8/12/16/24/48 kHz (`open()` fails otherwise - there is no resampler).
- Inbound packets pass a 3-packet reorder buffer; gaps get Opus packet-loss concealment. A peer that sends no media for 5 s is dropped (browsers stream continuously even when muted), which catches closed tabs long before the ~30 s ICE timeout.
- DTLS uses OpenSSL (Homebrew `openssl@3` is found automatically on macOS); pass `-DUSE_MBEDTLS=ON` or `-DUSE_GNUTLS=ON` to use those instead.

```cpp
auto peer = std::make_shared<va::webrtc::LibDataChannelWebRtcProvider>();  // STUN: stun.l.google.com
auto rtc  = std::make_shared<va::transport::WebRtcTransport>(peer);
rtc->setRingBuffer(ringBuffer);
rtc->initialize();
rtc->start();
peer->startSignalingServer(8765, "127.0.0.1", "assets/webrtc/index.html");
tts->setPlaybackFlush([rtc] { rtc->flushOutputAudio(); });
```

Both showcase apps take `--webrtc-port <port>` (and `--webrtc-page`) when built with the provider: run, open `http://127.0.0.1:<port>`, press *Start call*. The demo page is [`assets/webrtc/index.html`](../../assets/webrtc/index.html). Browsers only grant the microphone on `localhost`/`127.0.0.1` or HTTPS, so remote use needs TLS in front. In the call-center app one process is one call: a second caller joining the same process continues the first caller's goal state.

## MockTransport

([`MockTransport.h`](../../VASDK/include/VASDK/MockTransport.h)) The test double - no I/O at all:

| Member | Purpose |
| --- | --- |
| `injectFrame(frame)` | Simulate the receive side: the frame enters the pipeline as if it arrived from outside. |
| `getSentFrames()` / `getSentFramesByType(type)` / `getSentFrameCount()` | Everything the pipeline tried to send, captured for assertions. |
| `clearSentFrames()` | Reset between test cases. |
| `setAutoRespond(...)`, `setSimulatedLatency(ms)`, `simulateError(...)`, `simulateDisconnect()` | Scripted behaviors for failure-path tests. |

Full walkthrough: [testing-with-mock-transport.md](../examples/testing-with-mock-transport.md).

## TransportFactory

([`TransportFactory.h`](../../VASDK/include/VASDK/TransportFactory.h)) A by-name registry (`registerTransport(name, creator)` / `createTransport(name)`) with a `TransportRegistrar<T>` helper for static-initialization registration:

```cpp
static va::transport::TransportRegistrar<MyCustomTransport> reg("mycustom");
```

Useful when the transport choice comes from configuration rather than code. The showcase apps construct transports directly and don't use the factory - it's there for config-driven deployments.

## The network provider seam

Transports don't talk to sockets directly; they go through interfaces in [`network/`](../../VASDK/include/VASDK/network/), which is what makes the networking stack replaceable per platform:

- **`HttpInterface`** - `post()` (with optional streaming `onChunk` callback - this is how LLM token streaming arrives), `get()`, and `sendText`/`sendBytes` convenience wrappers; `HttpResponse{statusCode, body, headers, error}`. Implementation: `CurlHttpProvider` (built with `VASDK_ENABLE_HTTP_TRANSPORT=ON`, the default).
- **`WebSocketInterface`** - server-side: `listen(port, host)`, `sendText`/`sendBytes` per connection, `broadcastText`/`broadcastBytes`, connection callbacks. Implementation: `CppHttpLibWebSocketProvider`.

To port VASDK to a platform with its own networking stack (automotive middleware, a sandboxed OS), implement these two interfaces - no transport or pipeline code changes.

## Writing a custom transport

1. Derive from **`BaseTransport`** (not `Transport`) to inherit the queue/stats/state machinery.
2. Implement `initialize/start/stop/release`, `getName/getDescription`, and `sendFrame()` - where outbound frames leave the process.
3. On inbound data, build the right frame type and call `enqueueReceivedFrame(frame)`; it lands in the receive queue and fires `onFrameReceived`.
4. Keep state honest with `setState()` and count failures with `incrementErrorCount()` / `incrementDroppedFrameCount()` - the built-in metrics and any monitoring dashboards read these.
5. Register with `TransportFactory` only if you need name-based creation.
