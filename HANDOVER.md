# Handover

The state of browser-mini / browser-big at 4.39.0, for whoever picks it up
next. The short version: video calls work on WebKitGTK 2.54.0 with
GStreamer 1.28.7, given three upstream patches and the defaults in
browser-big.

## Test setup

- ThinkPad T14 Gen1 (AMD). Self-built WebKitGTK 2.54.0, GStreamer 1.28.7,
  GTK 4.20.3. No sound server (`audio = alsa`).
- Site: a mediasoup-based chat (mediasoup-client, `Safari12` handler,
  VP8-only router, socket.io signalling).
- Reference: the WebRTC samples, `peerconnection/pc1` for a call without a
  server.

## Upstream bugs (need patching, not configuration)

### 1. WebKit: camera frame flood (WebKit PR 74373, open)

`GStreamerVideoCapturer::createConverter` sets `drop-only` on `videorate`
only for GStreamer < 1.28. The capture pipeline runs with base time 0, so
buffer PTS are absolute CLOCK_MONOTONIC values, and `videorate` fills the
gap from 0 with uptime x fps copies. The result is a frozen or black camera,
400+ encoded frames per second, and a pinned CPU. It also affects 2.52.

```
--- a/Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCapturer.cpp
+++ b/Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCapturer.cpp
@@ -135,6 +135,10 @@
 
     auto* bin = gst_bin_new(nullptr);
     auto* videorate = makeGStreamerElement("videorate"_s, "videorate"_s);
+    // The capture pipeline runs with base time 0, so buffer PTS are absolute
+    // CLOCK_MONOTONIC values. Without skip-to-first, videorate fills the gap
+    // from segment start with uptime x fps duplicate frames (WebKit PR 74373).
+    g_object_set(videorate, "skip-to-first", TRUE, nullptr);
 
     // The workaround below doesn't seem necessary anymore in GStreamer 1.28 and beyond.
     // Fixed by: https://gitlab.freedesktop.org/gstreamer/gstreamer/-/commit/6f623af4d745efaacd0c8639b99536def4a65c78
```

Check: `pc1` encodes about 90 frames per 3 s report (30 fps).

### 2. GStreamer 1.28: glupload NULL video meta (fixed on main)

`_dma_buf_upload_accept` in `gst-libs/gst/gl/gstglupload.c` does
`out_info->width = meta->width` without checking `meta`. It runs on a format
change with a dma-buf buffer that has no video meta, which is exactly the
site's second `getUserMedia`. The web process crashes (SIGSEGV on the
`vqueue:src` thread). 1.26 had the check.

```
--- a/gst-libs/gst/gl/gstglupload.c
+++ b/gst-libs/gst/gl/gstglupload.c
@@ -1695,8 +1695,10 @@
      * matches the size we use to import the dmabuf. @outcaps will remains
      * display resolution as expected.
      */
-    out_info->width = meta->width;
-    out_info->height = meta->height;
+    if (meta) {
+      out_info->width = meta->width;
+      out_info->height = meta->height;
+    }
 
     /*
      * When we zero-copy tiles, we need to propagate the strides, which contains
```

Without the patch: `WEBKIT_GST_DISABLE_GL_SINK=1`.

### 3. WebKit: camera sizes given as a list are skipped

`GStreamerVideoCaptureSource::generatePresets` reads each caps structure
with `gst_structure_get(..., "width", G_TYPE_INT, ..., "height",
G_TYPE_INT, ...)` and skips it when that fails. V4L2 lists some modes as a
size list in one structure. The T14 camera (`--list-cameras`):

```
640 x { (int)480, (int)360 }   30/1
320 x { (int)240, (int)180 }   30/1
```

These are all of its 4:3 modes, in every format (DMA_DRM, MJPEG, YUY2).
WebKit had no 4:3 preset, so `{max: 20}` gave 848x480@20 and no frame rate
gave 1280x720@10. `gst_caps_normalize` splits them into discrete
structures (checked with the local GStreamer: 640x480, 640x360, 320x240,
320x180).

```
--- a/Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCaptureSource.cpp
+++ b/Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCaptureSource.cpp
@@ -298,7 +298,13 @@
 void GStreamerVideoCaptureSource::generatePresets()
 {
     Vector<VideoPreset> presets;
+    // A V4L2 device may list several sizes in one structure, for instance
+    // width=640, height={ 480, 360 }. Split them into one structure each,
+    // or every size listed that way is skipped below as not discrete - on
+    // a typical laptop camera that is every 4:3 mode.
     auto caps = m_capturer->caps();
+    if (caps)
+        caps = adoptGRef(gst_caps_normalize(gst_caps_copy(caps.get())));
     for (unsigned i = 0; i < gst_caps_get_size(caps.get()); i++) {
         GstStructure* str = gst_caps_get_structure(caps.get(), i);
```

Check: `--rtc-trace` shows the video track at 320x240, aspect 1.333.

All three patches are needed again for each new WebKit or GStreamer until
upstream ships them.

## WebKit behaviour browser-big works around (all default)

Each item was found in the WebKit 2.52.6 / 2.54.0 sources and confirmed by
a run.

1. **librice finds no srflx candidate.** `RiceBackend::resolveAddress`
   keeps only the first DNS answer (upstream FIXME), and the STUN address is
   built as `host:port` without IPv6 brackets. Measured: librice host-only,
   libnice srflx in 0.11 s. Default is libnice
   (`WEBKIT_GST_DISABLE_WEBRTC_NETWORK_SANDBOX=1`); `ice = rice` undoes it.
2. **Encoder fixed to the first codec of the own offer.**
   `doSetLocalDescription` -> `linkOutgoingSources` ->
   `configurePacketizers` links the first codec it can encode. The answer
   is never consulted, and `codecPreferencesChanged` refuses once the bin
   runs. When a track is added to an existing connection (renegotiation),
   the source is configured at `addTransceiver` time, so only
   `setCodecPreferences` switches it.
   - Fix: `setCodecPreferences(VP8 first)` on transceivers with a video
     sender track, plus VP8 moved first in the SDP (`sdpPreferCodecs`).
   - The mediasoup probe pc (no tracks) is left alone; reordering it broke
     mediasoup's device caps.
3. **No `a=ssrc` lines, and the real SSRC is unknown until connected.**
   mediasoup-client reads the SSRC from `pc.localDescription` after SLD and
   registers the producer under it. A mismatch drops every packet.
   - Fix: inject a placeholder SSRC (`sdpAddSsrc`). After SLD, learn the
     real one from `sender.getStats()` in the background (outbound-rtp, up
     to 15 s).
   - Rewrite `localDescription` on the pc (`hookLD`), and hold the one
     WebSocket message that contains the placeholder until it can be
     rewritten (`ssrc-ws`).
4. **Payload types differ between the probe and the real pc.** The answer
   carries the probe's numbers (VP8 = 111); WebKit sends its own (96).
   Fix: in the same held message, `codecs[0].payloadType` and the rtx
   `apt` are set to what the stats say is sent (`pt-fixed`).
5. **`RTCRtpSendParameters.codecs` required.** Filled in from
   `getParameters()` when a library omits it.
6. **A MediaStream player never starts while an audio track delivers
   nothing.** Remote cameras without a microphone stay at readyState 0
   although frames are decoded.
   - Fix: after 2.5 s at readyState 0, with live video and silent audio, the
     element gets a video-only stream; the `srcObject` getter keeps
     returning the page's stream.
   - The original goes back on audio `unmute`. Confirmed: every such cam
     played right after `video-audio-split`.
7. **`query-permission-state`** (new in 2.54) is answered from
   `permissions.tsv`. Unanswered means "prompt", and sites get no device
   labels.
8. **Microphone choice.** Device labels are hidden until the first grant,
   so `mic_match` swaps the matching input into the stream after the first
   successful `getUserMedia` (`mic-swapped`).

9. **Camera frame rate.** `bestSupportedSizeFrameRateAndZoom` skips
   every preset whose frame-rate list lacks the exact requested rate. The
   site asks 320x240 with `frameRate {max: 20}`; 320x240 only runs at 30,
   so even with patch 3 a 16:9 mode (848x480@20) won.
   - Fix: `looseFps()` drops a soft frameRate in `getUserMedia` and
     `applyConstraints` (exact and min are kept, `gum-fps-loosened`).
     `cam_size_fix = no` undoes it.
   - Chrome scales per track and drops frames instead.
10. **`mic_match` wins** over the site's own audio deviceId (`exact`). The
    site's second request named the headset jack again.

`fix_webrtc = no` turns off 3-6. `--no-cam-fix` injects no script at all.

## Removed (proven not to work)

- `--cam-exact`: WebKit answered an exact 320x240 with 424x240 instead of
  failing.
- `--cam-force`: a preference that WebKit ignored.
- `--cam-scale`: the canvas track has no deviceId or label, and the site
  rejected it at once.
- `--max-bitrate`, `--max-fps`: `setParameters` was accepted, and the
  send rate did not change.
- `fixSize` (4.38, re-apply the size once the probe track ended): the log
  showed no other track open (`waited:0`) and still 1280x720. The real
  cause was patch 3.
- `--cam-share-clone`, `--cam-hold`: `MediaStreamTrack.clone()` goes black
  after about 1 s on 2.52. Sharing the track itself works.

## Open issues

- **No keyframe after loss.** A remote stream that loses a few packets
  stops decoding. FIRs are sent (`in-video.fir` climbs) but `keys` does not
  move. Either the sender or server throttles keyframes, or the depayloader
  waits for one it does not recognise. `rtpvp8depay2` (gst-plugins-rs) is
  installed alongside `rtpvp8depay`; ranking it out changed nothing.
- `GStreamer-RTP-CRITICAL gst_rtp_header_extension_get_id` on receiving
  streams: `gstrtpbasedepayload.c:646` walks a header extension list with a
  NULL entry on a caps event. Harmless so far.
- The three patches above are not upstream yet.

## Checks that pin a problem down

- `--rtc-trace`: `sdp` (full `pt name` lists), `rtc-stats` (`out-*`,
  `far-*`, `in-*` with fir/keys), `video-state`, `video-attach`,
  `ssrc-real` / `ssrc-ws` / `pt-fixed`, `codec-mismatch`.
- `pc1` at 30 fps: the camera path and the WebKit patch are fine.
- A LibreWolf run of the same room separates site bugs from ours.
- gdb attached to `WebKitWebProcess` before the camera starts (see README).

## Build notes

- On 2.54, `WebKitPointerLockPermissionRequest` is only declared when
  WebKit is built with pointer lock (`#if ENABLE(POINTER_LOCK)` in
  `webkit.h.in`), so both uses are behind
  `#ifdef WEBKIT_TYPE_POINTER_LOCK_PERMISSION_REQUEST`.
- `MAX`/`MIN`/`CLAMP` evaluate their argument twice. Option values are read
  into a local first (`argv[++i]` inside `MAX` swallowed the next option).
- Always ship all seven files together; `make version` identifies the
  build.
