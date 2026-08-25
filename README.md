# minibrowser / bigbrowser

Two WebKitGTK 6.0 (GTK4) page viewers sharing one core.

**100% Vibecode but tested** — built warning-free with `-Wall -Wextra` against
GTK 4.14 / WebKitGTK 2.52.3, and run headless under Xvfb.

## Layout

| File | What it is |
| --- | --- |
| `browser_core.h` | Window state, the `BrowserApp` hook struct, shared helpers |
| `browser_core.c` | Window, popup, toast, zoom, downloads, profiles, keys, history |
| `minibrowser.c` | 20 lines. Fills in four fields and calls `browser_main()` |
| `bigbrowser.c` | Camera / GStreamer / watchdogs / diagnostics on top of the core |

minibrowser adds nothing to the core. Every hook is optional.

## Build

Debian and derivatives:

```
apt install build-essential pkg-config libgtk-4-dev libwebkitgtk-6.0-dev \
            libsoup-3.0-dev libgstreamer1.0-dev
make
```

`libgstreamer1.0-dev` is only needed for bigbrowser. `make install` puts both
in `/usr/local/bin`.

## Usage

```
minibrowser [URL] [options]
bigbrowser  [URL|PATH|diag] [options]
```

With no address the window comes up blank with the history open. A local
file needs three slashes — `file:///tmp/index.html` — though a bare path
works too (`./index.html`, `/tmp/index.html`), and anything without a
scheme is tried as https.

Run either with `-h` for the full option and key list.

## Keys

| Key | Action |
| --- | --- |
| `Ctrl+O`, `Ctrl+L` | Type an address |
| `Ctrl+J` | Back |
| `Ctrl+Shift+J` | Forward |
| `Ctrl+H` | The same popup, opened on the history |
| `Ctrl+F` | Find in page. `Enter` next, `Shift+Enter` previous, `Esc` out |
| `Ctrl+S` | Download directory: this page, this site, or everything |
| `Ctrl+K` | Add a search keyword |
| `F1`, `Ctrl+/` | The key list, on screen |
| `Ctrl+G` | Scroll to top |
| `Ctrl+Shift+G` | Scroll to bottom |
| `Ctrl+R`, `F5` | Reload |
| `Ctrl+D` | Recent downloads |
| `Ctrl+P` | Show current URL |
| `Ctrl+Y` | Copy URL, and show what was copied |
| `Ctrl+plus/minus/0` | Zoom |
| `F12` | Developer tools |

Use `--mod alt|super|meta` to move the modifier off Ctrl. Nothing else is
intercepted, so copy/paste keeps working inside the page.

bigbrowser adds `Ctrl+Shift+M` (diagnostics page), `Ctrl+Shift+C` (warm the
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

- `Ctrl+O` types an address. The list stays out of the way until `Tab` or
  `Down` asks for it — with nothing typed yet, that is the whole history.
- `Ctrl+H` opens the same popup on the history.

`Tab` and `Down` walk forward through the matches, `Shift+Tab` and `Up`
back, the wheel scrolls, a click opens. Whatever is selected is written into
the input, so `Enter` always loads exactly what you can read. `Esc`,
`Ctrl+H` or a click outside closes it.

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
~/.local/share/wkview/<profile>/download-dirs.tsv    key <TAB> directory
```

The key is a whole address or a bare host. A rule set on a page also covers
the files that page hands out, which usually live on another address — a
CDN, or just a different path — so a rule on a download page catches what
it serves.

A file whose name is already taken is downloaded beside the old one and the
question is asked in the download panel itself — no second popup. `Enter`
puts it in the old one's place, `Esc` keeps both, and the transfer only
reads as done once the file is where it is going to stay. A directory that does not exist
is created group writable (`0770`). The download panel shows the
destination in small text on the right of its header, so it is clear where
a file is going before it lands.

## Search keywords

A single word in front of the text in the address popup runs a search:

```
s tree      ->  https://google.com/search?hl=en&q=tree
```

`Ctrl+K` adds one — type `s https://google.com/search?hl=en&q={}`. `{}` is
replaced with the rest of the line, percent encoded. Keywords live in
`searches.tsv` next to the profile, and a config file can set them too:

```
search_s = https://google.com/search?hl=en&q={}
```

A bare `s` with nothing after it is still treated as an address, so a site
whose name collides with a keyword still works.

## Options worth knowing

| Option | Effect |
| --- | --- |
| `--profile NAME` | Named profile, persists cookies |
| `--private` | Ephemeral session, no history |
| `--clear-data` | Wipe the profile before starting |
| `--enable-middle-click-paste` | Let middle clicks reach the page (swallowed by default) |
| `--app-id ID` | Wayland `app_id` / X11 `WM_CLASS`, for sway rules |
| `--css FILE` | Inject a user stylesheet |
| `--ua-chrome`, `--ua-firefox`, … | Preset user agents |

## Look

Every overlay — the popup, toast, downloads — is drawn from
swov's palette and geometry, so the two programs read as one set. Defaults
match swov value for value: `tile` panels at `radius 14` with a `border 3`
outline, `text` on top, `hint` for headers, `hl` (the orange) for the caret
and the download bar, `urgent` for failures.

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
${XDG_CONFIG_HOME:-~/.config}/minibrowser/config   our own settings
```

So the palette lives once in swov's file and drives all three programs.
Keys a program does not know are ignored with a note on stderr — that is
what makes one file safe to share.

| Option | Effect |
| --- | --- |
| `-c PATH` | Read this file instead of the chain |
| `-n` | Ignore the config files |
| `-s KEY=VAL` | Set one key. `--KEY=VAL` and bare `KEY=VAL` also work |

Every key is also a command line option:

```
minibrowser https://example.com --hl=ff8800 -s radius=6 ui_scale=1.2
```

**Look keys** (shared with swov): `bg tile tile_sel tile_hover card
card_hover text subtext dim accent hl hltext hint urgent outline radius
border pad gap win_gap ui_scale font font_mono label_px title_px hint_px`

**Browser keys**: `title app_id zoom mod clip_cmd download_dir profile
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
