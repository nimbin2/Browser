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

## Version

```
browser-mini --version     # browser-mini 2.3.0 (build bcff1fa)
make version
```

The build id is an md5 of the sources, so two builds can be told apart
without guessing.

## Build

Debian and derivatives:

```
apt install build-essential pkg-config libgtk-4-dev libwebkitgtk-6.0-dev \
            libsoup-3.0-dev libgstreamer1.0-dev
make
```

`libgstreamer1.0-dev` is only needed for browser-big. `make install` puts both
in `/usr/local/bin`.

## Usage

```
browser-mini [URL] [options]
browser-big  [URL|PATH|diag] [options]
```

With no address the window comes up on a start page: the program's name set
large, a rule under it, the version and build id, and the three keys worth
knowing. It is drawn from the same palette as the panels, so the browser
looks like one thing from the first frame, and it is served without a base
URI so it stays out of the history by itself. A local
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
| `Ctrl+F` | Find in page. `Enter`/`Down`/`Ctrl+N` next, `Shift+Enter`/`Up`/`Ctrl+Shift+N` previous, `Esc` out |
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

## Search keywords

`Ctrl+K` opens a popup listing the keywords it knows. Add one by typing
`g https://google.com/search?q={}`, pick an existing one with the arrows or
the mouse to edit it, drop it with `Delete` or `Ctrl+X`.


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
Keys a program does not know are ignored with a note on stderr — that is
what makes one file safe to share.

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
