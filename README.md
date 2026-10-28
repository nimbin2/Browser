# browser-mini / browser-big

Two WebKitGTK 6.0 (GTK4) page viewers sharing one core.

**100% Vibecode but tested** — built warning-free with `-Wall -Wextra` against
GTK 4.14 / WebKitGTK 2.52.3, and run headless under Xvfb.

## Layout

| File | What it is |
| --- | --- |
| `browser_core.h` | Window state, the `BrowserApp` hook struct, shared helpers |
| `browser_core.c` | Window, popup, toast, zoom, downloads, profiles, keys, history |
| `browser-mini.c` | 20 lines. Fills in four fields and calls `browser_main()` |
| `browser-big.c` | Camera / GStreamer / watchdogs / diagnostics on top of the core |

browser-mini adds nothing to the core. Every hook is optional.

## WebKit runtime features

WebKit carries a few hundred runtime switches, and some a real site needs
are off by default. The one that matters most here is
**`AllowWebGLInWorkers`**: without it a library that renders in a worker
gets `Cannot create a canvas in this context`, falls back to the CPU, and
the result looks like a freeze rather than an error. Zoom's video pipeline
does exactly this.

```
browser-big zoom.us/test --feature AllowWebGLInWorkers --web-compat
browser-big --list-features webgl        # what exists and its default
```

`--feature` is repeatable and takes `NAME=off` too. Measured effect:
`offscreenWebgl` in the `rtc-caps` line goes from `false` to `true`.

Enabling it can expose a second problem underneath:

```
Failed to create a graphics context for WebGL using GBM, falling back to textures
```

That is WebKit failing to make a GBM-backed GL context in the web process.
`--feature UseGPUProcessForWebGL` moves WebGL to the GPU process instead,
which is the first thing to try; then `--no-dmabuf`, then `--no-gpu` to
take hardware paths out entirely.

If none of those help, there is a better fallback than the CPU path. A
library decides whether to attempt worker-side rendering by checking for
`OffscreenCanvas`; if that exists but WebGL inside it does not, it starts
the work anyway and falls back to the CPU, which is the freeze. Removing
the API removes the attempt:

```
browser-big zoom.us/test --feature OffscreenCanvas=off
```

Zoom's virtual-background worker (`vb_worker.min.js`) is exactly this
case. With OffscreenCanvas absent it does not try, the feature is
unavailable, and the meeting runs.

`--gst-debug` can only report anything if GStreamer was built with its
debug system, and `--media-debug` distinguishes the two things that are
easy to confuse:

```
gst-debug: supported; currently off, set GST_DEBUG to use it (threshold 0)
gst-debug: supported; currently ON (threshold 3)
gst-debug: COMPILED OUT of this GStreamer
```

`gst_debug_is_active()` is false whenever no level is set, even in a build
that fully supports debugging — reading it as "compiled out" is wrong. The
compile-time answer is the `GST_DISABLE_GST_DEBUG` macro, and in
`gstconfig.h` that name also appears inside an `#if 0` documentation
block, so grepping for it without context misleads too.

`LIBGL_ALWAYS_SOFTWARE=1` is only a useful test if Mesa was built with a
software renderer. Without one it fails as

```
libEGL warning: egl: failed to create dri2 screen
Could not create surfaceless EGL display: EGL_NOT_INITIALIZED. Aborting...
web process crashed
```

which says the fallback is missing, not that software rendering is slow.

## SharedArrayBuffer

Off by default in JavaScriptCore, and a site whose media engine uses
threaded WebAssembly needs it — without it the page hangs at a spinner
rather than reporting anything.

```
browser-big zoom.us/test --shared-array-buffer --web-compat
```

In the config, so it cannot be forgotten:

```
shared_array_buffer = yes
web_compat = yes
```

`--jsc NAME=VALUE` sets any other JavaScriptCore option the same way
(`--jsc useJIT=0`, and so on). Measured: `sharedArrayBuffer` in the
`rtc-caps` line goes from `false` to `true`. The page must also be
cross-origin isolated, which `rtc-caps` reports separately.

## Isolating a freeze that happens after a call connects

Once ICE has completed and the devices are live, two pipelines start that
were idle before: the video encoder and the audio path. Each can be taken
out on its own, so one run says which side it is:

```
browser-big URL --no-mic                       # camera only, no audio path at all
browser-big URL --video-codecs VP8             # software VP8 instead of VA-API H.264
browser-big URL --no-hw-decode                 # software decoding
browser-big URL --gst-rank vah264enc:NONE,vah264lpenc:NONE   # no VA-API encoding, keep H.264
```

And the decisive test for a GPU driver problem is to take the GPU out of
the media path entirely:

```
browser-big URL --no-dmabuf --no-hw-decode --no-va --video-codecs VP8
```

Slow, but if it is *stable* where the hardware path freezes, the driver is
the fault. `dmesg | grep -iE 'amdgpu|gpu reset|ring .* timeout'` during a
freeze is the other half of that proof: a hung GPU says so there.

The audio track here reports `sampleRate: 0`, which is not a valid rate;
a library that sizes its buffers from it can spin. `--no-mic` is the test
for that.

## APIs WebKitGTK does not implement

`--web-compat` supplies them. Currently `screen.orientation`: Zoom's media
code reads `screen.orientation.type` and throws before it starts, even
though its WebRTC negotiation completes fine.

## A site that will not accept the stream

WebKitGTK's WebRTC is GStreamer-based and its offer differs from the one a
site tested against Chrome. Two of those differences stop a publish before
it starts, and both are repaired in the offer rather than in the site:

| Flag | |
| --- | --- |
| `--fix-webrtc` | `--sdp-ssrc-fix` and `--rtc-params-fix` together |
| `--rtc-params-fix` | supplies `RTCRtpSendParameters.codecs` when a library omits it. WebKit requires it, Firefox does not, so a library on its Firefox path throws `Member RTCRtpSendParameters.codecs is required` |
| `--rtc-trace` | also reports an `rtc-caps` line up front: user agent, every WebRTC API, sendable codecs, plus WebGL, OffscreenCanvas, WebAssembly, SharedArrayBuffer, WebCodecs and `requestVideoFrameCallback`. A conferencing client processes frames in WebAssembly and renders with WebGL, so a missing one shows up as a freeze rather than an error. Emitted even when WebRTC is absent |
| `--rtc-trace` | reports every WebRTC step — connection made, tracks attached, offer created, answer set. The last event logged is the step that failed. Included in `--media-trace` |
| `--sdp-ssrc-fix` | adds the `a=ssrc:<n> cname:<v>` lines WebKit leaves out. A site parsing the offer for the CNAME throws `CNAME value not found` and never connects |
| `--video-codecs VP8` | sets the sending codec order. WebKit may offer something this machine has no encoder for — `--media-debug` lists the encoders you have |

The user agent comes first: a publisher library picks its code path from
it, and WebKitGTK's own string may get no path at all — in which case no
connection is ever built and nothing else matters.

```
browser-big room7.com --css no.css --ua-firefox --fix-webrtc --video-codecs VP8
```

`--fix-webrtc` does not turn tracing on, so add `--rtc-trace` when you want
to watch it work. With `--sdp-ssrc-fix` the log also reports
`sdp-ssrc-verify`, which says whether the injected lines were still present
in `pc.localDescription` afterwards — WebKit reparses the description, and
a library reads the lines back from there, so injecting them is not the
same as them surviving.

Check `--rtc-trace` first: the SDP rewrite only helps if an offer is ever
made, and a site that fails earlier will show no `createOffer` at all. No
`rtc-new` means the site never built a connection — it rejected the browser
before WebRTC, and the `rtc-caps` event reports what it would have looked
at: the user agent, and whether each API a publisher library needs exists.
In that case the lever is `--ua-chrome` or `--ua-firefox`, not the offer.

Both are off unless asked for. The SDP rewrite only touches sections that
send and that have no ssrc lines already, and is idempotent.

## A page that opens the same camera twice

Some pages request a camera, enumerate devices with the labels that grants
them, drop the stream and request the same device again — twice. A camera
cannot be opened twice: the second open returns a track that reports
`readyState: live` and never delivers a frame, and the `<video>` sits at
`readyState 0` reporting `waiting` then `stalled` forever.

This is a gap in WebKitGTK rather than a broken site: Chrome and Firefox
multiplex one device open into several tracks internally, so a page asking
twice just works there. Sharing does the same thing — it clones the live
track, so each caller gets an independent track over a single open, and
stopping one does not disturb the other.

The hold expires on its own. Keeping the capture open is the point, but
keeping it open after the page has stopped every clone leaves the camera
light on with nothing watching it — so once no handed-out track is live,
the source is released after `--cam-hold` seconds (3 by default, 0 to
release at once).

`--cam-share` hands the repeated request the stream that is already open
and changes nothing else, or permanently:

```
cam_share = yes
``` `--cam-fix` includes it, along with constraint
relaxing that the page did not ask for — `--cam-share` when the sharing is
all that is wanted.

## Slow resize, or slow after regaining focus

Measured, so it can be ruled out: the overlay positioning this browser does
on every allocation costs **11 microseconds per call, about 23 per
allocation** — 0.1% of a frame at 60Hz. It is not the cause of a stuttering
resize.

What does dominate a resize is GTK's own renderer, and the default is not
always the best one for a given driver:

```
browser-mini URL --gsk cairo      # software, no GPU involved
browser-mini URL --gsk gl         # the older GL renderer
browser-mini URL --gsk ngl        # the newer one, usually the default
browser-mini URL --gsk vulkan
```

In the config, so it holds:

```
gsk = gl
```

GTK 4.20 defaults to its Vulkan renderer where the driver claims support,
and a Vulkan stack that deadlocks while recreating its swapchain freezes
the whole browser the moment the window changes size — with the web
process idle and nothing in any log. The GL renderer has been GTK's
workhorse for years and loses nothing here: WebKit composites the page
itself, and GTK only draws the final surface and the overlays on top.

Then WebKit's own compositing, which is separate: `--no-dmabuf`,
`--no-compositing`, `--no-gpu`. WebKit also throttles a page that loses
focus and has to catch up when it returns, which no flag here changes.

## A page that logs heavily

WebKit writes the page's console output synchronously from the web process,
and writing to a terminal costs about six times what writing to a file does
(measured: 4000 lines, 4.3 ms to a file, 26.7 ms to a pty). A chatty
application therefore makes its own web process wait.

`--no-console` drops the page's logging and keeps ours. `-q` silences both.

## A page that freezes with the CPU pinned

Both browsers watch their web process. Every five seconds they sample its
CPU time and memory, and when it has been spinning at 90% or more for
fifteen seconds they print the threads that are running or blocked, with
the kernel function each is waiting in:

```
watch: [16:31:43.102] web process 19976 is spinning: 401% cpu, 0.2 GB resident, for 15s.
watch:   19976  R  WebKitWebProces    (running)
watch:   20031  R  queue1:src         (running)
watch:   20040  R  multiqueue0:src    (running)
watch: the busy threads are GStreamer's (media), not
watch: JavaScript - --no-jit will not help with this one
watch: [16:31:58.117] web process 19976 calm again after about 30s (4% cpu now)
```

Nothing is asked of the web process, so this works while it is frozen.
The `calm again` line closes the report, so the log shows how long the
page was frozen. Threads named `something:src` are GStreamer streaming
threads, and the hint says so instead of pointing at the JIT.

The browser watches itself the same way. A heartbeat thread notices when
the main loop has not run for five seconds — the one freeze the main loop
cannot report — and prints this process's main thread and anything busy
or blocked, straight to stderr:

```
watch: THIS PROCESS (13843) main loop has not run for 5s. Its running/blocked threads:
watch:   13843  S  browser-mini       vk_wait_fence
```

A main thread waiting in a GPU, Vulkan or DRM call is GTK's renderer:
`--gsk gl`, then `--gsk cairo`.

There is a third kind of freeze that is not one. GTK paints only when the
compositor returns a frame callback, and if one never arrives after a
resize the main loop keeps running, input keeps working, and nothing is
ever drawn again. The watch tells this case apart too:

```
watch: main loop is running but NO FRAME HAS BEEN PAINTED for 12s although one was requested.
watch: GTK is waiting for a frame callback the compositor never sent
```

That one is between GTK's Wayland backend and the compositor, not the
browser or the page. Resizing the window again usually shakes a new
callback loose.

And a fourth: the popups draw, `load:` lines keep arriving, the frame
watch stays quiet — and the page area itself is stale. GTK is painting
and the web process is answering; what stopped is the hand-off between
them. On Wayland that is the dmabuf renderer: the web process exports a
buffer, the widget imports it, and after a resize the import goes stale.
`--no-dmabuf` takes that path out. **F6** re-presents the view without a
reload for the case at hand — the view is hidden for one loop iteration
and shown again with a fresh allocation, the page is kept. A
thread that is `R` with no wait and no GStreamer name is JavaScript or
WebAssembly running away — on a self-built WebKit, try `--no-jit` first. A thread in `D` names the
device or pipe it is stuck on. `--no-proc-watch` turns it off.

## Find on a large page

WebKit is asked for at most 1000 matches (the hint says `1000+` past that)
and the search runs on the pause after typing, not on every keystroke. A
one-letter search on a page of thousands of lines used to walk every match
twice and freeze the browser; now it is a few milliseconds.

## Repeated messages

WebKit and GLib write to stderr from several processes, and some lines
repeat dozens of times a run — "the desktop portal is unreachable" is
printed on every attempt. We cannot stop WebKit asking, so stderr is
threaded through a pipe: each distinct line is shown twice, then counted,
and the totals print at exit.

```
--- repeated messages, shown twice each above ---
     12 x  Unable to connect to the Deskop portal: ...
```

Nothing is hidden and nothing is guessed at. `Ctrl+C` ends the run the same
way closing the window does, so the totals still appear. A fatal error —
`g_error`, `abort()`, a crash — drains the pipe from the signal handler
before the process dies, so its last message is printed rather than lost
in the pipe: `Aborted` on its own tells nobody anything.

Some aborts say nothing at all. For those the handler prints the stack the
process was on, which names the layer even without symbols for every frame:

```
---- fatal: abort in this process, stack at the time: ----
/lib/libc.so.6(abort+0xdf)
/lib/libEGL_mesa.so.0(+0x1a2f0)          ← the driver's EGL
/lib/libgtk-4.so.1(gdk_gl_context_...)   ← GTK's renderer asked for it
/lib/libglib-2.0.so.0(g_main_loop_run+0x127)
```

A frame in `libgtk`/`libgdk` is the toolkit, `libEGL`/`libGL`/`*_dri.so`
the driver, `libwayland-client` the compositor link, `libwebkit` the
engine. Built with `-rdynamic`, so the browser's own functions are named.

## Is the graphics stack wired up at all?

```
browser-mini --gl-info
```

Checks the stack the way this browser will use it and exits: the DRM
device nodes and whether *this user* can open them, membership of the
`video` and `render` groups, the GL context GTK actually gets (API and
version), the renderer in use, and the environment variables that steer
all of it. A render node the user cannot open is the single most common
cause of "everything falls back to software and some of it aborts" — the
web process is where the GPU work happens, and it runs as you. It ends
with the commands for the layers below GTK: `dmesg`, `glxinfo`,
`vulkaninfo`, `vainfo`, and the firmware directory.

## Switching virtual terminals

One real backtrace from a VT switch, bottom-up: `wl_display_dispatch` →
GTK → `g_signal_emit` → **libwebkitgtk** → `abort`. No renderer, no
driver: a Wayland event that GTK relays as a signal, and a release
assertion in WebKit's own handler. That is a WebKitGTK bug, and `gsk`
cannot touch it. What can:

```
no_dmabuf = yes          # the other backing-store path; different code, may not assert
```

and a newer WebKitGTK, which is where such assertions get fixed. To name
the function, resolve the WebKit frames if your build kept symbols:

```
gdb -batch -ex 'info symbol 0xf8d93d' /usr/lib/libwebkitgtk-6.0.so.4
```

Switching away from the compositor's VT releases the GPU, and a renderer
holding a GL context can treat that as fatal. If the browser aborts on a
VT switch, the message above the `Aborted` line now says which layer did
it. `gsk = cairo` draws GTK's surface in software, with no GL context to
lose; WebKit composites the page in its own process regardless, so that is
the whole cost.
`--all-messages` turns the whole thing off.

## Version

```
browser-mini --version     # browser-mini 4.18.0 (build 4bbf154)
make version
```

The build id is an md5 of the sources, so two builds can be told apart
without guessing.

`-h`, `-V` and `--paths` are answered before anything else on the command
line is looked at, so they work even next to a mistyped option.

## Build

Debian and derivatives:

```
apt install build-essential pkg-config libgtk-4-dev libwebkitgtk-6.0-dev \
            libsoup-3.0-dev libgstreamer1.0-dev
make
```

`libgstreamer1.0-dev` is only needed for browser-big.

```
make install      # both into /usr/bin, PREFIX=... to move it
make uninstall
make clean
```

## Usage

```
browser-mini [URL] [options]
browser-big  [URL|PATH|diag] [options]
```

With no address the window comes up on the start page (below). A local
file needs three slashes — `file:///tmp/index.html` — though a bare path
works too (`./index.html`, `/tmp/index.html`), and anything without a
scheme is tried as https.

Run either with `-h` for the full option and key list.

## Start page

The program's name set large, a rule under it, the version and build id,
and the three keys worth knowing — on one flat colour. It is served
without a base URI, so it stays out of the history by itself.

The dot at the top opens a row of swatches. Click one and it becomes the
default from then on, remembered in `~/.local/share/wkview/start-bg`.
Pure white and pure black are both there; between them are soft flat
tones. The text follows the colour, dark on a light background and light
on a dark one, so every swatch reads.

```
start_bg = f3f0e9      # the default, until you click a swatch
```

The file belongs to you rather than to a profile: every profile and both
browsers share it. Delete it to go back to `start_bg`.

## Media mode

`F2` toggles it. While it is on, a frame in the accent colour runs round
the page, and `Ctrl+click` sends media out instead of opening it:

| Ctrl+click on | Goes to |
| --- | --- |
| a YouTube link, or a `.mp4` `.webm` `.mkv` `.m3u8` … link | `player` |
| an image, or a link to a `.jpg` `.png` `.webp` … | `image_viewer` |
| anything else | opened as usual |

Set the programs once in the config:

```
player       = mpv
image_viewer = imv
```

imv opens files, not addresses, so an image is downloaded first (with the
page as referer) into `$XDG_RUNTIME_DIR/wkview-media/`, and the viewer gets
the file. Files there are removed after an hour.

| Option | Effect |
| --- | --- |
| `--player CMD` | the video player; given on the command line it also starts in media mode |
| `--image-viewer CMD` | the image viewer |
| `--media`, `media_mode = yes` | start in media mode |
| `player_match = youtube.com/watch, vimeo.com/` | which links count as video (default: YouTube videos, shorts, live) |

It follows `--mod`: with `--mod alt` it is `Alt+click`. If a program
cannot start, a message says why and the click works as it normally would.

## Where everything lives

```
browser-mini --paths
```

Prints every directory and file either browser writes, with what is on
disk now. It follows `--profile` and `--private` wherever they sit on the
line.

| Path | What |
| --- | --- |
| `~/.config/swov/config` | Shared palette, read first |
| `~/.config/browser-mini/config` | Our settings, on top of it |
| `~/.local/share/wkview/<profile>/cookies.sqlite` | **Cookies — this is your logins** |
| `~/.local/share/wkview/<profile>/history.tsv` | Addresses visited |
| `~/.local/share/wkview/<profile>/searches.tsv` | Search keywords (`Ctrl+K`), per profile |
| `~/.local/share/wkview/<profile>/permissions.tsv` | Camera / microphone answers per site |
| `~/.local/share/wkview/<profile>/` | Also WebKit's own storage: local storage, IndexedDB, service workers |
| `~/.cache/wkview/<profile>/` | Cache, safe to delete |
| `~/.local/share/wkview/download-dirs.tsv` | Per-site download rules |
| `~/.local/share/wkview/start-bg` | Start page background |

The last two are yours, not a profile's: every profile and both browsers
share them. Nothing else is written.

Downloads are two separate things and `--paths` keeps them apart: the
directory a file **lands in** (`--download-dir`, or the XDG download
directory), and `download-dirs.tsv`, the one file holding every per-site
**rule** that overrides it. The report prints the rules themselves, so you
can see which site goes where without opening the file.

Search keywords get the same treatment: `--paths` lists each keyword with
its URL and marks the default. They are per profile, so another
`--profile` shows a different list.

`--clear-data` wipes the profile and cache directories, `--forget-perms`
drops `permissions.tsv` alone, `--private` skips all of it and keeps the
session in memory.

Because `--paths` walks the directories instead of guessing, it also shows
WebKit's own storage under the profile, whatever the release names it.

## Keys

| Key | Action |
| --- | --- |
| `Ctrl+O`, `Ctrl+L` | Type an address |
| `Ctrl+J` | Back |
| `Ctrl+Shift+J` | Forward |
| `Ctrl+H` | The same popup, opened on the history |
| `Ctrl+F` | Find in page. `Enter`/`Down`/`Ctrl+N` next, `Shift+Enter`/`Up`/`Ctrl+Shift+N` previous, `Esc` out |
| `Ctrl+S` | Download directory: this page, this site, or everything |
| `Ctrl+K` | Add a search keyword |
| `F1`, `Ctrl+/` | The key list, on screen |
| `Ctrl+G` | Scroll to top |
| `Ctrl+Shift+G` | Scroll to bottom |
| `Ctrl+R`, `F5` | Reload |
| `Ctrl+D` | Recent downloads |
| `Ctrl+P` | Show the whole current URL, wrapped; again to hide |
| `F2` | Media mode on/off |
| `Ctrl+Y` | Copy URL, and show what was copied |
| `Ctrl+plus/minus/0` | Zoom |
| `F12` | Developer tools |

Use `--mod alt|super|meta` to move the modifier off Ctrl. Nothing else is
intercepted, so copy/paste keeps working inside the page.

browser-big adds `Ctrl+Shift+M` (diagnostics page), `Ctrl+Shift+C` (warm the
camera), `Ctrl+Shift+X` (release it), `Ctrl+Shift+V` (dump video state).

## History

Stored at:

```
~/.local/share/wkview/<profile>/history.tsv
```

One tab separated line per page: timestamp, URL, title. Only successful
loads of `http`, `https` and `file` are written, so every line is a link
that worked. Failed loads, error pages and internal pages are skipped.
Trimmed to the last 2000 lines at startup.

`--private` turns it off. The file is plain text — grep it, edit it, delete it.

One popup does both jobs, because they are the same job: an input line with
the matching history under it.

- `Ctrl+O` types an address. The history is listed straight away — eight
  lines by default, `list_rows` in a config file — and narrows as you type,
  so the two are one thing: pick a line, or press `Enter` and what you typed
  is loaded.
- `Ctrl+H` opens the same popup on the history.

Every panel — address, history, find, download directory, key list — is the
same width: `popup_width`, 550px by default, which is about fifty
characters of the mono font — enough for most addresses without the panel
taking over the window. It shrinks to fit a small window rather than
clipping, and the history list takes up to about two thirds of the window's
height.

```
popup_width = 900         # in swov's config, or the browser's
```

`Tab` and `Down` walk forward through the matches, `Shift+Tab` and `Up`
back, the wheel scrolls, a click opens. Whatever is selected is written into
the input, so `Enter` always loads exactly what you can read. `Esc`,
`Ctrl+H` or a click outside closes it.

Anything the popup can save shows a **Save** and a **Cancel** button with
the keys named on them (`Save (Enter)`, `Cancel (Esc)`) — Enter is never the
only way in. They are text, not boxes: Cancel sits at the left in `subtext`,
Save at the right in the accent and bold, and either underlines and
brightens on hover. When the directory popup asks about a clash the same two
buttons become `Drop that rule` and `Keep both`, so both answers are on
screen rather than implied.

While a page loads, its address appears top left and becomes the page title
as soon as one arrives, with a thin accent line across the very top showing
progress.

`Ctrl+A` selects all — in a popup's input, and in the page. WebKitGTK maps
that key to move-to-start-of-line, an Emacs habit that surprises anyone
typing into a web text field, so it is turned into WebKit's own select-all
command. `select_all = no` gives the key back to the page.

`Esc` closes whatever is on screen — popup, key list, download list or a
message — and reaches the page only when nothing of ours is up.

Back and forward walk WebKit's session list first, which keeps scroll
position and form state. Once that runs out they continue into the stored
history, so back still works on the first page after a restart.

## Download directories

Downloads land in `--download-dir` unless a rule says otherwise. `Ctrl+S`
sets one: type a directory, and the select decides how far it reaches —
this address only, everything on the site, or everything. The narrower rule
wins, so one page can go somewhere its site does not.

```
~/.local/share/wkview/download-dirs.tsv    key <TAB> directory [<TAB> overwrite]
```

The file belongs to you, not to a profile: every profile and both browsers
read the same rules, since which directory a site's files go in has nothing
to do with which cookie jar is in use. Rules written by an older build are
carried over from the profile on first run.

The key is a whole address or a bare host. The `Ctrl+S` popup lists everything
stored, so a directory can be picked with the arrows or the mouse instead
of typed, and the rule under the cursor dropped with `Delete` or `Ctrl+X`. The scopes are independent and the
narrower one always wins: a rule on `a.de/b` keeps its own directory when
`a.de` gets one. Setting a rule only ever writes the key for the scope you
picked. A rule set on a page also covers
the files that page hands out, which usually live on another address — a
CDN, or just a different path — so a rule on a download page catches what
it serves.

Ticking **always replace existing files** turns the question off for
whatever that rule covers: files take the name they ask for. It follows the
same select, so it can apply to one page, a whole site, or everything —
`always_overwrite` in a config file does the last one permanently.

If something else already points at the directory you set, you are asked
first: `Enter` drops the older rule, `Esc` lets both use it. Sharing a
directory is usually meant, but not always.

Setting the scope to **everything** lasts for that run only — put
`download_dir` in a config file to make it permanent.

Two browsers saving into one directory do not collide either: a name is
claimed by creating the file, so the second one moves to the next number
rather than landing on top of the first. Tested with two instances pulling
the same file into the same directory at once.

Rules are merged on save, not overwritten: a second browser on the same
profile re-reads the file and keeps what the first one wrote, so two
instances cannot trample each other's rules. The same goes for search
keywords.

A file whose name is already taken is downloaded beside the old one and the
question is asked in the download panel itself — no second popup. `Enter`
puts it in the old one's place, `Esc` keeps both, and the transfer only
reads as done once the file is where it is going to stay. A directory that does not exist
is created group writable (`0770`). The panel appears the moment the server answers, and reads `starting` until
the first byte lands. It normally fades out under the pointer so the page
can be read through it, but a download that has just appeared outranks that
for a few seconds — otherwise one starting while the pointer happened to
rest there would arrive invisibly. The panel shows the
destination in small text on the right of its header, so it is clear where
a file is going before it lands.

## A config that needs no flags

Everything a conferencing site has needed so far, as settled preferences:

```
# ~/.config/browser-big/config
audio = alsa                     # plain-ALSA machine, no sound server
web_compat = yes                 # screen.orientation for Zoom's media engine
feature = OffscreenCanvas=off    # no worker-side WebGL attempt, so no CPU fallback
```

Two things that used to be here are now the default, because they are safe
on every machine and match what other browsers do: GTK's **GL renderer**
(`gsk = auto` hands the choice back to GTK) and **SharedArrayBuffer**
(`shared_array_buffer = no` to disable; WebKit still hands it only to
cross-origin-isolated pages). The three above stay in the config because
they are specific to the machine or the site.

`audio` and `web_compat` are browser-big's; browser-mini notes them as
unknown and carries on. The other three apply to both.

## A test ladder that isolates each layer

The WebRTC samples at `webrtc.github.io/samples` are the reference
implementation: unobfuscated, one thing per page, and they print what
they got. Each rung adds one layer, so the first one that fails names it.

| Page | Tests | Flags to try if it fails |
| --- | --- | --- |
| `src/content/getusermedia/gum/` | camera only, no audio, no WebRTC | `--rtc-trace` to see it |
| `src/content/getusermedia/audio/` | microphone only | `--audio-alsa` |
| `src/content/getusermedia/resolution/` | asks for QVGA, VGA, HD by size, some `exact` | `--cam-scale` |
| `src/content/getusermedia/record/` | MediaRecorder from the camera | — |
| `src/content/peerconnection/pc1/` | **a full WebRTC call inside one page**, no server | `--fix-webrtc`, `--video-codecs VP8` |
| `src/content/peerconnection/constraints/` | codec and bitrate negotiation | `--video-codecs` |

`pc1` is the important one: two peer connections in one page, offer,
answer, ICE, media flowing — every part of the WebRTC stack with no site
logic in the way. If it works, the browser's WebRTC is sound and anything a
real site does differently is the site's own code.

```
browser-big https://webrtc.github.io/samples/src/content/getusermedia/gum/ --rtc-trace
browser-big https://webrtc.github.io/samples/src/content/peerconnection/pc1/ --rtc-trace
```

## Camera and microphone permission

A page asking for the camera or microphone is **asked about**, the way a
browser does: a panel at the top names the site and what it wants, `Enter`
allows, `Esc` denies, and the answer is remembered per site in
`<profile>/permissions.tsv`. Device labels for `enumerateDevices()` are
revealed only once a site has been granted a device, which is also what
other browsers do.

```
--forget-permissions forget every remembered answer, so sites ask again
--allow-media        grant every page without asking (a kiosk, a script)
--deny-media         deny every page
media = ask|allow|deny     the same, in the config
```

Until 4.0.0 every request was granted silently. That was a shortcut, not a
policy, and it is gone.

## When the camera is not picked up

browser-big probes the capture devices itself with GStreamer and logs what
it found. If that count is non-zero but the page still says access was
denied, the devices are fine — the web process could not reach them. It
gets at them through the desktop portal, and the portal needs a working
D-Bus.

The usual cause is a missing machine id, which makes D-Bus unable to start
at all:

A machine id is a D-Bus requirement, not a systemd one. Either tool works,
and neither needs an init system:

```
sudo dbus-uuidgen --ensure=/etc/machine-id       # any distro with dbus
sudo ln -sf /etc/machine-id /var/lib/dbus/machine-id
```

Without dbus's tools at all, the file is just 32 hex characters:

```
head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n' | sudo tee /etc/machine-id
```

Then a session bus. With no systemd user session, start one per run, or
better, start the compositor inside one so every app shares it:

```
dbus-run-session -- browser-big URL      # per run
dbus-run-session -- sway                 # whole session
```

The portal itself is a separate package: `xdg-desktop-portal` plus a
backend — `xdg-desktop-portal-wlr` for sway, or `-gtk`.

browser-big checks the result rather than trusting the flag. A sandboxed
web process is placed in its own mount namespace by `bwrap`, so it compares
namespaces and says which it is:

```
sandbox: web process 312, parent bwrap, mount namespace differs from ours  ->  sandboxed
sandbox: web process 370, parent browser-big, mount namespace same as ours  ->  NOT sandboxed
```

Portal warnings can keep appearing either way — WebKit asks the portal for
settings and other things too, so they are not evidence of sandboxing.

Or skip the portal entirely. `--no-sandbox` lets the web process open the
capture devices directly, which is the pragmatic answer on a machine with
no portal at all. It gives up the sandbox and prints a line saying so on
every run.

To rule the sandbox out instead:

```
WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 browser-big URL
```

`--media-trace` logs every `getUserMedia` call, the constraints it asked
for and how it ended, **without changing any of them** — relaxing, caching,
retrying and the audio fallback are all off. That is the first thing to
reach for when a site claims access was denied while the devices look fine:
it shows which call failed and what it wanted.

If permission is granted and no device ever reports capturing, browser-big
says so after a few seconds, with what to try next. That works on a plain
run — it watches the capture state rather than needing the `--cam-fix`
shim.

browser-big prints this advice itself the first time a getUserMedia call
fails, and with `--cam-fix` it stops relaxing constraints once it sees the
requested kind has no device at all — loosening a constraint cannot conjure
a camera.

**Finding the microphone.** `--prewarm` now separates real microphones
from PulseAudio monitors — a monitor records what is being played, so
`getUserMedia` cannot use it, and a log line listing only
`Monitor of Dummy Output` means no microphone was found. What WebKit sees
is exactly what this prints:

```
gst-device-monitor-1.0 Audio/Source
arecord -l                    # does ALSA see a capture device?
pactl info                    # is a Pulse server running, and did it get the card?
```

If ALSA has a capture device but GStreamer lists none, a half-working Pulse
server is usually in the way.

`--audio-alsa` ranks the pulse *elements* out, so playback and recording go
through ALSA. That fixes browser audio on a machine whose Pulse server came
up with a dummy sink.

It does **not** change enumeration. `GST_PLUGIN_FEATURE_RANK` is honoured
for elements but silently ignored for device providers — measured, not
assumed — so which devices exist is still decided by the providers that are
installed. A microphone therefore needs `alsadeviceprovider`:

```
gst-inspect-1.0 alsadeviceprovider     # part of gst-plugins-base's alsa plugin
gst-device-monitor-1.0 Audio/Source    # what WebKit will see
```

`--prewarm` reports whether that provider is present. `--gst-rank SPEC`
sets the variable by hand.

To have it every run, put it in browser-big's config rather than typing the
flag:

```
audio = alsa        # or pulse, or auto (the default)
```

It is deliberately **not** the built-in default. On a machine where
PipeWire or PulseAudio is healthy, ranking `pulsesink` out makes WebKit
open the card through `alsasink` directly, which without a dmix setup takes
it exclusively and silences everything else. Whether that is right is a
property of the machine, so it belongs in that machine's config.
`--audio-pulse` undoes it for one run.

**Seeing what the camera offers.** `--list-cameras` prints every format,
size and frame rate each device supports, with the aspect ratio worked out
for each, then exits. It runs before the window is created, so it works over
ssh and from a script:

```
browser-big --list-cameras

Integrated Camera: Integrated C   /dev/video0
    media type   format       width   height      framerate              aspect
    image/jpeg   -             1280 x 720         30/1                   16:9
    video/x-raw  YUY2           640 x 480         { 30/1, 15/1 }         4:3
    video/x-raw  YUY2          1280 x 720         10/1                   16:9
```

The listing ends with the exact flag to pin one of them:

```
browser-big URL --cam-force 640x480@30
```

That asks for 640x480 at 30fps whatever the page requests, and sets the
aspect ratio to match. It implies `--cam-fix`.

**Wrong shape.** `--cam-fix` used to delete the page's `aspectRatio` while
relaxing, and drop width and height entirely at level 2 — so a site asking
for 4:3 could be handed 16:9. It now keeps the aspect ratio as an ideal, and
derives one from the requested width and height when it gives up on the
resolution. To insist regardless of what the page asks:

```
browser-big URL --cam-force 640x480@30      # 4:3
```

It implies `--cam-fix`, and sets width, height and `aspectRatio` as ideals
so the request cannot become over-constrained.

**`--cam-scale` is usually the answer.** WebKit does not scale a capture:
asked for 320x240 it picks the nearest native camera mode — 848x480, say —
and hands that over. Chrome scales. `--cam-scale` does the same, drawing
the camera into a canvas of the requested size and capturing that, so the
track really does report the size the page asked for:

```
browser-big URL --cam-scale
→ cam-scale-mode  frame-callback  fps 15
→ cam-scaled      from 1280x720  to 320x240  fps 15
```

The scaling runs on the page's own thread, so it draws only when a frame
actually arrives (`requestVideoFrameCallback`) and the rate is capped at 15
by default — a timer-driven 30fps redraw was enough to freeze a conference
app that was already software-rendering. `--cam-scale-fps N` moves the cap.

A bare `width: 320` in a page's constraints is only a *preference*, and
WebKit may answer with a different size — 848x480 for a request of 320x240,
in one real case. A site that publishes at the size it asked for then has a
stream it cannot use. `--cam-exact 320x240@20` demands that size instead of
preferring it, and fails outright if the camera cannot deliver it, which is
at least an honest answer.

**No microphone.** A site asking for camera *and* microphone fails outright
on a machine with no audio input, even though the camera is fine. With
`--cam-fix`, browser-big retries once for video alone rather than handing
the site nothing. `--no-cam-drop-audio` turns that off, since the site did
not ask for it.

## When video or WebRTC does not work

WebKit plays media and talks WebRTC through GStreamer, so a missing plugin
looks like a broken site. browser-big checks at startup and says nothing
unless something needed is absent:

```
gstreamer: these are missing, and pages will fail without them:
  webrtcbin    WebRTC (gst-plugins-bad)
  avdec_h264   H.264 video (gst-libav)
```

WebRTC needs more than one plugin, which is why "install gst-plugins-bad"
often isn't enough:

| Element | From | Needs |
| --- | --- | --- |
| `webrtcbin` | gst-plugins-bad | **libnice** at build time |
| `x264enc` **or** `openh264enc` | gst-plugins-ugly / -bad | H.264 **encoding**, for publishing |
| `nicesrc` / `nicesink` | libnice itself | built with GStreamer support |
| `srtpenc` | gst-plugins-bad | libsrtp2 |
| `dtlssrtpenc` | gst-plugins-bad | OpenSSL |
| `rtpbin` | gst-plugins-good | — |

Two more come from `gst-plugins-rs` — `audiornnoise` (noise suppression)
and `rtpgccbwe` (RTP bandwidth estimation). Neither is required; WebKit
complains about each at the moment it wants it, which reads like a fault,
so browser-big lists them once up front as optional.

`libgstwebrtc.so` links `libnice.so` directly, so if libnice is absent when
gst-plugins-bad is configured, meson drops the `webrtc` feature and the
build still succeeds — silently. Verify each with
`gst-inspect-1.0 <element>`, or just start browser-big and read its report.

## browser-big: fix and debug flags

Nothing below is on unless asked for. A default run injects no JavaScript,
probes no devices, starts no watchdogs and reloads nothing.

**Look at what is happening**

| Flag | |
| --- | --- |
| `--list-cameras` | every format, size, rate and aspect each camera offers, then exit |
| `--prewarm` | what GStreamer can enumerate from here, cameras and microphones counted |
| `--media-trace` | every `getUserMedia` call, the frame that made it, its constraints and outcome, nothing altered |
| `--media-watchdog` | reports a `<video>` whose playback has stopped advancing, and tries to recover it |
| `--media-debug` | the above plus capture state, process table and GStreamer environment |
| `--gst-debug SPEC` | GStreamer's own logging, e.g. `v4l2*:6,webrtc*:5` |

`Ctrl+Shift+T` lists every thread of the web process with its state (`R`
running, `S` sleeping, `D` blocked in the kernel) and the kernel function
it is waiting in. It reads `/proc` and does not ask the web process
anything, so it works precisely when the page is frozen and the log has
gone quiet — a thread in `D` in `snd_pcm_*` is the sound card, one in a
pipe write is stderr, one spinning `R` is JavaScript or WebAssembly.

`Ctrl+Shift+V` dumps the state of every `<video>` on the page — readyState,
paused, currentTime, dimensions — which answers "is it stalled or is it
empty" directly. `Ctrl+Shift+M` opens the built-in diagnostics page.

**Change what is happening**

| Flag | |
| --- | --- |
| `--cam-force WxH@FPS` | pin size, rate and aspect whatever the page asks |
| `--cam-share` | hand a repeated request the stream already open, nothing else |
| `--cam-fix` | relax tight constraints, retry looser, hold the stream |
| `--fix-media` | `--cam-fix` plus prewarm, media watchdog and auto-reload |
| `--audio-alsa` | play and record through ALSA rather than PulseAudio |
| `--no-sandbox` | let the web process reach devices without the portal |
| `--no-gpu`, `--no-dmabuf`, `--no-compositing`, `--no-jit` | rendering and JIT fallbacks |

## browser-big's media workarounds are opt-in

By default browser-big is plain WebKitGTK — no shim over `getUserMedia`, no
device probe, no watchdogs, no reloads of its own. The fixes exist for sites
that misbehave and are reached for deliberately:

| Flag | What it does |
| --- | --- |
| `--fix-media` | all four below |
| `--cam-fix` | relax tight `getUserMedia` constraints, retry looser, hold the stream |
| `--prewarm` | probe capture devices at startup |
| `--media-watchdog` | notice a stalled `<video>` and recover it |
| `--auto-reload` | reload a page that never commits |
| `--warm-cam` | open and release the camera before the first page |

`--no-cam-relax` and `--no-cam-keepalive` trim `--cam-fix` when it is on.

Only the camera flags (`--cam-fix`, `--cam-share`, `--fix-media`,
`cam_share = yes`) change the camera. `--rtc-trace`, `--web-compat`,
`--no-mic` and the offer fixes use the same injected script but pass the
page's tracks through untouched. The log's `shim` line says `repair:true`
when a camera repair is on.

Before 4.14.0 they did not: any of those flags also cloned and held the
capture track. On WebKitGTK 2.52 the page then gets a clone that freezes
after about a second.

## Search keywords

Four are built in, and `g` is the default:

| Key | Searches |
| --- | --- |
| `g` | Google |
| `d` | DuckDuckGo |
| `w` | Wikipedia |
| `s` | the SDL3 wiki (through DuckDuckGo; the wiki has no search address) |

`w tree` searches Wikipedia; a bare `tree` goes to the default. A line that
looks like an address (a scheme, a path or a dotted host) is opened, not
searched. A bare `s` with nothing after it is an address too.

`Ctrl+K` lists the keywords and marks the default. Type
`k https://example.org/?q={}` to add one; `{}` is where the words go,
percent encoded. Tick the box to make it the default; `Delete` or `Ctrl+X`
drops the one under the cursor. `--paths` lists them all.

Keywords live in `searches.tsv` next to the profile. One there, or in a
config file, replaces a built-in of the same name:

```
search_s = https://wiki.libsdl.org/SDL3/{}
search_default = d
```

A deleted built-in comes back on the next start; replace it instead.

## When a page renders blank or zero sized

If the log says `web process crashed` or JavaScriptCore prints
`received NeedDebuggerBreak trap`, the page never had a chance — the web
process died, which is a WebKit or driver problem rather than anything the
browser did. Narrow it in this order:

```
browser-mini URL --no-jit        # JavaScriptCore's JIT
browser-mini URL --no-gpu        # accelerated compositing
WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 browser-mini URL   # the sandbox
```

The first one that helps names the culprit. A mismatched WebKit and
JavaScriptCore after a partial upgrade is the usual cause of a JIT trap.

Two other causes were found in the loading bar and fixed in 2.3.3. It reported a
negative minimum width, which makes GTK abandon the allocation pass and
leaves the web view unallocated — a zero-sized viewport and a white window
you can scroll forever. It also changed size on every progress tick, which
re-allocated the overlay and so re-laid-out the page dozens of times a
second: black and white flicker in time with the page's own activity. The
bar is drawn with cairo now, at a constant size, so a change repaints and
nothing more. `--no-load-bar` turns it off.

Otherwise it is usually

Usually a broken GL or compositing stack rather than the page. In devtools
a `body` of `0px x 0px` means the viewport itself has no size, so nothing
the page's CSS says can help. Try these in order, both browsers:

```
browser-mini URL --no-dmabuf
browser-mini URL --no-compositing
browser-mini URL --no-gpu
```

`--no-hw-decode` is the same idea for video. Any of them can go in a config
file once you know which one it was.

## Options worth knowing

| Option | Effect |
| --- | --- |
| `--profile NAME` | Named profile, persists cookies |
| `--private` | Ephemeral session, no history |
| `--clear-data` | Wipe the profile before starting |
| `--paths` | Print every directory and file written, then exit |
| `--player CMD` | start in media mode (`F2`), videos go to CMD |
| `--enable-middle-click-paste` | Let middle clicks reach the page (swallowed by default) |
| `--app-id ID` | Wayland `app_id` / X11 `WM_CLASS`, for sway rules |
| `--css FILE` | Inject a user stylesheet |
| `--ua-chrome`, `--ua-firefox`, … | Preset user agents |

## Where things appear

One rule, so there is nothing to learn:

| | |
| --- | --- |
| things you type into | top, centred — the popup and the key list |
| things that just tell you something | top right — messages and downloads |
| where you are going | bottom left — the address being opened |

Corners touching an edge stay square. Panels you can't interact with fade
out while the pointer is over them, so you can read what's underneath;
interactive ones don't.

## Look

Every overlay — the popup, toast, downloads — is drawn from
swov's palette and geometry, so the two programs read as one set. Defaults
match swov value for value: `tile` panels at `radius 14` with a `border 3`
outline, `text` on top, `hint` for headers, `hl` (the orange) for the caret,
the download bar and, at a third of its weight, for selections, and `find_hl`
(a pale blue) for the match `Ctrl+F` is sitting on, `urgent` for failures.

Nothing is hardcoded. `ui_css_install()` builds the stylesheet from the
theme, so changing `hl` in a config file moves every accent at once.

Panels hug the window edge rather than curve away from it: only the corners
facing into the page are rounded, and the border on the touching side is
dropped. The toast and the download list sit flush in the top right with one
rounded corner; the popup hangs off the top edge, centered, with two; the
address label sits in the top left with one; the key list floats clear in
the middle with four.

Every overlay is placed by hand from the window's real size, so a narrow or
short window shrinks them instead of clipping them. Tested down to 240x180.

## Config

Same format and the same key names as swov: `key = value`, `#` or `;`
comments, colours as `RRGGBB` or `RRGGBBAA`.

Files are read in this order, later wins, command line on top of both:

```
${XDG_CONFIG_HOME:-~/.config}/swov/config          shared palette
${XDG_CONFIG_HOME:-~/.config}/browser-mini/config   our own settings
```

So the palette lives once in swov's file and drives all three programs.
swov's file is full of keys for swov, so unknown keys there are counted and
ignored quietly — one line, not forty. An unknown key in the browser's own
file, or on the command line, is named, because there it means a typo.

| Option | Effect |
| --- | --- |
| `-c PATH` | Read this file instead of the chain |
| `-n` | Ignore the config files |
| `-s KEY=VAL` | Set one key. `--KEY=VAL` and bare `KEY=VAL` also work |

Every key is also a command line option:

```
browser-mini https://example.com --hl=ff8800 -s radius=6 ui_scale=1.2
```

**Look keys** (shared with swov): `bg tile tile_sel tile_hover card
card_hover text subtext dim accent hl hltext hint urgent outline find_hl
start_bg radius border pad gap win_gap ui_scale font font_mono label_px
title_px hint_px`

**Browser keys**: `title app_id zoom mod clip_cmd player image_viewer media_mode player_match download_dir profile
private no_media page_title middle_click_paste css user_agent quiet`

`font` is empty by default, meaning the GTK theme font. `font_mono` is
browser-only and covers URLs and file names, where fixed width earns its
place.

## Notes

- Profile directories keep the historic name `wkview`. Renaming it would
  orphan existing cookie jars.
- Middle-click paste is off by default: the press is claimed in the capture
  phase, since WebKitGTK has no setting for it. `--enable-middle-click-paste`
  puts it back.
- Downloads appear the moment one starts, in aligned columns: name on the
  left, percentage / size / ETA right aligned, the bar underneath.
