# browser-mini / browser-big

Two WebKitGTK 6.0 (GTK4) browsers sharing one core.

**100% Vibecode but tested.** Builds warning-free with `-Wall -Wextra`.
Used for real video calls on WebKitGTK 2.54.0 with GStreamer 1.28.7.

- **browser-mini**: a plain, fast page viewer.
- **browser-big**: the same, plus camera, microphone and WebRTC repairs and
  diagnostics.

## Files

| File | What it is |
| --- | --- |
| `browser_core.c` / `.h` | Window, keys, history, downloads, profiles, config |
| `browser-mini.c` | Fills in the hooks and calls `browser_main()` |
| `browser-big.c` | Camera, GStreamer and WebRTC repairs, diagnostics |
| `patches/` | Three upstream fixes for WebKitGTK and GStreamer (see *Patches*) |
| `HANDOVER.md` | Background: each upstream bug and workaround, with evidence |

## Build

Debian or any Linux distro:

```
apt install build-essential pkg-config libgtk-4-dev libwebkitgtk-6.0-dev \
            libsoup-3.0-dev libgstreamer1.0-dev
make                # build both
make install        # into /usr/bin (PREFIX=... to change)
make uninstall
make clean
make version        # 4.39.0 (build …), md5 of the sources
```

## Usage

```
browser-mini [URL] [options]
browser-big  [URL|PATH|diag] [options]
```

No address opens the start page. No scheme means https; a path opens as a
file. `-h` lists every option and key.

## Keys

| Key | Action |
| --- | --- |
| `Ctrl+O`, `Ctrl+L` | Address (history below it) |
| `Ctrl+H` | History |
| `Ctrl+J` / `Ctrl+Shift+J` | Back / forward |
| `Ctrl+F` | Find |
| `Ctrl+S` | Download directory for page, site or everything |
| `Ctrl+K` | Search keywords |
| `Ctrl+D` | Recent downloads |
| `Ctrl+P` / `Ctrl+Y` | Show / copy the URL |
| `Ctrl+G` / `Ctrl+Shift+G` | Top / bottom |
| `Ctrl+R`, `F5` | Reload |
| `Ctrl+plus/minus/0` | Zoom |
| `F1` | Key list |
| `F2` | Media mode (`Ctrl+click` sends videos to `player`, images to `image_viewer`) |
| `F6` | Redraw without reloading |
| `F12` | Developer tools |

`--mod alt|super|meta` moves the modifier off Ctrl. browser-big adds
`Ctrl+Shift+M` (diagnostics page), `Ctrl+Shift+T` (web process threads),
`Ctrl+Shift+V` (video element state), `Ctrl+Shift+C` / `X` (warm / release
the camera).

## Features

- **History** survives restarts; back and forward continue into it.
  `--private` keeps nothing.
- **Downloads**: `Ctrl+S` sets a directory per page, site or everything;
  the narrowest rule wins.
- **Search keywords**: `g` (default), `d`, `w`, `s`. `w tree` searches
  Wikipedia. Add one with `Ctrl+K`: `k https://example.org/?q={}`.
- **Lock**: `--lock-page` or `--lock-site` refuses other addresses.
- **Popups**: `--allow-popups` lets pages open windows without a click
  (Zoom needs it).
- **Errors** show top right in red for 10 s, and on stderr.

## Paths

`browser-mini --paths` prints them all.

| Path | What |
| --- | --- |
| `~/.config/swov/config` | Shared palette, read first |
| `~/.config/browser-{mini,big}/config` | Settings |
| `~/.local/share/wkview/<profile>/` | Cookies, history, search keywords, permissions |
| `~/.local/share/wkview/download-dirs.tsv` | Download rules (all profiles) |
| `~/.cache/wkview/<profile>/` | Cache, safe to delete |

`--clear-data` wipes the profile. `--forget-permissions` drops camera and
microphone answers.

## Config

`key = value`, `#` comments, colours as `RRGGBB[AA]`. The command line wins.
Every key is also an option: `--hl=ff8800`. `-c PATH` reads one file only,
`-n` reads none.

## Patches

Video calls need three upstream fixes. They are real bugs in current
releases, not configuration, and none is upstream yet.

| File | Applies to | Without it |
| --- | --- | --- |
| `webkit-videorate-skip-to-first.patch` | WebKitGTK 2.52 / 2.54 | Own camera freezes or goes black, hundreds of frames per second, CPU pinned (WebKit PR 74373) |
| `webkit-caps-normalize.patch` | WebKitGTK 2.54 | Camera modes listed as `640 x {480, 360}` are ignored. Many laptop cameras get no 4:3 mode, so 16:9 with black bars |
| `gst-glupload-null-meta.patch` | GStreamer 1.28 (gst-plugins-base) | Web process crashes on a site's second camera request |

Apply in the source tree, then rebuild and reinstall that package:

```
cd webkitgtk-2.54.0
patch -p1 < /path/to/patches/webkit-videorate-skip-to-first.patch
patch -p1 < /path/to/patches/webkit-caps-normalize.patch

cd gstreamer-1.28.x/subprojects/gst-plugins-base   # or the plain gst-plugins-base tarball
patch -p1 < /path/to/patches/gst-glupload-null-meta.patch
```

Add `--dry-run` first to check. Without the GStreamer patch,
`WEBKIT_GST_DISABLE_GL_SINK=1` avoids the crash at some CPU cost.

Check that they work:

- Videorate: the WebRTC sample `pc1` (below) encodes about 90 frames per
  3 s report, not hundreds.
- Caps: `--rtc-trace` shows a 4:3 camera track (`aspectRatio 1.333`) on a
  site that asks for one.

## Video calls

browser-big repairs these WebKit problems by default:

| Repair | Why |
| --- | --- |
| libnice ICE | librice finds no public address, so nothing connects past the LAN. `ice = rice` undoes it |
| VP8 first | WebKit encodes the first codec of its own offer and ignores the answer. `video_codecs = native` undoes it |
| Real SSRC and payload type | WebKit writes no `a=ssrc` lines. mediasoup sites get the values WebKit really sends |
| `RTCRtpSendParameters.codecs` | WebKit requires it; some libraries omit it |
| Camera without sound | A remote stream with silent audio is shown video-only, since WebKit's player waits for audio |
| Permission state | Answered from remembered permissions, so sites see device labels |
| Camera frame rate | WebKit only takes modes with the exact requested frame rate. A soft limit is dropped. `cam_size_fix = no` undoes it |

`--no-fix-webrtc` turns off the SSRC, parameters and silent-audio repairs.
`--no-cam-fix` injects nothing.

**Permissions.** A request asks at the top of the window: `Enter` allows,
`Esc` denies, remembered per site. `media = ask|allow|deny` in the config.

**Per machine:**

```
# ~/.config/browser-big/config
audio = alsa                     # no sound server
mic_match = ThinkPad             # mic whose label contains this text
feature = OffscreenCanvas=off    # Zoom
web_compat = yes                 # Zoom: screen.orientation
```

`mic_match` / `cam_match` pick a device by label. `mic_match` overrides the
microphone a site asks for. `arecord -l` lists microphones.

## When a call does not work

```
browser-big URL --rtc-trace 2>&1 | tee /tmp/bb.log
```

| Line | Means |
| --- | --- |
| `sdp` | Each description, codecs as `payload-type name` |
| `ssrc-real`, `ssrc-ws`, `pt-fixed` | The server got the real SSRC and payload type |
| `rtc-stats` | Every 3 s: `out-*` sent, `far-*` server report, `in-*` received |
| `video-state` | Each `<video>`: size, frames shown |
| `video-audio-split` | A silent-audio stream shown video-only |
| `codec-mismatch` | We encode something the far end did not agree to |

- `out-video.enc` climbing, no `far-video`: the server drops our stream.
- `in-video.dec` climbing, `video-state.frames` at 0: decoded, never shown.
- `in-video.fir` climbing, `keys` still: no keyframe arrives.

**Without a server:**
`browser-big https://webrtc.github.io/samples/src/content/peerconnection/pc1/ --rtc-trace`.
If that works, WebKit's WebRTC is fine and the problem is site-specific.

**Camera:** `browser-big --list-cameras` shows every mode each camera
offers. Check `ls -l /dev/video*` and membership in group `video`.

**Crash:** attach gdb before starting the camera:

```
gdb -p $(pgrep -nx WebKitWebProcess) -batch \
    -ex 'handle SIGUSR1 SIGUSR2 SIGPIPE SIG34 SIG35 nostop noprint pass' \
    -ex continue -ex 'thread apply all bt 30' > /tmp/bt.txt 2>&1
```

## browser-big options

| Option | |
| --- | --- |
| `--rtc-trace` | Every WebRTC step, stats, video elements |
| `--media-trace` | Every `getUserMedia` call and result |
| `--media-debug` | Verbose media, permission and capture logs |
| `--list-cameras` | Camera modes, then exit |
| `--gst-debug SPEC` | GStreamer logging, e.g. `webrtc*:5` |
| `--gst-rank SPEC` | Append to `GST_PLUGIN_FEATURE_RANK` |
| `--cam-share` | A repeated request gets the camera already open |
| `--no-mic` | Camera only |
| `--audio-alsa` | ALSA instead of PulseAudio |
| `--no-hw-decode`, `--no-va` | Software decoding / no VA-API |

## When a page renders badly

Try one at a time; the first that helps names the layer:

```
browser-mini URL --no-jit          # JavaScript JIT
browser-mini URL --no-dmabuf       # WebKit's dmabuf renderer
browser-mini URL --no-compositing
browser-mini URL --no-gpu
browser-mini URL --gsk cairo       # GTK renderer (default gl)
```

`--feature NAME[=on|off]` sets a WebKit runtime feature, `--list-features`
lists them. `--gl-info` checks the graphics stack.
