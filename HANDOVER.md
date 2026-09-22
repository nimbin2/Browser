# browser-mini / browser-big: handover notes

State at **4.18.0 (build 4bbf154)**. Written for a fresh chat whose job is
to clean the code up. Read this first, then the README.

## 1. The project

| File | Lines | What |
| --- | --- | --- |
| `browser_core.c` | ~7800 | window, popups, keys, history, downloads, search, media mode, config, watchdogs |
| `browser_core.h` | ~180 | `Win` struct, `BrowserApp` hooks, `LOG` macro |
| `browser-big.c` | ~2660 | camera / GStreamer / WebRTC layer on top of the core |
| `browser-mini.c` | 22 | fills in `BrowserApp`, calls `browser_main()` |
| `Makefile` | | `PREFIX ?= /usr`, `install`, `uninstall`, `clean`, `version` |

- WebKitGTK 6.0 (GTK4) only. `make` builds both binaries.
- Build id = md5 of the sources (`make version`).
- The user's preferences apply: md5 build versioning reported at the end of
  each reply, Makefile defaults to `/usr/bin` with `clean` and `uninstall`,
  a short README branded "100% Vibecode but tested", no Ubuntu in the README,
  no extra files unless asked, minimal testing.

## 2. The user's system (verified in the logs)

- Linux From Scratch, pkgusr package users, **no systemd**, native Wayland,
  plain ALSA (no PulseAudio/PipeWire running), no desktop portal,
  `XDG_RUNTIME_DIR=/tmp/xdg-n76310` (mode 042770).
- Lenovo 20UD (AMD Renoir): Mesa radeonsi, RADV Vulkan, VA-API H.264/HEVC
  work; firmware complete.
- **WebKitGTK 2.52.5** (upgraded from 2.50.5 during this work),
  GTK 4.20.3, GLib 2.88.3, GStreamer 1.28.6. 2.52 defaults to librice
  instead of libnice for ICE.
- WebKit build flags: `USE_GSTREAMER_WEBRTC=ON` (the **only** WebRTC backend
  the GTK port has; OFF means no WebRTC at all), `USE_LIBRICE=ON`,
  `ENABLE_WEB_RTC=ON`, `ENABLE_MEDIA_STREAM=ON`, no bubblewrap sandbox.
  The pkgusr script had `USE_GTK4=OFF`, which builds the wrong library for
  these browsers; it must be `ON`.
- Camera `/dev/video1`: MJPEG up to 1280x720@30, raw YUY2/DMA_DRM 1280x720
  only @10. Plain `gst-launch v4l2src` starts instantly (60 frames in 2.3 s).
- gdb is **not** installed, and `wchan` reads 0 for every thread on this
  kernel. Neither can be used for thread dumps.
- The user's wrapper `su - user -c 'cmd "$@"' -- "$@"` ate the first
  argument (it becomes `$0`). Fixed by `-- browser-mini "$@"`.

## 3. Done, and working

**Core**
- `-h` answered after the config is read, so it prints effective values.
- Ctrl+K rows no longer load search templates as URLs.
- Popups unified: things you type into are top centre, messages top right,
  the address bottom left. Non-interactive panels fade on hover.
- Quiet config: unknown keys in swov's shared file are counted, not listed.
- Camera/mic permission prompt (Enter = allow, Esc = deny), remembered per
  site in `permissions.tsv`; `media = ask|allow|deny`; `--forget-perms`.
- Find-in-page no longer freezes big pages: 1000-match cap, 120 ms
  debounce, one `search()` pass.
- Fatal messages survive `abort()`: a signal handler drains the log pipe;
  backtraces via `-rdynamic`.
- Watchdogs: web process spinning (every 5 s; from 4.15 with timestamps,
  a "calm again after Ns" line, and a GStreamer-vs-JIT hint), UI main loop
  blocked, and a missing Wayland frame callback.
- `--gsk` defaults to `gl` (GTK's Vulkan renderer deadlocked on resize).
- `--paths` (4.12+) lists every file and directory written, including the
  download rules, search keywords (with built-ins), start page colour and
  media temp dir.
- Start page: flat colour, with a toggleable swatch palette at the top;
  the choice is saved in `~/.local/share/wkview/start-bg`.
- Ctrl+P shows the whole URL, wrapped, and toggles (4.13).
- Media mode (4.16/4.17): F2 toggles it and a frame shows it. Ctrl+click
  sends a video to `player` (mpv) and an image to `image_viewer` (imv);
  images are downloaded with libsoup first. `--player` on the command line
  starts in the mode.
- Built-in search keywords (4.18): g Google (default), d DuckDuckGo,
  w Wikipedia, s SDL3 wiki via DuckDuckGo. The user's own keywords win by
  name.

**browser-big**
- Media workarounds are opt-in only (the user's rule: clean by default).
- 4.14 fix: the tracing and compat flags (`--rtc-trace`, `--web-compat`,
  `--no-mic`, offer fixes) used to silently turn on camera *cloning and
  holding*. On 2.52 that froze the page's camera after ~1 s. Now only
  `--cam-fix`, `--cam-share`, `--fix-media` and `cam_share=yes` touch the
  camera; the log's `shim` line says `repair:true|false`.
- `--list-cameras`, `--media-debug`, `--gl-info`, `--list-features`,
  `--feature NAME[=on|off]` all verified useful.

## 4. Root causes found

| Symptom | Cause | Status |
| --- | --- | --- |
| WebRTC never connects, `createAnswer` never settles | WebKitGTK **2.50.5** bug: `create-answer` never emitted to webrtcbin (log proves `_create_answer_task` absent, zero warnings) | **fixed by 2.52.5**, pc1 sample works |
| pc1 camera freezes after 1 s on 2.52 | **our** shim cloned the track (see 4.14) | fixed |
| Zoom crash: `Trying to dispose element … video-frame-converter-gl … PAUSED`, segfaults in libc/libgstbase | WebKit 2.52 use-after-free in its GL video frame converter | avoided with `--feature WebCodecsVideo=off` |
| Zoom still crashes with WebCodecs off: camera freezes, Zoom releases it, the process dies on teardown | WebKit capture teardown bug (heap corruption, `free(): invalid pointer`) | **open** |
| 20–30 s frozen preview at 400 % CPU (`queue*:src`, `multiqueue*:src`) | `v4l2src` blocked downstream for ~28 s, then "Timestamp does not correlate with any clock". Seen in `-n` runs, where playback went to a **PulseAudio** sink with clock-skew warnings (libpulse autospawn is suspected) | **open**: the run with `audio = alsa` was never reported |
| `--cam-exact 640x480@30` ignored, 1280x720 returned with no error | WebKit 2.52 ignores exact capture constraints | WebKit bug; nothing to do |
| `screen.orientation` missing | WebKitGTK does not implement it; Zoom throws | `--web-compat` supplies it |
| GoDaddy login refused (`fp` → 429) | bot detection; changing the UA did not help | not ours; use another browser |
| TTY switch aborts every browser | GTK Wayland dispatch → WebKit abort | not ours |

## 5. Tried, and wrong or useless (do not repeat)

- A "stale binary" theory for the argument bug: it was the `su` wrapper.
- `--audio-alsa` aimed at the device provider: `GST_PLUGIN_FEATURE_RANK`
  does not rank device providers. Retargeted at the elements.
- Requiring a `nicesrc` element: wrong. The real check builds a webrtcbin
  and queries its `ice-agent`.
- "gst-debug compiled out", claimed twice: wrong both times. Use the
  `GST_DISABLE_GST_DEBUG` macro, not grep or `gst_debug_is_active()`.
- `dbus-launch`: caused a 61 s hang (a bus with no portal behind it).
- The GPU driver theory: an all-software run froze the same way.
- `--fix-webrtc` does not enable tracing (misread once as a regression).
- "Switch to the libwebrtc backend": that option does not exist for GTK.
- `WebCodecsVideoEnabled`: wrong feature name; it is `WebCodecsVideo`.
- `--cam-force` (ideal) and `--cam-exact`: ignored by WebKit 2.52.
- `--no-hw-decode` and `--no-gpu`: no help with the freeze or the crash.
- `wchan` thread dumps: always 0 on this kernel.
- GStreamer's log has colour codes when redirected; strip them with
  `sed -E 's/\x1b\[[0-9;]*m//g'` before grepping.

## 6. Cleanup candidates for the next chat

**Likely obsolete on 2.52**, so check each and remove what is dead:
- `--sdp-ssrc-fix`, `--rtc-params-fix`, `--fix-webrtc`: written against
  2.50 behaviour, before the real bug (the `createAnswer` hang) was known.
- The camera repair shim: `--cam-fix`, `--cam-share`, relax/retry/keepalive/
  hold/drop-audio, `--cam-scale*`, `--cam-force`, `--cam-exact`,
  `--cam-match`, `--cam-retries`, `--cam-hold`. Cloning is actively harmful
  on 2.52, and exact constraints are ignored.
- `--prewarm`, `--warm-cam`, `--media-watchdog`, `--auto-reload`,
  `--load-timeout`, `--max-reloads`, `--stall-timeout`: never shown to help.
- `--no-mic`: its diagnostic question has been answered.

**Keep:** `--rtc-trace`, `--media-debug`, `--list-cameras`, `--gst-*`,
`--web-compat` (screen.orientation), `--feature`, `--list-features`,
`--gl-info`, the watchdogs, `--paths`, the permission prompt, media mode,
search keywords, `--audio-alsa` / `audio = alsa`.

**Code smells worth a pass:**
- `browser_core.c` is ~7800 lines in one file. Natural splits: config,
  history, downloads, search, popup/omni, media mode, watchdogs, start page.
- `LOG` lines are inconsistent: only browser-big's `mlog` and the watchdog
  carry timestamps.
- The shim JS is one large C string in `browser-big.c`; it could be a
  separate `.js` embedded at build time.
- The README is ~1200 lines, with long debugging narratives from the 2.50
  era; trim it to what is still true on 2.52.

## 7. Open questions to settle first

1. pc1 **with** the config (`audio = alsa`, no `-n`): is the 20–30 s freeze
   gone? If yes, the PulseAudio sink clock was the cause, and browser-big
   should force ALSA or warn when `autoaudiosink` picks pulse.
2. `command -v pulseaudio`: is libpulse autospawning a daemon?
3. Zoom with `--web-compat --feature WebCodecsVideo=off`, then
   `dmesg | grep -iE 'segfault|WebKitWeb'`: which library dies now?
4. arte.tv videos fail in both browsers. The decoder check and the three
   test pages were proposed but never run (see the last messages: check
   for `avdec_aac`/`faad`/`fdkaacdec`, `avdec_h264`/`vah264dec`,
   `qtdemux`, `h264parse`, `aacparse`).
5. A gdb build would give real backtraces for the WebKit crash reports.

## 8. Useful test pages

- WebRTC loopback: `https://webrtc.github.io/samples/src/content/peerconnection/pc1/`
- No camera needed: `…/datachannel/basic/` and `…/capture/canvas-pc/`
- Camera only: `…/getusermedia/gum/`
- Zoom test meeting: `zoom.us/test`
- Plain MP4: `https://www.w3schools.com/html/mov_bbb.mp4`
- MSE/HLS: `https://hlsjs.video-dev.org/demo/`
