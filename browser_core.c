/*
 * browser_core - the shared half of browser-mini and browser-big.
 *
 * See browser_core.h for the front-end interface. Nothing in here knows
 * anything about cameras, GStreamer or diagnostics; that is browser-big's
 * business and it hangs off the BrowserApp hooks.
 */

#include "browser_core.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <grp.h>
#include <glib/gstdio.h>
#include <execinfo.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <glib-unix.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#define DEFAULT_CLIP_CMD "wl-copy"

/* Directory name used under ~/.local/share and ~/.cache for profiles.
 * Kept as "wkview" on purpose: renaming it would orphan existing
 * cookie jars / logins. Move those dirs by hand if you change it. */
#define PROFILE_DIR_NAME "wkview"

/* download overlay */
#define DL_LINGER_SECONDS  4      /* keep a finished download on screen */
#define DL_TICK_MS         100    /* progress / ETA refresh rate */
#define DL_REVEAL_MS       3000   /* a new download outranks the hover fade */
#define DL_ACTIVE_ROWS     4      /* max rows in the live overlay */
#define DL_HISTORY_ROWS    8      /* max rows in the <mod>+D list */
#define DL_KEEP            32     /* max downloads remembered */
#define DL_NAME_CHARS      30     /* filenames are elided to this */

/* stored history */
#define HIST_FILE     "history.tsv"
#define DLRULES_FILE  "download-dirs.tsv"
#define SEARCH_FILE   "searches.tsv"
#define STARTBG_FILE  "start-bg"
#define HIST_KEEP  2000           /* lines kept when the file is trimmed */
#define PICK_ROWS  200            /* most matches the picker will build   */
#define HIST_SETTLE_MS 400        /* wait for the page <title> to arrive */

/* -------------------------------------------------------------- globals */

WebKitSettings       *g_settings;
WebKitNetworkSession *g_session;

gboolean    g_quiet;
gboolean    g_deny_media;
/* ask (the default, as a browser does), allow, or deny - every site */
typedef enum { MEDIA_ASK, MEDIA_ALLOW, MEDIA_DENY } MediaPolicy;
static MediaPolicy g_media_policy = MEDIA_ASK;
#define PERM_FILE "permissions.tsv"
static GHashTable *g_perms;        /* "host" -> "allow" | "deny" */
gboolean    g_private;
double      g_zoom = 1.0;
char       *g_title;               /* window title, see --title    */
char       *g_app_id;              /* wayland app_id / X11 WM_CLASS */
GPtrArray  *g_wins;                /* Win*, every open window */

GdkModifierType g_mod      = GDK_CONTROL_MASK;
const char     *g_mod_name = "Ctrl";

static const BrowserApp *g_app;

static GPtrArray  *g_ucms;         /* WebKitUserContentManager*, unowned */
static const char *g_css_path;
static char       *g_css_data;
static gboolean    g_find_css_on;   /* the find highlight sheet is in */
static int         g_windows;

static char       *g_download_dir;

/*
 * Media mode (F2): a <mod>+click on a video goes to `player` (mpv) and on
 * an image to `image_viewer` (imv) instead of being followed. The address,
 * or for an image the downloaded file, is appended as the last argument;
 * no shell is involved. --player on the command line starts in the mode.
 */
static char       *g_player;
static char       *g_image_viewer;
static gboolean    g_media_mode;
static char      **g_player_match;   /* NULL: the YouTube set below */

static const char *const PLAYER_MATCH_DEFAULT[] = {
    "youtube.com/watch", "youtube.com/shorts/", "youtube.com/live/",
    "youtube.com/embed/", "youtube-nocookie.com/embed/", "youtu.be/", NULL
};

/* direct files, by the end of the path */
static const char *const VIDEO_EXT[] = {
    ".mp4", ".m4v", ".webm", ".mkv", ".mov", ".ogv", ".m3u8", ".mpd", NULL
};
static const char *const IMAGE_EXT[] = {
    ".jpg", ".jpeg", ".png", ".gif", ".webp", ".avif", ".jxl", ".bmp",
    ".tif", ".tiff", ".svg", ".heic", NULL
};
static char       *g_data_dir;     /* profile data dir, NULL when private */
static char       *g_profile;      /* NULL -> "default" */
static gboolean    g_clear_data;
static char       *g_user_agent;
static gboolean    g_follow_page_title = TRUE; /* see --title / --page-title */
static char       *g_clip_cmd;     /* --clip-cmd, NULL -> wl-copy  */
static gboolean    g_no_middle_paste = TRUE;  /* --enable-middle-click-paste */
static gboolean    g_fix_select_all = TRUE;   /* see on_key */
static gboolean    g_load_bar       = TRUE;   /* the hairline while loading */
static gboolean    g_no_gpu, g_no_dmabuf, g_no_compositing, g_no_hw_decode;
static gboolean    g_no_jit;
static gboolean    g_no_devtools;    /* --no-devtools: no inspector machinery at all */
static gboolean    g_all_messages;    /* --all-messages: no deduplication */
static gboolean    g_no_console;      /* --no-console: drop the page's logging */
static const char *g_gsk_renderer = "gl";   /* GTK4's renderer; "auto" = GTK's own choice */
static GPtrArray  *g_features;       /* --feature NAME[=on|off] */
static gboolean    g_gl_info;        /* --gl-info: check the graphics stack, exit */
static GPtrArray  *g_jsc_opts;       /* --jsc NAME=VALUE */
static gboolean    g_shared_ab = TRUE;  /* off with shared_array_buffer = no */
static gboolean    g_forget_perms;   /* --forget-permissions */
static gboolean truthy (const char *v);
static const char *g_list_features;  /* --list-features [filter] */

/*
 * WebKit and GLib write straight to stderr from several processes, and
 * some lines repeat dozens of times a run - the desktop portal being
 * unreachable is printed on every attempt. We cannot stop WebKit asking,
 * so stderr is threaded through a pipe and each distinct line is shown
 * twice, then counted. Nothing is hidden: the totals are printed at exit.
 */
#define NOISE_SHOW   2       /* occurrences printed before counting starts */
#define NOISE_MAX  500       /* distinct lines remembered */

static GMainLoop  *g_loop;           /* so a signal can end the run tidily */
static int         g_real_stderr = -1;
static int         g_noise_fd    = -1;   /* read end, for the final drain */
static GHashTable *g_noise;          /* line -> occurrences */
static gboolean    g_no_sandbox;
static gboolean    g_want_page_title;
static char       *g_css_owned;    /* when --css came from the config */

/* stored history: every address that actually came up, oldest first */
typedef struct {
    char *uri;
    char *title;
} HistEntry;

static GPtrArray  *g_hist;         /* HistEntry* */
static char       *g_hist_path;    /* NULL when history is off */

/* One tracked download. Lives in g_downloads; the WebKitDownload keeps a
 * pointer to it under the "dl" data key. */
typedef enum { DL_ACTIVE, DL_ASK, DL_DONE, DL_FAILED } DlState;

typedef struct {
    char    *name;
    char    *dest;          /* where it is actually being written */
    char    *want;          /* the name it wanted, when that existed already */
    gboolean overwrite;     /* the user said yes; rename over it when done */
    gboolean ask;           /* the name was taken, waiting for an answer   */
    DlState  state;
    double   progress;      /* 0..1, 0 when the size is unknown */
    guint64  received;
    guint64  total;
    gint64   start_us;
    gint64   end_us;

    /* smoothed transfer rate, so the ETA is not thrown off by the first
     * burst out of the socket buffer */
    double   rate;          /* bytes/s, exponentially smoothed */
    gint64   rate_us;       /* when the rate was last sampled */
    guint64  rate_bytes;    /* bytes received at that sample */
} Dl;

static GPtrArray *g_downloads;   /* Dl*,  oldest first */
static guint      g_dl_tick;     /* refresh timer, runs only when needed */

/* ---------------------------------------------------------- prototypes */

static GtkWidget *window_new (WebKitWebView *view, gboolean primary);
static gboolean   on_key     (GtkEventControllerKey *c, guint keyval, guint code,
                              GdkModifierType state, gpointer user_data);
static void       on_close   (GtkWindow *w, gpointer user_data);
static gboolean   on_decide_policy (WebKitWebView *view, WebKitPolicyDecision *decision,
                                    WebKitPolicyDecisionType type, gpointer u);
static gboolean   on_permission    (WebKitWebView *view, WebKitPermissionRequest *req,
                                    gpointer u);
static GtkWidget *on_create        (WebKitWebView *view, WebKitNavigationAction *act,
                                    gpointer u);
static void       on_ready_to_show (WebKitWebView *view, gpointer u);
static void       on_session_download_started (WebKitNetworkSession *session,
                                               WebKitDownload *download, gpointer u);
static void       downloads_refresh (void);
static void       downloads_reveal  (void);
static void       downloads_tick_start (void);
static void       dl_panel_rebuild  (Win *w);
static void       css_apply_one     (WebKitUserContentManager *ucm);
static void       css_apply_all     (void);
static void       find_highlight    (gboolean on);
static void       history_note      (Win *w);

static gboolean   parse_mod         (const char *name);
static void       omni_hide         (Win *w);
static void       dlrules_save      (void);
static void       omni_rebuild      (Win *w);

/* ------------------------------------------------------ small formatters */

static char *
format_size (guint64 bytes)
{
    if (bytes >= 1024ULL * 1024 * 1024)
        return g_strdup_printf ("%.1f GB", bytes / (1024.0 * 1024 * 1024));
    if (bytes >= 1024ULL * 1024)
        return g_strdup_printf ("%.1f MB", bytes / (1024.0 * 1024));
    if (bytes >= 1024)
        return g_strdup_printf ("%.0f kB", bytes / 1024.0);
    return g_strdup_printf ("%" G_GUINT64_FORMAT " B", bytes);
}

static char *
format_seconds (double secs)
{
    if (secs < 1)
        return g_strdup ("<1s");
    if (secs < 60)
        return g_strdup_printf ("%.0fs", secs);
    if (secs < 3600)
        return g_strdup_printf ("%dm%02ds", (int) secs / 60, (int) secs % 60);
    return g_strdup_printf ("%dh%02dm", (int) secs / 3600, ((int) secs % 3600) / 60);
}

/* Middle-elide, so both the stem and the extension stay readable. */
static char *
elide (const char *s, int max_chars)
{
    if (!s)
        return g_strdup ("");

    glong len = g_utf8_strlen (s, -1);
    if (len <= max_chars)
        return g_strdup (s);

    int keep  = max_chars - 1;
    int left  = keep / 2;
    int right = keep - left;

    char *head = g_strndup (s, (gsize) (g_utf8_offset_to_pointer (s, left) - s));
    char *out  = g_strconcat (head, "…", g_utf8_offset_to_pointer (s, len - right), NULL);
    g_free (head);
    return out;
}

/* ------------------------------------------------------------- helpers */

/*
 * GST_PLUGIN_FEATURE_RANK is one variable that several options feed, and
 * the user may have set it too. Append, never replace: GStreamer applies
 * the entries in order, so a later one for the same element wins.
 */
void
gst_rank_env_add (const char *spec)
{
    const char *had = g_getenv ("GST_PLUGIN_FEATURE_RANK");
    char       *v   = (had && *had) ? g_strconcat (had, ",", spec, NULL) : g_strdup (spec);

    g_setenv ("GST_PLUGIN_FEATURE_RANK", v, TRUE);
    g_free (v);
}

/* A front-end asking for a WebKit runtime feature, same as --feature. */
void
feature_request (const char *spec)
{
    if (!g_features)
        g_features = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; i < g_features->len; i++)
        if (!g_strcmp0 (g_ptr_array_index (g_features, i), spec))
            return;
    g_ptr_array_add (g_features, g_strdup (spec));
}

/* "example.com" -> "https://example.com", "./page.html" -> "file:///...". */
char *
normalize_uri (const char *in)
{
    if (!in)
        return NULL;

    char *s = g_strstrip (g_strdup (in));
    if (!*s) {
        g_free (s);
        return NULL;
    }

    if (g_str_has_prefix (s, "file://") && !g_str_has_prefix (s, "file:///")) {
        char *fixed = g_strconcat ("file:///", s + strlen ("file://"), NULL);
        g_free (s);
        return fixed;
    }

    if (strstr (s, "://")      ||
        g_str_has_prefix (s, "about:")  ||
        g_str_has_prefix (s, "data:")   ||
        g_str_has_prefix (s, "mailto:"))
        return s;

    if (g_file_test (s, G_FILE_TEST_EXISTS)) {
        char *abs = g_canonicalize_filename (s, NULL);
        char *uri = g_filename_to_uri (abs, NULL, NULL);
        g_free (abs);
        if (uri) {
            g_free (s);
            return uri;
        }
    }

    char *https = g_strconcat ("https://", s, NULL);
    g_free (s);
    return https;
}

static gboolean
parse_mod (const char *name)
{
    if (!g_ascii_strcasecmp (name, "ctrl") || !g_ascii_strcasecmp (name, "control")) {
        g_mod = GDK_CONTROL_MASK; g_mod_name = "Ctrl";  return TRUE;
    }
    if (!g_ascii_strcasecmp (name, "alt")  || !g_ascii_strcasecmp (name, "mod1")) {
        g_mod = GDK_ALT_MASK;     g_mod_name = "Alt";   return TRUE;
    }
    if (!g_ascii_strcasecmp (name, "super") || !g_ascii_strcasecmp (name, "win")) {
        g_mod = GDK_SUPER_MASK;   g_mod_name = "Super"; return TRUE;
    }
    if (!g_ascii_strcasecmp (name, "meta")) {
        g_mod = GDK_META_MASK;    g_mod_name = "Meta";  return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------- rm -rf helper */

static gboolean
rm_rf (const char *path)
{
    if (!path || !*path || !g_file_test (path, G_FILE_TEST_EXISTS))
        return TRUE;

    if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
        GError *err = NULL;
        GDir   *d   = g_dir_open (path, 0, &err);
        if (!d) {
            g_printerr ("clear-data: cannot open dir %s: %s\n", path, err->message);
            g_error_free (err);
            return FALSE;
        }

        const char *name;
        while ((name = g_dir_read_name (d))) {
            char *child = g_build_filename (path, name, NULL);
            gboolean ok = rm_rf (child);
            g_free (child);
            if (!ok) {
                g_dir_close (d);
                return FALSE;
            }
        }
        g_dir_close (d);

        if (g_rmdir (path) != 0) {
            g_printerr ("clear-data: cannot remove dir %s\n", path);
            return FALSE;
        }
        return TRUE;
    }

    if (g_remove (path) != 0) {
        g_printerr ("clear-data: cannot remove file %s\n", path);
        return FALSE;
    }
    return TRUE;
}

static void
profile_dirs (const char *profile, char **out_data_dir, char **out_cache_dir)
{
    if (!profile || !*profile)
        profile = "default";

    *out_data_dir  = g_build_filename (g_get_user_data_dir (),  PROFILE_DIR_NAME, profile, NULL);
    *out_cache_dir = g_build_filename (g_get_user_cache_dir (), PROFILE_DIR_NAME, profile, NULL);
}

/* ------------------------------------------------- per-site download dirs */

/*
 * A download normally lands in --download-dir. Anything more specific is
 * a rule, remembered in
 *
 *   <profile data dir>/download-dirs.tsv     key <TAB> directory
 *
 * where the key is either a whole address or a bare host. The narrower
 * one wins, so a single page can go somewhere its site does not:
 *
 *   https://host/a/b  ->  the rule for that exact address
 *   host              ->  the rule for the site
 *   neither           ->  --download-dir
 *
 * <mod>+S edits whichever of the three you pick.
 */

typedef enum { DL_SCOPE_PAGE, DL_SCOPE_SITE, DL_SCOPE_ALL } DlScope;

static GHashTable *g_dlrules;      /* key -> dir, both owned */
static GHashTable *g_dlgone;       /* keys this session deleted, as a set */
static GHashTable *g_dlalways;     /* keys whose files may be replaced */
static gboolean    g_always_overwrite;   /* ... and the same for everything */
static gboolean    g_always_overwrite_set;  /* said so on the command line */

#define DL_ALWAYS_TOKEN  "overwrite"
#define DL_ALL_KEY       "*"         /* the line that carries the global flag */

/*
 * These rules belong to the person, not to a profile: which directory a
 * site's files go in has nothing to do with which cookie jar is in use,
 * and two browsers on different profiles should agree about it. So they
 * live one level up from the profiles, in
 *
 *   ~/.local/share/wkview/download-dirs.tsv
 *
 * and every instance of either browser shares them.
 */
static char *
dlrules_path (void)
{
    return g_build_filename (g_get_user_data_dir (), PROFILE_DIR_NAME,
                             DLRULES_FILE, NULL);
}

static char *
uri_host (const char *uri)
{
    if (!uri)
        return NULL;

    GUri *u = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);
    if (!u)
        return NULL;

    const char *h = g_uri_get_host (u);
    char       *out = (h && *h) ? g_strdup (h) : NULL;

    g_uri_unref (u);
    return out;
}

/*
 * Two browsers on one profile would otherwise trample each other: both
 * read the file at startup, and whichever saved last would write its own
 * idea of the world over the other's. So a save re-reads the file first
 * and merges: what is on disk survives unless we changed that key or
 * deleted it here. The merged table becomes ours, so the other instance's
 * rules are picked up rather than lost.
 */
/* key <TAB> dir <TAB> overwrite, the last field optional. */
static void
dlrules_parse (const char *text, GHashTable *dirs, GHashTable *always,
               gboolean *global_always, gboolean skip_gone)
{
    char **lines = g_strsplit (text, "\n", -1);

    for (int i = 0; lines[i]; i++) {
        if (!*lines[i])
            continue;

        char **f = g_strsplit (lines[i], "\t", 3);
        gboolean ok = g_strv_length (f) >= 2 && *f[0];

        if (ok && skip_gone && g_dlgone && g_hash_table_contains (g_dlgone, f[0]))
            ok = FALSE;

        if (ok) {
            gboolean always_on = g_strv_length (f) >= 3 &&
                                 !g_strcmp0 (g_strstrip (f[2]), DL_ALWAYS_TOKEN);

            if (!g_strcmp0 (f[0], DL_ALL_KEY)) {
                if (global_always)
                    *global_always = always_on;
            } else if (*f[1]) {
                if (dirs)
                    g_hash_table_insert (dirs, g_strdup (f[0]), g_strdup (f[1]));
                if (always && always_on)
                    g_hash_table_add (always, g_strdup (f[0]));
            }
        }
        g_strfreev (f);
    }

    g_strfreev (lines);
}

static void
dlrules_write (GString *s, const char *key, const char *dir)
{
    gboolean always = g_dlalways && g_hash_table_contains (g_dlalways, key);

    g_string_append_printf (s, "%s\t%s%s%s\n", key, dir,
                            always ? "\t" : "", always ? DL_ALWAYS_TOKEN : "");
}

static void
dlrules_save (void)
{
    if (!g_dlrules)
        return;

    char       *path   = dlrules_path ();
    GHashTable *merged = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    char       *data   = NULL;

    if (g_file_get_contents (path, &data, NULL, NULL)) {
        /* the other instance's always-flags survive with its rules */
        dlrules_parse (data, merged, g_dlalways, NULL, TRUE);
        g_free (data);
    }

    GHashTableIter it;
    gpointer       k, v;

    g_hash_table_iter_init (&it, g_dlrules);
    while (g_hash_table_iter_next (&it, &k, &v))
        g_hash_table_insert (merged, g_strdup (k), g_strdup (v));   /* ours wins */

    GString *s = g_string_new (NULL);
    g_hash_table_iter_init (&it, merged);
    while (g_hash_table_iter_next (&it, &k, &v))
        dlrules_write (s, k, v);

    if (g_always_overwrite)
        g_string_append (s, DL_ALL_KEY "\t\t" DL_ALWAYS_TOKEN "\n");

    if (!g_file_set_contents (path, s->str, -1, NULL))
        g_printerr ("download: cannot write %s\n", path);

    g_hash_table_unref (g_dlrules);
    g_dlrules = merged;

    g_free (path);
    g_string_free (s, TRUE);
}

/* Keys other than except_key that already point at this directory. */
static GPtrArray *
dl_dir_users (const char *dir, const char *except_key)
{
    GPtrArray      *out = g_ptr_array_new ();
    GHashTableIter  it;
    gpointer        k, v;

    g_hash_table_iter_init (&it, g_dlrules);
    while (g_hash_table_iter_next (&it, &k, &v))
        if (!g_strcmp0 ((char *) v, dir) && g_strcmp0 ((char *) k, except_key))
            g_ptr_array_add (out, k);

    return out;
}

static void
dl_dir_forget (const char *key)
{
    if (!key)
        return;
    if (!g_dlgone)
        g_dlgone = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    g_hash_table_add (g_dlgone, g_strdup (key));
    g_hash_table_remove (g_dlrules, key);
}

/* "replace the existing link": every other rule aiming there is dropped. */
static void
dl_dir_clear_users (const char *dir, const char *except_key)
{
    GPtrArray *users = dl_dir_users (dir, except_key);
    GPtrArray *keys  = g_ptr_array_new_with_free_func (g_free);

    for (guint i = 0; i < users->len; i++)
        g_ptr_array_add (keys, g_strdup (g_ptr_array_index (users, i)));

    for (guint i = 0; i < keys->len; i++) {
        LOG ("download: dropped rule %s\n", (char *) g_ptr_array_index (keys, i));
        dl_dir_forget (g_ptr_array_index (keys, i));
    }

    g_ptr_array_free (users, TRUE);
    g_ptr_array_free (keys, TRUE);
}

/* profile_dir is only consulted to carry across rules written by an
 * older build, which kept them per profile. */
static void
dlrules_setup (const char *profile_dir)
{
    g_dlrules  = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    g_dlalways = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    char *path = dlrules_path ();
    char *data = NULL;

    if (g_file_get_contents (path, &data, NULL, NULL)) {
        dlrules_parse (data, g_dlrules, g_dlalways,
                       g_always_overwrite_set ? NULL : &g_always_overwrite, FALSE);
        g_free (data);
        LOG ("download: %u rules from %s\n", g_hash_table_size (g_dlrules), path);
        g_free (path);
        return;
    }

    if (profile_dir) {
        char *old = g_build_filename (profile_dir, DLRULES_FILE, NULL);

        if (g_file_get_contents (old, &data, NULL, NULL)) {
            dlrules_parse (data, g_dlrules, g_dlalways, &g_always_overwrite, FALSE);
            g_free (data);
            LOG ("download: carried %u rules over from %s\n",
                 g_hash_table_size (g_dlrules), old);
            dlrules_save ();           /* now they live with the others */
        }
        g_free (old);
    }

    g_free (path);
}

/* Whether files reached through this key may be replaced without asking. */
static gboolean
dl_always_for_key (const char *key)
{
    return g_always_overwrite ||
           (key && g_dlalways && g_hash_table_contains (g_dlalways, key));
}

static void
dl_always_set (const char *key, gboolean on)
{
    if (!key) {                        /* the everything scope */
        g_always_overwrite = on;
        return;
    }

    if (on)
        g_hash_table_add (g_dlalways, g_strdup (key));
    else
        g_hash_table_remove (g_dlalways, key);
}

/* The directory a download from this address should land in: the most
 * specific rule that matches, else the general one. */
static const char *
dl_dir_for_uri (const char *uri)
{
    if (uri) {
        const char *exact = g_hash_table_lookup (g_dlrules, uri);
        if (exact && *exact)
            return exact;
    }

    char *host = uri_host (uri);
    if (host) {
        const char *dir = g_hash_table_lookup (g_dlrules, host);
        g_free (host);
        if (dir && *dir)
            return dir;
    }

    return (g_download_dir && *g_download_dir) ? g_download_dir : ".";
}

/* Which rule is answering for this address right now. */
static DlScope
dl_scope_for_uri (const char *uri)
{
    if (uri && g_hash_table_contains (g_dlrules, uri))
        return DL_SCOPE_PAGE;

    char    *host = uri_host (uri);
    gboolean site = host && g_hash_table_contains (g_dlrules, host);
    g_free (host);

    return site ? DL_SCOPE_SITE : DL_SCOPE_ALL;
}

/* Downloads are often shared with other users or a daemon, so the target
 * is group writable. mkdir honours the umask, hence the explicit chmod. */
static gboolean
dl_dir_make (const char *dir)
{
    if (g_mkdir_with_parents (dir, 0770) != 0) {
        g_printerr ("download: cannot create dir %s: %s\n", dir, g_strerror (errno));
        return FALSE;
    }

    if (g_chmod (dir, 0770) != 0)
        LOG ("download: cannot set 0770 on %s: %s\n", dir, g_strerror (errno));

    return TRUE;
}

/*
 * A rule set on a page has to apply to the files that page hands out, and
 * those usually live on another address entirely - a CDN, or just a
 * different path. So the page is asked about first, and only then the
 * file's own address. Without this, a rule set with <mod>+S on a download
 * page would never match anything it served.
 */
static const char *
dl_dir_for_download (WebKitDownload *download)
{
    WebKitWebView    *view = webkit_download_get_web_view (download);
    WebKitURIRequest *req  = webkit_download_get_request (download);

    const char *page = view ? webkit_web_view_get_uri (view) : NULL;
    const char *file = req  ? webkit_uri_request_get_uri (req) : NULL;
    const char *dir;

    if (page && (dir = g_hash_table_lookup (g_dlrules, page)) && *dir)
        return dir;
    if (file && (dir = g_hash_table_lookup (g_dlrules, file)) && *dir)
        return dir;

    char *host = uri_host (page);
    if (host) {
        dir = g_hash_table_lookup (g_dlrules, host);
        g_free (host);
        if (dir && *dir)
            return dir;
    }

    return dl_dir_for_uri (file);      /* the file's host, then the general one */
}

/* The key that supplied the directory, so its always-flag can be found. */
static char *
dl_key_for_download (WebKitDownload *download)
{
    WebKitWebView    *view = webkit_download_get_web_view (download);
    WebKitURIRequest *req  = webkit_download_get_request (download);

    const char *page = view ? webkit_web_view_get_uri (view) : NULL;
    const char *file = req  ? webkit_uri_request_get_uri (req) : NULL;

    if (page && g_hash_table_contains (g_dlrules, page))
        return g_strdup (page);
    if (file && g_hash_table_contains (g_dlrules, file))
        return g_strdup (file);

    char *host = uri_host (page);
    if (host && g_hash_table_contains (g_dlrules, host))
        return host;
    g_free (host);

    host = uri_host (file);
    if (host && g_hash_table_contains (g_dlrules, host))
        return host;
    g_free (host);

    return NULL;                       /* the general directory answered */
}

static gboolean
dl_always_for_download (WebKitDownload *download)
{
    char    *key = dl_key_for_download (download);
    gboolean on  = dl_always_for_key (key);

    g_free (key);
    return on;
}

/*
 * The select says which key to write, and nothing else is touched. An
 * earlier version cleared the narrower rules when a broader one was set,
 * on the theory that a stale page rule would shadow a new site rule -
 * but shadowing is the whole point: a.de/b keeps its own directory when
 * a.de gets one, which is what having three scopes is for. Clearing them
 * also left a tombstone, so the page rule did not come back after a
 * restart.
 */
static void
dl_dir_set (const char *uri, const char *dir, DlScope scope)
{
    char *host = uri_host (uri);

    if (scope == DL_SCOPE_PAGE && uri) {
        LOG ("download: %s -> %s\n", uri, dir);
        if (g_dlgone) g_hash_table_remove (g_dlgone, uri);
        g_hash_table_insert (g_dlrules, g_strdup (uri), g_strdup (dir));
        dlrules_save ();
        g_free (host);
        return;
    }

    if (scope == DL_SCOPE_SITE && host) {
        LOG ("download: %s -> %s\n", host, dir);
        if (g_dlgone) g_hash_table_remove (g_dlgone, host);
        g_hash_table_insert (g_dlrules, host, g_strdup (dir));   /* takes host */
        dlrules_save ();
        return;
    }

    g_free (host);
    dlrules_save ();

    g_free (g_download_dir);
    g_download_dir = g_strdup (dir);
}

/* ------------------------------------------------------- search keywords */

/*
 * A single letter in front of the text in the address popup runs a search:
 * "s tree" with s = https://google.com/search?q={} loads the result page.
 * {} is replaced with the rest of the line, percent encoded.
 *
 *   <profile data dir>/searches.tsv          letter <TAB> url
 *
 * <mod>+K adds one, and a config file can too: search_s = https://...
 */

static GHashTable *g_searches;     /* keyword -> url template */
static char       *g_search_default;  /* the keyword used with no prefix */

/*
 * Always there, whatever the profile: a keyword of the same name in
 * searches.tsv or a config file (search_g = ...) replaces one of these,
 * and search_default picks another default. The SDL wiki has no search
 * address of its own, so s asks DuckDuckGo for it.
 */
static const char *const SEARCH_BUILTIN[][2] = {
    { "g", "https://www.google.com/search?q={}" },
    { "d", "https://duckduckgo.com/?q={}" },
    { "w", "https://en.wikipedia.org/w/index.php?search={}" },
    { "s", "https://duckduckgo.com/?q=site%3Awiki.libsdl.org+SDL3+{}" },
};
#define SEARCH_BUILTIN_DEFAULT "g"

static void searches_setup (const char *data_dir);

static void
searches_save (void)
{
    if (!g_searches || !g_data_dir)
        return;

    /* Merged for the same reason as the download rules, and into a table
     * of its own: reading straight into g_searches would let a stale line
     * on disk overwrite the very change being saved. */
    char       *path   = g_build_filename (g_data_dir, SEARCH_FILE, NULL);
    GHashTable *merged = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    char       *data   = NULL;

    if (g_file_get_contents (path, &data, NULL, NULL)) {
        char **lines = g_strsplit (data, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!*lines[i])
                continue;
            char **f = g_strsplit (lines[i], "\t", 2);
            if (g_strv_length (f) == 2 && *f[0] && *f[1])
                g_hash_table_insert (merged, g_strdup (f[0]), g_strdup (f[1]));
            g_strfreev (f);
        }
        g_strfreev (lines);
        g_free (data);
    }

    GString        *s = g_string_new (NULL);
    GHashTableIter  it;
    gpointer        k, v;

    g_hash_table_iter_init (&it, g_searches);
    while (g_hash_table_iter_next (&it, &k, &v))
        g_hash_table_insert (merged, g_strdup (k), g_strdup (v));   /* ours wins */

    g_hash_table_iter_init (&it, merged);
    while (g_hash_table_iter_next (&it, &k, &v)) {
        gboolean dflt = !g_strcmp0 ((char *) k, g_search_default);
        g_string_append_printf (s, "%s\t%s%s\n", (char *) k, (char *) v,
                                dflt ? "\tdefault" : "");
    }

    g_file_set_contents (path, s->str, -1, NULL);

    g_hash_table_unref (g_searches);
    g_searches = merged;

    g_free (path);
    g_string_free (s, TRUE);
}

static void
searches_setup (const char *data_dir)
{
    if (!g_searches)
        g_searches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

    char *path = data_dir ? g_build_filename (data_dir, SEARCH_FILE, NULL) : NULL;
    char *data = NULL;

    if (path && g_file_get_contents (path, &data, NULL, NULL)) {
        char **lines = g_strsplit (data, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!*lines[i])
                continue;
            char **f = g_strsplit (lines[i], "\t", 3);
            if (g_strv_length (f) >= 2 && *f[0] && *f[1]) {
                g_hash_table_insert (g_searches, g_strdup (f[0]), g_strdup (f[1]));
                if (g_strv_length (f) >= 3 && !g_strcmp0 (g_strstrip (f[2]), "default")) {
                    g_free (g_search_default);
                    g_search_default = g_strdup (f[0]);
                }
            }
            g_strfreev (f);
        }
        g_strfreev (lines);
        g_free (data);
        LOG ("search: %u keywords from %s\n", g_hash_table_size (g_searches), path);
    }

    /* the built-ins fill in whatever the file and the config left out */
    for (gsize i = 0; i < G_N_ELEMENTS (SEARCH_BUILTIN); i++)
        if (!g_hash_table_contains (g_searches, SEARCH_BUILTIN[i][0]))
            g_hash_table_insert (g_searches, g_strdup (SEARCH_BUILTIN[i][0]),
                                 g_strdup (SEARCH_BUILTIN[i][1]));
    if (!g_search_default || !g_hash_table_contains (g_searches, g_search_default)) {
        g_free (g_search_default);
        g_search_default = g_strdup (SEARCH_BUILTIN_DEFAULT);
    }
    LOG ("search: %u keywords, default %s\n",
         g_hash_table_size (g_searches), g_search_default);

    g_free (path);
}

static void
search_set (const char *key, const char *url, gboolean make_default)
{
    if (!g_searches)
        g_searches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

    g_hash_table_insert (g_searches, g_strdup (key), g_strdup (url));

    if (make_default) {
        g_free (g_search_default);
        g_search_default = g_strdup (key);
    } else if (!g_strcmp0 (key, g_search_default)) {
        g_clear_pointer (&g_search_default, g_free);   /* unticked */
    }

    searches_save ();
    LOG ("search: %s -> %s%s\n", key, url, make_default ? "  (default)" : "");
}

/*
 * Whether a line is meant as an address or as something to search for.
 * The rule everyone already knows from other browsers: a scheme, a path
 * or a dotted host is an address; words with a space in them, or a single
 * bare word, are a search.
 */
static gboolean
looks_like_address (const char *s)
{
    if (!s || !*s)
        return FALSE;

    if (strstr (s, "://")           || g_str_has_prefix (s, "about:") ||
        g_str_has_prefix (s, "data:")  || g_str_has_prefix (s, "file:") ||
        g_str_has_prefix (s, "mailto:"))
        return TRUE;

    if (*s == '/' || *s == '.' || *s == '~')
        return TRUE;                   /* a path */

    if (strchr (s, ' ') || strchr (s, '\t'))
        return FALSE;                  /* words */

    if (!g_ascii_strcasecmp (s, "localhost") || g_str_has_prefix (s, "localhost:"))
        return TRUE;

    if (g_file_test (s, G_FILE_TEST_EXISTS))
        return TRUE;

    const char *dot = strrchr (s, '.');
    return dot && dot != s && dot[1];  /* a dotted host */
}

/* Fill the default template with the whole line. */
static char *
search_default_uri (const char *line)
{
    const char *tmpl = g_search_default
                     ? g_hash_table_lookup (g_searches, g_search_default) : NULL;
    if (!tmpl || !line || !*line)
        return NULL;

    char *enc = g_uri_escape_string (line, NULL, TRUE);
    char *out;

    if (strstr (tmpl, "{}")) {
        char **parts = g_strsplit (tmpl, "{}", -1);
        out = g_strjoinv (enc, parts);
        g_strfreev (parts);
    } else {
        out = g_strconcat (tmpl, enc, NULL);
    }

    g_free (enc);
    return out;
}

/*
 * "s tree" -> the s template with tree in it. The keyword is one word and
 * has to be followed by something, so a bare "s" is still a search for a
 * site called s, and an address with a space in it is unaffected.
 */
static char *
search_expand (const char *line)
{
    if (!g_searches || !line)
        return NULL;

    const char *sp = strchr (line, ' ');
    if (!sp || sp == line)
        return NULL;

    char *key = g_strndup (line, (gsize) (sp - line));
    const char *tmpl = g_hash_table_lookup (g_searches, key);
    g_free (key);

    if (!tmpl)
        return NULL;

    const char *query = sp + 1;
    while (*query == ' ')
        query++;
    if (!*query)
        return NULL;

    char *enc = g_uri_escape_string (query, NULL, TRUE);
    char *out;

    if (strstr (tmpl, "{}")) {
        char **parts = g_strsplit (tmpl, "{}", -1);
        out = g_strjoinv (enc, parts);
        g_strfreev (parts);
    } else {
        out = g_strconcat (tmpl, enc, NULL);   /* template without a slot */
    }

    g_free (enc);
    return out;
}

/* Ctrl+C should end the run the same way closing the window does, so the
 * repeated-message totals and anything else at shutdown still happen. */
static gboolean
on_quit_signal (gpointer u)
{
    (void) u;
    if (g_loop)
        g_main_loop_quit (g_loop);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------- gl info */

/*
 * Whether the machine's graphics are wired up the way this browser needs
 * them, checked the way it will use them: can the render node be opened,
 * is the user allowed to, does GTK get a GL context and of what kind,
 * does GTK's own choice of renderer differ from ours. Read this before
 * blaming any page.
 */
static void
gl_info (void)
{
    g_print ("graphics check\n\n");

    /* the device nodes, and whether this user can open them */
    GDir *d = g_dir_open ("/dev/dri", 0, NULL);
    const char *n;
    gboolean render_ok = FALSE;
    g_print ("%-24s %-10s %s\n", "device", "mode", "open for this user");
    while (d && (n = g_dir_read_name (d))) {
        char *p = g_build_filename ("/dev/dri", n, NULL);
        GStatBuf st;
        if (g_stat (p, &st) == 0 && S_ISCHR (st.st_mode)) {   /* nodes only */
            int fd = open (p, O_RDWR | O_CLOEXEC);
            gboolean ok = fd >= 0;
            if (ok) close (fd);
            if (ok && g_str_has_prefix (n, "renderD")) render_ok = TRUE;
            g_print ("%-24s %04o       %s%s\n", p, st.st_mode & 07777,
                     ok ? "yes" : "NO",
                     ok ? "" : "   <- the web process cannot use the GPU without this");
        }
        g_free (p);
    }
    if (d) g_dir_close (d); else g_print ("/dev/dri does not exist: no DRM device at all\n");

    /* groups: video and render are what the nodes are usually owned by */
    gid_t groups[64]; int ng = getgroups (64, groups);
    GString *gs = g_string_new ("");
    for (int i = 0; i < ng; i++) {
        struct group *gr = getgrgid (groups[i]);
        if (gr && (!strcmp (gr->gr_name, "video") || !strcmp (gr->gr_name, "render")))
            g_string_append_printf (gs, "%s ", gr->gr_name);
    }
    g_print ("\ngroups this user is in of video/render: %s\n",
             gs->len ? gs->str : "NONE   <- add the user to both, then log in again");
    g_string_free (gs, TRUE);
    if (!render_ok)
        g_print ("render node not openable: GL, Vulkan, dmabuf and VA-API in the web\n"
                 "process all fall back or fail. This is usually the whole problem.\n");

    /* what GTK gets when it asks for GL, which is what the window uses */
    GdkDisplay *disp = gdk_display_get_default ();
    GError *err = NULL;
    g_print ("\nnote: an EACCES (-13) from amdgpu_query_info on the GBM platform is the\n"
             "card node refusing acceleration to a client the compositor has not\n"
             "authenticated - normal when the browser runs as a different user than the\n"
             "session; Mesa falls back to the render node, WebKit's worker WebGL may not.\n");
    g_print ("\nGTK display: %s\n", disp ? G_OBJECT_TYPE_NAME (disp) : "none");
    if (disp && !gdk_display_prepare_gl (disp, &err)) {
        g_print ("GTK GL: NOT AVAILABLE: %s\n", err ? err->message : "?");
        g_clear_error (&err);
    } else if (disp) {
        GdkGLContext *ctx = gdk_display_create_gl_context (disp, &err);
        if (!ctx) {
            g_print ("GTK GL: context creation failed: %s\n", err ? err->message : "?");
            g_clear_error (&err);
        } else if (!gdk_gl_context_realize (ctx, &err)) {
            g_print ("GTK GL: context could not be realized: %s\n", err ? err->message : "?");
            g_clear_error (&err);
        } else {
            int maj = 0, min = 0;
            gdk_gl_context_get_version (ctx, &maj, &min);
            GdkGLAPI api = gdk_gl_context_get_api (ctx);
            g_print ("GTK GL: %s %d.%d%s\n",
                     api == GDK_GL_API_GLES ? "OpenGL ES" : "OpenGL", maj, min,
                     gdk_gl_context_is_legacy (ctx) ? " (legacy profile)" : "");
        }
        if (ctx) g_object_unref (ctx);
    }

    g_print ("GTK renderer in use: %s (GSK_RENDERER=%s)\n",
             g_gsk_renderer ? g_gsk_renderer : "GTK's choice",
             g_getenv ("GSK_RENDERER") ? g_getenv ("GSK_RENDERER") : "unset");

    g_print ("\nenvironment that steers this:\n");
    const char *vars[] = { "WAYLAND_DISPLAY", "DISPLAY", "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE",
                           "LIBGL_ALWAYS_SOFTWARE", "MESA_LOADER_DRIVER_OVERRIDE",
                           "GBM_BACKEND", "WEBKIT_DISABLE_DMABUF_RENDERER",
                           "WEBKIT_DISABLE_COMPOSITING_MODE", "LIBVA_DRIVER_NAME", NULL };
    for (int i = 0; vars[i]; i++)
        g_print ("  %-34s %s\n", vars[i], g_getenv (vars[i]) ? g_getenv (vars[i]) : "(unset)");

    g_print ("\nby hand, for the layers below GTK:\n"
             "  dmesg | grep -i amdgpu | head        firmware loaded, no errors?\n"
             "  glxinfo -B  /  eglinfo -B            renderer string: radeonsi, not llvmpipe\n"
             "  vulkaninfo --summary                 radv device present?\n"
             "  vainfo                               VA-API profiles?\n"
             "  ls /lib/firmware/amdgpu | head       firmware files present for this chip?\n");
}

/* --------------------------------------------------------- webkit features */

/*
 * WebKit carries a few hundred runtime feature switches, some off by
 * default and needed by real sites. AllowWebGLInWorkers is the one that
 * matters most here: without it a library that renders in a worker gets
 * "Cannot create a canvas in this context" and falls back to the CPU.
 */
static void
features_list (const char *filter)
{
    WebKitFeatureList *all = webkit_settings_get_all_features ();
    gsize              n   = webkit_feature_list_get_length (all);
    guint              shown = 0;

    g_print ("%-46s %-8s %s\n", "feature", "default", "category");

    for (gsize i = 0; i < n; i++) {
        WebKitFeature *f  = webkit_feature_list_get (all, i);
        const char    *id = webkit_feature_get_identifier (f);

        if (!id || (filter && *filter &&
                    !strcasestr (id, filter)))
            continue;

        g_print ("%-46s %-8s %s\n", id,
                 webkit_feature_get_default_value (f) ? "on" : "off",
                 webkit_feature_get_category (f) ? webkit_feature_get_category (f) : "");
        shown++;
    }

    g_print ("\n%u of %zu features shown\n", shown, n);
    webkit_feature_list_unref (all);
}

static void
features_apply (WebKitSettings *s)
{
    if (!g_features)
        return;

    WebKitFeatureList *all = webkit_settings_get_all_features ();
    gsize              n   = webkit_feature_list_get_length (all);

    for (guint k = 0; k < g_features->len; k++) {
        char     *spec = g_ptr_array_index (g_features, k);
        char     *eq   = strchr (spec, '=');
        gboolean  want = TRUE;
        char     *name = g_strdup (spec);

        if (eq) {
            name[eq - spec] = '\0';
            want = truthy (eq + 1);
        }

        gboolean found = FALSE;
        for (gsize i = 0; i < n; i++) {
            WebKitFeature *f  = webkit_feature_list_get (all, i);
            const char    *id = webkit_feature_get_identifier (f);

            if (!id || g_ascii_strcasecmp (id, name) != 0)
                continue;

            webkit_settings_set_feature_enabled (s, f, want);
            LOG ("feature: %s = %s\n", id, want ? "on" : "off");
            found = TRUE;
            break;
        }

        if (!found)
            g_printerr ("feature: no such feature '%s'"
                        " (try --list-features)\n", name);
        g_free (name);
    }

    webkit_feature_list_unref (all);
}

/* ------------------------------------------------------- process watch */

/*
 * A page that freezes with the web process at 100% CPU and gigabytes of
 * memory is a runaway, and the moment to look at it is while it is
 * happening. This samples the web process every few seconds and, when it
 * is spinning, prints what its threads are doing - with no help from the
 * process itself, which is the point.
 */
static gboolean g_proc_watch = TRUE;

typedef struct { pid_t pid; guint64 ticks; gint64 at; guint hot; gboolean told; } ProcSample;
static ProcSample g_ps;

static gboolean
proc_stat (pid_t pid, guint64 *ticks, guint64 *rss_kb, char *state)
{
    char *p = g_strdup_printf ("/proc/%d/stat", (int) pid), *s = NULL;
    gboolean ok = g_file_get_contents (p, &s, NULL, NULL);
    g_free (p);
    if (!ok) return FALSE;

    char *rp = strrchr (s, ')');             /* fields after the comm */
    if (!rp) { g_free (s); return FALSE; }

    char **f = g_strsplit (rp + 2, " ", -1);   /* f[0] = state */
    gboolean good = g_strv_length (f) > 22;
    if (good) {
        *state  = f[0][0];
        *ticks  = g_ascii_strtoull (f[11], NULL, 10) + g_ascii_strtoull (f[12], NULL, 10);
        *rss_kb = g_ascii_strtoull (f[21], NULL, 10) * (guint64) (sysconf (_SC_PAGESIZE) / 1024);
    }
    g_strfreev (f);
    g_free (s);
    return good;
}

static pid_t
web_process_pid (void)
{
    GDir *d = g_dir_open ("/proc", 0, NULL);
    const char *n; pid_t self = getpid (), found = 0;
    GHashTable *parent = g_hash_table_new (g_direct_hash, g_direct_equal);
    GPtrArray  *webs   = g_ptr_array_new ();
    if (!d) return 0;

    while ((n = g_dir_read_name (d))) {
        if (!g_ascii_isdigit (n[0])) continue;
        char *p = g_strdup_printf ("/proc/%s/stat", n), *s = NULL;
        if (g_file_get_contents (p, &s, NULL, NULL)) {
            char *rp = strrchr (s, ')');
            if (rp) {
                pid_t pid  = (pid_t) atoi (n);
                pid_t ppid = (pid_t) g_ascii_strtoll (rp + 4, NULL, 10);
                g_hash_table_insert (parent, GINT_TO_POINTER (pid), GINT_TO_POINTER (ppid));
                if (strstr (s, "(WebKitWebProc"))
                    g_ptr_array_add (webs, GINT_TO_POINTER (pid));
            }
            g_free (s);
        }
        g_free (p);
    }
    g_dir_close (d);

    /* ours may be a child, or a grandchild behind bwrap: walk up */
    for (guint i = 0; i < webs->len && !found; i++) {
        pid_t p = GPOINTER_TO_INT (g_ptr_array_index (webs, i)), q = p;
        for (int hops = 0; q > 1 && hops < 8; hops++) {
            q = GPOINTER_TO_INT (g_hash_table_lookup (parent, GINT_TO_POINTER (q)));
            if (q == self) { found = p; break; }
        }
    }
    g_ptr_array_free (webs, TRUE);
    g_hash_table_destroy (parent);
    return found;
}

/* The same [hh:mm:ss.mmm] the rest of the log carries, so a freeze can
 * be lined up with what the page was doing at the time. */
static const char *
watch_stamp (void)
{
    static char buf[24];
    GDateTime *dt = g_date_time_new_now_local ();
    g_snprintf (buf, sizeof buf, "[%02d:%02d:%02d.%03d]",
                g_date_time_get_hour (dt), g_date_time_get_minute (dt),
                g_date_time_get_second (dt),
                g_date_time_get_microsecond (dt) / 1000);
    g_date_time_unref (dt);
    return buf;
}

/* GStreamer names its streaming threads after the pad they push from:
 * "queue0:src", "multiqueue1:src", "rtpgccbwe1:src". */
static gboolean
is_media_thread (const char *comm)
{
    return comm && (g_str_has_suffix (comm, ":src") || g_str_has_suffix (comm, ":sink") ||
                    g_str_has_prefix (comm, "gst") || strstr (comm, "queue"));
}

/* Prints the busy threads; returns how many of them there were, and in
 * *media how many of those were GStreamer's. */
static guint
threads_report (pid_t pid, guint *media)
{
    guint busy = 0;
    *media = 0;
    char *tdir = g_strdup_printf ("/proc/%d/task", (int) pid);
    GDir *td = g_dir_open (tdir, 0, NULL);
    const char *tn;
    while (td && (tn = g_dir_read_name (td))) {
        char *p, *comm = NULL, *wchan = NULL, *stat = NULL; char st = '?';
        p = g_strdup_printf ("%s/%s/comm", tdir, tn);  g_file_get_contents (p, &comm, NULL, NULL);  g_free (p);
        p = g_strdup_printf ("%s/%s/wchan", tdir, tn); g_file_get_contents (p, &wchan, NULL, NULL); g_free (p);
        p = g_strdup_printf ("%s/%s/stat", tdir, tn);
        if (g_file_get_contents (p, &stat, NULL, NULL)) { char *rp = strrchr (stat, ')'); if (rp && rp[2]) st = rp[2]; }
        g_free (p);
        if (comm) g_strchomp (comm);
        if (wchan) g_strchomp (wchan);
        /* only the threads that are doing something: running, or blocked */
        if (st == 'R' || st == 'D') {
            LOG ("watch:   %-6s %c  %-18s %s\n", tn, st, comm ? comm : "?",
                 (wchan && *wchan && strcmp (wchan, "0")) ? wchan : "(running)");
            busy++;
            if (is_media_thread (comm))
                (*media)++;
        }
        g_free (comm); g_free (wchan); g_free (stat);
    }
    if (td) g_dir_close (td);
    g_free (tdir);
    return busy;
}

static gboolean
proc_watch_tick (gpointer u)
{
    (void) u;
    pid_t pid = web_process_pid ();
    if (!pid) return G_SOURCE_CONTINUE;

    guint64 ticks = 0, rss = 0; char state = '?';
    if (!proc_stat (pid, &ticks, &rss, &state)) return G_SOURCE_CONTINUE;

    gint64 now = g_get_monotonic_time ();
    if (g_ps.pid == pid && g_ps.at) {
        double secs = (now - g_ps.at) / 1e6;
        double cpu  = 100.0 * (ticks - g_ps.ticks) / (double) sysconf (_SC_CLK_TCK) / secs;

        if (cpu >= 90.0) {
            g_ps.hot++;
        } else {
            /* a spin we reported has ended: say when, so the log shows
             * how long the page was frozen */
            if (g_ps.told)
                LOG ("watch: %s web process %d calm again after about %us "
                     "(%.0f%% cpu now)\n", watch_stamp (), (int) pid,
                     g_ps.hot * 5, cpu);
            g_ps.hot  = 0;
            g_ps.told = FALSE;
        }

        /* three samples in a row spinning: say so, and say where */
        if (g_ps.hot == 3 || (g_ps.hot > 3 && g_ps.hot % 12 == 0)) {
            guint media = 0, busy;

            LOG ("watch: %s web process %d is spinning: %.0f%% cpu, %.1f GB resident, "
                 "for %ds. Threads running or blocked:\n", watch_stamp (),
                 (int) pid, cpu, rss / 1048576.0, g_ps.hot * 5);
            busy = threads_report (pid, &media);
            g_ps.told = TRUE;

            /* The hint depends on who is busy: media threads are not
             * JavaScript, and --no-jit would only send you the wrong way. */
            if (busy && media * 2 >= busy)
                LOG ("watch: the busy threads are GStreamer's (media), not\n"
                     "watch: JavaScript - --no-jit will not help with this one\n");
            else
                LOG ("watch: if this recurs, try --no-jit first; a self-built\n"
                     "watch: JavaScriptCore that spins is usually its JIT\n");
        }
    }
    g_ps.pid = pid; g_ps.ticks = ticks; g_ps.at = now;
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------ view kick */

/*
 * The page area can go stale while everything else keeps working: the
 * web process still renders, the popups still draw, but the buffer the
 * page hands over stops being presented after a resize. Hiding the view
 * for one loop iteration and showing it again with a fresh allocation
 * makes the widget re-import what the web process has, without a
 * reload and without losing the page.
 */
static gboolean
view_kick_show (gpointer u)
{
    Win *w = u;
    gtk_widget_set_visible (GTK_WIDGET (w->view), TRUE);
    gtk_widget_queue_resize (GTK_WIDGET (w->view));
    gtk_widget_queue_draw (GTK_WIDGET (w->view));
    gtk_widget_grab_focus (GTK_WIDGET (w->view));
    LOG ("view: kicked (re-presented without reloading)\n");
    return G_SOURCE_REMOVE;
}

static void
view_kick (Win *w)
{
    gtk_widget_set_visible (GTK_WIDGET (w->view), FALSE);
    g_idle_add (view_kick_show, w);
}

/* ------------------------------------------------------ main-loop watch */

/*
 * The process watch above runs on the main loop, so it cannot report the
 * one thing that stops the main loop: this process freezing. A thread
 * with a heartbeat can. It prints straight to the real stderr, since the
 * message filter also lives on the main loop.
 */
static volatile gint g_heartbeat;

/*
 * A window can stop updating while the process is perfectly healthy:
 * GTK paints only when the compositor returns a frame callback, and if
 * one never comes after a resize, the main loop keeps running and nothing
 * is ever drawn again. That looks like a freeze and is not one. Tracking
 * paints against paint requests tells the two apart.
 */
static volatile gint64 g_last_paint;      /* monotonic us of the last frame */
static volatile gint64 g_last_request;    /* ... of the last request to paint */

static void
on_frame_after_paint (GdkFrameClock *fc, gpointer u)
{
    (void) fc; (void) u;
    g_last_paint = g_get_monotonic_time ();
}

static void
on_frame_update (GdkFrameClock *fc, gpointer u)
{
    (void) fc; (void) u;
    g_last_request = g_get_monotonic_time ();
}

static void
frame_watch_attach (GtkWidget *win)
{
    GdkFrameClock *fc = gtk_widget_get_frame_clock (win);
    if (!fc)
        return;
    g_signal_connect (fc, "after-paint", G_CALLBACK (on_frame_after_paint), NULL);
    g_signal_connect (fc, "update",      G_CALLBACK (on_frame_update), NULL);
    g_last_paint = g_last_request = g_get_monotonic_time ();
}

static gboolean
heartbeat_tick (gpointer u)
{
    (void) u;
    g_atomic_int_inc (&g_heartbeat);
    return G_SOURCE_CONTINUE;
}

static void
raw_err (const char *s)
{
    int fd = g_real_stderr >= 0 ? g_real_stderr : STDERR_FILENO;
    (void) !write (fd, s, strlen (s));
}

static gpointer
ui_watch_thread (gpointer u)
{
    (void) u;
    gint last = g_atomic_int_get (&g_heartbeat), stuck = 0;

    gint64 frame_warned = 0;

    for (;;) {
        g_usleep (G_USEC_PER_SEC);
        gint now = g_atomic_int_get (&g_heartbeat);

        /* main loop alive: is the window still being painted? */
        if (now != last) {
            last = now; stuck = 0;
            gint64 t = g_get_monotonic_time ();
            gint64 lp = g_last_paint, lr = g_last_request;
            if (lr > lp && t - lp > 5 * G_USEC_PER_SEC && lp != frame_warned) {
                frame_warned = lp;
                char buf[320];
                g_snprintf (buf, sizeof buf,
                    "\nwatch: main loop is running but NO FRAME HAS BEEN PAINTED for %.0fs "
                    "although one was requested.\n"
                    "watch: GTK is waiting for a frame callback the compositor never sent - "
                    "this follows a resize on some Wayland compositors.\n"
                    "watch: the process is healthy; the window is what stopped. "
                    "Try: --gsk cairo, or resize the window again.\n",
                    (t - lp) / 1e6);
                raw_err (buf);
            }
            continue;
        }

        stuck++;
        if (stuck != 5 && stuck % 30 != 0)
            continue;

        /* the main loop has not run for five seconds: say where it is */
        char  buf[256];
        g_snprintf (buf, sizeof buf,
                    "\nwatch: THIS PROCESS (%d) main loop has not run for %ds. "
                    "Its running/blocked threads:\n", (int) getpid (), stuck);
        raw_err (buf);

        GDir *td = g_dir_open ("/proc/self/task", 0, NULL);
        const char *tn;
        while (td && (tn = g_dir_read_name (td))) {
            char *p, *comm = NULL, *wchan = NULL, *stat = NULL; char st = '?';
            p = g_strdup_printf ("/proc/self/task/%s/comm", tn);  g_file_get_contents (p, &comm, NULL, NULL);  g_free (p);
            p = g_strdup_printf ("/proc/self/task/%s/wchan", tn); g_file_get_contents (p, &wchan, NULL, NULL); g_free (p);
            p = g_strdup_printf ("/proc/self/task/%s/stat", tn);
            if (g_file_get_contents (p, &stat, NULL, NULL)) { char *rp = strrchr (stat, ')'); if (rp && rp[2]) st = rp[2]; }
            g_free (p);
            if (comm) g_strchomp (comm);
            if (wchan) g_strchomp (wchan);
            /* the main thread always, whatever it is doing: it is the one
             * that stopped; the others only if they are busy or blocked */
            if (st == 'R' || st == 'D' || atoi (tn) == (int) getpid ()) {
                g_snprintf (buf, sizeof buf, "watch:   %-6s %c  %-18s %s\n", tn, st,
                            comm ? comm : "?",
                            (wchan && *wchan && strcmp (wchan, "0")) ? wchan : "(running)");
                raw_err (buf);
            }
            g_free (comm); g_free (wchan); g_free (stat);
        }
        if (td) g_dir_close (td);
        raw_err ("watch: a main thread waiting in a GPU/Vulkan/DRM call is the renderer:"
                 " try --gsk gl, then --gsk cairo\n");
    }
    return NULL;
}

/* ------------------------------------------------------------ log filter */

/*
 * Whatever is still in the pipe when the loop ends would otherwise be
 * dropped: the watch never runs again. Drained by hand at shutdown, or the
 * last thing printed before exit is silently lost.
 */
static void
noise_drain (void)
{
    if (g_real_stderr < 0 || g_noise_fd < 0)
        return;

    fflush (stderr);

    char    buf[4096];
    ssize_t n;

    while ((n = read (g_noise_fd, buf, sizeof buf)) > 0)
        (void) !write (g_real_stderr, buf, (size_t) n);
}

static void
noise_summary (void)
{
    if (!g_noise || g_real_stderr < 0)
        return;

    noise_drain ();

    GHashTableIter it;
    gpointer       k, v;
    gboolean       header = FALSE;

    g_hash_table_iter_init (&it, g_noise);
    while (g_hash_table_iter_next (&it, &k, &v)) {
        guint n = GPOINTER_TO_UINT (v);
        if (n <= NOISE_SHOW)
            continue;

        if (!header) {
            const char *h = "--- repeated messages, shown twice each above ---\n";
            (void) !write (g_real_stderr, h, strlen (h));
            header = TRUE;
        }

        char *line = g_strdup_printf ("  %5u x  %s\n", n, (char *) k);
        (void) !write (g_real_stderr, line, strlen (line));
        g_free (line);
    }
}

static gboolean
on_stderr_line (GIOChannel *ch, GIOCondition cond, gpointer u)
{
    (void) u;
    char   *line = NULL;
    gsize   len  = 0;

    if (cond & (G_IO_HUP | G_IO_ERR))
        return G_SOURCE_REMOVE;

    while (g_io_channel_read_line (ch, &line, &len, NULL, NULL) == G_IO_STATUS_NORMAL
           && line) {
        char *key = g_strchomp (g_strdup (line));
        guint n   = 1;

        if (*key) {
            gpointer had = g_hash_table_lookup (g_noise, key);
            n = GPOINTER_TO_UINT (had) + 1;

            if (had || g_hash_table_size (g_noise) < NOISE_MAX)
                g_hash_table_insert (g_noise, g_strdup (key), GUINT_TO_POINTER (n));
        }

        if (n <= NOISE_SHOW)
            (void) !write (g_real_stderr, line, len);

        g_free (key);
        g_free (line);
        line = NULL;
    }

    return G_SOURCE_CONTINUE;
}

/*
 * A fatal error prints to stderr and then aborts, and the message is
 * still sitting in the pipe when the process dies: the main loop, which
 * would have printed it, never runs again. So the last act before dying
 * is to drain the pipe by hand, with nothing but read() and write(),
 * which are safe to call from a signal handler.
 */
static void
on_fatal_signal (int sig)
{
    int fd = g_real_stderr >= 0 ? g_real_stderr : STDERR_FILENO;

    if (g_real_stderr >= 0 && g_noise_fd >= 0) {
        char    buf[4096];
        ssize_t n;
        while ((n = read (g_noise_fd, buf, sizeof buf)) > 0)
            (void) !write (fd, buf, (size_t) n);
    }

    /*
     * Some aborts say nothing at all. The stack is then the only witness:
     * which library was on it names the layer, even without symbols for
     * every frame. backtrace_symbols_fd() is safe to call here.
     */
    {
        const char *what = sig == SIGABRT ? "abort" : sig == SIGSEGV ? "segfault"
                         : sig == SIGBUS  ? "bus error" : sig == SIGTRAP ? "trap" : "signal";
        char head[128];
        int  len = g_snprintf (head, sizeof head,
                               "\n---- fatal: %s in this process, stack at the time: ----\n", what);
        (void) !write (fd, head, (size_t) len);

        void *frames[64];
        int   n = backtrace (frames, 64);
        backtrace_symbols_fd (frames, n, fd);

        const char *tail = "---- (a frame in libgtk/libgdk is the toolkit, libEGL/libGL or "
                           "*_dri.so the driver, libwayland-client the compositor link) ----\n";
        (void) !write (fd, tail, strlen (tail));
    }

    signal (sig, SIG_DFL);
    raise (sig);
}

/* Must run before any child is spawned, so they inherit the pipe. */
static void
noise_filter_start (void)
{
    int fds[2];

    if (g_all_messages)
        return;

    /*
     * Not when a firehose was asked for. GST_DEBUG at a high level writes
     * faster than this can drain, the pipe fills, and the process that is
     * writing blocks - which looked exactly like --gst-debug producing no
     * output at all.
     */
    if (g_getenv ("GST_DEBUG") || g_getenv ("WEBKIT_DEBUG"))
        return;

    if (pipe (fds) != 0)
        return;

    /* headroom, so a burst does not stall the writer before we read it */
#ifdef F_SETPIPE_SZ
    fcntl (fds[1], F_SETPIPE_SZ, 1 << 20);
#endif

    g_real_stderr = dup (STDERR_FILENO);
    if (g_real_stderr < 0) {
        close (fds[0]);
        close (fds[1]);
        return;
    }

    dup2 (fds[1], STDERR_FILENO);
    close (fds[1]);

    g_noise = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    g_noise_fd = fds[0];

    /* non-blocking, so the drain in a signal handler cannot hang */
    fcntl (fds[0], F_SETFL, fcntl (fds[0], F_GETFL) | O_NONBLOCK);
    signal (SIGABRT, on_fatal_signal);   /* abort(), assert, C++ terminate */
    signal (SIGTRAP, on_fatal_signal);   /* g_error() ends in a breakpoint */
    signal (SIGSEGV, on_fatal_signal);
    signal (SIGBUS,  on_fatal_signal);

    GIOChannel *ch = g_io_channel_unix_new (fds[0]);
    g_io_channel_set_flags (ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_encoding (ch, NULL, NULL);
    g_io_add_watch (ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_stderr_line, NULL);
    g_io_channel_unref (ch);
}

/* --------------------------------------------------------------- config */

/*
 * Same file format and the same key names as swov, so one file can drive
 * both. `key = value` per line, `#` or `;` comments, a trailing comment
 * needs a space in front of the `#`. Colours are RRGGBB or RRGGBBAA with
 * an optional leading `#`.
 *
 * Loaded in this order, later wins:
 *
 *   ${XDG_CONFIG_HOME:-~/.config}/swov/config       the shared palette
 *   ${XDG_CONFIG_HOME:-~/.config}/<prgname>/config  our own settings
 *
 * -c PATH replaces the chain, -n skips it, and every key is also a command
 * line option: --pad=12, -s pad=12 or plain pad=12.
 */

typedef struct { double r, g, b, a; } Color;

typedef struct {
    /* palette, swov names */
    Color bg, tile, tile_sel, tile_hover, mini_bg, card, card_hover, card_focus;
    Color text, subtext, dim, accent, hl, hltext, hint, urgent, outline;
    Color find_hl;          /* the match <mod>+F is sitting on */
    Color start_bg;         /* flat background of the start page */
    Color current, match, shadow;   /* carried for interchange */

    /* geometry */
    double radius, border, pad, gap, win_gap, margin, ui_scale;

    /* text */
    int   label_px, title_px, hint_px, ws_px;

    /* how wide a popup wants to be, before the window has its say */
    int   popup_w;
    int   list_rows;        /* how many lines a popup list shows */
    char *font;        /* "" -> whatever GTK is themed with */
    char *font_mono;   /* URLs and file names read better fixed width */
} Theme;

static Theme g_theme;

static Color
rgba_hex (guint32 v)
{
    Color c = { ((v >> 24) & 0xff) / 255.0,
                ((v >> 16) & 0xff) / 255.0,
                ((v >>  8) & 0xff) / 255.0,
                ((v      ) & 0xff) / 255.0 };
    return c;
}

/* RRGGBB or RRGGBBAA, '#' optional - swov's parse_color, same rules. */
static gboolean
parse_color (const char *s, Color *out)
{
    if (!s)
        return FALSE;
    while (*s == '#' || *s == ' ')
        s++;

    size_t n = strlen (s);
    if (n != 6 && n != 8)
        return FALSE;
    for (size_t i = 0; i < n; i++)
        if (!g_ascii_isxdigit (s[i]))
            return FALSE;

    guint64 v = g_ascii_strtoull (s, NULL, 16);
    if (n == 6)
        v = (v << 8) | 0xff;

    *out = rgba_hex ((guint32) v);
    return TRUE;
}

/* GTK wants rgba(), and the alpha matters: these panels sit over a page. */
static char *
css_rgba (Color c)
{
    return g_strdup_printf ("rgba(%d,%d,%d,%.3f)",
                            (int) (c.r * 255 + 0.5),
                            (int) (c.g * 255 + 0.5),
                            (int) (c.b * 255 + 0.5), c.a);
}

/* The same colour at a different weight: a selection wants the accent's
 * hue without its full strength, which is what makes it read as a wash
 * over the text rather than a block on top of it. */
static char *
css_rgba_at (Color c, double alpha)
{
    c.a *= alpha;
    return css_rgba (c);
}

static void
theme_defaults (void)
{
    Theme *t = &g_theme;

    /* swov's palette, value for value */
    t->bg         = rgba_hex (0x0d1117cc);
    t->tile       = rgba_hex (0x1e2733f2);
    t->tile_sel   = rgba_hex (0x26313ff2);
    t->tile_hover = rgba_hex (0x2b3644f2);
    t->mini_bg    = rgba_hex (0x11171f9e);
    t->card       = rgba_hex (0x33404ff7);
    t->card_focus = rgba_hex (0x3b4a5bf7);
    t->card_hover = rgba_hex (0x46566af7);
    t->hl         = rgba_hex (0xcb9b00ff);
    t->text       = rgba_hex (0xe8e8e8ff);
    t->subtext    = rgba_hex (0xb3c0cdff);
    t->dim        = rgba_hex (0x5a6b7aff);
    t->accent     = rgba_hex (0x89afc4ff);
    t->hltext     = rgba_hex (0x141414ff);
    t->hint       = rgba_hex (0xa7b5c4ff);
    t->urgent     = rgba_hex (0xe0533cff);
    t->outline    = rgba_hex (0x0a0e1499);
    t->find_hl    = rgba_hex (0x9fd6f5ff);   /* pale blue, not the orange */
    /* The start page is a full page, not a panel: flat and opaque, so it
     * never mixes with whatever WebKit paints underneath. Soft paper. */
    t->start_bg   = rgba_hex (0xf3f0e9ff);
    t->current    = rgba_hex (0x4fb3a5ff);
    t->match      = rgba_hex (0xb58ae0ff);
    t->shadow     = rgba_hex (0x00000073);

    t->radius   = 14.0;
    t->border   = 3.0;
    t->pad      = 10.0;
    t->gap      = 14.0;
    t->margin   = 26.0;        /* clearance from the window edge */
    t->win_gap  = 5.0;
    t->ui_scale = 1.0;

    t->list_rows = 8;
    t->popup_w  = 550;         /* about 50 characters of the mono font */
    t->ws_px    = 26;
    t->label_px = 16;
    t->title_px = 13;
    t->hint_px  = 14;

    t->font      = g_strdup ("");
    t->font_mono = g_strdup ("monospace");
}

static gboolean
key_is (const char *k, const char *a)
{
    return g_ascii_strcasecmp (k, a) == 0;
}

static gboolean
truthy (const char *v)
{
    return !(key_is (v, "0") || key_is (v, "no") || key_is (v, "off") ||
             key_is (v, "false"));
}

static void
set_str (char **dst, const char *v)
{
    g_free (*dst);
    *dst = g_strdup (v);
}

/* Returns FALSE for keys we do not know, so the caller can decide whether
 * that is worth a word on stderr. */
static gboolean
cfg_set (const char *k, const char *v)
{
    Theme *t = &g_theme;

    /* ---- palette, shared with swov ---- */
    if (key_is (k, "bg"))         return parse_color (v, &t->bg);
    if (key_is (k, "tile") || key_is (k, "panel") || key_is (k, "ring"))
                                  return parse_color (v, &t->tile);
    if (key_is (k, "tile_sel") || key_is (k, "ring2"))
                                  return parse_color (v, &t->tile_sel);
    if (key_is (k, "tile_hover")) return parse_color (v, &t->tile_hover);
    if (key_is (k, "card"))       return parse_color (v, &t->card);
    if (key_is (k, "card_focus"))  return parse_color (v, &t->card_focus);
    if (key_is (k, "mini_bg") || key_is (k, "center"))
                                   return parse_color (v, &t->mini_bg);
    if (key_is (k, "current"))     return parse_color (v, &t->current);
    if (key_is (k, "match"))       return parse_color (v, &t->match);
    if (key_is (k, "shadow_color")) return parse_color (v, &t->shadow);
    if (key_is (k, "card_hover") || key_is (k, "hover"))
                                  return parse_color (v, &t->card_hover);
    if (key_is (k, "text"))       return parse_color (v, &t->text);
    if (key_is (k, "subtext"))    return parse_color (v, &t->subtext);
    if (key_is (k, "dim"))        return parse_color (v, &t->dim);
    if (key_is (k, "accent"))     return parse_color (v, &t->accent);
    if (key_is (k, "hl"))         return parse_color (v, &t->hl);
    if (key_is (k, "hltext"))     return parse_color (v, &t->hltext);
    if (key_is (k, "hint"))       return parse_color (v, &t->hint);
    if (key_is (k, "urgent"))     return parse_color (v, &t->urgent);
    if (key_is (k, "outline"))    return parse_color (v, &t->outline);
    if (key_is (k, "find_hl") || key_is (k, "find"))
                                  return parse_color (v, &t->find_hl);
    if (key_is (k, "start_bg") || key_is (k, "startpage_bg"))
                                  return parse_color (v, &t->start_bg);

    /* ---- geometry, shared with swov ---- */
    if (key_is (k, "radius") || key_is (k, "corner")) { t->radius = g_ascii_strtod (v, NULL); return TRUE; }
    if (key_is (k, "border"))   { t->border  = g_ascii_strtod (v, NULL); return TRUE; }
    if (key_is (k, "pad"))      { t->pad     = g_ascii_strtod (v, NULL); return TRUE; }
    if (key_is (k, "gap"))      { t->gap     = g_ascii_strtod (v, NULL); return TRUE; }
    if (key_is (k, "margin") || key_is (k, "screen_pad"))
                                { t->margin  = g_ascii_strtod (v, NULL); return TRUE; }
    if (key_is (k, "win_gap") || key_is (k, "window_gap")) { t->win_gap = g_ascii_strtod (v, NULL); return TRUE; }
    if (key_is (k, "ui_scale") || key_is (k, "font_scale") || key_is (k, "text_scale")) {
        t->ui_scale = g_ascii_strtod (v, NULL);
        if (t->ui_scale <= 0.1) t->ui_scale = 1.0;
        return TRUE;
    }

    /* ---- text, shared with swov ---- */
    if (key_is (k, "popup_w") || key_is (k, "popup_width")) { t->popup_w = atoi (v); return TRUE; }
    if (key_is (k, "list_rows") || key_is (k, "rows")) { t->list_rows = atoi (v); return TRUE; }
    if (key_is (k, "ws_px") || key_is (k, "workspace_px")) { t->ws_px    = atoi (v); return TRUE; }
    if (key_is (k, "label_px") || key_is (k, "app_px"))    { t->label_px = atoi (v); return TRUE; }
    if (key_is (k, "title_px"))                            { t->title_px = atoi (v); return TRUE; }
    if (key_is (k, "hint_px") || key_is (k, "count_px"))   { t->hint_px  = atoi (v); return TRUE; }
    if (key_is (k, "font"))       { set_str (&t->font, v); return TRUE; }
    if (key_is (k, "font_mono"))  { set_str (&t->font_mono, v); return TRUE; }

    /* ---- ours ---- */
    if (key_is (k, "title"))         { set_str (&g_title, v); return TRUE; }
    if (key_is (k, "app_id"))        { set_str (&g_app_id, v); return TRUE; }
    if (key_is (k, "clip_cmd"))      { set_str (&g_clip_cmd, v); return TRUE; }
    if (key_is (k, "player"))        { set_str (&g_player, v); return TRUE; }
    if (key_is (k, "image_viewer"))  { set_str (&g_image_viewer, v); return TRUE; }
    if (key_is (k, "media_mode"))    { g_media_mode = truthy (v); return TRUE; }
    if (key_is (k, "player_match")) {
        g_strfreev (g_player_match);
        g_player_match = g_strsplit_set (v, ", ", -1);
        return TRUE;
    }
    if (key_is (k, "download_dir"))  { set_str (&g_download_dir, v); return TRUE; }
    if (key_is (k, "profile"))       { set_str (&g_profile, v); return TRUE; }
    if (key_is (k, "user_agent"))    { set_str (&g_user_agent, v); return TRUE; }
    if (key_is (k, "css"))           { set_str (&g_css_owned, v); g_css_path = g_css_owned; return TRUE; }
    if (key_is (k, "zoom"))          { g_zoom = CLAMP (g_ascii_strtod (v, NULL), ZOOM_MIN, ZOOM_MAX); return TRUE; }
    if (key_is (k, "private"))       { g_private = truthy (v); return TRUE; }
    if (key_is (k, "no_media"))      { g_deny_media = truthy (v); return TRUE; }
    if (key_is (k, "quiet"))         { g_quiet = truthy (v); return TRUE; }
    if (key_is (k, "page_title"))    { g_want_page_title = truthy (v); return TRUE; }
    if (key_is (k, "select_all"))    { g_fix_select_all = truthy (v); return TRUE; }
    if (key_is (k, "load_bar"))      { g_load_bar = truthy (v); return TRUE; }
    if (key_is (k, "middle_click_paste")) { g_no_middle_paste = !truthy (v); return TRUE; }
    if (key_is (k, "always_overwrite")) {
        g_always_overwrite     = truthy (v);
        g_always_overwrite_set = TRUE;      /* the file must not undo this */
        return TRUE;
    }
    if (key_is (k, "mod"))           return parse_mod (v);

    /* search_s = https://example.com/?q={} */
    if (key_is (k, "shared_array_buffer")) { g_shared_ab = truthy (v); return TRUE; }
    /* the rendering escape hatches, as settled preferences */
    if (key_is (k, "no_dmabuf"))      { g_no_dmabuf      = truthy (v); return TRUE; }
    if (key_is (k, "no_compositing")) { g_no_compositing = truthy (v); return TRUE; }
    if (key_is (k, "no_gpu"))         { g_no_gpu         = truthy (v); return TRUE; }
    if (key_is (k, "no_hw_decode"))   { g_no_hw_decode   = truthy (v); return TRUE; }

    if (key_is (k, "feature")) {           /* feature = Name[=on|off], repeatable */
        if (!g_features)
            g_features = g_ptr_array_new_with_free_func (g_free);
        g_ptr_array_add (g_features, g_strdup (v));
        return TRUE;
    }
    if (key_is (k, "gsk") || key_is (k, "renderer")) {
        /* the default is a literal, so never freed; a config value is
         * kept for the life of the process, which is the same thing */
        g_gsk_renderer = g_strdup (v);
        return TRUE;
    }

    if (key_is (k, "media")) {
        if (!g_ascii_strcasecmp (v, "allow"))      g_media_policy = MEDIA_ALLOW;
        else if (!g_ascii_strcasecmp (v, "deny")) { g_media_policy = MEDIA_DENY; g_deny_media = TRUE; }
        else if (!g_ascii_strcasecmp (v, "ask"))   g_media_policy = MEDIA_ASK;
        else g_printerr ("config: media must be ask, allow or deny\n");
        return TRUE;
    }

    if (key_is (k, "search_default") || key_is (k, "default_search")) {
        g_free (g_search_default);
        g_search_default = g_strdup (v);
        return TRUE;
    }

    if (g_ascii_strncasecmp (k, "search_", 7) == 0 && k[7]) {
        search_set (k + 7, v, FALSE);
        return TRUE;
    }

    if (g_app->cfg_set && g_app->cfg_set (k, v))
        return TRUE;

    return FALSE;
}

/* Whether an unrecognised key is worth mentioning. It is in our own file,
 * or on the command line, where it means a typo. It is not in swov's file,
 * which is full of keys for swov. */
static gboolean g_cfg_strict = TRUE;
static guint    g_cfg_ignored;

static void
cfg_set_line (const char *pair)
{
    const char *eq = strchr (pair, '=');
    if (!eq)
        return;

    char *k = g_strstrip (g_strndup (pair, (gsize) (eq - pair)));
    char *v = g_strstrip (g_strdup (eq + 1));

    if (*k && !cfg_set (k, v)) {
        if (g_cfg_strict)
            LOG ("config: unknown key '%s' (ignored)\n", k);
        else
            g_cfg_ignored++;
    }

    g_free (k);
    g_free (v);
}

static gboolean
cfg_load_file (const char *path)
{
    char *data = NULL;

    if (!path || !g_file_get_contents (path, &data, NULL, NULL))
        return FALSE;

    char **lines = g_strsplit (data, "\n", -1);

    for (int i = 0; lines[i]; i++) {
        char *s = g_strstrip (lines[i]);
        if (!*s || *s == '#' || *s == ';')
            continue;

        char *eq = strchr (s, '=');
        if (!eq)
            continue;

        /* a trailing comment needs a space in front of the hash, so that
         * a '#' inside a value survives */
        char *hash = strchr (eq + 1, '#');
        if (hash && hash > eq + 1 && hash[-1] == ' ')
            *hash = '\0';

        cfg_set_line (s);
    }

    g_strfreev (lines);
    g_free (data);

    if (g_cfg_ignored)
        LOG ("config: read %s (%u keys are not ours, ignored)\n",
             path, g_cfg_ignored);
    else
        LOG ("config: read %s\n", path);

    g_cfg_ignored = 0;
    return TRUE;
}

static char *
cfg_path_for (const char *app)
{
    const char *xdg = g_getenv ("XDG_CONFIG_HOME");

    if (xdg && *xdg)
        return g_build_filename (xdg, app, "config", NULL);
    return g_build_filename (g_get_home_dir (), ".config", app, "config", NULL);
}

/* swov's palette first, then ours on top of it */
static void
cfg_load_default_chain (const char *app)
{
    char *shared = cfg_path_for ("swov");
    char *own    = cfg_path_for (app);

    g_cfg_strict = FALSE;              /* swov's file, swov's keys */
    cfg_load_file (shared);

    g_cfg_strict = TRUE;               /* ours: an unknown key is a typo */
    cfg_load_file (own);

    g_free (shared);
    g_free (own);
}

/* "pad=12" is a setting, "https://x/?a=b" is not: a key is a bare word. */
static gboolean
looks_like_setting (const char *a)
{
    const char *eq = strchr (a, '=');
    if (!eq || eq == a)
        return FALSE;

    for (const char *p = a; p < eq; p++)
        if (!g_ascii_isalnum (*p) && *p != '_' && *p != '-')
            return FALSE;
    return TRUE;
}

/* --------------------------------------------------------------- usage */

/* Where the stored history lives, for --help. Follows --profile, so pass
 * -h last if you want the path for a specific profile. */
static char *
history_path (void)
{
    if (g_private)
        return NULL;

    char *data_dir = NULL, *cache_dir = NULL;
    profile_dirs (g_profile, &data_dir, &cache_dir);

    char *path = g_build_filename (data_dir, HIST_FILE, NULL);
    g_free (data_dir);
    g_free (cache_dir);
    return path;
}

static void
usage (const char *argv0, gboolean to_stdout)
{
    GString    *s    = g_string_new (NULL);
    char       *hpath = history_path ();
    char       *hblock;
    char       *cfg_shared = cfg_path_for ("swov");
    char       *cfg_own    = cfg_path_for (g_app->default_app_id);

    if (hpath)
        hblock = g_strdup_printf (
            "  Every address that came up is kept in\n"
            "    %s\n"
            "  as a tab separated line: timestamp, URL, page title. One line per\n"
"  address: visiting a page again moves it to the end instead of adding\n"
"  a second copy.\n", hpath);
    else
        hblock = g_strdup ("  Off, because --private was given.\n");
    const char *arg   = g_app->usage_arg ? g_app->usage_arg : "URL";

    g_string_append_printf (s, "%s - %s\n\n", argv0, g_app->tagline);
    g_string_append_printf (s, "usage: %s [%s] [options]\n"
"\n"
"  With no address the window comes up on a start page carrying the\n"
"  program's name, version and the three keys worth knowing, on one flat\n"
"  colour. The dot at the top opens a row of swatches - white and black\n"
"  among them - and the one you click becomes the default, remembered in\n"
"  ~/.local/share/wkview/start-bg. The text follows it, light or dark.\n"
"\n"
"  A local file needs three slashes: file:///tmp/index.html. A bare path\n"
"  works too, ./index.html or /tmp/index.html, and anything without a\n"
"  scheme is tried as https.\n"
"\n", argv0, arg);

    g_string_append_printf (s,
"window:\n"
"  --title NAME        pin the window title to NAME (default: follow the\n"
"                      page <title>, falling back to \"%s\")\n"
"  --page-title        keep following the page <title> even with --title,\n"
"                      which then only supplies the fallback\n"
"  --app-id ID         Wayland app_id / X11 WM_CLASS (default: \"%s\")\n"
"                      this is what sway matches in for_window [app_id=...]\n"
"  --zoom N            initial zoom level (default: 1.0)\n"
"\n"
"keys:\n"
"  --mod KEY           modifier for the shortcuts below:\n"
"                      ctrl | alt | super | meta (default: ctrl)\n"
"  --clip-cmd CMD      command used by %s+Y, gets the URL on stdin\n"
"                      (default: \"%s\"; \"internal\" uses the GTK clipboard)\n"
"  --player CMD        video player for media mode, e.g. mpv. Given here\n"
"                      it also starts the window in media mode\n"
"  --image-viewer CMD  image viewer for media mode, e.g. imv. Images are\n"
"                      downloaded first, the viewer gets the file\n"
"  --media             start in media mode (F2 toggles it): <mod>+click\n"
"                      on a video sends it to the player, on an image or\n"
"                      an image link to the viewer. A frame shows the mode\n"
"  --player-match LIST which links are videos, comma separated parts of\n"
"                      the address (default: YouTube videos, shorts and\n"
"                      live, plus .mp4 .webm .mkv .m3u8 ... files)\n"
"\n"
"page:\n"
"  --css FILE          inject FILE as a user stylesheet\n"
"  --devtools          open the inspector once the first page commits\n"
"  --no-media          deny camera / microphone / screen-share requests\n"
"  --no-load-bar       do not draw the loading line at the top\n"
"  --no-devtools       no inspector machinery at all (F12 does nothing).\n"
"                      JavaScriptCore printing \"received NeedDebuggerBreak\n"
"                      trap\" with no inspector open is the reason to try it\n"
"  --no-jit            run JavaScript interpreted (JSC_useJIT=0). The one\n"
"                      to try if the web process traps or crashes\n"
"  --no-sandbox        run the web process unsandboxed. Lets it open the\n"
"                      capture devices directly on a machine with no\n"
"                      desktop portal. Says so on every run, because it\n"
"                      does give up the sandbox\n"
"  --no-gpu            hardware acceleration policy NEVER\n"
"  --no-dmabuf         WEBKIT_DISABLE_DMABUF_RENDERER=1\n"
"  --no-compositing    WEBKIT_DISABLE_COMPOSITING_MODE=1\n"
"  --no-hw-decode      rank the hardware video decoders out\n"
"                      (GST_PLUGIN_FEATURE_RANK), so software decodes\n"
"                      the four to reach for when a machine comes back up\n"
"                      and pages render blank or zero sized\n"
"  --enable-middle-click-paste\n"
"                      let middle-button clicks reach the page. They are\n"
"                      swallowed by default, so a stray click cannot paste\n"
"                      the primary selection into a form\n"
"\n"
"  A file whose name is already taken is downloaded beside the old one and\n"
"  the question is asked in the download panel itself: Enter puts it in\n"
"  the old one's place, Esc keeps both. The transfer only reads as done\n"
"  once the file is where it is going to stay.\n"
"\n"
"  A download shows a progress bar in the top right corner with the file\n"
"  name, percentage and estimated time left. Move the pointer over it and\n"
"  it fades out so you can read the page underneath, though one that has\n"
"  just appeared stays visible anyway. It shows up as soon as the server\n"
"  answers, reading \"starting\" until the first byte lands, and it\n"
"  disappears on its own %d seconds after the transfer ends.\n"
"\n",
        g_app->default_title, g_app->default_app_id, g_mod_name,
        DEFAULT_CLIP_CMD, DL_LINGER_SECONDS);

    if (g_app->usage_options)
        g_app->usage_options (s);

    g_string_append_printf (s,
"session:\n"
"  --download-dir DIR  download target (default: XDG download dir)\n"
"                      single pages and whole sites can be sent elsewhere\n"
"                      with <mod>+S; those rules live next to the profile\n"
"                      in ~/.local/share/wkview/download-dirs.tsv, which\n"
"                      belongs to you rather than to a profile: every\n"
"                      profile and both browsers share it. Search keywords\n"
"                      are per profile, in searches.tsv. A directory that\n"
"                      does not exist is created group writable (0770)\n"
"  --profile NAME      named profile, persists cookies (default: \"default\")\n"
"  --clear-data        wipe the profile's data and cache before starting\n"
"  --private           ephemeral session, no stored history, ignores\n"
"                      --profile/--clear-data\n"
"  --paths             print every directory and file either browser\n"
"                      writes - settings, cookies, history, cache - with\n"
"                      what is on disk now, then exit. Follows --profile\n"
"                      and --private wherever they sit on the line\n"
"  --user-agent UA     set an explicit user agent string\n"
"  --ua-chrome | --ua-win-chrome | --ua-win-edge | --ua-firefox | --ua-safari\n"
"\n"
"history:\n"
"%s"
"  Only successful loads of http, https and file are recorded, so every\n"
"  line is a link that worked. Failed loads, error pages and internal\n"
"  pages are left out. The last %d lines are kept.\n"
"\n"
"config:\n"
"  -c, --config PATH   read this file instead of the default chain\n"
"  -n, --no-config     ignore the config files\n"
"  -s, --set KEY=VAL   set one key, as does --KEY=VAL or plain KEY=VAL\n"
"\n"
"  Read in this order, later wins, then the command line on top:\n"
"    %s\n"
"    %s\n"
"  Same format and the same key names as swov, so the palette can live in\n"
"  swov's file and drive both. That file is full of keys for swov, so they\n"
"  are counted and ignored quietly; an unknown key in our own file, or on\n"
"  the command line, is named, because there it means a typo.\n"
"\n"
"  look:    popup_width (how wide the panels want to be, %d)\n"
"           list_rows (lines a popup list shows, %d)\n"
"           bg tile tile_sel tile_hover card card_hover text subtext dim\n"
"           accent hl hltext hint urgent outline find_hl\n"
"           start_bg (start page background; the palette on that page\n"
"           overrides it once you click a swatch)\n"
"           radius border pad gap win_gap margin ui_scale\n"
"           font font_mono label_px title_px hint_px\n"
"  ours:    title app_id zoom mod clip_cmd download_dir profile private\n"
"           no_media page_title middle_click_paste select_all load_bar\n"
"           css\n"
"           user_agent quiet\n"
"           always_overwrite  search_default (which keyword needs no prefix)\n"
"           gsk (GTK's renderer: gl, cairo, vulkan, ngl)\n"
"           feature = Name[=on|off] (a WebKit runtime feature; repeatable)\n"
"           no_dmabuf no_compositing no_gpu no_hw_decode (the same as the\n"
"           --no-* flags, as settled preferences)\n"
"           search_KEY (e.g. search_s = https://google.com/search?q={})\n"
"\n"
"misc:\n"
"  -q, --quiet         silence the diagnostic output on stderr\n"
"  --no-shared-array-buffer\n"
"                      disable SharedArrayBuffer. It is on by default, as\n"
"                      in other browsers, and WebKit still hands it only to\n"
"                      pages that are cross-origin isolated\n"
"  --jsc NAME=VALUE    set any JavaScriptCore option, e.g. useJIT=0.\n"
"                      Repeatable\n"
"  --feature NAME[=on|off]\n"
"                      turn a WebKit runtime feature on or off. Several\n"
"                      real sites need AllowWebGLInWorkers, which is off\n"
"                      by default: without it a library that renders in a\n"
"                      worker cannot get a WebGL context and falls back to\n"
"                      the CPU. Repeatable\n"
"  --gl-info           check the graphics stack the way this browser uses\n"
"                      it - device nodes and whether you may open them,\n"
"                      the GL context GTK gets, the renderer - then exit\n"
"  --list-features [F] print the runtime features, optionally filtered,\n"
"                      then exit\n"
"  --gsk RENDERER      GTK's own renderer: gl (the default), cairo,\n"
"                      vulkan, ngl, or auto to let GTK choose. GTK's own\n"
"                      choice is Vulkan where the driver claims it, which\n"
"                      has deadlocked on resize; GL gives up nothing here\n"
"  --no-proc-watch     do not watch the web process. By default it is\n"
"                      sampled every 5s and, when it spins at 90%%+ cpu for\n"
"                      15s, its running and blocked threads are printed -\n"
"                      the thing to read when a page freezes\n"
"  --no-console        do not print the page's own console output, while\n"
"                      keeping ours. A page that logs heavily makes the\n"
"                      web process wait on every message\n"
"  --all-messages      do not collapse repeated messages. By default a\n"
"                      (collapsing is skipped anyway when GST_DEBUG or\n"
"                      WEBKIT_DEBUG is set)\n"
"                      line that keeps coming back - WebKit reporting the\n"
"                      desktop portal unreachable, say - is shown twice\n"
"                      and then counted, with the totals printed at exit\n"
"  -h, --help          this text\n"
"  -V, --version       version and build id, to tell two builds apart\n"
"\n"
"key bindings (<mod> = %s):\n"
"  <mod>+O  or <mod>+L type an address. The history is listed straight\n"
"                      away and narrows as you type, so the two are one\n"
"                      thing: pick a line, or press Enter and what you\n"
"                      typed is loaded\n"
"                      A search keyword in front runs a search instead:\n"
"                      \"s tree\" with s = https://google.com/search?q={}\n"
"  <mod>+H             the same popup, opened on the history\n"
"  <mod>+F             find in the page. Enter, Down or <mod>+N for the\n"
"                      next hit, Shift+Enter, Up or <mod>+Shift+N for the\n"
"                      previous, Esc to stop. The hit you are on is shown\n"
"                      in find_hl, a pale blue by default\n"
"  <mod>+S             download directory. The select decides how far it\n"
"                      reaches: this address only, everything on the site,\n"
"                      or everything. The popup lists what is already\n"
"                      stored: arrows or the mouse pick one, Delete or\n"
"                      <mod>+X drops the rule under the cursor.\n"
"                      The narrower rule wins, and a rule\n"
"                      set on a page also covers the files it hands out.\n"
"                      If something else already points at that directory\n"
"                      you are asked: Enter drops the older rule, Esc lets\n"
"                      both use it. Tick \"always replace existing files\"\n"
"                      and files covered by that rule take the name they\n"
"                      ask for, with no question. \"everything\" lasts for\n"
"                      this run only; put download_dir in a config file to\n"
"                      keep it\n"
"  <mod>+K             search keywords. The popup lists the ones it knows,\n"
"                      marking which is the default. Add one as \"g URL\"\n"
"                      with {} where the words go, tick the box to make it\n"
"                      the default, drop the one under the cursor with\n"
"                      Delete or <mod>+X. Then \"g tree\" searches with g,\n"
"                      and a bare \"tree\" with the default one.\n"
"                      Built in: g Google (the default), d DuckDuckGo,\n"
"                      w Wikipedia, s SDL3 wiki. One of your own with the\n"
"                      same letter replaces it\n"
"  F1  or  <mod>+/     the key list, on screen\n"
"  F2                  media mode on/off: <mod>+click sends videos to\n"
"                      player and images to image_viewer\n"
"  <mod>+J             back: one page back, then on into the stored history\n"
"  <mod>+Shift+J       forward, the same way round\n"
"\n"
"  Anything the popup can save shows a Save and a Cancel button with the\n"
"  keys named on them, so Enter is never the only way in.\n"
"\n"
"  In the popup: Tab and Down walk forward through the matches, Shift+Tab\n"
"  and Up back, the wheel scrolls, a click opens, Delete or <mod>+X removes\n"
"  the entry. Whatever is selected is written into the input, so\n"
"  Enter always loads what you can read. Esc, <mod>+H or a click outside\n"
"  closes it.\n"
"\n"
"  <mod>+G             scroll to the top of the page\n"
"  <mod>+Shift+G       scroll to the bottom\n"
"  <mod>+R  or F5      reload the page\n"
"  <mod>+Shift+R       re-read the --css file, then reload\n"
"  <mod>+D             list the recent downloads for %d seconds\n"
"  F12, <mod>+Shift+D  toggle the developer tools\n"
"  <mod>+Shift+I       toggle the developer tools\n"
"  <mod>+P             show the whole current URL, top right: wrapped,\n"
"                      never cut short, up longer the longer it is (from\n"
"                      %d seconds). Press it again to put it away\n"
"  <mod>+plus          zoom in\n"
"  <mod>+minus         zoom out\n"
"  <mod>+0             reset zoom to 100%%\n"
"  <mod>+Y             copy the current URL, and show what was copied\n",
        hblock, HIST_KEEP, cfg_shared, cfg_own, g_theme.popup_w, g_theme.list_rows,
        g_mod_name, DL_HISTORY_SECONDS, URL_TOAST_SECONDS);

    if (g_app->usage_keys)
        g_app->usage_keys (s);

    g_string_append_printf (s,
"\n"
"  Esc closes whatever is on screen - popup, key list, download list or a\n"
"  message - and reaches the page only when nothing of ours is up.\n"
"\n"
"  Only the combinations listed above are intercepted; everything else\n"
"  (<mod>+C, <mod>+V, ...) goes straight to the page, so copy and paste\n"
"  keep working inside input fields. <mod>+A is turned into WebKit's\n"
"  select-all, because WebKit itself reads it as move-to-start-of-line;\n"
"  set select_all=no if a page needs the key for itself. While the URL bar is\n"
"  open every key except Esc belongs to it, so <mod>+A selects its text.\n"
"%s",
        g_mod == GDK_CONTROL_MASK
            ? "  Use --mod alt if you would rather keep Ctrl+P for the page's"
              " print dialog.\n"
            : "");

    if (to_stdout)
        g_print ("%s", s->str);
    else
        g_printerr ("%s", s->str);

    g_free (cfg_shared);
    g_free (cfg_own);
    g_free (hblock);
    g_free (hpath);
    g_string_free (s, TRUE);
}

static void
usage_short (const char *argv0)
{
    g_printerr ("usage: %s [%s] [options]   (try %s -h)\n",
                argv0, g_app->usage_arg ? g_app->usage_arg : "URL", argv0);
}

/* ----------------------------------------------------------------- CSS */

/* User stylesheet injected into the pages (--css). One content manager
 * per view, so the sheet has to be applied to each of them. */
/*
 * WebKit shows the match it is sitting on as the document selection, so
 * colouring ::selection is what colours the current hit. The sheet only
 * goes in while <mod>+F is open, so ordinary selections keep the page's
 * own colours the rest of the time.
 */
static void
css_apply_find (WebKitUserContentManager *ucm)
{
    if (!g_find_css_on)
        return;

    char *bg  = css_rgba (g_theme.find_hl);
    char *fg  = css_rgba (g_theme.hltext);
    char *css = g_strdup_printf ("::selection{background-color:%s;color:%s}"
                                 "::-moz-selection{background-color:%s;color:%s}",
                                 bg, fg, bg, fg);

    WebKitUserStyleSheet *ss =
        webkit_user_style_sheet_new (css,
                                     WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                     WEBKIT_USER_STYLE_LEVEL_USER,
                                     NULL, NULL);
    webkit_user_content_manager_add_style_sheet (ucm, ss);
    webkit_user_style_sheet_unref (ss);

    g_free (css);
    g_free (fg);
    g_free (bg);
}

static void
css_apply_one (WebKitUserContentManager *ucm)
{
    webkit_user_content_manager_remove_all_style_sheets (ucm);
    css_apply_find (ucm);

    if (!g_css_data)
        return;

    WebKitUserStyleSheet *ss =
        webkit_user_style_sheet_new (g_css_data,
                                     WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                     WEBKIT_USER_STYLE_LEVEL_USER,
                                     NULL, NULL);
    webkit_user_content_manager_add_style_sheet (ucm, ss);
    webkit_user_style_sheet_unref (ss);
}

static void
css_apply_all (void)
{
    for (guint i = 0; i < g_ucms->len; i++)
        css_apply_one (g_ptr_array_index (g_ucms, i));
}

void
css_reload (void)
{
    GError *err = NULL;

    g_clear_pointer (&g_css_data, g_free);

    if (g_css_path) {
        if (!g_file_get_contents (g_css_path, &g_css_data, NULL, &err)) {
            g_printerr ("css: %s\n", err->message);
            g_error_free (err);
        } else {
            LOG ("css: injected %s\n", g_css_path);
        }
    }

    css_apply_all ();
}

/*
 * Styling for our own widgets, not for the page. Every value comes from
 * the theme, which is swov's palette and geometry by default, so the two
 * programs look like one set. See the config section.
 */
static void
ui_css_install (void)
{
    Theme *t = &g_theme;
    double s = t->ui_scale;

    char *c_tile    = css_rgba (t->tile);
    char *c_sel     = css_rgba (t->tile_sel);
    char *c_hover   = css_rgba (t->tile_hover);
    char *c_card    = css_rgba (t->card);
    char *c_mini    = css_rgba (t->mini_bg);
    char *c_current = css_rgba (t->current);
    char *c_text    = css_rgba (t->text);
    char *c_subtext = css_rgba (t->subtext);
    char *c_dim     = css_rgba (t->dim);
    char *c_hl      = css_rgba (t->hl);
    char *c_hint    = css_rgba (t->hint);
    char *c_hltext  = css_rgba (t->hltext);
    char *c_hl_soft = css_rgba_at (t->hl, 0.34);   /* selections */
    char *c_hl_dim  = css_rgba_at (t->hl, 0.78);   /* a button at rest */
    char *c_urgent  = css_rgba (t->urgent);
    char *c_outline = css_rgba (t->outline);

    /* an empty font key means "whatever GTK is themed with" */
    char *ui_font = *t->font
                  ? g_strdup_printf ("font-family: \"%s\";", t->font)
                  : g_strdup ("");
    char *mono_font = g_strdup_printf ("font-family: \"%s\";", t->font_mono);

    GString *css = g_string_new (NULL);

    /* ---- surfaces: one panel look for every overlay ---- */
    /* A panel pinned to an edge should not curve away from it: only the
     * corners that face into the page are rounded, and the border on the
     * touching side is dropped so the panel sits flush. */
    g_string_append_printf (css,
        "box.br-dl, box.br-omni, box.br-keys, box.br-perm, label.br-toast, label.br-urltoast {"
        "  background-color: %s;"
        "  border: %.0fpx solid %s;"
        "  padding: %.0fpx;"
        "}"
        /* top right corner: only the bottom left is free */
        "box.br-dl, label.br-toast {"
        "  border-radius: 0 0 0 %.0fpx;"
        "  border-right-width: 0;"
        "}"
        /* centered on the top edge: both bottom corners are free */
        "box.br-omni, box.br-perm {"
        "  border-radius: 0 0 %.0fpx %.0fpx;"
        "  border-top-width: 0;"
        "}"
        /* clear of every edge: all four corners are rounded */
        "box.br-keys { border-radius: %.0fpx; }"
        /* media mode: the accent round the page, nothing inside */
        "box.br-media-frame { border: %.0fpx solid %s; background: none; }"
        /* bottom left corner: only the top right is free */
        "label.br-urltoast {"
        "  border-radius: 0 %.0fpx 0 0;"
        "  border-left-width: 0; border-bottom-width: 0;"
        "}",
        c_tile, t->border, c_outline, t->pad,
        t->radius, t->radius, t->radius, t->radius,
        MAX (t->border, 2.0), c_hl,
        t->radius);

    /* ---- primary text ---- */
    g_string_append_printf (css,
        "label.br-toast, label.br-urltoast, label.br-dl-done, label.br-pick-main,"
        "progressbar.br-dl-bar > text {"
        "  color: %s; %s font-size: %.0fpx;"
        "}",
        c_text, ui_font, t->label_px * s);

    /* an error toast: the same panel, the failure colour, and wrapped */
    g_string_append_printf (css,
        "label.br-toast.br-toast-error { color: %s; }", c_urgent);

    /* URLs and file names: fixed width, so they line up and elide sanely */
    g_string_append_printf (css,
        "entry.br-omni-entry, entry.br-omni-entry > text,"
        "label.br-urltoast, label.br-toast-url, label.br-dl-done, label.br-dl-failed,"
        "progressbar.br-dl-bar > text {"
        "  %s font-size: %.0fpx;"
        "}",
        mono_font, t->label_px * s);

    /* ---- secondary text ---- */
    g_string_append_printf (css,
        "label.br-dl-head { color: %s; %s font-size: %.0fpx; }"
        "label.br-pick-sub { color: %s; %s font-size: %.0fpx; }"
        "label.br-dl-name { color: %s; %s font-size: %.0fpx; }"
        "label.br-dl-status { color: %s; %s font-size: %.0fpx; }"
        "label.br-dl-live { color: %s; %s font-size: %.0fpx; }"
        "label.br-dl-ask { color: %s; %s font-size: %.0fpx; }"
        "label.br-dl-failed { color: %s; %s font-size: %.0fpx; }"
        "label.br-dl-dir { color: %s; %s font-size: %.0fpx; }"
        "label.br-key { color: %s; %s font-size: %.0fpx; }"
        "label.br-key-desc { color: %s; %s font-size: %.0fpx; }"
        "label.br-key-head { color: %s; %s font-size: %.0fpx; }"
        "checkbutton.br-check, checkbutton.br-check label {"
        "  color: %s; %s font-size: %.0fpx;"
        "}"
        "button.br-btn {"
        "  background: none; background-color: transparent;"
        "  background-image: none; box-shadow: none;"
        "  border: none; border-radius: %.0fpx; outline: none;"
        "  color: %s; %s font-size: %.0fpx;"
        "  padding: %.0fpx %.0fpx; min-height: 0;"
        "}"
        "button.br-btn:hover {"
        "  color: %s; text-decoration-line: underline;"
        "}"
        "button.br-btn-primary { color: %s; font-weight: bold; }"
        "button.br-btn-primary:hover {"
        "  color: %s; text-decoration-line: underline;"
        "}",
        c_hint, ui_font, t->hint_px * s,
        c_subtext, mono_font, t->title_px * s,
        c_text, mono_font, t->label_px * s,
        c_subtext, mono_font, t->title_px * s,
        c_current, mono_font, t->title_px * s,
        c_hl, mono_font, t->title_px * s,
        c_urgent, mono_font, t->title_px * s,
        c_dim, mono_font, t->title_px * s,
        c_hl, mono_font, t->title_px * s,
        c_text, ui_font, t->title_px * s,
        c_hint, ui_font, t->hint_px * s,
        c_subtext, ui_font, t->title_px * s,
        t->radius / 3.0, c_subtext, ui_font, t->title_px * s,
        t->pad / 2.0, t->pad / 2.0,
        c_text,
        c_hl_dim,
        c_hl);

    /* ---- entries: the URL bar and the picker search share a look ---- */
    /* The input reads as a field: its own well, a rule under it, and the
     * accent on that rule while it has the keyboard. */
    g_string_append_printf (css,
        "entry.br-omni-entry > text {"
        "  background-color: transparent; background-image: none;"
        "  color: %s; caret-color: %s;"
        "  border: none; box-shadow: none; outline: none; margin: 0;"
        "}"
        "entry.br-omni-entry {"
        "  background-color: %s; background-image: none;"
        "  color: %s; caret-color: %s;"
        "  border: none; border-bottom: 2px solid %s;"
        "  border-radius: %.0fpx %.0fpx 0 0;"
        "  box-shadow: none; outline: none;"
        "  padding: %.0fpx %.0fpx; margin: 0;"
        "}"
        "entry.br-omni-entry:focus-within { border-bottom-color: %s; }"
        /* tile_sel vanished against the panel and solid hl shouted; the
         * accent at a third of its weight reads as a wash over the text */
        "entry.br-omni-entry > text > selection {"
        "  background-color: %s; color: %s;"
        "}",
        c_text, c_hl,
        c_card, c_text, c_hl, c_dim,
        t->radius / 3.0, t->radius / 3.0,
        t->pad / 2.0, t->pad / 2.0,
        c_hl,
        c_hl_soft, c_text);

    /* ---- download rows: the bar picks up the accent ---- */
    g_string_append_printf (css,
        "progressbar.br-dl-bar > trough {"
        "  background-color: %s; border: none;"
        "  border-radius: %.0fpx; min-height: %.0fpx;"
        "}"
        "progressbar.br-dl-bar > trough > progress {"
        "  background-color: %s; border: none;"
        "  border-radius: %.0fpx; min-height: %.0fpx;"
        "}",
        c_mini, t->radius / 3.0, 6.0 * s,
        c_hl,   t->radius / 3.0, 6.0 * s);

    /* ---- the loading hairline, across the very top ---- */
    g_string_append_printf (css,
        "/* the loading hairline is drawn, not styled */");

    /* ---- picker rows ---- */
    g_string_append_printf (css,
        "scrolledwindow.br-pick-scroll,"
        "list.br-pick-list, list.br-pick-list > row {"
        "  background-color: transparent; border: none;"
        "}"
        "list.br-pick-list > row {"
        "  border-radius: %.0fpx; padding: %.0fpx %.0fpx;"
        "}"
        "list.br-pick-list > row:hover { background-color: %s; }"
        /* the selected row carries the accent at the same weight, so the
         * two kinds of selection look like one idea */
        "list.br-pick-list > row:selected { background-color: %s; }"
        "list.br-pick-list > row:selected label.br-pick-main { color: %s; }"
        "list.br-pick-list > row:selected label.br-pick-sub { color: %s; }",
        t->radius / 2.0, t->win_gap / 2.0, t->pad / 2.0,
        c_hover, c_hl_soft, c_text, c_subtext);

    GtkCssProvider *p = gtk_css_provider_new ();
#if GTK_CHECK_VERSION (4, 12, 0)
    gtk_css_provider_load_from_string (p, css->str);
#else
    gtk_css_provider_load_from_data (p, css->str, -1);
#endif
    gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                GTK_STYLE_PROVIDER (p),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (p);

    g_string_free (css, TRUE);
    g_free (ui_font);   g_free (mono_font);
    g_free (c_tile);    g_free (c_sel);     g_free (c_hover);
    g_free (c_card);    g_free (c_text);    g_free (c_subtext);
    g_free (c_dim);     g_free (c_hl);      g_free (c_hint);
    g_free (c_urgent);  g_free (c_outline);  g_free (c_hltext);
    g_free (c_hl_soft); g_free (c_hl_dim);
    g_free (c_mini);    g_free (c_current);
}

/* ----------------------------------------------------------- clipboard */

static void
clip_reap (GPid pid, int status, gpointer u)
{
    (void) status; (void) u;
    g_spawn_close_pid (pid);
}

static gboolean
clip_spawn (const char *cmd, const char *text)
{
    char  **argv = NULL;
    GError *err  = NULL;

    if (!g_shell_parse_argv (cmd, NULL, &argv, &err)) {
        g_printerr ("clip: bad --clip-cmd %s: %s\n", cmd, err->message);
        g_error_free (err);
        return FALSE;
    }

    GPid pid;
    int  in_fd = -1;
    gboolean ok = g_spawn_async_with_pipes (NULL, argv, NULL,
                                            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                            NULL, NULL, &pid, &in_fd, NULL, NULL, &err);
    g_strfreev (argv);

    if (!ok) {
        g_printerr ("clip: cannot run %s: %s\n", cmd, err->message);
        g_error_free (err);
        return FALSE;
    }

    size_t len = strlen (text), off = 0;
    while (off < len) {
        ssize_t n = write (in_fd, text + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_printerr ("clip: write failed: %s\n", g_strerror (errno));
            break;
        }
        off += (size_t) n;
    }
    close (in_fd);
    g_child_watch_add (pid, clip_reap, NULL);
    return TRUE;
}

static void
clipboard_copy (Win *w, const char *text)
{
    if (!text || !*text)
        return;

    const char *cmd = g_clip_cmd ? g_clip_cmd : DEFAULT_CLIP_CMD;

    gboolean ok;

    if (!g_ascii_strcasecmp (cmd, "internal") || !g_ascii_strcasecmp (cmd, "gtk")) {
        gdk_clipboard_set_text (gtk_widget_get_clipboard (w->win), text);
        LOG ("clip: copied via GTK clipboard\n");
        ok = TRUE;
    } else {
        ok = clip_spawn (cmd, text);
        if (ok)
            LOG ("clip: copied via %s\n", cmd);
    }

    /* show what landed on the clipboard, not just that something did */
    char *msg = g_strdup_printf (ok ? "copied  %s" : "copy failed  %s", text);
    toast_show (w, msg, URL_TOAST_SECONDS);
    g_free (msg);
}

/* -------------------------------------------------------------- download */

static Dl *
dl_get (WebKitDownload *download)
{
    return g_object_get_data (G_OBJECT (download), "dl");
}

static void
dl_free (gpointer p)
{
    Dl *d = p;
    g_free (d->name);
    g_free (d->dest);
    g_free (d->want);
    g_free (d);
}

static Dl *
dl_new (WebKitDownload *download, const char *name)
{
    Dl *d = g_new0 (Dl, 1);

    d->name     = g_strdup (name ? name : "download");
    d->state    = DL_ACTIVE;
    d->start_us = g_get_monotonic_time ();
    d->rate_us  = d->start_us;

    g_ptr_array_add (g_downloads, d);
    while (g_downloads->len > DL_KEEP)
        g_ptr_array_remove_index (g_downloads, 0);

    g_object_set_data (G_OBJECT (download), "dl", d);
    downloads_tick_start ();
    downloads_refresh ();       /* on screen now, not at the first tick */
    downloads_reveal ();        /* ... and not hidden under the pointer */
    return d;
}

/* The Dl may have been pushed out of the history by newer downloads. */
static gboolean
dl_alive (Dl *d)
{
    if (!d)
        return FALSE;
    for (guint i = 0; i < g_downloads->len; i++)
        if (g_ptr_array_index (g_downloads, i) == d)
            return TRUE;
    return FALSE;
}

static void
dl_set_name (WebKitDownload *download, const char *name)
{
    Dl *d = dl_get (download);
    if (!dl_alive (d) || !name)
        return;
    g_free (d->name);
    d->name = g_strdup (name);
    downloads_refresh ();
}

static void
dl_finish (WebKitDownload *download, DlState state)
{
    Dl *d = dl_get (download);
    if (!dl_alive (d) || d->state != DL_ACTIVE)
        return;

    d->state  = state;
    d->end_us = g_get_monotonic_time ();

    /* Not finished until the file is where it is going to stay. */
    if (state == DL_DONE && d->ask) {
        d->state = DL_ASK;
        downloads_tick_start ();
        downloads_refresh ();
        return;
    }

    if (state == DL_DONE && d->overwrite && d->want && d->dest) {
        if (g_rename (d->dest, d->want) == 0) {
            LOG ("download: replaced %s\n", d->want);
            g_free (d->name);
            d->name = g_path_get_basename (d->want);
        } else {
            g_printerr ("download: cannot replace %s: %s\n",
                        d->want, g_strerror (errno));
        }
    }

    if (state == DL_FAILED && d->dest) {
        GStatBuf st;
        if (g_stat (d->dest, &st) == 0 && st.st_size == 0)
            g_remove (d->dest);        /* drop the empty name claim */
    }
    if (state == DL_DONE && d->total == 0)
        d->total = d->received;

    downloads_tick_start ();      /* keeps ticking for the linger period */
    downloads_refresh ();
}

/*
 * Reserve the name by creating the file rather than by looking and
 * hoping. Two browsers saving the same file into the same directory would
 * otherwise both see the name free, both take it, and one would land on
 * top of the other. O_EXCL makes the claim atomic, so the loser moves on
 * to the next number. The empty file left behind is the one WebKit then
 * writes into, which is why the caller has to allow overwriting it.
 */
static char *
unique_download_path (const char *dir, const char *suggested)
{
    const char *dot = strrchr (suggested, '.');
    char       *stem;
    const char *ext = "";

    if (dot && dot != suggested) {
        stem = g_strndup (suggested, (gsize) (dot - suggested));
        ext  = dot;                       /* includes the '.' */
    } else {
        stem = g_strdup (suggested);
    }

    char *path = NULL;

    for (int i = 0; i < 10000; i++) {
        char *name = i == 0 ? g_strdup (suggested)
                            : g_strdup_printf ("%s (%d)%s", stem, i, ext);
        g_free (path);
        path = g_build_filename (dir, name, NULL);
        g_free (name);

        int fd = g_open (path, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0) {
            close (fd);
            break;
        }
        if (errno != EEXIST) {
            g_printerr ("download: cannot create %s: %s\n", path, g_strerror (errno));
            break;
        }
    }

    g_free (stem);
    return path;
}

static gboolean
on_decide_destination (WebKitDownload *download,
                       const gchar    *suggested_filename,
                       gpointer        u)
{
    (void) u;

    const char *dir = dl_dir_for_download (download);

    char *absdir;
    if (g_path_is_absolute (dir)) {
        absdir = g_strdup (dir);
    } else {
        char *cwd = g_get_current_dir ();
        absdir = g_build_filename (cwd, dir, NULL);
        g_free (cwd);
    }

    if (!dl_dir_make (absdir)) {
        g_free (absdir);
        webkit_download_cancel (download);
        return TRUE;
    }

    if (!suggested_filename || !*suggested_filename)
        suggested_filename = "download";

    /* WebKitGTK 2.52.x wants an absolute filesystem path here. */
    char *wanted = g_build_filename (absdir, suggested_filename, NULL);

    /* "always replace" turns the question off for whatever this rule
     * covers: the file simply takes the name it asked for. */
    gboolean always = dl_always_for_download (download);
    char *path = always ? g_strdup (wanted)
                        : unique_download_path (absdir, suggested_filename);
    gboolean clash = !always && g_strcmp0 (wanted, path) != 0;

    /* Before the destination, not after: setting the destination is what
     * opens the file, and it refuses an existing one unless told. The file
     * exists because we just created it to claim the name. */
    webkit_download_set_allow_overwrite (download, TRUE);
    webkit_download_set_destination (download, path);

    char *base = g_path_get_basename (path);
    dl_set_name (download, base);
    g_free (base);

    LOG ("download: %s -> %s\n", suggested_filename, path);

    /* Nothing is overwritten behind your back: the file lands beside the
     * old one and <mod>+Enter puts it in its place if you say so. */
    Dl *d = dl_get (download);
    if (dl_alive (d)) {
        g_free (d->dest);
        d->dest = g_strdup (path);
        g_free (d->want);
        d->want = clash ? g_strdup (wanted) : NULL;
        d->ask  = clash;
        if (clash)
            downloads_refresh ();      /* the question goes in the panel */
    }


    g_free (wanted);
    g_free (path);
    g_free (absdir);
    return TRUE;                          /* handled */
}

static void
on_download_failed (WebKitDownload *download, GError *err, gpointer u)
{
    (void) u;
    const char *dest = webkit_download_get_destination (download);
    g_printerr ("download: FAILED dest=%s err=%s\n",
                dest ? dest : "(none)", err ? err->message : "(unknown)");
    dl_finish (download, DL_FAILED);
}

static void
on_download_finished (WebKitDownload *download, gpointer u)
{
    (void) u;
    const char *dest = webkit_download_get_destination (download);
    LOG ("download: FINISHED dest=%s\n", dest ? dest : "(none)");
    dl_finish (download, DL_DONE);   /* no-op if "failed" already fired */
}

/* Feeds the overlay on every chunk; logs at most once per 10%. */
static void
on_download_received_data (WebKitDownload *download, guint64 data_length, gpointer u)
{
    (void) data_length; (void) u;

    Dl *d = dl_get (download);
    if (dl_alive (d)) {
        d->progress = webkit_download_get_estimated_progress (download);
        d->received = webkit_download_get_received_data_length (download);

        if (d->total == 0) {
            WebKitURIResponse *r = webkit_download_get_response (download);
            if (r)
                d->total = webkit_uri_response_get_content_length (r);
        }

        /* resample at most twice a second, then smooth */
        gint64 now = g_get_monotonic_time ();
        double dt  = (now - d->rate_us) / 1e6;

        if (dt >= 0.5) {
            double inst = (d->received - d->rate_bytes) / dt;
            d->rate       = d->rate > 0 ? 0.7 * d->rate + 0.3 * inst : inst;
            d->rate_us    = now;
            d->rate_bytes = d->received;
        }
    }

    if (g_quiet)
        return;

    int decile = (int) (webkit_download_get_estimated_progress (download) * 10.0);
    int last   = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (download), "decile"));
    if (decile <= last)
        return;

    g_object_set_data (G_OBJECT (download), "decile", GINT_TO_POINTER (decile));
    g_printerr ("download: progress %d%%\n", decile * 10);
}

static void
download_wire (WebKitDownload *download)
{
    WebKitURIRequest *req  = webkit_download_get_request (download);
    char             *name = NULL;

    /* provisional name until decide-destination knows the real one */
    if (req) {
        char *path = g_filename_display_basename (webkit_uri_request_get_uri (req));
        if (path && *path && !g_str_has_suffix (path, "/"))
            name = path;
        else
            g_free (path);
    }

    dl_new (download, name);
    g_free (name);

    g_signal_connect (download, "decide-destination", G_CALLBACK (on_decide_destination), NULL);
    g_signal_connect (download, "received-data",      G_CALLBACK (on_download_received_data), NULL);
    g_signal_connect (download, "finished",           G_CALLBACK (on_download_finished), NULL);
    g_signal_connect (download, "failed",             G_CALLBACK (on_download_failed), NULL);
}

static void
on_session_download_started (WebKitNetworkSession *session,
                             WebKitDownload       *download,
                             gpointer              u)
{
    (void) session; (void) u;
    WebKitURIRequest *req = webkit_download_get_request (download);
    LOG ("download: session started uri=%s\n",
         req ? webkit_uri_request_get_uri (req) : "(unknown)");
    download_wire (download);
}

static gboolean player_try (WebKitWebView *view, WebKitNavigationAction *act);
static void     media_mode_toggle (Win *w);

static gboolean
on_decide_policy (WebKitWebView            *view,
                  WebKitPolicyDecision     *decision,
                  WebKitPolicyDecisionType  type,
                  gpointer                  u)
{
    (void) u;

    /* A <mod>+click reaches us as a new-window action (that is what the
     * modifier means to WebKit, and why a site's own click handler lets
     * it through), or as a plain navigation on some pages. Both carry
     * the modifiers, so both are checked. */
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION ||
        type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitNavigationAction *act = webkit_navigation_policy_decision_get_navigation_action (
            WEBKIT_NAVIGATION_POLICY_DECISION (decision));
        if (act && player_try (view, act)) {
            webkit_policy_decision_ignore (decision);
            return TRUE;
        }
        return FALSE;
    }

    if (type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
        return FALSE;

    WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION (decision);
    gboolean supported = webkit_response_policy_decision_is_mime_type_supported (r);

    WebKitURIResponse *resp = webkit_response_policy_decision_get_response (r);
    const char *mime = resp ? webkit_uri_response_get_mime_type (resp) : NULL;

    gboolean attachment = FALSE;
    if (resp) {
        SoupMessageHeaders *hdrs = webkit_uri_response_get_http_headers (resp);
        if (hdrs) {
            const char *cd = soup_message_headers_get_one (hdrs, "Content-Disposition");
            if (cd && g_strrstr (cd, "attachment"))
                attachment = TRUE;
        }
    }

    if (!supported || attachment) {
        LOG ("download: policy -> download mime=%s supported=%d attachment=%d\n",
             mime ? mime : "(unknown)", supported, attachment);
        webkit_policy_decision_download (decision);
        return TRUE;                      /* handled */
    }

    return FALSE;                         /* let WebKit continue normally */
}

/* ----------------------------------------------------------- permissions */

static const char *
perm_type_name (WebKitPermissionRequest *req)
{
    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST (req))          return "user-media";
    if (WEBKIT_IS_POINTER_LOCK_PERMISSION_REQUEST (req))        return "pointer-lock";
    if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST (req))         return "geolocation";
    if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST (req))        return "notification";
    if (WEBKIT_IS_CLIPBOARD_PERMISSION_REQUEST (req))           return "clipboard";
    if (WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST (req))    return "media-key-system";
    if (WEBKIT_IS_WEBSITE_DATA_ACCESS_PERMISSION_REQUEST (req)) return "website-data-access";
    return "other";
}

static Win *
win_for_view (WebKitWebView *view)
{
    for (guint i = 0; g_wins && i < g_wins->len; i++) {
        Win *w = g_ptr_array_index (g_wins, i);
        if (w->view == view)
            return w;
    }
    return NULL;
}

/* ----------------------------------------------------------- media mode */

static gboolean
uri_has_ext (const char *uri, const char *const *exts)
{
    GUri *u = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);
    if (!u)
        return FALSE;

    char    *path = g_ascii_strdown (g_uri_get_path (u), -1);
    gboolean hit  = FALSE;
    for (int i = 0; exts[i] && !hit; i++)
        hit = g_str_has_suffix (path, exts[i]);

    g_free (path);
    g_uri_unref (u);
    return hit;
}

static gboolean
is_video_uri (const char *uri)
{
    const char *const *pats = g_player_match
                            ? (const char *const *) g_player_match
                            : PLAYER_MATCH_DEFAULT;

    for (int i = 0; pats[i]; i++) {
        if (!*pats[i])
            continue;
        if (!strcmp (pats[i], "*") || strstr (uri, pats[i]))
            return TRUE;
    }
    return uri_has_ext (uri, VIDEO_EXT);
}

static gboolean
is_image_uri (const char *uri)
{
    return uri_has_ext (uri, IMAGE_EXT);
}

static gboolean
is_web_or_file (const char *uri)
{
    return uri && (g_str_has_prefix (uri, "http://") || g_str_has_prefix (uri, "https://") ||
                   g_str_has_prefix (uri, "file://"));
}

/* Run CMD with ARG appended. The toast names what was sent where. */
static gboolean
media_spawn (Win *w, const char *cmd, const char *arg, const char *shown)
{
    char  **args = NULL;
    GError *err  = NULL;
    int     argc = 0;

    if (!g_shell_parse_argv (cmd, &argc, &args, &err)) {
        char *msg = g_strdup_printf ("media: cannot parse \"%s\": %s", cmd, err->message);
        toast_error (w, msg);
        g_free (msg);
        g_clear_error (&err);
        return FALSE;
    }

    char **full = g_new0 (char *, argc + 2);
    for (int i = 0; i < argc; i++)
        full[i] = args[i];
    full[argc] = (char *) arg;

    gboolean ok = g_spawn_async (NULL, full, NULL,
                                 G_SPAWN_SEARCH_PATH |
                                 G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                 NULL, NULL, NULL, &err);
    char *msg = ok ? g_strdup_printf ("%s  %s", args[0], shown)
                   : g_strdup_printf ("media: cannot start %s: %s", args[0], err->message);
    if (ok) {
        LOG ("media: %s %s\n", cmd, arg);
        if (w)
            toast_show (w, msg, 2);
    } else {
        toast_error (w, msg);
    }

    g_free (msg);
    g_clear_error (&err);
    g_free (full);
    g_strfreev (args);
    return ok;
}

/*
 * imv and most image viewers open files, not addresses, so a web image is
 * fetched first - with the page as referer and the browser's user agent,
 * which is what most image hosts check - into a private directory under
 * the runtime dir. Files there older than an hour are swept on the next
 * fetch; by then the viewer has long read them.
 */
#define MEDIA_IMG_MAX (64 * 1024 * 1024)

static SoupSession *g_media_soup;

typedef struct { GWeakRef view; char *uri; } ImgFetch;

static char *
media_tmp_dir (void)
{
    const char *base = g_get_user_runtime_dir ();
    char *dir = g_build_filename (base && *base ? base : g_get_tmp_dir (),
                                  "wkview-media", NULL);
    g_mkdir_with_parents (dir, 0700);

    /* sweep what earlier views left behind */
    GDir *d = g_dir_open (dir, 0, NULL);
    const char *n;
    gint64 now = g_get_real_time () / G_USEC_PER_SEC;
    while (d && (n = g_dir_read_name (d))) {
        char *f = g_build_filename (dir, n, NULL);
        GStatBuf st;
        if (g_stat (f, &st) == 0 && now - (gint64) st.st_mtime > 3600)
            g_unlink (f);
        g_free (f);
    }
    if (d)
        g_dir_close (d);
    return dir;
}

static void
img_fetch_free (ImgFetch *f)
{
    g_weak_ref_clear (&f->view);
    g_free (f->uri);
    g_free (f);
}

static void
on_image_fetched (GObject *src, GAsyncResult *res, gpointer u)
{
    ImgFetch      *f    = u;
    SoupMessage   *msg  = soup_session_get_async_result_message (SOUP_SESSION (src), res);
    GError        *err  = NULL;
    GBytes        *body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &err);
    WebKitWebView *view = g_weak_ref_get (&f->view);
    Win           *w    = view ? win_for_view (view) : NULL;
    guint          code = msg ? soup_message_get_status (msg) : 0;

    if (!body || code < 200 || code >= 300 || g_bytes_get_size (body) == 0 ||
        g_bytes_get_size (body) > MEDIA_IMG_MAX) {
        char *why = err ? g_strdup (err->message)
                  : g_strdup_printf ("HTTP %u, %" G_GSIZE_FORMAT " bytes", code,
                                     body ? g_bytes_get_size (body) : 0);
        char *t = g_strdup_printf ("media: image not fetched: %s\n%s", why, f->uri);
        toast_error (w, t);
        g_free (t);
        g_free (why);
    } else {
        char *dir  = media_tmp_dir ();
        char *tmpl = g_build_filename (dir, "img-XXXXXX", NULL);
        int   fd   = g_mkstemp (tmpl);
        if (fd >= 0) {
            gsize n;
            const guint8 *p = g_bytes_get_data (body, &n);
            gboolean wrote = write (fd, p, n) == (ssize_t) n;
            close (fd);
            if (wrote) {
                media_spawn (w, g_image_viewer, tmpl, f->uri);
            } else {
                char *t = g_strdup_printf ("media: cannot write %s", tmpl);
                toast_error (w, t);
                g_free (t);
            }
        } else {
            char *t = g_strdup_printf ("media: cannot create a file in %s", dir);
            toast_error (w, t);
            g_free (t);
        }
        g_free (tmpl);
        g_free (dir);
    }

    if (body)
        g_bytes_unref (body);
    g_clear_error (&err);
    if (view)
        g_object_unref (view);
    img_fetch_free (f);
}

static gboolean
image_open (Win *w, const char *uri)
{
    if (!g_image_viewer || !*g_image_viewer || !is_web_or_file (uri))
        return FALSE;

    if (g_str_has_prefix (uri, "file://")) {
        char *path = g_filename_from_uri (uri, NULL, NULL);
        gboolean ok = path && media_spawn (w, g_image_viewer, path, uri);
        g_free (path);
        return ok;
    }

    if (!g_media_soup) {
        g_media_soup = soup_session_new ();
        soup_session_set_timeout (g_media_soup, 30);
    }
    const char *ua = g_settings ? webkit_settings_get_user_agent (g_settings) : NULL;
    if (ua)
        soup_session_set_user_agent (g_media_soup, ua);

    SoupMessage *msg = soup_message_new ("GET", uri);
    if (!msg)
        return FALSE;
    const char *page = w ? webkit_web_view_get_uri (w->view) : NULL;
    if (page && g_str_has_prefix (page, "http"))
        soup_message_headers_replace (soup_message_get_request_headers (msg), "Referer", page);

    ImgFetch *f = g_new0 (ImgFetch, 1);
    g_weak_ref_init (&f->view, w ? w->view : NULL);
    f->uri = g_strdup (uri);

    if (w)
        toast_show (w, "fetching the image ...", URL_TOAST_SECONDS);
    soup_session_send_and_read_async (g_media_soup, msg, G_PRIORITY_DEFAULT, NULL,
                                      on_image_fetched, f);
    g_object_unref (msg);
    return TRUE;
}

static gboolean
video_open (Win *w, const char *uri)
{
    if (!g_player || !*g_player || !is_web_or_file (uri))
        return FALSE;
    return media_spawn (w, g_player, uri, uri);
}

/* A link the page is following: an image or a video goes out, anything
 * else is followed as usual. */
static gboolean
media_link (Win *w, const char *uri)
{
    if (!uri)
        return FALSE;
    if (is_image_uri (uri) && image_open (w, uri))
        return TRUE;
    if (is_video_uri (uri) && video_open (w, uri))
        return TRUE;
    return FALSE;
}

static gboolean
player_try (WebKitWebView *view, WebKitNavigationAction *act)
{
    if (!g_media_mode)
        return FALSE;
    if (webkit_navigation_action_get_navigation_type (act) != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED)
        return FALSE;
    if (!(webkit_navigation_action_get_modifiers (act) & g_mod))
        return FALSE;
    if (webkit_navigation_action_get_mouse_button (act) > 1)
        return FALSE;                     /* middle click stays WebKit's */

    WebKitURIRequest *req = webkit_navigation_action_get_request (act);
    return media_link (win_for_view (view),
                       req ? webkit_uri_request_get_uri (req) : NULL);
}

/* ------------------------------------------------ remembered decisions */

static void
perms_load (void)
{
    if (g_perms || !g_data_dir)
        return;

    g_perms = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

    char  *path = g_build_filename (g_data_dir, PERM_FILE, NULL);
    char  *data = NULL;

    if (g_file_get_contents (path, &data, NULL, NULL)) {
        char **lines = g_strsplit (data, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char **f = g_strsplit (lines[i], "\t", 3);
            if (g_strv_length (f) >= 3 && !strcmp (f[1], "media"))
                g_hash_table_insert (g_perms, g_strdup (f[0]), g_strdup (g_strstrip (f[2])));
            g_strfreev (f);
        }
        g_strfreev (lines);
        g_free (data);
        LOG ("permissions: %u sites remembered in %s\n",
             g_hash_table_size (g_perms), path);
    }
    g_free (path);
}

static void
perms_save (void)
{
    if (!g_perms || !g_data_dir)
        return;

    GString       *s = g_string_new ("");
    GHashTableIter it;
    gpointer       k, v;

    g_hash_table_iter_init (&it, g_perms);
    while (g_hash_table_iter_next (&it, &k, &v))
        g_string_append_printf (s, "%s\tmedia\t%s\n", (char *) k, (char *) v);

    char *path = g_build_filename (g_data_dir, PERM_FILE, NULL);
    g_file_set_contents (path, s->str, -1, NULL);
    g_free (path);
    g_string_free (s, TRUE);
}

static const char *
perm_remembered (const char *host)
{
    perms_load ();
    return (g_perms && host) ? g_hash_table_lookup (g_perms, host) : NULL;
}

static void
perm_remember (const char *host, gboolean allow)
{
    perms_load ();
    if (!g_perms || !host)
        return;
    g_hash_table_insert (g_perms, g_strdup (host), g_strdup (allow ? "allow" : "deny"));
    perms_save ();
}

static char *
req_host (WebKitWebView *view)
{
    const char *uri = webkit_web_view_get_uri (view);
    if (!uri)
        return NULL;

    GUri *u = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);
    if (!u)
        return NULL;

    char *host = g_strdup (g_uri_get_host (u));
    g_uri_unref (u);
    return host;
}

/* ---------------------------------------------------------- the prompt */

/* Answer every request waiting on this prompt the same way, then hide it. */
static void
perm_answer (Win *w, gboolean allow)
{
    if (!w->permqueue)
        return;

    LOG ("perm: %s -> %s (%u request%s)\n",
         w->permhost ? w->permhost : "?", allow ? "allow" : "deny",
         w->permqueue->len, w->permqueue->len == 1 ? "" : "s");

    for (guint i = 0; i < w->permqueue->len; i++) {
        WebKitPermissionRequest *r = g_ptr_array_index (w->permqueue, i);
        if (allow)
            webkit_permission_request_allow (r);
        else
            webkit_permission_request_deny (r);
    }

    g_ptr_array_set_size (w->permqueue, 0);
    if (w->permhost)
        perm_remember (w->permhost, allow);

    gtk_widget_set_visible (w->perm, FALSE);
    if (allow && g_app->media_asked)
        g_app->media_asked (TRUE, TRUE);
    gtk_widget_grab_focus (GTK_WIDGET (w->view));
}

static void on_perm_allow (GtkButton *b, gpointer u) { (void) b; perm_answer (u, TRUE);  }
static void on_perm_deny  (GtkButton *b, gpointer u) { (void) b; perm_answer (u, FALSE); }

static void
perm_prompt (Win *w, const char *host, gboolean audio, gboolean video)
{
    const char *what = (audio && video) ? "camera and microphone"
                     : video            ? "camera"
                     : audio            ? "microphone" : "media devices";
    char *text = g_strdup_printf ("%s wants to use your %s", host ? host : "This page", what);

    gtk_label_set_text (GTK_LABEL (w->permlabel), text);
    g_free (text);

    g_free (w->permhost);
    w->permhost = g_strdup (host);

    gtk_widget_set_visible (w->perm, TRUE);
}

/* Something is decided before the prompt ever appears: the policy from
 * the command line, or a decision already remembered for this site. */
static gboolean
perm_settled (WebKitPermissionRequest *req, const char *host, gboolean *allowed)
{
    if (g_media_policy == MEDIA_ALLOW) { *allowed = TRUE;  goto done; }
    if (g_media_policy == MEDIA_DENY)  { *allowed = FALSE; goto done; }

    const char *had = perm_remembered (host);
    if (!had)
        return FALSE;
    *allowed = !strcmp (had, "allow");

done:
    if (*allowed)
        webkit_permission_request_allow (req);
    else
        webkit_permission_request_deny (req);
    return TRUE;
}

static gboolean
on_permission (WebKitWebView *view, WebKitPermissionRequest *req, gpointer u)
{
    (void) u;
    Win        *w    = win_for_view (view);
    const char *type = perm_type_name (req);
    char       *host = req_host (view);

    /*
     * Camera and microphone: ask, the way a browser does, and remember
     * the answer per site. --allow-media and --deny-media answer without
     * asking, for a kiosk or a script.
     */
    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST (req)) {
        WebKitUserMediaPermissionRequest *m = WEBKIT_USER_MEDIA_PERMISSION_REQUEST (req);
        gboolean audio  = webkit_user_media_permission_is_for_audio_device (m);
        gboolean video  = webkit_user_media_permission_is_for_video_device (m);
        gboolean screen = webkit_user_media_permission_is_for_display_device (m);
        gboolean allowed;

        if (perm_settled (req, host, &allowed)) {
            LOG ("perm: %s audio=%d video=%d screen=%d -> %s (%s)\n", type,
                 audio, video, screen, allowed ? "allow" : "deny",
                 g_media_policy == MEDIA_ASK ? "remembered" : "policy");
            if (allowed && g_app->media_asked)
                g_app->media_asked (audio, video);
            g_free (host);
            return TRUE;
        }

        if (!w) {                              /* no window to ask in */
            webkit_permission_request_deny (req);
            g_free (host);
            return TRUE;
        }

        LOG ("perm: %s audio=%d video=%d screen=%d -> asking\n",
             type, audio, video, screen);

        if (!w->permqueue)
            w->permqueue = g_ptr_array_new_with_free_func (g_object_unref);
        g_ptr_array_add (w->permqueue, g_object_ref (req));

        if (!gtk_widget_get_visible (w->perm))
            perm_prompt (w, host, audio, video);
        g_free (host);
        return TRUE;
    }

    /* Device labels for enumerateDevices() are not decided here: WebKit
     * 2.54 no longer emits WebKitDeviceInfoPermissionRequest and asks
     * query-permission-state instead, see on_query_permission(). */

    if (WEBKIT_IS_POINTER_LOCK_PERMISSION_REQUEST (req)) {
        webkit_permission_request_allow (req);
        g_free (host);
        return TRUE;
    }

    LOG ("perm: %s -> deny\n", type);
    webkit_permission_request_deny (req);
    g_free (host);
    return TRUE;
}

/*
 * navigator.permissions.query({name:'camera'|'microphone'}), and WebKit's
 * own question before enumerateDevices() and getUserMedia(): is this site
 * already allowed? Unanswered, WebKit's documented default is "prompt",
 * so a remembered site would never be told it is allowed and device
 * labels would stay hidden. Answered from the same per-site memory as the
 * prompt, keyed by the top-level origin's host.
 */
static gboolean
on_query_permission (WebKitWebView *view, WebKitPermissionStateQuery *q, gpointer u)
{
    (void) view; (void) u;
    const char *name = webkit_permission_state_query_get_name (q);

    if (g_strcmp0 (name, "camera") != 0 && g_strcmp0 (name, "microphone") != 0)
        return FALSE;                          /* WebKit answers "prompt" */

    WebKitSecurityOrigin *o    = webkit_permission_state_query_get_security_origin (q);
    const char           *host = o ? webkit_security_origin_get_host (o) : NULL;
    WebKitPermissionState st;

    if (g_media_policy == MEDIA_ALLOW)
        st = WEBKIT_PERMISSION_STATE_GRANTED;
    else if (g_media_policy == MEDIA_DENY)
        st = WEBKIT_PERMISSION_STATE_DENIED;
    else {
        const char *had = perm_remembered (host);
        if (!had)
            return FALSE;
        st = !strcmp (had, "allow") ? WEBKIT_PERMISSION_STATE_GRANTED
                                    : WEBKIT_PERMISSION_STATE_DENIED;
    }

    LOG ("perm: query %s for %s -> %s\n", name, host ? host : "?",
         st == WEBKIT_PERMISSION_STATE_GRANTED ? "granted" : "denied");
    webkit_permission_state_query_finish (q, st);
    return TRUE;
}

/* ------------------------------------------------------------ inspector */

static void
inspector_toggle (WebKitWebView *view)
{
    WebKitWebInspector *insp = webkit_web_view_get_inspector (view);
    if (!insp)
        return;

    if (webkit_web_inspector_get_web_view (insp))
        webkit_web_inspector_close (insp);
    else
        webkit_web_inspector_show (insp);
}

/* open the inspector once, after the first commit (--devtools) */
static void
on_load_changed_devtools (WebKitWebView *view, WebKitLoadEvent ev, gpointer u)
{
    (void) u;
    if (ev != WEBKIT_LOAD_COMMITTED)
        return;
    g_signal_handlers_disconnect_by_func (view, G_CALLBACK (on_load_changed_devtools), NULL);
    webkit_web_inspector_show (webkit_web_view_get_inspector (view));
}

/* ------------------------------------------------------------- history */

/*
 * A flat, greppable log of every address that actually came up:
 *
 *   2026-08-24T10:12:03+02:00 <TAB> https://example.com/x <TAB> Example
 *
 * Only successful loads of http/https/file are recorded, so every line is
 * a link that worked. It doubles as the back/forward list: <mod>+J walks
 * WebKit's own session list first and continues into this file once that
 * is exhausted, so back still gets you somewhere on a fresh window.
 *
 * --private leaves g_hist_path NULL and nothing is read or written.
 */

static void
hist_entry_free (gpointer p)
{
    HistEntry *e = p;
    g_free (e->uri);
    g_free (e->title);
    g_free (e);
}

static HistEntry *
hist_at (guint i)
{
    return g_ptr_array_index (g_hist, i);
}

static gboolean
history_uri_ok (const char *uri)
{
    return uri && (g_str_has_prefix (uri, "http://")  ||
                   g_str_has_prefix (uri, "https://") ||
                   g_str_has_prefix (uri, "file://"));
}

/* tabs and newlines would break the one-record-per-line format */
static char *
history_clean (const char *s)
{
    char *out = g_strdup (s ? s : "");
    for (char *p = out; *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r')
            *p = ' ';
    return out;
}

static void
hist_add (const char *uri, const char *title)
{
    HistEntry *e = g_new0 (HistEntry, 1);
    e->uri   = g_strdup (uri);
    e->title = g_strdup (title ? title : "");
    g_ptr_array_add (g_hist, e);
}

/* Read the file into g_hist, trimming it back to HIST_KEEP lines. */
static void
history_load (void)
{
    char *data = NULL;

    if (!g_hist_path || !g_file_get_contents (g_hist_path, &data, NULL, NULL))
        return;

    char **lines = g_strsplit (data, "\n", -1);
    guint  n     = g_strv_length (lines);
    guint  first = 0;

    /* the last element is the empty string after the trailing newline */
    if (n && !*lines[n - 1])
        n--;
    if (n > HIST_KEEP)
        first = n - HIST_KEEP;

    /*
     * One line per address, keeping the newest visit. Files written before
     * this rule existed are full of repeats, so the cleanup happens here
     * rather than only on the way in - the file is tidied on first read.
     */
    GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);
    GPtrArray  *keep = g_ptr_array_new ();   /* line indices, newest first */

    for (guint i = n; i > first; i--) {
        char **f = g_strsplit (lines[i - 1], "\t", 3);

        if (g_strv_length (f) >= 2 && history_uri_ok (f[1]) &&
            !g_hash_table_contains (seen, f[1])) {
            g_hash_table_add (seen, g_strdup (f[1]));
            g_ptr_array_add (keep, GUINT_TO_POINTER (i - 1));
        }
        g_strfreev (f);
    }

    GString *kept  = g_string_new (NULL);
    guint    dropped = 0;

    for (guint i = keep->len; i > 0; i--) {          /* back into file order */
        guint  idx = GPOINTER_TO_UINT (g_ptr_array_index (keep, i - 1));
        char **f   = g_strsplit (lines[idx], "\t", 3);

        hist_add (f[1], g_strv_length (f) >= 3 ? f[2] : "");
        g_string_append (kept, lines[idx]);
        g_string_append_c (kept, '\n');
        g_strfreev (f);
    }

    dropped = (n - first) - keep->len;

    if (first > 0 || dropped > 0)
        g_file_set_contents (g_hist_path, kept->str, -1, NULL);

    if (dropped)
        LOG ("history: dropped %u duplicate lines\n", dropped);

    g_ptr_array_free (keep, TRUE);
    g_hash_table_destroy (seen);

    g_string_free (kept, TRUE);
    g_strfreev (lines);
    g_free (data);

    LOG ("history: %u entries from %s\n", g_hist->len, g_hist_path);
}

/*
 * Delete removes the entry from the list and from the file. The file is
 * rewritten from itself rather than from memory, so the timestamps of the
 * surviving lines are kept: entry i is line i, and stays that way because
 * loading and appending both keep the two in step.
 */
static void
history_delete (int pos)
{
    if (pos < 0 || pos >= (int) g_hist->len)
        return;

    if (g_hist_path) {
        char *data = NULL;

        if (g_file_get_contents (g_hist_path, &data, NULL, NULL)) {
            char   **lines = g_strsplit (data, "\n", -1);
            GString *out   = g_string_new (NULL);
            int      n     = 0;

            for (int i = 0; lines[i]; i++) {
                if (!*lines[i])
                    continue;
                if (n++ == pos)
                    continue;                  /* the one being removed */
                g_string_append (out, lines[i]);
                g_string_append_c (out, '\n');
            }

            g_file_set_contents (g_hist_path, out->str, -1, NULL);
            g_string_free (out, TRUE);
            g_strfreev (lines);
            g_free (data);
        }
    }

    LOG ("history: moved %s to the end\n", hist_at (pos)->uri);
    g_ptr_array_remove_index (g_hist, pos);

    /* every window's cursor may now point past the end */
    for (guint i = 0; i < g_wins->len; i++) {
        Win *w = g_ptr_array_index (g_wins, i);
        if (w->hist_pos >= (int) g_hist->len)
            w->hist_pos = (int) g_hist->len - 1;
    }
}

/* dir is -1 for back, +1 for forward. */
static void
history_go (Win *w, int dir)
{
    /* WebKit's own list first: it restores scroll position and form state */
    if (!w->hist_walk) {
        gboolean can = dir < 0 ? webkit_web_view_can_go_back (w->view)
                               : webkit_web_view_can_go_forward (w->view);
        if (can) {
            w->hist_loading = TRUE;
            if (dir < 0)
                webkit_web_view_go_back (w->view);
            else
                webkit_web_view_go_forward (w->view);

            /* keep the stored cursor roughly in step with the page list */
            if (g_hist->len)
                w->hist_pos = CLAMP (w->hist_pos + dir, 0, (int) g_hist->len - 1);
            return;
        }
    }

    int next = w->hist_pos + dir;

    if (next < 0 || next >= (int) g_hist->len) {
        toast_show (w, dir < 0 ? "start of history" : "end of history", 2);
        return;
    }

    w->hist_pos     = next;
    w->hist_walk    = TRUE;
    w->hist_loading = TRUE;

    LOG ("history: %s %s\n", dir < 0 ? "back" : "forward", hist_at (next)->uri);
    webkit_web_view_load_uri (w->view, hist_at (next)->uri);
}

/* Core's own load signals: they only feed the history. */
static gboolean
urltoast_timeout (gpointer u)
{
    Win *w = u;
    gtk_widget_set_visible (w->urltoast, FALSE);
    w->urltoast_id = 0;
    return G_SOURCE_REMOVE;
}

/* The address of the page being opened, top left, while it comes up. */
static void
urltoast_show (Win *w, const char *uri)
{
    if (!uri || !*uri || g_str_has_prefix (uri, "about:"))
        return;

    gtk_label_set_text (GTK_LABEL (w->urltoast), uri);
    gtk_widget_set_visible (w->urltoast, TRUE);

    if (w->urltoast_id)
        g_source_remove (w->urltoast_id);
    w->urltoast_id = g_timeout_add_seconds (URL_TOAST_SECONDS, urltoast_timeout, w);
}

/* The title only turns up part way through the load, so the label starts
 * as the address and is upgraded in place when the name arrives. */
static void
on_title_toast (GObject *obj, GParamSpec *ps, gpointer u)
{
    (void) ps; (void) u;
    WebKitWebView *view = WEBKIT_WEB_VIEW (obj);
    Win           *w    = win_of (view);
    const char    *t    = webkit_web_view_get_title (view);

    if (w && t && *t && gtk_widget_get_visible (w->urltoast))
        gtk_label_set_text (GTK_LABEL (w->urltoast), t);
}

static void
on_load_progress (GObject *obj, GParamSpec *ps, gpointer u)
{
    (void) ps; (void) u;
    WebKitWebView *view = WEBKIT_WEB_VIEW (obj);
    Win           *w    = win_of (view);

    if (!w)
        return;

    double p = webkit_web_view_get_estimated_load_progress (view);

    /*
     * Only the fill inside the bar changes. Nothing here may alter the
     * bar's own size: the previous version resized it on every tick,
     * which re-allocated the whole overlay - and re-allocating a
     * WebKitWebView makes the web process re-lay-out and recomposite.
     * At the rate estimated-load-progress fires, that is the black and
     * white flicker while a page loads.
     */
    w->load_frac = CLAMP (p, 0.0, 1.0);
    gtk_widget_set_visible (w->loadbar, g_load_bar && p > 0.0 && p < 1.0);
    gtk_widget_queue_draw (w->loadbar);          /* repaint, not re-layout */
}

static void
on_load_core (WebKitWebView *view, WebKitLoadEvent ev, gpointer u)
{
    (void) u;
    Win *w = win_of (view);
    if (!w)
        return;

    if (ev == WEBKIT_LOAD_STARTED) {
        w->load_failed = FALSE;
        urltoast_show (w, webkit_web_view_get_uri (view));
    } else if (ev == WEBKIT_LOAD_COMMITTED) {
        urltoast_show (w, webkit_web_view_get_uri (view));   /* after redirects */
    } else if (ev == WEBKIT_LOAD_FINISHED) {
        gtk_widget_set_visible (w->loadbar, FALSE);
        history_note (w);
    }
}

/*
 * A page that never appears is usually the web process dying, and until
 * now only browser-big said so. The reason distinguishes a crash (a
 * broken WebKit or JIT) from the memory limit from a deliberate kill.
 */
static void
on_web_process_gone (WebKitWebView *view, WebKitWebProcessTerminationReason reason,
                     gpointer u)
{
    (void) u;
    const char *why = reason == WEBKIT_WEB_PROCESS_CRASHED
                    ? "crashed"
                    : reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT
                    ? "exceeded its memory limit"
                    : "was terminated";

    g_printerr ("web process %s: %s\n", why,
                webkit_web_view_get_uri (view) ? webkit_web_view_get_uri (view)
                                               : "(no url)");
    g_printerr ("web process: if this repeats, try --no-jit, then --no-gpu,\n"
                "             then WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1\n");
}

static gboolean
on_load_failed_core (WebKitWebView *view, WebKitLoadEvent ev,
                     gchar *uri, GError *error, gpointer u)
{
    (void) ev; (void) uri; (void) error; (void) u;
    Win *w = win_of (view);
    if (w)
        w->load_failed = TRUE;
    return FALSE;               /* let WebKit show its error page */
}

/* ------------------------------------------------------------ omni popup */

/*
 * One popup does both jobs, because they are the same job: an input line
 * with the matching history under it.
 *
 *   <mod>+O   type an address. The list stays out of the way until Tab
 *             or an arrow asks for it.
 *   <mod>+H   browse the history. The list is there from the start.
 *
 * Tab and Down walk forward through the matches, Shift+Tab and Up back.
 * Whatever is selected is written into the input, so Enter always loads
 * exactly what you can read. Esc, <mod>+H or a click outside closes it.
 */

static gboolean
omni_matches (const HistEntry *e, const char *needle)
{
    if (!needle || !*needle)
        return TRUE;

    char    *u   = g_utf8_casefold (e->uri, -1);
    char    *t   = g_utf8_casefold (e->title, -1);
    gboolean hit = strstr (u, needle) || strstr (t, needle);

    g_free (u);
    g_free (t);
    return hit;
}

/* One row shape for both lists: a strong line and a quiet one under it. */
static GtkWidget *
omni_row_new (const char *main_text, const char *sub_text)
{
    GtkWidget *row = gtk_list_box_row_new ();
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *main_l = gtk_label_new (main_text);
    gtk_widget_add_css_class (main_l, "br-pick-main");
    gtk_label_set_xalign (GTK_LABEL (main_l), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (main_l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append (GTK_BOX (box), main_l);

    if (sub_text && *sub_text) {
        GtkWidget *sub = gtk_label_new (sub_text);
        gtk_widget_add_css_class (sub, "br-pick-sub");
        gtk_label_set_xalign (GTK_LABEL (sub), 0.0);
        gtk_label_set_ellipsize (GTK_LABEL (sub), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append (GTK_BOX (box), sub);
    }

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    return row;
}

static void
omni_empty_row (Win *w, const char *text)
{
    GtkWidget *row = gtk_list_box_row_new ();
    GtkWidget *l   = gtk_label_new (text);

    gtk_widget_add_css_class (l, "br-pick-sub");
    gtk_label_set_xalign (GTK_LABEL (l), 0.0);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), l);
    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
    gtk_list_box_append (GTK_LIST_BOX (w->omnilist), row);
}

/* The directory popup lists what is already stored, so a directory can be
 * picked instead of typed, and dropped with Delete. */
static void
omni_rebuild_rules (Win *w, const char *needle)
{
    GList *keys = g_hash_table_get_keys (g_dlrules);
    guint  shown = 0;

    keys = g_list_sort (keys, (GCompareFunc) g_strcmp0);

    for (GList *l = keys; l; l = l->next) {
        const char *key = l->data;
        const char *dir = g_hash_table_lookup (g_dlrules, key);

        if (needle && *needle) {
            char    *k = g_utf8_casefold (key, -1);
            char    *d = g_utf8_casefold (dir, -1);
            gboolean hit = strstr (k, needle) || strstr (d, needle);
            g_free (k);
            g_free (d);
            if (!hit)
                continue;
        }

        gboolean always = dl_always_for_key (key);
        char    *sub    = g_strdup_printf ("%s%s", key, always ? "   (always replaces)" : "");
        GtkWidget *row  = omni_row_new (dir, sub);

        g_object_set_data_full (G_OBJECT (row), "rule", g_strdup (key), g_free);
        gtk_list_box_append (GTK_LIST_BOX (w->omnilist), row);
        g_free (sub);
        shown++;
    }

    g_list_free (keys);

    if (shown == 0)
        omni_empty_row (w, g_hash_table_size (g_dlrules)
                        ? "no match" : "no directories stored yet");
}

/* <mod>+K lists the keywords it already knows, the same way the directory
 * popup lists its rules. */
static void
omni_rebuild_searches (Win *w, const char *needle)
{
    GList *keys  = g_searches ? g_hash_table_get_keys (g_searches) : NULL;
    guint  shown = 0;

    keys = g_list_sort (keys, (GCompareFunc) g_strcmp0);

    for (GList *l = keys; l; l = l->next) {
        const char *key = l->data;
        const char *url = g_hash_table_lookup (g_searches, key);

        if (needle && *needle) {
            char    *k = g_utf8_casefold (key, -1);
            char    *u = g_utf8_casefold (url, -1);
            gboolean hit = strstr (k, needle) || strstr (u, needle);
            g_free (k);
            g_free (u);
            if (!hit)
                continue;
        }

        gboolean   dflt      = !g_strcmp0 (key, g_search_default);
        char      *main_text = g_strdup_printf ("%s   %s%s", key, url,
                                                dflt ? "   (default)" : "");
        GtkWidget *row       = omni_row_new (main_text, NULL);

        g_object_set_data_full (G_OBJECT (row), "search", g_strdup (key), g_free);
        gtk_list_box_append (GTK_LIST_BOX (w->omnilist), row);
        g_free (main_text);
        shown++;
    }

    g_list_free (keys);

    if (shown == 0)
        omni_empty_row (w, "no keywords yet - type one as: g https://host/?q={}");
}

static void
omni_rebuild (Win *w)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child (w->omnilist)))
        gtk_list_box_remove (GTK_LIST_BOX (w->omnilist), child);

    char *needle = g_utf8_casefold (w->omni_needle ? w->omni_needle : "", -1);
    guint shown  = 0;

    if (w->omni_mode == OMNI_DLDIR) {
        omni_rebuild_rules (w, needle);
        g_free (needle);
        return;
    }

    if (w->omni_mode == OMNI_SEARCH) {
        omni_rebuild_searches (w, needle);
        g_free (needle);
        return;
    }

    /* newest first: what you want in a history list */
    for (guint i = g_hist->len; i > 0 && shown < PICK_ROWS; i--) {
        HistEntry *e = hist_at (i - 1);
        if (!omni_matches (e, needle))
            continue;

        GtkWidget *row = omni_row_new (*e->title ? e->title : e->uri,
                                       *e->title ? e->uri : NULL);
        g_object_set_data (G_OBJECT (row), "pos", GINT_TO_POINTER ((int) i - 1));
        gtk_list_box_append (GTK_LIST_BOX (w->omnilist), row);
        shown++;
    }

    if (shown == 0)
        omni_empty_row (w, g_hist->len ? "no match" : "no history yet");

    g_free (needle);
}

/* Keep the selected row inside the scrolled area without stealing the
 * focus from the input - the user is still typing into it. */
static void
omni_scroll_to (Win *w, GtkListBoxRow *row)
{
    GtkAdjustment *adj =
        gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (w->omniscroll));
    graphene_rect_t b;

    if (!adj || !gtk_widget_compute_bounds (GTK_WIDGET (row), w->omnilist, &b))
        return;

    double top  = b.origin.y;
    double bot  = top + b.size.height;
    double val  = gtk_adjustment_get_value (adj);
    double page = gtk_adjustment_get_page_size (adj);

    if (top < val)
        gtk_adjustment_set_value (adj, top);
    else if (bot > val + page)
        gtk_adjustment_set_value (adj, bot - page);
}

static void
history_append (const char *uri, const char *title)
{
    if (!g_hist_path)
        return;

    /* One line per address. Visiting a page again moves it to the end
     * rather than adding a second copy, so the list stays a set and the
     * newest visit is the one you scroll to first. */
    int seen = -1;
    for (guint i = 0; i < g_hist->len; i++)
        if (!g_strcmp0 (hist_at (i)->uri, uri)) {
            seen = (int) i;
            break;
        }

    if (seen >= 0 && seen == (int) g_hist->len - 1)
        return;                        /* already the newest, nothing to do */

    if (seen >= 0)
        history_delete (seen);         /* drops it from the file too */

    char *t = history_clean (title);
    hist_add (uri, t);

    GDateTime *now  = g_date_time_new_now_local ();
    char      *when = g_date_time_format_iso8601 (now);
    FILE      *f    = g_fopen (g_hist_path, "a");

    if (f) {
        fprintf (f, "%s\t%s\t%s\n", when, uri, t);
        fclose (f);
    } else {
        g_printerr ("history: cannot write %s: %s\n", g_hist_path, g_strerror (errno));
    }

    g_free (t);
    g_free (when);
    g_date_time_unref (now);
}

static void
history_setup (const char *data_dir)
{
    g_hist = g_ptr_array_new_with_free_func (hist_entry_free);

    if (g_private || !data_dir) {
        LOG ("history: off\n");
        return;
    }

    g_hist_path = g_build_filename (data_dir, HIST_FILE, NULL);
    history_load ();
}

/* The <title> is often still empty when the load finishes, so the record
 * is written a moment later. It also debounces redirect chains. */
static gboolean
history_write (gpointer u)
{
    Win *w = u;
    w->hist_id = 0;

    const char *uri = webkit_web_view_get_uri (w->view);
    if (!history_uri_ok (uri))
        return G_SOURCE_REMOVE;
    if (g_app->history_skip && g_app->history_skip (uri))
        return G_SOURCE_REMOVE;         /* a front-end's internal page */

    history_append (uri, webkit_web_view_get_title (w->view));
    w->hist_pos = (int) g_hist->len - 1;
    return G_SOURCE_REMOVE;
}

/* Called when a load settles. Records the page unless we navigated there
 * ourselves, in which case the cursor is already where it belongs. */
static void
history_note (Win *w)
{
    if (w->hist_loading) {
        w->hist_loading = FALSE;
        return;
    }

    w->hist_walk = FALSE;               /* the user went somewhere new */

    if (w->load_failed)
        return;

    if (w->hist_id)
        g_source_remove (w->hist_id);
    w->hist_id = g_timeout_add (HIST_SETTLE_MS, history_write, w);
}

/* Delete in the popup: drop the selected row and keep the place. */
static void
omni_delete_selected (Win *w)
{
    GtkListBox    *list = GTK_LIST_BOX (w->omnilist);
    GtkListBoxRow *row  = gtk_list_box_get_selected_row (list);

    if (!row || !gtk_list_box_row_get_selectable (row))
        return;

    int         idx  = gtk_list_box_row_get_index (row);
    const char *rule = g_object_get_data (G_OBJECT (row), "rule");

    const char *search = g_object_get_data (G_OBJECT (row), "search");

    if (search) {
        LOG ("search: dropped %s\n", search);
        g_hash_table_remove (g_searches, search);
        searches_save ();
    } else if (rule) {
        LOG ("download: dropped rule %s\n", rule);
        dl_dir_forget (rule);
        dlrules_save ();
    } else {
        history_delete (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "pos")));
    }

    omni_rebuild (w);

    GtkListBoxRow *next = gtk_list_box_get_row_at_index (list, idx);
    if (!next)
        next = gtk_list_box_get_row_at_index (list, idx - 1);
    if (next && gtk_list_box_row_get_selectable (next)) {
        gtk_list_box_select_row (list, next);
        omni_scroll_to (w, next);
    }
}

/* The selected entry is written into the input, so Enter loads what you
 * see. The guard keeps that write from re-filtering the list. */
static void
omni_reflect (Win *w, GtkListBoxRow *row)
{
    if (!row || !gtk_list_box_row_get_selectable (row))
        return;

    const char *text;
    char       *owned  = NULL;
    const char *search = g_object_get_data (G_OBJECT (row), "search");
    const char *rule   = g_object_get_data (G_OBJECT (row), "rule");

    if (search) {
        const char *url = g_hash_table_lookup (g_searches, search);
        if (!url)
            return;
        owned = g_strdup_printf ("%s %s", search, url);
        text  = owned;
        gtk_check_button_set_active (GTK_CHECK_BUTTON (w->omnialways),
                                     !g_strcmp0 (search, g_search_default));
    } else if (rule) {
        text = g_hash_table_lookup (g_dlrules, rule);   /* its directory */
        if (!text)
            return;
    } else {
        int pos = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "pos"));
        if (pos < 0 || pos >= (int) g_hist->len)
            return;
        text = hist_at (pos)->uri;
    }

    w->omni_setting = TRUE;
    gtk_editable_set_text (GTK_EDITABLE (w->omnientry), text);
    gtk_editable_set_position (GTK_EDITABLE (w->omnientry), -1);
    w->omni_setting = FALSE;

    g_free (owned);
}

/*
 * A fixed number of lines, measured from a real row rather than guessed,
 * so the box is the same height whether the history has four entries or
 * four hundred. Min and max are set together: a list that grows and
 * shrinks as you type is harder to read than one that stays put.
 */
static void
omni_size_list (Win *w)
{
    int rows  = g_theme.list_rows > 0 ? g_theme.list_rows : 8;
    int row_h = 0;

    GtkListBoxRow *first = gtk_list_box_get_row_at_index (GTK_LIST_BOX (w->omnilist), 0);
    if (first) {
        int min_h, nat_h;
        gtk_widget_measure (GTK_WIDGET (first), GTK_ORIENTATION_VERTICAL, -1,
                            &min_h, &nat_h, NULL, NULL);
        row_h = nat_h;
    }

    if (row_h <= 0)                    /* nothing built yet, estimate */
        row_h = (int) ((g_theme.label_px + g_theme.title_px) * g_theme.ui_scale
                       + g_theme.win_gap);

    int want = rows * row_h;
    int win_h = gtk_widget_get_height (w->win);

    if (win_h > 0)
        want = MIN (want, (int) (win_h * 0.7));

    if (want < row_h)
        want = row_h;

    /*
     * GTK asserts that min <= max, checking against whichever value is
     * already set, so either order can trip it. Clearing both to -1 first
     * makes the pair order-independent.
     */
    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW (w->omniscroll);

    gtk_scrolled_window_set_min_content_height (sw, -1);
    gtk_scrolled_window_set_max_content_height (sw, -1);
    gtk_scrolled_window_set_max_content_height (sw, want);
    gtk_scrolled_window_set_min_content_height (sw, want);
}

static void
omni_list_show (Win *w)
{
    if (w->omni_list)
        return;

    omni_rebuild (w);
    omni_size_list (w);                /* after the rows exist, so one can be measured */
    gtk_widget_set_visible (w->omniscroll, TRUE);
    w->omni_list = TRUE;
}

/* dir is +1 down the list, -1 up it. The first press just reveals the
 * matches and takes the top one. */
static void
omni_move (Win *w, int dir)
{
    GtkListBox    *list = GTK_LIST_BOX (w->omnilist);
    GtkListBoxRow *next;

    if (!w->omni_list) {
        omni_list_show (w);
        next = gtk_list_box_get_row_at_index (list, 0);
    } else {
        GtkListBoxRow *cur = gtk_list_box_get_selected_row (list);
        int            idx = cur ? gtk_list_box_row_get_index (cur) : -1;
        next = gtk_list_box_get_row_at_index (list, idx + dir);
        if (!next)
            return;                     /* stop at the ends, do not wrap */
    }

    if (!next)
        return;

    gtk_list_box_select_row (list, next);
    omni_scroll_to (w, next);
    omni_reflect (w, next);
}

static void
omni_hide (Win *w)
{
    if (!gtk_widget_get_visible (w->omni))
        return;

    if (w->omni_mode == OMNI_FIND) {
        webkit_find_controller_search_finish (webkit_web_view_get_find_controller (w->view));
        find_highlight (FALSE);
    }

    gtk_widget_set_visible (w->omni, FALSE);
    gtk_widget_set_visible (w->omniscroll, FALSE);
    w->omni_list = FALSE;
    gtk_widget_grab_focus (GTK_WIDGET (w->view));
}

/* The select only means anything when setting a download directory. Its
 * three entries name the address, its site, and everything else. */
static void
omni_setup_scope (Win *w, OmniMode mode, const char *uri)
{
    if (mode == OMNI_SEARCH) {
        gtk_widget_set_visible (w->omniscope, FALSE);
        gtk_check_button_set_label (GTK_CHECK_BUTTON (w->omnialways),
                                    "use this one when nothing is prefixed");
        gtk_check_button_set_active (GTK_CHECK_BUTTON (w->omnialways), FALSE);
        gtk_widget_set_visible (w->omnialways, TRUE);
        return;
    }

    if (mode != OMNI_DLDIR) {
        gtk_widget_set_visible (w->omniscope, FALSE);
        gtk_widget_set_visible (w->omnialways, FALSE);
        return;
    }

    char *host  = uri_host (uri);
    char *short_uri = uri ? elide (uri, 44) : g_strdup ("this page");
    char *page  = g_strdup_printf ("only %s", short_uri);
    char *site  = host ? g_strdup_printf ("everything on %s", host)
                       : g_strdup ("everything on this site");

    const char *items[] = { page, site, "everything", NULL };
    GtkStringList *list = gtk_string_list_new (items);

    gtk_drop_down_set_model (GTK_DROP_DOWN (w->omniscope), G_LIST_MODEL (list));
    gtk_drop_down_set_selected (GTK_DROP_DOWN (w->omniscope), dl_scope_for_uri (uri));
    gtk_widget_set_visible (w->omniscope, TRUE);

    /* the same select decides how far this reaches */
    DlScope now = dl_scope_for_uri (uri);
    char   *key = now == DL_SCOPE_PAGE ? g_strdup (uri)
                : now == DL_SCOPE_SITE ? uri_host (uri)
                                       : NULL;

    gtk_check_button_set_label (GTK_CHECK_BUTTON (w->omnialways),
                                "always replace existing files");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->omnialways),
                                 dl_always_for_key (key));
    gtk_widget_set_visible (w->omnialways, TRUE);
    g_free (key);

    g_object_unref (list);
    g_free (site);
    g_free (page);
    g_free (short_uri);
    g_free (host);
}

/* The mode is what separates <mod>+O, <mod>+H, <mod>+F and <mod>+S. */
static void
omni_show (Win *w, OmniMode mode)
{
    const char *uri = webkit_web_view_get_uri (w->view);

    g_clear_pointer (&w->omni_needle, g_free);
    w->omni_needle = g_strdup ("");
    w->omni_mode   = mode;

    /* The download-directory clash question hides the input and turns the
     * hint into its question. Whatever way it was left - answered, Esc, a
     * click outside - the next popup starts from the ordinary layout, and
     * an unanswered question is dropped rather than answered later. */
    w->dl_conflict = FALSE;
    g_clear_pointer (&w->dl_pending_dir, g_free);
    gtk_widget_set_visible (w->omnientry, TRUE);
    gtk_label_set_wrap (GTK_LABEL (w->omnihint), FALSE);
    gtk_widget_set_hexpand (w->omnihint, FALSE);
    gtk_widget_set_halign (w->omnihint, GTK_ALIGN_END);

    w->omni_setting = TRUE;
    gtk_editable_set_text (GTK_EDITABLE (w->omnientry),
                           mode == OMNI_URL   ? (uri ? uri : "")
                         : mode == OMNI_DLDIR ? dl_dir_for_uri (uri)
                                              : "");
    gtk_entry_set_placeholder_text (GTK_ENTRY (w->omnientry),
                                    mode == OMNI_FIND    ? "find in page"
                                  : mode == OMNI_HISTORY ? "search history"
                                  : mode == OMNI_DLDIR   ? "download directory"
                                  : mode == OMNI_SEARCH  ? "s https://example.com/?q={}"
                                                         : "address");
    w->omni_setting = FALSE;

    w->omni_list = FALSE;
    gtk_widget_set_visible (w->omniscroll, FALSE);
    gtk_widget_set_visible (w->omnihint, mode == OMNI_FIND);
    gtk_label_set_text (GTK_LABEL (w->omnihint), "");

    /* Typing into a box and hoping is not an interface: the two things
     * Enter and Esc would do are on screen, with their keys named. */
    gboolean confirmable = mode == OMNI_DLDIR || mode == OMNI_SEARCH;

    gtk_button_set_label (GTK_BUTTON (w->omnisave), "Save  (Enter)");
    gtk_button_set_label (GTK_BUTTON (w->omnicancel), "Cancel  (Esc)");
    gtk_widget_set_visible (w->omnibuttons, confirmable);
    omni_setup_scope (w, mode, uri);
    gtk_widget_set_visible (w->omni, TRUE);

    gtk_widget_grab_focus (w->omnientry);

    if (mode == OMNI_URL)
        gtk_editable_select_region (GTK_EDITABLE (w->omnientry), 0, -1);

    if (mode == OMNI_URL || mode == OMNI_HISTORY)
        omni_list_show (w);            /* the history is worth seeing unasked */

    if (mode == OMNI_FIND)
        find_highlight (TRUE);
    else if (mode == OMNI_DLDIR || mode == OMNI_SEARCH)
        omni_list_show (w);            /* what is already stored */
}

static void
omni_toggle (Win *w, OmniMode mode)
{
    if (gtk_widget_get_visible (w->omni) && w->omni_mode == mode)
        omni_hide (w);
    else
        omni_show (w, mode);
}

/* Enter, or a click on a row: load whatever the input says. */
static void
omni_go (Win *w)
{
    char *raw = g_strdup (gtk_editable_get_text (GTK_EDITABLE (w->omnientry)));

    omni_hide (w);
    w->hist_walk = FALSE;              /* typing an address ends a walk */

    /* "s tree" with a keyword s in front is a search, not an address */
    char *search = search_expand (raw);
    if (search) {
        LOG ("search: %s\n", search);
        webkit_web_view_load_uri (w->view, search);
        g_free (search);
        g_free (raw);
        return;
    }

    /* the front-end may claim it, e.g. browser-big's "diag" */
    if (g_app->load_uri && g_app->load_uri (w->view, raw)) {
        g_free (raw);
        return;
    }

    /* Words are a search, not a host. Without this, "tree" became
     * https://tree and every query needed a keyword in front of it. */
    if (!looks_like_address (raw)) {
        char *found = search_default_uri (raw);
        if (found) {
            LOG ("search: %s\n", found);
            webkit_web_view_load_uri (w->view, found);
            g_free (found);
            g_free (raw);
            return;
        }
    }

    char *uri = normalize_uri (raw);
    g_free (raw);

    if (uri) {
        LOG ("load: %s\n", uri);
        webkit_web_view_load_uri (w->view, uri);
        g_free (uri);
    }
}

/* ------------------------------------------------------------ find in page */

#define FIND_OPTS (WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | \
                   WEBKIT_FIND_OPTIONS_WRAP_AROUND)

/*
 * How many matches WebKit is asked to find. Past this it stops walking
 * the document, which is what keeps a one-letter search on a page of
 * thousands of lines from freezing the browser. Nobody reads past a
 * thousand highlights anyway; the hint says "1000+".
 */
#define FIND_MAX    1000
#define FIND_DEBOUNCE_MS 120

static guint g_find_timer;

/* WebKit scrolls the hit into view for us. */
/* The sheet is only worth injecting while the find bar is up. */
static void
find_highlight (gboolean on)
{
    if (g_find_css_on == on)
        return;

    g_find_css_on = on;
    css_apply_all ();
}

static gboolean
find_run_now (gpointer u)
{
    Win *w = u;
    g_find_timer = 0;

    if (w->omni_mode != OMNI_FIND)
        return G_SOURCE_REMOVE;

    WebKitFindController *fc = webkit_web_view_get_find_controller (w->view);
    const char           *s  = w->omni_needle ? w->omni_needle : "";

    if (!*s) {
        webkit_find_controller_search_finish (fc);
        gtk_label_set_text (GTK_LABEL (w->omnihint), "");
        return G_SOURCE_REMOVE;
    }

    /* one pass: search() reports the count through found-text itself */
    webkit_find_controller_search (fc, s, FIND_OPTS, FIND_MAX);
    return G_SOURCE_REMOVE;
}

/* Typing is bursty; searching on the pause after it is what a browser
 * does, and on a large page it is the difference between usable and not. */
static void
find_run (Win *w)
{
    if (g_find_timer)
        g_source_remove (g_find_timer);
    g_find_timer = g_timeout_add (FIND_DEBOUNCE_MS, find_run_now, w);
}

static void
find_step (Win *w, int dir)
{
    WebKitFindController *fc = webkit_web_view_get_find_controller (w->view);

    if (!w->omni_needle || !*w->omni_needle)
        return;

    if (dir < 0)
        webkit_find_controller_search_previous (fc);
    else
        webkit_find_controller_search_next (fc);
}

static void
on_found_count (WebKitFindController *fc, guint count, gpointer u)
{
    (void) u;
    Win *w = win_of (webkit_find_controller_get_web_view (fc));
    if (!w || w->omni_mode != OMNI_FIND)
        return;

    char *s = count >= FIND_MAX ? g_strdup_printf ("%u+", FIND_MAX)
            : count             ? g_strdup_printf ("%u", count)
            :                     g_strdup ("no match");
    gtk_label_set_text (GTK_LABEL (w->omnihint), s);
    g_free (s);
}

static void
on_found_none (WebKitFindController *fc, gpointer u)
{
    (void) u;
    Win *w = win_of (webkit_find_controller_get_web_view (fc));
    if (w && w->omni_mode == OMNI_FIND)
        gtk_label_set_text (GTK_LABEL (w->omnihint), "no match");
}

/* Enter in the search-keyword popup: "s https://host/?q={}". */
static void
omni_apply_search (Win *w)
{
    char *line = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (w->omnientry))));
    char *sp   = strchr (line, ' ');
    gboolean as_default = gtk_check_button_get_active (GTK_CHECK_BUTTON (w->omnialways));

    if (!sp || sp == line) {
        toast_show (w, "want: KEY URL, with {} where the words go", URL_TOAST_SECONDS);
        g_free (line);
        return;
    }

    *sp = '\0';
    char *url = g_strstrip (sp + 1);

    if (!*url) {
        toast_show (w, "want: KEY URL, with {} where the words go", URL_TOAST_SECONDS);
        g_free (line);
        return;
    }

    search_set (line, url, as_default);
    omni_hide (w);

    char *msg = g_strdup_printf ("search  %s  %s%s", line, url,
                                 as_default ? "  (default)" : "");
    toast_show (w, msg, URL_TOAST_SECONDS);

    g_free (msg);
    g_free (line);
}

/* Enter in the download-directory popup. */
static void
omni_finish_dldir (Win *w, gboolean replace_others)
{
    const char *uri   = webkit_web_view_get_uri (w->view);
    DlScope     scope = (DlScope) w->dl_pending_scope;
    char       *dir   = w->dl_pending_dir;
    char       *key   = scope == DL_SCOPE_PAGE ? g_strdup (uri)
                      : scope == DL_SCOPE_SITE ? uri_host (uri)
                                               : NULL;

    if (replace_others)
        dl_dir_clear_users (dir, key);

    dl_dir_make (dir);
    dl_dir_set (uri, dir, scope);
    dl_always_set (key, gtk_check_button_get_active (GTK_CHECK_BUTTON (w->omnialways)));
    dlrules_save ();

    char *msg = g_strdup_printf (scope == DL_SCOPE_PAGE ? "downloads for this page  %s"
                               : scope == DL_SCOPE_SITE ? "downloads for this site  %s"
                                                        : "downloads  %s", dir);

    w->dl_conflict = FALSE;
    g_clear_pointer (&w->dl_pending_dir, g_free);
    omni_hide (w);
    toast_show (w, msg, URL_TOAST_SECONDS);

    g_free (msg);
    g_free (key);
}

static void
omni_apply_dldir (Win *w)
{
    char       *dir   = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (w->omnientry))));
    const char *uri   = webkit_web_view_get_uri (w->view);
    DlScope     scope = (DlScope) gtk_drop_down_get_selected (GTK_DROP_DOWN (w->omniscope));

    if (!*dir) {
        g_free (dir);
        omni_hide (w);
        return;
    }

    g_free (w->dl_pending_dir);
    w->dl_pending_dir   = dir;          /* takes it */
    w->dl_pending_scope = scope;

    /* Somebody else is already pointing there. Both can, but say so
     * first: sharing a directory is usually meant, and sometimes not. */
    char      *key   = scope == DL_SCOPE_PAGE ? g_strdup (uri)
                     : scope == DL_SCOPE_SITE ? uri_host (uri)
                                              : NULL;
    GPtrArray *users = dl_dir_users (dir, key);

    if (users->len > 0) {
        char *who = elide (g_ptr_array_index (users, 0), 40);
        char *ask = users->len == 1
                  ? g_strdup_printf ("%s already goes there.  Enter drops that rule,  Esc keeps both.", who)
                  : g_strdup_printf ("%u rules already go there.  Enter drops them,  Esc keeps all.", users->len);

        gtk_label_set_text (GTK_LABEL (w->omnihint), ask);
        gtk_label_set_wrap (GTK_LABEL (w->omnihint), TRUE);
        gtk_label_set_xalign (GTK_LABEL (w->omnihint), 0.0);
        gtk_widget_set_hexpand (w->omnihint, TRUE);
        gtk_widget_set_halign (w->omnihint, GTK_ALIGN_FILL);
        gtk_widget_set_visible (w->omnihint, TRUE);
        gtk_widget_set_visible (w->omnientry, FALSE);
        gtk_widget_set_visible (w->omniscope, FALSE);
        gtk_widget_set_visible (w->omnialways, FALSE);
        gtk_button_set_label (GTK_BUTTON (w->omnisave), "Drop that rule  (Enter)");
        gtk_button_set_label (GTK_BUTTON (w->omnicancel), "Keep both  (Esc)");
        gtk_widget_set_visible (w->omnibuttons, TRUE);
        w->dl_conflict = TRUE;

        g_free (ask);
        g_free (who);
        g_ptr_array_free (users, TRUE);
        g_free (key);
        return;                          /* the popup stays up and asks */
    }

    g_ptr_array_free (users, TRUE);
    g_free (key);
    omni_finish_dldir (w, FALSE);
}

/* Enter and the Save button are the same thing, so they cannot drift. */
static void
omni_confirm (Win *w)
{
    if (w->omni_mode == OMNI_DLDIR && w->dl_conflict)
        omni_finish_dldir (w, TRUE);          /* drop the older rule */
    else if (w->omni_mode == OMNI_FIND)
        find_step (w, +1);
    else if (w->omni_mode == OMNI_DLDIR)
        omni_apply_dldir (w);
    else if (w->omni_mode == OMNI_SEARCH)
        omni_apply_search (w);
    else
        omni_go (w);
}

/* Esc and the second button, likewise. */
static void
omni_dismiss (Win *w)
{
    if (w->omni_mode == OMNI_DLDIR && w->dl_conflict)
        omni_finish_dldir (w, FALSE);         /* let both use it */
    else
        omni_hide (w);
}

static void
on_omni_activate (GtkEntry *entry, gpointer u)
{
    (void) entry;
    omni_confirm (u);
}

static void
on_omni_save (GtkButton *b, gpointer u)
{
    (void) b;
    omni_confirm (u);
}

static void
on_omni_cancel (GtkButton *b, gpointer u)
{
    (void) b;
    omni_dismiss (u);
}

static void
on_omni_changed (GtkEditable *e, gpointer u)
{
    Win *w = u;

    if (w->omni_setting)
        return;                        /* our own write, not the user's */

    g_free (w->omni_needle);
    w->omni_needle = g_strdup (gtk_editable_get_text (e));

    if (w->omni_mode == OMNI_FIND) {
        find_run (w);
        return;
    }

    /* Typing an address searches the history at the same time: the two
     * were never really different jobs. Enter still loads what is typed,
     * so a page that is not in the list is one keystroke away either way. */
    if (!w->omni_list && w->omni_mode == OMNI_URL && *w->omni_needle)
        omni_list_show (w);
    else if (w->omni_list)
        omni_rebuild (w);
}

static void
on_omni_row_activated (GtkListBox *list, GtkListBoxRow *row, gpointer u)
{
    (void) list;
    Win *w = u;

    omni_reflect (w, row);

    /* Only the address and history lists navigate. In the settings popups
     * a click picks a value to edit - loading a search template as if it
     * were an address is never what was meant. */
    if (w->omni_mode == OMNI_URL || w->omni_mode == OMNI_HISTORY)
        omni_go (w);
}

/* --------------------------------------------------------------- toast */

static gboolean
toast_timeout (gpointer u)
{
    Win *w = u;
    gtk_widget_set_visible (w->toast, FALSE);
    w->toast_id = 0;
    return G_SOURCE_REMOVE;
}

/* An ordinary message: one line, elided in the middle if it runs long. */
static void
toast_single_line (Win *w)
{
    GtkLabel *l = GTK_LABEL (w->toast);

    gtk_widget_remove_css_class (w->toast, "br-toast-url");
    gtk_widget_remove_css_class (w->toast, "br-toast-error");
    w->error_until_us = 0;
    gtk_label_set_wrap (l, FALSE);
    gtk_label_set_lines (l, -1);
    gtk_label_set_ellipsize (l, PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars (l, 90);
}

void
toast_show (Win *w, const char *text, guint seconds)
{
    toast_single_line (w);
    gtk_label_set_text (GTK_LABEL (w->toast), text);
    gtk_widget_set_visible (w->toast, TRUE);

    if (w->toast_id)
        g_source_remove (w->toast_id);
    w->toast_id = g_timeout_add_seconds (seconds, toast_timeout, w);
}

/*
 * Something the user asked for did not happen. A message that is cut in
 * the middle, gone in four seconds or faded under the pointer is no
 * message at all, so this one is wrapped whole, in the failure colour,
 * stays up for ERROR_TOAST_SECONDS, does not fade, and always goes to
 * stderr as well - -q or not.
 */
#define ERROR_TOAST_SECONDS 10

void
toast_error (Win *w, const char *text)
{
    g_printerr ("error: %s\n", text);
    if (!w)
        return;

    GtkLabel *l = GTK_LABEL (w->toast);

    toast_single_line (w);
    gtk_widget_add_css_class (w->toast, "br-toast-error");
    gtk_label_set_ellipsize (l, PANGO_ELLIPSIZE_NONE);
    gtk_label_set_wrap (l, TRUE);
    gtk_label_set_wrap_mode (l, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars (l, 60);
    gtk_label_set_xalign (l, 0.0);
    gtk_label_set_text (l, text);
    gtk_widget_set_opacity (w->topright, 1.0);
    gtk_widget_set_visible (w->toast, TRUE);
    w->error_until_us = g_get_monotonic_time () + (gint64) ERROR_TOAST_SECONDS * G_USEC_PER_SEC;

    if (w->toast_id)
        g_source_remove (w->toast_id);
    w->toast_id = g_timeout_add_seconds (ERROR_TOAST_SECONDS, toast_timeout, w);
}
/*
 * <mod>+P: the whole address. It is wrapped at any character, in the mono
 * font, and as wide as the window allows, so nothing is cut out. Only a
 * monster longer than URL_LINES lines loses its middle. It stays up longer
 * the longer it is, and the key again puts it away.
 */
#define URL_LINES 12

static void
toast_show_url (Win *w, const char *uri)
{
    GtkLabel *l = GTK_LABEL (w->toast);

    if (gtk_widget_get_visible (w->toast) &&
        gtk_widget_has_css_class (w->toast, "br-toast-url")) {
        if (w->toast_id)
            g_source_remove (w->toast_id);
        toast_timeout (w);
        return;
    }

    if (!uri || !*uri) {
        toast_show (w, "(no url)", URL_TOAST_SECONDS);
        return;
    }

    gtk_widget_add_css_class (w->toast, "br-toast-url");
    gtk_label_set_wrap (l, TRUE);
    gtk_label_set_wrap_mode (l, PANGO_WRAP_CHAR);
    gtk_label_set_lines (l, URL_LINES);
    gtk_label_set_ellipsize (l, PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars (l, 160);
    gtk_label_set_xalign (l, 0.0);
    gtk_label_set_text (l, uri);
    gtk_widget_set_visible (w->toast, TRUE);

    guint secs = CLAMP (URL_TOAST_SECONDS + strlen (uri) / 60, URL_TOAST_SECONDS, 15);
    if (w->toast_id)
        g_source_remove (w->toast_id);
    w->toast_id = g_timeout_add_seconds (secs, toast_timeout, w);
}

/* ---------------------------------------------------------------- zoom */

static void
zoom_by (Win *w, double factor)
{
    double z = webkit_web_view_get_zoom_level (w->view) * factor;

    z = CLAMP (z, ZOOM_MIN, ZOOM_MAX);
    webkit_web_view_set_zoom_level (w->view, z);

    char *msg = g_strdup_printf ("zoom %.0f%%", z * 100.0);
    toast_show (w, msg, 1);
    g_free (msg);
}

static void
zoom_reset (Win *w)
{
    webkit_web_view_set_zoom_level (w->view, 1.0);
    toast_show (w, "zoom 100%", 1);
}

/* ------------------------------------------------- download overlay */

/* One row: a progress bar with the filename in its text for a running
 * download, a plain line for a finished one. */
/*
 * One download, laid out in columns rather than as one run-on string:
 *
 *   some-long-file-name.tar.zst        45%  1.2 MB  12s
 *   [=================                                 ]
 *
 * The name takes the space that is left and elides in the middle; the
 * numbers are right aligned, so they line up down the panel and the eye
 * can read one column at a time.
 */
static GtkWidget *
dl_row_new (Dl *d)
{
    GtkWidget *row  = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    char      *name = elide (d->name, DL_NAME_CHARS);
    char      *status;

    GtkWidget *name_l = gtk_label_new (name);
    gtk_widget_add_css_class (name_l, d->state == DL_FAILED ? "br-dl-failed" : "br-dl-name");
    if (d->state == DL_ASK) {
        /* the old name is what the question is about */
        char *old = g_path_get_basename (d->want ? d->want : d->name);
        gtk_label_set_text (GTK_LABEL (name_l), old);
        g_free (old);
    }
    gtk_label_set_xalign (GTK_LABEL (name_l), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (name_l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand (name_l, TRUE);
    gtk_box_append (GTK_BOX (line), name_l);

    if (d->state == DL_ACTIVE) {
        char *eta = NULL;

        /* Before the first byte there is no percentage and no size worth
         * showing, but the row still has to say something. */
        if (d->received == 0) {
            status = g_strdup ("starting");
            goto have_status;
        }

        /* Needs a size, a measured rate, and enough elapsed time that the
         * rate means something - otherwise show no estimate at all rather
         * than a wrong one. */
        double elapsed = (g_get_monotonic_time () - d->start_us) / 1e6;

        if (d->total > d->received && d->rate > 0 && elapsed > 1.5)
            eta = format_seconds ((d->total - d->received) / d->rate);

        char *got = format_size (d->received);

        if (d->total > 0)
            status = g_strdup_printf ("%.0f%%  %s%s%s", d->progress * 100.0, got,
                                      eta ? "  " : "", eta ? eta : "");
        else
            status = g_strdup (got);     /* no Content-Length, no percentage */

        g_free (got);
        g_free (eta);
    have_status:
        ;
    } else if (d->state == DL_ASK) {
        status = g_strdup ("exists:  Enter replaces,  Esc keeps both");
    } else if (d->state == DL_DONE) {
        char *size = format_size (d->total ? d->total : d->received);
        char *took = format_seconds ((d->end_us - d->start_us) / 1e6);
        status = g_strdup_printf ("done  %s  %s", size, took);
        g_free (size);
        g_free (took);
    } else {
        status = g_strdup ("failed");
    }

    GtkWidget *status_l = gtk_label_new (status);
    gtk_widget_add_css_class (status_l,
                              d->state == DL_FAILED ? "br-dl-failed"
                            : d->state == DL_ASK    ? "br-dl-ask"
                            : d->state == DL_ACTIVE ? "br-dl-live"
                                                    : "br-dl-status");
    gtk_label_set_xalign (GTK_LABEL (status_l), 1.0);
    gtk_widget_set_halign (status_l, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (line), status_l);

    gtk_box_append (GTK_BOX (row), line);

    /* the bar carries no text of its own: the columns above say it all */
    if (d->state == DL_ACTIVE) {
        GtkWidget *bar = gtk_progress_bar_new ();
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (bar), CLAMP (d->progress, 0.0, 1.0));
        gtk_widget_add_css_class (bar, "br-dl-bar");
        gtk_box_append (GTK_BOX (row), bar);
    }

    g_free (status);
    g_free (name);
    return row;
}


/*
 * Enter and Esc answer the oldest download whose name was already taken.
 * The row carries the question, so there is nothing else to dismiss, and
 * the transfer is only reported as finished once the bytes are where they
 * are going to stay.
 */
static Dl *
dl_pending_ask (void)
{
    for (guint i = 0; i < g_downloads->len; i++) {
        Dl *d = g_ptr_array_index (g_downloads, i);
        if (d->ask)
            return d;
    }
    return NULL;
}

static gboolean
dl_answer_pending (gboolean overwrite)
{
    Dl *d = dl_pending_ask ();
    if (!d)
        return FALSE;

    d->ask       = FALSE;
    d->overwrite = overwrite;

    /* still running: dl_finish will move it when the last byte lands */
    if (d->state == DL_ASK) {
        if (overwrite && d->want && d->dest) {
            if (g_rename (d->dest, d->want) == 0) {
                LOG ("download: replaced %s\n", d->want);
                g_free (d->name);
                d->name = g_path_get_basename (d->want);
            } else {
                g_printerr ("download: cannot replace %s: %s\n",
                            d->want, g_strerror (errno));
            }
        }
        d->state = DL_DONE;
        d->end_us = g_get_monotonic_time ();
    }

    downloads_tick_start ();
    downloads_refresh ();
    return TRUE;
}

/*
 * The overlay fades out under the pointer so the page can be read through
 * it. A download that starts while the pointer happens to be parked there
 * would then arrive invisibly, which reads as the panel being slow. So a
 * new download un-fades it and outranks the hover for a few seconds.
 */
static void
downloads_reveal (void)
{
    gint64 now = g_get_monotonic_time ();

    for (guint i = 0; i < g_wins->len; i++) {
        Win *w = g_ptr_array_index (g_wins, i);
        w->dl_reveal_us = now;
        gtk_widget_set_opacity (w->topright, 1.0);
    }
}
static void
dl_panel_rebuild (Win *w)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child (w->dlpanel)))
        gtk_box_remove (GTK_BOX (w->dlpanel), child);

    gint64     now    = g_get_monotonic_time ();
    gint64     linger = (gint64) DL_LINGER_SECONDS * G_USEC_PER_SEC;
    GPtrArray *show   = g_ptr_array_new ();
    guint      active = 0;

    for (guint i = 0; i < g_downloads->len; i++)
        if (((Dl *) g_ptr_array_index (g_downloads, i))->state == DL_ACTIVE ||
            ((Dl *) g_ptr_array_index (g_downloads, i))->state == DL_ASK)
            active++;

    if (w->dl_history) {
        for (guint i = 0; i < g_downloads->len; i++)
            g_ptr_array_add (show, g_ptr_array_index (g_downloads, i));
        while (show->len > DL_HISTORY_ROWS)
            g_ptr_array_remove_index (show, 0);
    } else {
        for (guint i = 0; i < g_downloads->len; i++) {
            Dl *d = g_ptr_array_index (g_downloads, i);
            if (d->state == DL_ACTIVE || d->state == DL_ASK ||
                now - d->end_us < linger)
                g_ptr_array_add (show, d);
        }
        while (show->len > DL_ACTIVE_ROWS)
            g_ptr_array_remove_index (show, 0);
    }

    if (show->len == 0 && !w->dl_history) {
        gtk_widget_set_visible (w->dlpanel, FALSE);
        g_ptr_array_free (show, TRUE);
        return;
    }

    /* header: how many downloads there are */
    char *head;
    if (w->dl_history)
        head = g_downloads->len
             ? g_strdup_printf ("last downloads (%u)", g_downloads->len)
             : g_strdup ("no downloads yet");
    else if (active > 0)
        head = g_strdup_printf ("downloading %u of %u", active, g_downloads->len);
    else
        head = g_strdup_printf ("downloads (%u)", g_downloads->len);

    GtkWidget *headline = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *hl = gtk_label_new (head);
    gtk_widget_add_css_class (hl, "br-dl-head");
    gtk_label_set_xalign (GTK_LABEL (hl), 0.0);
    gtk_widget_set_hexpand (hl, TRUE);
    gtk_box_append (GTK_BOX (headline), hl);

    /* where these files are going, small, on the right - <mod>+S edits it */
    GtkWidget *dirl = gtk_label_new (dl_dir_for_uri (webkit_web_view_get_uri (w->view)));
    gtk_widget_add_css_class (dirl, "br-dl-dir");
    gtk_label_set_xalign (GTK_LABEL (dirl), 1.0);
    gtk_label_set_ellipsize (GTK_LABEL (dirl), PANGO_ELLIPSIZE_START);
    gtk_label_set_max_width_chars (GTK_LABEL (dirl), 28);
    gtk_widget_set_halign (dirl, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (headline), dirl);

    gtk_box_append (GTK_BOX (w->dlpanel), headline);
    g_free (head);

    for (guint i = 0; i < show->len; i++)
        gtk_box_append (GTK_BOX (w->dlpanel), dl_row_new (g_ptr_array_index (show, i)));

    gtk_widget_set_visible (w->dlpanel, TRUE);
    g_ptr_array_free (show, TRUE);
}

static void
downloads_refresh (void)
{
    for (guint i = 0; i < g_wins->len; i++)
        dl_panel_rebuild (g_ptr_array_index (g_wins, i));
}

/* Runs only while something is on screen: any active download, or a
 * finished one still inside its linger window. */
static gboolean
dl_tick (gpointer u)
{
    (void) u;

    gint64   now    = g_get_monotonic_time ();
    gint64   linger = (gint64) DL_LINGER_SECONDS * G_USEC_PER_SEC;
    gboolean busy   = FALSE;

    for (guint i = 0; i < g_downloads->len && !busy; i++) {
        Dl *d = g_ptr_array_index (g_downloads, i);
        busy = (d->state == DL_ACTIVE) || (d->state == DL_ASK) ||
               (now - d->end_us < linger);
    }

    downloads_refresh ();

    if (busy)
        return G_SOURCE_CONTINUE;

    g_dl_tick = 0;
    return G_SOURCE_REMOVE;
}

static void
downloads_tick_start (void)
{
    if (!g_dl_tick)
        g_dl_tick = g_timeout_add (DL_TICK_MS, dl_tick, NULL);
}

static gboolean
dl_history_timeout (gpointer u)
{
    Win *w = u;
    w->dl_history    = FALSE;
    w->dl_history_id = 0;
    dl_panel_rebuild (w);
    return G_SOURCE_REMOVE;
}

/* <mod>+D: show the recent downloads, press again to dismiss. */
static void
downloads_toggle_history (Win *w)
{
    if (w->dl_history_id) {
        g_source_remove (w->dl_history_id);
        w->dl_history_id = 0;
    }

    w->dl_history = !w->dl_history;

    if (w->dl_history)
        w->dl_history_id = g_timeout_add_seconds (DL_HISTORY_SECONDS,
                                                  dl_history_timeout, w);
    dl_panel_rebuild (w);
}

/* The overlay sits over the page, so fade it out while the pointer is on
 * top of it - there may be something underneath worth reading. It is not
 * click-targetable either, so this only affects what you see. */
/* A panel you cannot click is in the way if the pointer is over it, so it
 * gets out of the way. Anything interactive keeps its opacity. */
static void
fade_if_under (Win *w, GtkWidget *panel, double x, double y, gboolean keep)
{
    graphene_rect_t  bounds;
    graphene_point_t point = GRAPHENE_POINT_INIT ((float) x, (float) y);
    double           want  = 1.0;

    if (!keep && gtk_widget_compute_bounds (panel, w->win, &bounds)) {
        graphene_rect_inset (&bounds, -8.0f, -8.0f);      /* a little margin */
        if (bounds.size.width > 1 && graphene_rect_contains_point (&bounds, &point))
            want = 0.0;
    }

    if (gtk_widget_get_opacity (panel) != want)
        gtk_widget_set_opacity (panel, want);
}

static void
overlay_hover_update (Win *w, double x, double y)
{
    /* a download that just arrived outranks the fade for a few seconds */
    gboolean keep_dl = w->dl_reveal_us &&
                       g_get_monotonic_time () - w->dl_reveal_us
                       < (gint64) DL_REVEAL_MS * 1000;

    gboolean keep_err = w->error_until_us && g_get_monotonic_time () < w->error_until_us;

    fade_if_under (w, w->topright, x, y, keep_dl || keep_err);
    fade_if_under (w, w->urltoast, x, y, FALSE);
}

static void
on_motion (GtkEventControllerMotion *c, double x, double y, gpointer u)
{
    (void) c;
    overlay_hover_update (u, x, y);
}

static void
on_motion_leave (GtkEventControllerMotion *c, gpointer u)
{
    (void) c;
    Win *w = u;

    gtk_widget_set_opacity (w->topright, 1.0);
    gtk_widget_set_opacity (w->urltoast, 1.0);
}

/* ------------------------------------------------------------- windows */

Win *
win_of (WebKitWebView *view)
{
    return view ? g_object_get_data (G_OBJECT (view), "win") : NULL;
}

void
view_eval (WebKitWebView *view, const char *js)
{
    webkit_web_view_evaluate_javascript (view, js, -1, NULL, NULL, NULL, NULL, NULL);
}

/* The page's own scrolling element, not the window: that is what actually
 * moves on a document with a scrolling body. */
static void
scroll_edge (Win *w, gboolean bottom)
{
    view_eval (w->view,
               bottom
               ? "(function(){var e=document.scrollingElement||document.documentElement;"
                 "e.scrollTo(0,e.scrollHeight);})();"
               : "(function(){var e=document.scrollingElement||document.documentElement;"
                 "e.scrollTo(0,0);})();");
}

static void keys_toggle (Win *w);

/* Esc closes whatever is on screen, from the front backwards. */
static gboolean
close_popups (Win *w)
{
    gboolean closed = FALSE;

    if (gtk_widget_get_visible (w->omni)) {
        omni_hide (w);
        closed = TRUE;
    }
    if (gtk_widget_get_visible (w->keys)) {
        gtk_widget_set_visible (w->keys, FALSE);
        closed = TRUE;
    }
    if (w->dl_history) {
        if (w->dl_history_id) {
            g_source_remove (w->dl_history_id);
            w->dl_history_id = 0;
        }
        w->dl_history = FALSE;
        dl_panel_rebuild (w);
        closed = TRUE;
    }
    if (gtk_widget_get_visible (w->toast)) {
        if (w->toast_id) {
            g_source_remove (w->toast_id);
            w->toast_id = 0;
        }
        gtk_widget_set_visible (w->toast, FALSE);
        closed = TRUE;
    }
    if (gtk_widget_get_visible (w->urltoast)) {
        if (w->urltoast_id) {
            g_source_remove (w->urltoast_id);
            w->urltoast_id = 0;
        }
        gtk_widget_set_visible (w->urltoast, FALSE);
        closed = TRUE;
    }

    return closed;
}

static gboolean
on_key (GtkEventControllerKey *c, guint keyval, guint code,
        GdkModifierType state, gpointer user_data)
{
    (void) c; (void) code;

    Win     *w     = user_data;
    guint    key   = gdk_keyval_to_lower (keyval);
    gboolean mod   = (state & MOD_MASK_ALL) == g_mod;   /* exactly our modifier */
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;

    /* While the popup is open it owns the keyboard, apart from the few
     * keys that drive it. Everything else reaches the input, so typing
     * filters and <mod>+A still selects the text in it. */
    /* Esc belongs to whatever is on screen; with nothing up it is the
     * page's own key, so it is passed on. */
    /* the directory popup, mid-question: both answers apply the setting,
     * they only differ on what happens to the rule already pointing there */
    if (gtk_widget_get_visible (w->omni) && w->omni_mode == OMNI_DLDIR && w->dl_conflict) {
        if (keyval == GDK_KEY_Escape) {
            omni_finish_dldir (w, FALSE);      /* keep both */
            return TRUE;
        }
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            omni_finish_dldir (w, TRUE);       /* drop the other rule */
            return TRUE;
        }
        return TRUE;
    }

    /* the permission prompt owns Enter and Esc while it is up */
    if (gtk_widget_get_visible (w->perm)) {
        if (keyval == GDK_KEY_Escape) {
            perm_answer (w, FALSE);
            return TRUE;
        }
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            perm_answer (w, TRUE);
            return TRUE;
        }
    }

    if (keyval == GDK_KEY_Escape) {
        if (close_popups (w))
            return TRUE;
        return dl_answer_pending (FALSE);      /* keep both */
    }

    /* an unanswered download question owns Enter, but only when nothing
     * of ours is on screen to type into */
    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
        !gtk_widget_get_visible (w->omni) && !gtk_widget_get_visible (w->keys) &&
        dl_answer_pending (TRUE))
        return TRUE;

    if (gtk_widget_get_visible (w->keys)) {
        if (keyval == GDK_KEY_F1 ||
            (mod && (key == GDK_KEY_slash || key == GDK_KEY_question))) {
            gtk_widget_set_visible (w->keys, FALSE);
            return TRUE;
        }
        return FALSE;
    }

    if (gtk_widget_get_visible (w->omni)) {
        /* Select-all, whatever GTK's key theme has to say about it: with
         * the Emacs bindings <mod>+A is beginning-of-line, which is not
         * what anyone expects from a text field. */
        if (mod && !shift && key == GDK_KEY_a) {
            gtk_editable_select_region (GTK_EDITABLE (w->omnientry), 0, -1);
            return TRUE;
        }


        if ((mod && ((key == GDK_KEY_h && w->omni_mode == OMNI_HISTORY) ||
                     (key == GDK_KEY_f && w->omni_mode == OMNI_FIND) ||
                     (key == GDK_KEY_s && w->omni_mode == OMNI_DLDIR) ||
                     (key == GDK_KEY_k && w->omni_mode == OMNI_SEARCH)))) {
            omni_hide (w);
            return TRUE;
        }

        /* finding: several ways to walk the hits, since the hands are
         * already on the keyboard and everyone reaches for a different one */
        if (w->omni_mode == OMNI_FIND) {
            if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
                (mod && key == GDK_KEY_n)) {
                find_step (w, shift ? -1 : +1);
                return TRUE;
            }
            if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
                find_step (w, +1);
                return TRUE;
            }
            if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
                find_step (w, -1);
                return TRUE;
            }
            return FALSE;
        }

        /* the keyword popup has no list to steer */
        if (w->omni_mode == OMNI_SEARCH)
            return FALSE;

        /* Delete is missing from plenty of keyboards, so <mod>+X does the
         * same thing wherever an entry can be dropped. */
        if (keyval == GDK_KEY_Delete || keyval == GDK_KEY_KP_Delete ||
            (mod && key == GDK_KEY_x)) {
            omni_delete_selected (w);
            return TRUE;
        }
        if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_KP_Tab) {
            omni_move (w, shift ? -1 : +1);
            return TRUE;
        }
        if (keyval == GDK_KEY_ISO_Left_Tab) {     /* Shift+Tab on X11 */
            omni_move (w, -1);
            return TRUE;
        }
        if (keyval == GDK_KEY_Up   || keyval == GDK_KEY_KP_Up)   { omni_move (w, -1); return TRUE; }
        if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) { omni_move (w, +1); return TRUE; }
        return FALSE;
    }

    /* --- unmodified function keys ------------------------------------ */
    if (keyval == GDK_KEY_F1) {
        keys_toggle (w);
        return TRUE;
    }
    if (keyval == GDK_KEY_F2) {
        media_mode_toggle (w);
        return TRUE;
    }
    if (keyval == GDK_KEY_F12) {
        inspector_toggle (w->view);
        return TRUE;
    }
    if (keyval == GDK_KEY_F5) {
        webkit_web_view_reload (w->view);
        return TRUE;
    }
    if (keyval == GDK_KEY_F6) {
        view_kick (w);
        return TRUE;
    }

    if (!mod)
        return FALSE;

    /* the front-end gets first refusal on everything under our modifier */
    if (g_app->key && g_app->key (w, key, shift))
        return TRUE;

    /* --- <mod>+Shift+key --------------------------------------------- */
    if (shift) {
        switch (key) {
        case GDK_KEY_j:                   /* <mod>+J goes back */
            history_go (w, +1);
            return TRUE;
        case GDK_KEY_g:
            scroll_edge (w, TRUE);
            return TRUE;
        case GDK_KEY_i:
        case GDK_KEY_d:                   /* <mod>+D itself lists downloads */
            inspector_toggle (w->view);
            return TRUE;
        case GDK_KEY_r:
            css_reload ();
            webkit_web_view_reload (w->view);
            return TRUE;
        default:
            break;                        /* fall through to zoom keys */
        }
    }

    /* --- <mod>+key ---------------------------------------------------- */
    switch (key) {
    case GDK_KEY_j:
        history_go (w, -1);
        return TRUE;

    case GDK_KEY_h:
        omni_toggle (w, OMNI_HISTORY);
        return TRUE;

    case GDK_KEY_f:
        omni_toggle (w, OMNI_FIND);
        return TRUE;

    case GDK_KEY_s:
        omni_toggle (w, OMNI_DLDIR);
        return TRUE;

    case GDK_KEY_k:
        omni_toggle (w, OMNI_SEARCH);
        return TRUE;

    case GDK_KEY_slash:
    case GDK_KEY_question:
        keys_toggle (w);
        return TRUE;

    case GDK_KEY_g:
        scroll_edge (w, FALSE);
        return TRUE;

    case GDK_KEY_o:
    case GDK_KEY_l:                       /* the usual browser shortcut */
        omni_show (w, OMNI_URL);
        return TRUE;

    case GDK_KEY_r:
        if (shift)
            break;
        webkit_web_view_reload (w->view);
        return TRUE;

    case GDK_KEY_d:
        downloads_toggle_history (w);
        return TRUE;

    case GDK_KEY_p:
        toast_show_url (w, webkit_web_view_get_uri (w->view));
        return TRUE;

    case GDK_KEY_y:
        clipboard_copy (w, webkit_web_view_get_uri (w->view));
        return TRUE;

    /* WebKitGTK maps <mod>+A to MoveToBeginningOfLine, an Emacs habit that
     * surprises everyone typing into a web text field. The editing command
     * is what the key is supposed to do, so run that instead. Turn it off
     * with select_all=no for pages that bind <mod>+A themselves. */
    case GDK_KEY_a:
        if (shift || !g_fix_select_all)
            break;
        webkit_web_view_execute_editing_command (w->view,
                                                 WEBKIT_EDITING_COMMAND_SELECT_ALL);
        return TRUE;

    /* '+' usually needs Shift, and some layouts send '=' or the keypad key */
    case GDK_KEY_plus:
    case GDK_KEY_equal:
    case GDK_KEY_KP_Add:
        zoom_by (w, ZOOM_STEP);
        return TRUE;

    case GDK_KEY_minus:
    case GDK_KEY_underscore:
    case GDK_KEY_KP_Subtract:
        zoom_by (w, 1.0 / ZOOM_STEP);
        return TRUE;

    case GDK_KEY_0:
    case GDK_KEY_KP_0:
        zoom_reset (w);
        return TRUE;

    default:
        break;
    }

    return FALSE;                         /* not ours - hand it to the page */
}

/* --------------------------------------------------------- key reference */

/*
 * F1 or <mod>+/ drops the key list on screen, grouped the way the keys
 * are actually used rather than alphabetically. The front-end's own keys
 * are taken from its usage_keys() text, so the panel and --help can never
 * drift apart.
 */

typedef struct {
    const char *key;        /* NULL starts a new section, desc is its name */
    const char *desc;
} KeyRow;

static const KeyRow g_keyrows[] = {
    { NULL, "go" },
    { "%s+O",         "type an address, or \"s words\" to search" },
    { "%s+H",         "history" },
    { "%s+J",         "back" },
    { "%s+Shift+J",   "forward" },
    { "%s+R",         "reload" },

    { NULL, "in the popup" },
    { "Tab / Down",   "next match" },
    { "Shift+Tab / Up", "previous match" },
    { "Enter",        "open" },
    { "Delete / %s+X", "remove the entry under the cursor" },
    { "Esc",          "close" },

    { NULL, "page" },
    { "%s+F",         "find in the page" },
    { "Enter / Down / %s+N", "next match, while finding" },
    { "Shift+Enter / Up",    "previous match" },
    { "%s+G",         "scroll to the top" },
    { "%s+Shift+G",   "scroll to the bottom" },
    { "%s+plus / minus / 0", "zoom in, out, reset" },
    { "%s+P",         "show the current URL" },
    { "%s+Y",         "copy the current URL" },

    { NULL, "files" },
    { "%s+D",         "recent downloads" },
    { "%s+S",         "download directory: this page, this site, or all" },
    { "%s+K",         "add a search keyword" },

    { NULL, "tools" },
    { "F12 / %s+Shift+I", "developer tools" },
    { "%s+Shift+R",   "re-read the --css file, then reload" },
    { "F1 / %s+/",    "this list" },
    { "F2",           "media mode: video to the player, image to the viewer" },
};

static void
keys_row_add (GtkGrid *grid, int *row, const char *key, const char *desc)
{
    GtkWidget *k = gtk_label_new (key);
    gtk_widget_add_css_class (k, "br-key");
    gtk_label_set_xalign (GTK_LABEL (k), 0.0);
    gtk_widget_set_valign (k, GTK_ALIGN_START);
    gtk_grid_attach (grid, k, 0, *row, 1, 1);

    GtkWidget *d = gtk_label_new (desc);
    gtk_widget_add_css_class (d, "br-key-desc");
    gtk_label_set_xalign (GTK_LABEL (d), 0.0);
    gtk_label_set_wrap (GTK_LABEL (d), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (d), 46);
    gtk_grid_attach (grid, d, 1, *row, 1, 1);

    (*row)++;
}

static void
keys_head_add (GtkGrid *grid, int *row, const char *name)
{
    GtkWidget *h = gtk_label_new (name);
    gtk_widget_add_css_class (h, "br-key-head");
    gtk_label_set_xalign (GTK_LABEL (h), 0.0);
    gtk_widget_set_margin_top (h, *row ? (int) g_theme.pad : 0);
    gtk_grid_attach (grid, h, 0, *row, 2, 1);
    (*row)++;
}

/* The front-end writes its keys as "  <mod>+Shift+M   what it does", with
 * a run of spaces between the two. Continuation lines have no key. */
static void
keys_add_app_rows (GtkGrid *grid, int *row)
{
    if (!g_app->usage_keys)
        return;

    GString *s = g_string_new (NULL);
    g_app->usage_keys (s);

    char **lines = g_strsplit (s->str, "\n", -1);
    gboolean titled = FALSE;

    for (int i = 0; lines[i]; i++) {
        char *line = lines[i];
        if (!*g_strchug (line))
            continue;

        char *sep = strstr (line, "  ");
        if (!sep)
            continue;

        char *key  = g_strstrip (g_strndup (line, (gsize) (sep - line)));
        char *desc = g_strstrip (g_strdup (sep));

        if (*key && *desc) {
            if (!titled) {
                keys_head_add (grid, row, "this browser");
                titled = TRUE;
            }
            keys_row_add (grid, row, key, desc);
        }

        g_free (key);
        g_free (desc);
    }

    g_strfreev (lines);
    g_string_free (s, TRUE);
}

static GtkWidget *
keys_panel_new (void)
{
    GtkWidget *grid = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid), (int) g_theme.gap);
    gtk_grid_set_row_spacing (GTK_GRID (grid), (int) g_theme.win_gap);

    int row = 0;
    for (gsize i = 0; i < G_N_ELEMENTS (g_keyrows); i++) {
        const KeyRow *r = &g_keyrows[i];

        if (!r->key) {
            keys_head_add (GTK_GRID (grid), &row, r->desc);
            continue;
        }

        char *key = g_strdup_printf (r->key, g_mod_name);
        keys_row_add (GTK_GRID (grid), &row, key, r->desc);
        g_free (key);
    }

    keys_add_app_rows (GTK_GRID (grid), &row);

    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_widget_add_css_class (scroll, "br-pick-scroll");
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), grid);

    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), 460);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, (int) g_theme.win_gap);
    gtk_widget_add_css_class (box, "br-keys");
    gtk_widget_set_halign (box, GTK_ALIGN_FILL);
    gtk_widget_set_valign (box, GTK_ALIGN_FILL);
    gtk_widget_set_visible (box, FALSE);
    gtk_box_append (GTK_BOX (box), scroll);

    return box;
}

static void
keys_toggle (Win *w)
{
    gboolean on = !gtk_widget_get_visible (w->keys);

    if (on)
        omni_hide (w);                  /* one thing on screen at a time */

    gtk_widget_set_visible (w->keys, on);
}

static void
on_close (GtkWindow *win, gpointer user_data)
{
    (void) win;
    Win *w = user_data;

    if (w->primary || --g_windows <= 0) {
        GMainLoop *loop = g_object_get_data (G_OBJECT (w->win), "loop");
        if (loop)
            g_main_loop_quit (loop);
    }
}

static void
win_free (gpointer data)
{
    Win *w = data;

    if (g_app->win_gone)
        g_app->win_gone (w);

    g_ptr_array_remove_fast (g_wins, w);

    if (w->toast_id)
        g_source_remove (w->toast_id);
    if (w->dl_history_id)
        g_source_remove (w->dl_history_id);
    if (w->hist_id)
        g_source_remove (w->hist_id);
    if (w->urltoast_id)
        g_source_remove (w->urltoast_id);
    g_free (w->omni_needle);
    g_free (w->dl_pending_dir);
    g_free (w->hover_link);
    g_free (w->hover_image);
    g_free (w->hover_media);

    g_free (w);
}

static void
on_title (GObject *obj, GParamSpec *ps, gpointer user_data)
{
    (void) ps;
    Win        *w = user_data;
    const char *t = webkit_web_view_get_title (WEBKIT_WEB_VIEW (obj));
    gtk_window_set_title (GTK_WINDOW (w->win), (t && *t) ? t : g_title);
}

/* --------------------------------------------------------------- app id */

/* Wayland: the xdg_toplevel app_id, i.e. what `swaymsg -t get_tree` shows
 * as app_id and what sway's `for_window [app_id="..."]` rules match on.
 * X11:     the WM_CLASS pair.
 *
 * Both come from g_get_prgname() unless a GtkApplication supplies an id;
 * with prgname unset a Wayland toplevel falls back to the literal "GTK
 * Application". So g_set_prgname() in browser_main() is what fixes this.
 *
 * X11 needs nothing further: a toplevel takes WM_CLASS's res_name from
 * prgname and falls back to it for res_class too, so calling
 * gdk_x11_display_set_program_class() would only repeat what prgname
 * already did - and it is deprecated since GTK 4.18. */
static void
on_window_map (GtkWidget *win, gpointer u)
{
    (void) win; (void) u;

#ifdef GDK_WINDOWING_WAYLAND
    GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (win));
    if (surface && GDK_IS_WAYLAND_TOPLEVEL (surface))
        gdk_wayland_toplevel_set_application_id (GDK_TOPLEVEL (surface), g_app_id);
#endif
}

/* Middle click pastes the primary selection into whatever is focused.
 * Claiming the press in the capture phase means the page never sees the
 * button at all, which is the only reliable way to stop that: WebKitGTK
 * has no setting for it. --disable-middle-click-paste. */
static void
on_middle_press (GtkGestureClick *g, int n, double x, double y, gpointer u)
{
    (void) n; (void) x; (void) y; (void) u;
    gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* --- media mode: F2, the frame, and the click that goes outside ---------- */

static void
media_mode_set (gboolean on)
{
    g_media_mode = on;
    for (guint i = 0; g_wins && i < g_wins->len; i++) {
        Win *x = g_ptr_array_index (g_wins, i);
        if (x->mediaframe)
            gtk_widget_set_visible (x->mediaframe, on);
    }
}

static void
media_mode_toggle (Win *w)
{
    if ((!g_player || !*g_player) && (!g_image_viewer || !*g_image_viewer)) {
        char *cfg = g_build_filename (g_get_user_config_dir (), g_app->default_app_id,
                                      "config", NULL);
        char *msg = g_strdup_printf ("F2 media mode: no player or image viewer is set.\n"
                                     "Add to %s:\n"
                                     "    player = mpv\n"
                                     "    image_viewer = imv\n"
                                     "or start with --player mpv", cfg);
        toast_error (w, msg);
        g_free (msg);
        g_free (cfg);
        return;
    }

    media_mode_set (!g_media_mode);

    char *msg;
    if (!g_media_mode)
        msg = g_strdup ("media mode off");
    else
        msg = g_strdup_printf ("media mode: %s+click  video -> %s   image -> %s",
                               g_mod_name,
                               g_player && *g_player ? g_player : "(none)",
                               g_image_viewer && *g_image_viewer ? g_image_viewer : "(none)");
    toast_show (w, msg, URL_TOAST_SECONDS);
    LOG ("media: mode %s\n", g_media_mode ? "on" : "off");
    g_free (msg);
}

static void
on_mouse_target (WebKitWebView *view, WebKitHitTestResult *hit, guint mods, gpointer u)
{
    (void) view; (void) mods;
    Win *w = u;

    g_clear_pointer (&w->hover_link,  g_free);
    g_clear_pointer (&w->hover_image, g_free);
    g_clear_pointer (&w->hover_media, g_free);

    if (webkit_hit_test_result_context_is_link (hit))
        w->hover_link = g_strdup (webkit_hit_test_result_get_link_uri (hit));
    if (webkit_hit_test_result_context_is_image (hit))
        w->hover_image = g_strdup (webkit_hit_test_result_get_image_uri (hit));
    if (webkit_hit_test_result_context_is_media (hit))
        w->hover_media = g_strdup (webkit_hit_test_result_get_media_uri (hit));
}

/*
 * The <mod>+click itself, taken before the page sees it. What is under the
 * pointer decides: a link to a video or an image goes by its address, a
 * link to anything else leaves the image inside it to the viewer, a bare
 * image goes to the viewer, a <video> with a real address to the player.
 * Nothing that fits: the click is the page's, as usual.
 */
static void
on_media_press (GtkGestureClick *g, int n, double x, double y, gpointer u)
{
    Win *w = u;

    if (!g_media_mode || n != 1)
        return;
    GdkModifierType st = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (g));
    if (!(st & g_mod))
        return;

    GtkWidget *hit = gtk_widget_pick (w->win, x, y, GTK_PICK_DEFAULT);
    if (!hit || (hit != GTK_WIDGET (w->view) && !gtk_widget_is_ancestor (hit, GTK_WIDGET (w->view))))
        return;                           /* on one of our panels */

    gboolean done = FALSE;
    if (w->hover_link && (is_image_uri (w->hover_link) || is_video_uri (w->hover_link)))
        done = media_link (w, w->hover_link);
    if (!done && w->hover_image)
        done = image_open (w, w->hover_image);
    if (!done && !w->hover_link && w->hover_media)
        done = video_open (w, w->hover_media);

    if (done)
        gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
}

/*
 * Place our overlays ourselves, so they survive a window that is narrower
 * or shorter than they would like: everything is clamped to what the
 * window actually has, and the popup is centered in whatever is left.
 */
/*
 * Drawn by hand. A GtkProgressBar kept reporting a negative minimum width
 * for its inner node once its padding and height were overridden, and
 * anything that changes size on every progress tick re-allocates the
 * overlay - which re-lays-out the page. This changes pixels only.
 */
static void
draw_loadbar (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer u)
{
    (void) area;
    Win   *w = u;
    Color  c = g_theme.hl;

    if (w->load_frac <= 0.0)
        return;

    cairo_set_source_rgba (cr, c.r, c.g, c.b, c.a);
    cairo_rectangle (cr, 0, 0, width * CLAMP (w->load_frac, 0.0, 1.0), height);
    cairo_fill (cr);
}

static gboolean
on_overlay_position (GtkOverlay *ov, GtkWidget *child, GdkRectangle *alloc, gpointer u)
{
    Win *w      = u;
    int  W      = gtk_widget_get_width (GTK_WIDGET (ov));
    int  H      = gtk_widget_get_height (GTK_WIDGET (ov));
    int  margin = (int) (g_theme.margin * g_theme.ui_scale);
    int  want, min_h, nat_h;

    if (W <= 0 || H <= 0)
        return FALSE;

    /*
     * One rule, so there is nothing to learn:
     *
     *   things you type into      top, centred        omni, key list
     *   things that just tell you top right corner    toast, downloads
     *   where you are going      bottom left corner   the address label
     *
     * The corners that touch an edge stay square, and only the panels you
     * cannot interact with fade out under the pointer.
     */
    if (child == w->omni || child == w->keys || child == w->perm)
        want = (int) (g_theme.popup_w * g_theme.ui_scale);
    else if (child == w->topright)
        want = (int) (360 * g_theme.ui_scale);
    else if (child == w->urltoast)
        want = (int) (g_theme.popup_w * g_theme.ui_scale);
    else
        return FALSE;                   /* not ours, let GTK align it */

    int width = MIN (want, W - 2 * margin);
    if (width < 120)
        width = W;                      /* very narrow: take what there is */

    gtk_widget_measure (child, GTK_ORIENTATION_VERTICAL, width,
                        &min_h, &nat_h, NULL, NULL);

    int height = MIN (nat_h, H);
    if (height < min_h)
        height = MIN (min_h, H);

    alloc->width  = width;
    alloc->height = height;

    if (child == w->topright) {
        alloc->x = W - width;           /* pinned to the top right */
        alloc->y = 0;
    } else if (child == w->urltoast) {
        int nat_w, min_w;
        gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1,
                            &min_w, &nat_w, NULL, NULL);
        alloc->width = MIN (nat_w, width);
        alloc->x     = 0;               /* bottom left, where a browser
                                         * puts the address it is opening */
        alloc->y     = H - height;
    } else {
        alloc->x = (W - width) / 2;     /* centred, hanging off the top */
        alloc->y = 0;
    }

    return TRUE;
}

/* Dismiss the popup when the click lands anywhere else. The
 * press is claimed so it does not also follow a link underneath. */
static void
on_click_away (GtkGestureClick *g, int n, double x, double y, gpointer u)
{
    (void) n;
    Win *w = u;

    GtkWidget *open_one = gtk_widget_get_visible (w->omni) ? w->omni
                        : gtk_widget_get_visible (w->keys) ? w->keys
                        : NULL;
    if (!open_one)
        return;

    graphene_rect_t  bounds;
    graphene_point_t point = GRAPHENE_POINT_INIT ((float) x, (float) y);

    if (gtk_widget_compute_bounds (open_one, w->win, &bounds) &&
        graphene_rect_contains_point (&bounds, &point))
        return;                          /* inside, let the panel have it */

    if (open_one == w->keys)
        gtk_widget_set_visible (w->keys, FALSE);
    else
        omni_hide (w);

    gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
}

static GtkWidget *
window_new (WebKitWebView *view, gboolean primary)
{
    Win *w = g_new0 (Win, 1);
    w->view     = view;
    w->primary  = primary;
    w->win      = gtk_window_new ();
    w->hist_pos = (int) g_hist->len - 1;

    gtk_window_set_default_size (GTK_WINDOW (w->win), 1280, 800);
    gtk_window_set_title (GTK_WINDOW (w->win), g_title);

    GtkWidget *overlay = gtk_overlay_new ();
    gtk_overlay_set_child (GTK_OVERLAY (overlay), GTK_WIDGET (view));

    /* The popup: centered, hanging off the top edge, input first and the
     * history under it. Only its bottom corners are rounded, the top ones
     * sit flush against the window edge. */
    w->omni = gtk_box_new (GTK_ORIENTATION_VERTICAL, (int) g_theme.win_gap);
    gtk_widget_add_css_class (w->omni, "br-omni");
    /* FILL, not CENTER: on_overlay_position hands this widget an exact
     * rectangle, and a centered child would shrink to its content inside
     * it instead of taking the width we worked out. */
    gtk_widget_set_halign (w->omni, GTK_ALIGN_FILL);
    gtk_widget_set_valign (w->omni, GTK_ALIGN_START);
    gtk_widget_set_size_request (w->omni, -1, -1);
    gtk_widget_set_visible (w->omni, FALSE);

    GtkWidget *inputline = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);

    w->omnientry = gtk_entry_new ();
    gtk_entry_set_has_frame (GTK_ENTRY (w->omnientry), FALSE);
    gtk_widget_add_css_class (w->omnientry, "br-omni-entry");
    gtk_widget_set_hexpand (w->omnientry, TRUE);
    g_signal_connect (w->omnientry, "activate", G_CALLBACK (on_omni_activate), w);
    g_signal_connect (w->omnientry, "changed",  G_CALLBACK (on_omni_changed), w);
    gtk_box_append (GTK_BOX (inputline), w->omnientry);

    /* match count while finding, empty otherwise */
    w->omnihint = gtk_label_new ("");
    gtk_widget_add_css_class (w->omnihint, "br-pick-sub");
    gtk_widget_set_halign (w->omnihint, GTK_ALIGN_END);
    gtk_widget_set_visible (w->omnihint, FALSE);
    gtk_box_append (GTK_BOX (inputline), w->omnihint);

    gtk_box_append (GTK_BOX (w->omni), inputline);

    /* only shown when a download directory is being set */
    w->omniscope = gtk_drop_down_new (NULL, NULL);
    gtk_widget_add_css_class (w->omniscope, "br-scope");
    gtk_widget_set_visible (w->omniscope, FALSE);
    gtk_box_append (GTK_BOX (w->omni), w->omniscope);

    w->omnialways = gtk_check_button_new_with_label ("always replace existing files");
    gtk_widget_add_css_class (w->omnialways, "br-check");
    gtk_widget_set_visible (w->omnialways, FALSE);
    gtk_box_append (GTK_BOX (w->omni), w->omnialways);

    /* the confirm row: the same two actions Enter and Esc perform */
    /* one at each end: they are opposite answers, not a pair of options */
    w->omnibuttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, (int) g_theme.win_gap);
    gtk_widget_set_halign (w->omnibuttons, GTK_ALIGN_FILL);
    gtk_widget_set_visible (w->omnibuttons, FALSE);

    w->omnicancel = gtk_button_new_with_label ("Cancel  (Esc)");
    gtk_widget_add_css_class (w->omnicancel, "flat");
    gtk_widget_add_css_class (w->omnicancel, "br-btn");
    gtk_widget_set_halign (w->omnicancel, GTK_ALIGN_START);
    gtk_widget_set_hexpand (w->omnicancel, TRUE);
    g_signal_connect (w->omnicancel, "clicked", G_CALLBACK (on_omni_cancel), w);
    gtk_box_append (GTK_BOX (w->omnibuttons), w->omnicancel);

    w->omnisave = gtk_button_new_with_label ("Save  (Enter)");
    gtk_widget_add_css_class (w->omnisave, "flat");
    gtk_widget_add_css_class (w->omnisave, "br-btn");
    gtk_widget_set_halign (w->omnisave, GTK_ALIGN_END);
    gtk_widget_add_css_class (w->omnisave, "br-btn-primary");
    g_signal_connect (w->omnisave, "clicked", G_CALLBACK (on_omni_save), w);
    gtk_box_append (GTK_BOX (w->omnibuttons), w->omnisave);

    gtk_box_append (GTK_BOX (w->omni), w->omnibuttons);

    w->omnilist = gtk_list_box_new ();
    gtk_widget_add_css_class (w->omnilist, "br-pick-list");
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (w->omnilist), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (w->omnilist), TRUE);
    g_signal_connect (w->omnilist, "row-activated", G_CALLBACK (on_omni_row_activated), w);

    w->omniscroll = gtk_scrolled_window_new ();
    gtk_widget_add_css_class (w->omniscroll, "br-pick-scroll");
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (w->omniscroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (w->omniscroll), w->omnilist);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (w->omniscroll), TRUE);
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (w->omniscroll), 300);
    gtk_widget_set_visible (w->omniscroll, FALSE);
    gtk_box_append (GTK_BOX (w->omni), w->omniscroll);

    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), w->omni);

    /* the key reference sits in the middle, clear of every edge, so all
     * four of its corners are rounded */
    w->keys = keys_panel_new ();
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), w->keys);

    /* the permission prompt: one line of text, Allow and Deny */
    w->perm = gtk_box_new (GTK_ORIENTATION_VERTICAL, (int) g_theme.gap);
    gtk_widget_add_css_class (w->perm, "br-perm");
    gtk_widget_set_halign (w->perm, GTK_ALIGN_FILL);
    gtk_widget_set_valign (w->perm, GTK_ALIGN_START);
    gtk_widget_set_visible (w->perm, FALSE);

    w->permlabel = gtk_label_new ("");
    gtk_label_set_wrap (GTK_LABEL (w->permlabel), TRUE);
    gtk_label_set_xalign (GTK_LABEL (w->permlabel), 0.0f);
    gtk_widget_add_css_class (w->permlabel, "br-perm-text");
    gtk_box_append (GTK_BOX (w->perm), w->permlabel);

    GtkWidget *permrow  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, (int) g_theme.gap);
    GtkWidget *permdeny = gtk_button_new_with_label ("Deny  (Esc)");
    GtkWidget *permok   = gtk_button_new_with_label ("Allow  (Enter)");
    gtk_widget_add_css_class (permdeny, "flat");
    gtk_widget_add_css_class (permdeny, "br-btn");
    gtk_widget_add_css_class (permok, "flat");
    gtk_widget_add_css_class (permok, "br-btn");
    gtk_widget_add_css_class (permok, "br-btn-primary");
    gtk_widget_set_hexpand (permdeny, TRUE);
    gtk_widget_set_halign (permdeny, GTK_ALIGN_START);
    gtk_widget_set_halign (permok, GTK_ALIGN_END);
    gtk_widget_set_focusable (permdeny, FALSE);
    gtk_widget_set_focusable (permok, FALSE);
    g_signal_connect (permdeny, "clicked", G_CALLBACK (on_perm_deny),  w);
    g_signal_connect (permok,   "clicked", G_CALLBACK (on_perm_allow), w);
    gtk_box_append (GTK_BOX (permrow), permdeny);
    gtk_box_append (GTK_BOX (permrow), permok);
    gtk_box_append (GTK_BOX (w->perm), permrow);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), w->perm);

    /* The address of the page being opened, bottom left. It touches the
     * bottom and the left, so only its top right corner is rounded. */
    w->urltoast = gtk_label_new ("");
    gtk_widget_add_css_class (w->urltoast, "br-urltoast");
    gtk_widget_set_halign (w->urltoast, GTK_ALIGN_FILL);
    gtk_widget_set_valign (w->urltoast, GTK_ALIGN_FILL);
    gtk_widget_set_can_target (w->urltoast, FALSE);
    gtk_widget_set_visible (w->urltoast, FALSE);
    gtk_label_set_ellipsize (GTK_LABEL (w->urltoast), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars (GTK_LABEL (w->urltoast), 70);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), w->urltoast);

    /* a hairline across the very top, so a slow page still says something */
    w->loadbar = gtk_drawing_area_new ();
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (w->loadbar), draw_loadbar, w, NULL);
    gtk_widget_add_css_class (w->loadbar, "br-load");
    gtk_widget_set_halign (w->loadbar, GTK_ALIGN_FILL);
    gtk_widget_set_valign (w->loadbar, GTK_ALIGN_START);
    gtk_widget_set_can_target (w->loadbar, FALSE);
    gtk_widget_set_visible (w->loadbar, FALSE);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), w->loadbar);

    /* Top right column: URL toast on top, downloads below. The whole
     * column is click-through, so it never swallows a click on the page. */
    w->topright = gtk_box_new (GTK_ORIENTATION_VERTICAL, (int) g_theme.gap);
    gtk_widget_set_halign (w->topright, GTK_ALIGN_FILL);
    gtk_widget_set_valign (w->topright, GTK_ALIGN_START);
    gtk_widget_set_can_target (w->topright, FALSE);

    w->toast = gtk_label_new ("");
    gtk_widget_add_css_class (w->toast, "br-toast");
    gtk_widget_set_halign (w->toast, GTK_ALIGN_END);
    gtk_widget_set_visible (w->toast, FALSE);
    gtk_label_set_ellipsize (GTK_LABEL (w->toast), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars (GTK_LABEL (w->toast), 90);
    gtk_box_append (GTK_BOX (w->topright), w->toast);

    w->dlpanel = gtk_box_new (GTK_ORIENTATION_VERTICAL, (int) g_theme.win_gap);
    gtk_widget_add_css_class (w->dlpanel, "br-dl");
    gtk_widget_set_halign (w->dlpanel, GTK_ALIGN_END);
    gtk_widget_set_visible (w->dlpanel, FALSE);
    gtk_widget_set_size_request (w->dlpanel, 320, -1);
    gtk_box_append (GTK_BOX (w->topright), w->dlpanel);

    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), w->topright);

    /* media mode: a frame round the whole page, click-through */
    w->mediaframe = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (w->mediaframe, "br-media-frame");
    gtk_widget_set_halign (w->mediaframe, GTK_ALIGN_FILL);
    gtk_widget_set_valign (w->mediaframe, GTK_ALIGN_FILL);
    gtk_widget_set_can_target (w->mediaframe, FALSE);
    gtk_widget_set_visible (w->mediaframe, g_media_mode);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), w->mediaframe);

    g_signal_connect (overlay, "get-child-position",
                      G_CALLBACK (on_overlay_position), w);

    gtk_window_set_child (GTK_WINDOW (w->win), overlay);

    GtkEventController *kc = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (kc, GTK_PHASE_CAPTURE);
    g_signal_connect (kc, "key-pressed", G_CALLBACK (on_key), w);
    gtk_widget_add_controller (w->win, kc);

    GtkEventController *mc = gtk_event_controller_motion_new ();
    g_signal_connect (mc, "motion", G_CALLBACK (on_motion), w);
    g_signal_connect (mc, "leave",  G_CALLBACK (on_motion_leave), w);
    gtk_widget_add_controller (w->win, mc);

    /* a click anywhere outside the popup dismisses it */
    GtkGesture *away = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (away), 0);   /* any button */
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (away),
                                                GTK_PHASE_CAPTURE);
    g_signal_connect (away, "pressed", G_CALLBACK (on_click_away), w);
    gtk_widget_add_controller (w->win, GTK_EVENT_CONTROLLER (away));

    GtkGesture *mclick = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (mclick), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (mclick),
                                                GTK_PHASE_CAPTURE);
    g_signal_connect (mclick, "pressed", G_CALLBACK (on_media_press), w);
    gtk_widget_add_controller (w->win, GTK_EVENT_CONTROLLER (mclick));
    g_signal_connect (view, "mouse-target-changed", G_CALLBACK (on_mouse_target), w);

    if (g_no_middle_paste) {
        GtkGesture *click = gtk_gesture_click_new ();
        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_MIDDLE);
        gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click),
                                                    GTK_PHASE_CAPTURE);
        g_signal_connect (click, "pressed", G_CALLBACK (on_middle_press), NULL);
        gtk_widget_add_controller (w->win, GTK_EVENT_CONTROLLER (click));
    }

    if (g_follow_page_title)
        g_signal_connect (view, "notify::title", G_CALLBACK (on_title), w);

    g_signal_connect (w->win, "map", G_CALLBACK (on_window_map), NULL);
    g_signal_connect (w->win, "destroy", G_CALLBACK (on_close), w);
    g_object_set_data_full (G_OBJECT (w->win), "win", w, win_free);

    /* history and the front-end's watchdogs need view -> window */
    g_object_set_data (G_OBJECT (view), "win", w);

    g_ptr_array_add (g_wins, w);

    if (g_app->win_ready)
        g_app->win_ready (w);

    dl_panel_rebuild (w);         /* picks up downloads already running */

    g_windows++;
    return w->win;
}

static void
on_ready_to_show (WebKitWebView *view, gpointer u)
{
    (void) u;
    gtk_window_present (GTK_WINDOW (window_new (view, FALSE)));
}

/* ------------------------------------------------ start page background */

/*
 * The colour picked from the start page's palette. It belongs to the
 * person rather than to a profile - the same reasoning as the download
 * rules - so it lives one level up, in
 *
 *   ~/.local/share/wkview/start-bg      one line, #rrggbb
 *
 * and every profile and both browsers agree about it. start_bg in a
 * config file is the default until a swatch is clicked; from then on
 * this file is the answer.
 */
static char *
startbg_path (void)
{
    return g_build_filename (g_get_user_data_dir (), PROFILE_DIR_NAME,
                             STARTBG_FILE, NULL);
}

static void
startbg_load (void)
{
    char *path = startbg_path ();
    char *txt  = NULL;

    if (g_file_get_contents (path, &txt, NULL, NULL)) {
        parse_color (g_strstrip (txt), &g_theme.start_bg);
        g_free (txt);
    }
    g_free (path);
}

static void
startbg_save (const char *hex)
{
    char *path = startbg_path ();
    char *dir  = g_path_get_dirname (path);
    char *line = g_strdup_printf ("%s\n", hex);

    g_mkdir_with_parents (dir, 0700);
    if (g_file_set_contents (path, line, -1, NULL))
        LOG ("start page: background %s -> %s\n", hex, path);
    else
        g_printerr ("start page: cannot write %s\n", path);

    g_free (line);
    g_free (dir);
    g_free (path);
}

/* The palette on the start page posts the colour it was given. Anything
 * that is not a colour is dropped rather than written to the file. */
static void
on_start_message (WebKitUserContentManager *ucm, JSCValue *value, gpointer u)
{
    (void) ucm;
    (void) u;

    if (!value || !jsc_value_is_string (value))
        return;

    char *s = jsc_value_to_string (value);
    Color c;

    if (s && parse_color (s, &c)) {
        g_theme.start_bg = c;
        startbg_save (s);
    }
    g_free (s);
}

/* ----------------------------------------------------------- --paths */

/*
 * Every directory and file either browser can write, with what is
 * actually on disk right now. The profile and cache directories are
 * walked rather than guessed: WebKit lays out its own storage in there
 * (local storage, IndexedDB, service workers, the salts behind
 * deviceId) and the names have changed between releases, so the honest
 * answer is whatever is there.
 */

static goffset
path_size (const char *path)
{
    GStatBuf st;

    if (g_stat (path, &st) != 0)
        return -1;

    if (!S_ISDIR (st.st_mode))
        return (goffset) st.st_size;

    GDir *d = g_dir_open (path, 0, NULL);
    if (!d)
        return 0;

    goffset total = 0;
    const char *n;
    while ((n = g_dir_read_name (d))) {
        char *child = g_build_filename (path, n, NULL);
        goffset s = path_size (child);
        if (s > 0)
            total += s;
        g_free (child);
    }
    g_dir_close (d);
    return total;
}

static char *
fmt_size (goffset n)
{
    if (n < 0)
        return g_strdup ("-");
    if (n < 1024)
        return g_strdup_printf ("%" G_GOFFSET_FORMAT " B", n);
    if (n < 1024 * 1024)
        return g_strdup_printf ("%.0f K", n / 1024.0);
    if (n < 1024 * 1024 * 1024)
        return g_strdup_printf ("%.1f M", n / (1024.0 * 1024.0));
    return g_strdup_printf ("%.1f G", n / (1024.0 * 1024.0 * 1024.0));
}

static void
path_line (GString *s, const char *path, const char *note)
{
    gboolean here = g_file_test (path, G_FILE_TEST_EXISTS);
    char    *sz   = here ? fmt_size (path_size (path)) : g_strdup ("not yet");

    g_string_append_printf (s, "  %-52s %8s%s%s\n", path, sz,
                            (note && *note) ? "  " : "", (note && *note) ? note : "");
    g_free (sz);
}

/* What we know a name in the profile directory is for. Anything not in
 * here is WebKit's and is listed as such. */
static const char *
data_note (const char *name)
{
    if (!strcmp (name, "cookies.sqlite"))   return "cookies - this is your logins";
    if (!strcmp (name, HIST_FILE))          return "addresses visited";
    if (!strcmp (name, SEARCH_FILE))        return "search keywords (see below)";
    if (!strcmp (name, PERM_FILE))          return "camera / microphone answers per site";
    if (g_str_has_prefix (name, "cookies.sqlite-")) return "sqlite journal";
    return "WebKit storage";
}

static void
walk_into (GString *s, const char *dir, gboolean annotate)
{
    GDir *d = g_dir_open (dir, 0, NULL);

    if (!d) {
        g_string_append (s, "    (not created yet)\n");
        return;
    }

    GList *names = NULL;
    const char *n;
    while ((n = g_dir_read_name (d)))
        names = g_list_prepend (names, g_strdup (n));
    g_dir_close (d);
    names = g_list_sort (names, (GCompareFunc) g_strcmp0);

    if (!names)
        g_string_append (s, "    (empty)\n");

    for (GList *l = names; l; l = l->next) {
        char *child = g_build_filename (dir, l->data, NULL);
        char *sz    = fmt_size (path_size (child));
        char *label = g_strconcat (l->data,
                                   g_file_test (child, G_FILE_TEST_IS_DIR) ? "/" : "",
                                   NULL);
        const char *note = annotate ? data_note (l->data) : "";

        g_string_append_printf (s, "    %-49s %8s%s%s\n", label, sz,
                                *note ? "  " : "", note);
        g_free (label);
        g_free (sz);
        g_free (child);
    }
    g_list_free_full (names, g_free);
}

/* The keywords themselves, the same way: what each one searches and
 * which one a bare word goes to. */
static void
searches_print (GString *s, const char *path)
{
    GPtrArray *keys = g_ptr_array_new_with_free_func (g_free);
    GPtrArray *urls = g_ptr_array_new_with_free_func (g_free);
    GPtrArray *mine = g_ptr_array_new ();      /* GINT_TO_POINTER: from the file */
    char      *dflt = NULL;
    char      *text = NULL;

    if (g_file_get_contents (path, &text, NULL, NULL)) {
        char **lines = g_strsplit (text, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char **f = g_strsplit (lines[i], "\t", 3);
            if (g_strv_length (f) >= 2 && *f[0] && *f[1]) {
                if (g_strv_length (f) >= 3 && !g_strcmp0 (g_strstrip (f[2]), "default")) {
                    g_free (dflt);
                    dflt = g_strdup (f[0]);
                }
                g_ptr_array_add (keys, g_strdup (f[0]));
                g_ptr_array_add (urls, g_strdup (f[1]));
                g_ptr_array_add (mine, GINT_TO_POINTER (1));
            }
            g_strfreev (f);
        }
        g_strfreev (lines);
        g_free (text);
    }

    for (gsize b = 0; b < G_N_ELEMENTS (SEARCH_BUILTIN); b++) {
        gboolean have = FALSE;
        for (guint i = 0; i < keys->len && !have; i++)
            have = !strcmp (g_ptr_array_index (keys, i), SEARCH_BUILTIN[b][0]);
        if (have)
            continue;
        g_ptr_array_add (keys, g_strdup (SEARCH_BUILTIN[b][0]));
        g_ptr_array_add (urls, g_strdup (SEARCH_BUILTIN[b][1]));
        g_ptr_array_add (mine, GINT_TO_POINTER (0));
    }

    const char *d = dflt ? dflt : SEARCH_BUILTIN_DEFAULT;
    for (guint i = 0; i < keys->len; i++) {
        const char *k = g_ptr_array_index (keys, i);
        g_string_append_printf (s, "      %-6s %s%s%s\n", k,
                                (char *) g_ptr_array_index (urls, i),
                                GPOINTER_TO_INT (g_ptr_array_index (mine, i)) ? "" : "   (built in)",
                                !strcmp (k, d) ? "   <- default" : "");
    }

    g_free (dflt);
    g_ptr_array_free (keys, TRUE);
    g_ptr_array_free (urls, TRUE);
    g_ptr_array_free (mine, TRUE);
}

/* The rules themselves, so the file's one job is visible rather than
 * described: which address or site goes where. */
static void
dlrules_print (GString *s, const char *path)
{
    char *text = NULL;

    if (!g_file_get_contents (path, &text, NULL, NULL))
        return;

    char **lines = g_strsplit (text, "\n", -1);
    int    n     = 0;

    for (int i = 0; lines[i]; i++) {
        if (!*lines[i])
            continue;

        char **f = g_strsplit (lines[i], "\t", 3);
        gboolean always = g_strv_length (f) >= 3 &&
                          !g_strcmp0 (g_strstrip (f[2]), DL_ALWAYS_TOKEN);

        if (g_strv_length (f) >= 2 && *f[0]) {
            if (!g_strcmp0 (f[0], DL_ALL_KEY))
                g_string_append_printf (s, "      %-38s %s\n", "(everything)",
                                        always ? "always replace existing files"
                                               : "");
            else
                g_string_append_printf (s, "      %-38s -> %s%s\n", f[0], f[1],
                                        always ? "   (always replace)" : "");
            n++;
        }
        g_strfreev (f);
    }
    g_strfreev (lines);
    g_free (text);

    if (!n)
        g_string_append (s, "      (no rules yet)\n");
}

static void
paths_report (void)
{
    GString *s = g_string_new (NULL);
    char    *cfg_shared = cfg_path_for ("swov");
    char    *cfg_own    = cfg_path_for (g_app->default_app_id);
    char    *dlrules    = dlrules_path ();
    char    *startbg    = startbg_path ();
    char    *data_dir = NULL, *cache_dir = NULL;

    profile_dirs (g_profile, &data_dir, &cache_dir);

    g_string_append_printf (s, "%s %s (build %s) - everything on disk\n\n",
                            g_app->default_app_id, BROWSER_VERSION, BROWSER_BUILD);

    g_string_append (s, "settings, read at startup, first file wins per key:\n");
    path_line (s, cfg_shared, "shared palette (swov)");
    path_line (s, cfg_own,    "ours, overrides the above");
    g_string_append (s, "\n");

    if (g_private) {
        g_string_append (s,
            "profile: --private, so nothing below is written. Cookies,\n"
            "logins, history and cache live in memory and go with the\n"
            "window. No search keywords are loaded either.\n\n");
    } else {
        g_string_append_printf (s,
            "profile \"%s\" - logins live here:\n  %s/\n",
            g_profile ? g_profile : "default", data_dir);
        walk_into (s, data_dir, TRUE);
        g_string_append (s, "\n");

        char *spath = g_build_filename (data_dir, SEARCH_FILE, NULL);
        g_string_append_printf (s,
            "search keywords - set with %s+K, one list per profile:\n", g_mod_name);
        path_line (s, spath, NULL);
        searches_print (s, spath);
        g_string_append_printf (s,
            "  \"g tree\" searches with g; a bare \"tree\" uses the default.\n"
            "  Another --profile has its own file and its own keywords.\n\n");
        g_free (spath);

        g_string_append_printf (s, "cache, safe to delete:\n  %s/\n", cache_dir);
        walk_into (s, cache_dir, FALSE);
        g_string_append (s, "\n");
    }

    /* Two separate things, and mixing them up is the easy mistake: where
     * a file lands, and the file that holds the per-site exceptions. */
    const char *land = (g_download_dir && *g_download_dir)
                     ? g_download_dir
                     : g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);

    g_string_append (s, "downloads:\n");
    g_string_append_printf (s, "  a file lands in\n    %s%s\n",
                            land ? land : ".",
                            (g_download_dir && *g_download_dir)
                                ? "   (download_dir / --download-dir)"
                                : "   (the XDG download directory)");
    g_string_append_printf (s,
        "  unless a rule for its address or its site says otherwise.\n"
        "  The rules are all in this one file, written by %s+S:\n", g_mod_name);
    path_line (s, dlrules, NULL);
    dlrules_print (s, dlrules);
    g_string_append (s,
        "  It is yours, not a profile's: every profile and both browsers\n"
        "  read and write this same file, and a save merges rather than\n"
        "  overwrites, so two running browsers do not trample each other.\n"
        "\n");

    {
        const char *rt = g_get_user_runtime_dir ();
        char *md = g_build_filename (rt && *rt ? rt : g_get_tmp_dir (), "wkview-media", NULL);
        g_string_append (s, "media mode (F2), images fetched for the image viewer:\n");
        path_line (s, md, "removed after an hour");
        g_string_append (s, "\n");
        g_free (md);
    }

    g_string_append (s, "start page:\n");
    path_line (s, startbg, "background colour");
    g_string_append (s, "  Also yours rather than a profile's, and written by the palette\n"
                        "  on the start page itself.\n");

    g_string_append (s,
        "\nNothing else is written. --clear-data wipes the profile and cache\n"
        "directories above, --forget-perms drops " PERM_FILE " alone,\n"
        "--private skips all of it.\n");

    g_print ("%s", s->str);

    g_free (cfg_shared);
    g_free (cfg_own);
    g_free (dlrules);
    g_free (startbg);
    g_free (data_dir);
    g_free (cache_dir);
    g_string_free (s, TRUE);
}

/* --------------------------------------------------- user content manager */

/* One manager per view, so a front-end's script-message handler can tell
 * which view a message came from. */
static void
ucm_gone (gpointer data, GObject *where)
{
    (void) data;
    if (g_ucms)
        g_ptr_array_remove_fast (g_ucms, where);
}

static WebKitUserContentManager *
ucm_new (void)
{
    WebKitUserContentManager *ucm = webkit_user_content_manager_new ();

    /* the start page's colour palette answers here */
    g_signal_connect (ucm, "script-message-received::wkStart",
                      G_CALLBACK (on_start_message), NULL);
    webkit_user_content_manager_register_script_message_handler (ucm, "wkStart", NULL);

    if (g_app->ucm_ready)
        g_app->ucm_ready (ucm);

    css_apply_one (ucm);

    g_ptr_array_add (g_ucms, ucm);
    g_object_weak_ref (G_OBJECT (ucm), ucm_gone, NULL);
    return ucm;
}

static void
view_wire (WebKitWebView *view)
{
    /* NB: in WebKitGTK 6.0 "download-started" only exists on the network
     * session, not on the web view - it is wired up once in browser_main(). */
    g_signal_connect (view, "decide-policy",      G_CALLBACK (on_decide_policy), NULL);
    g_signal_connect (view, "permission-request", G_CALLBACK (on_permission), NULL);
    g_signal_connect (view, "query-permission-state", G_CALLBACK (on_query_permission), NULL);
    g_signal_connect (view, "create",             G_CALLBACK (on_create), NULL);
    g_signal_connect (view, "load-changed",       G_CALLBACK (on_load_core), NULL);
    g_signal_connect (view, "load-failed",        G_CALLBACK (on_load_failed_core), NULL);
    g_signal_connect (view, "notify::title",     G_CALLBACK (on_title_toast), NULL);
    g_signal_connect (view, "web-process-terminated",
                      G_CALLBACK (on_web_process_gone), NULL);
    g_signal_connect (view, "notify::estimated-load-progress",
                      G_CALLBACK (on_load_progress), NULL);

    WebKitFindController *fc = webkit_web_view_get_find_controller (view);
    g_signal_connect (fc, "found-text",          G_CALLBACK (on_found_count), NULL);
    g_signal_connect (fc, "failed-to-find-text", G_CALLBACK (on_found_none), NULL);

    if (g_app->view_ready)
        g_app->view_ready (view);
}

static WebKitWebView *
view_new (WebKitWebView *related)
{
    WebKitUserContentManager *ucm = ucm_new ();
    WebKitWebView            *view;

    if (related)
        view = g_object_new (WEBKIT_TYPE_WEB_VIEW,
                             "related-view",         related,
                             "settings",             g_settings,
                             "user-content-manager", ucm,
                             NULL);
    else
        view = g_object_new (WEBKIT_TYPE_WEB_VIEW,
                             "network-session",      g_session,
                             "settings",             g_settings,
                             "user-content-manager", ucm,
                             NULL);

    /* a message handler can resolve the view from its manager */
    g_object_set_data (G_OBJECT (ucm), "view", view);
    g_object_unref (ucm);                 /* the view owns it now */

    webkit_web_view_set_zoom_level (view, g_zoom);
    view_wire (view);
    return view;
}

static GtkWidget *
on_create (WebKitWebView *view, WebKitNavigationAction *act, gpointer u)
{
    (void) act; (void) u;

    /* popups (SSO, OAuth, "open in new window" players) share the session */
    WebKitWebView *nv = view_new (view);
    g_signal_connect (nv, "ready-to-show", G_CALLBACK (on_ready_to_show), NULL);
    return GTK_WIDGET (nv);
}

/* -------------------------------------------------------------- session */

static void
setup_session (void)
{
    if (g_private) {
        g_session = webkit_network_session_new_ephemeral ();
        LOG ("profile: private (ephemeral)\n");
        return;
    }

    char *cache_dir = NULL;
    profile_dirs (g_profile, &g_data_dir, &cache_dir);

    if (g_clear_data) {
        LOG ("clear-data: wiping profile data...\n");
        rm_rf (g_data_dir);
        rm_rf (cache_dir);
    }

    g_mkdir_with_parents (g_data_dir, 0700);

    if (g_forget_perms) {
        char *p = g_build_filename (g_data_dir, PERM_FILE, NULL);
        if (g_unlink (p) == 0)
            LOG ("permissions: forgotten (%s removed)\n", p);
        g_free (p);
    }
    g_mkdir_with_parents (cache_dir, 0700);

    g_session = webkit_network_session_new (g_data_dir, cache_dir);

    WebKitCookieManager *cm = webkit_network_session_get_cookie_manager (g_session);

    /* helps with some SSO / multi-domain auth flows */
    webkit_cookie_manager_set_accept_policy (cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);

    /* persist cookies so logins survive restarts */
    char *cookie_path = g_build_filename (g_data_dir, "cookies.sqlite", NULL);
    webkit_cookie_manager_set_persistent_storage (cm, cookie_path,
                                                  WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);

    LOG ("profile: name=%s\n", g_profile ? g_profile : "default");
    LOG ("profile: data=%s cache=%s cookies=%s\n", g_data_dir, cache_dir, cookie_path);

    g_free (cookie_path);
    g_free (cache_dir);
}

static void
setup_settings (void)
{
    g_settings = webkit_settings_new ();

    /* best effort at behaving like a mainstream browser */
    webkit_settings_set_enable_site_specific_quirks (g_settings, TRUE);


    if (g_user_agent && *g_user_agent) {
        webkit_settings_set_user_agent (g_settings, g_user_agent);
        LOG ("ua: %s\n", g_user_agent);
    }

    /* the inspector's debugger comes with these even with no inspector
     * open; a "NeedDebuggerBreak trap" on a plain page is the reason for
     * --no-devtools */
    webkit_settings_set_enable_developer_extras                 (g_settings, !g_no_devtools);
    webkit_settings_set_enable_media_stream                     (g_settings, TRUE);
    webkit_settings_set_enable_webrtc                           (g_settings, TRUE);
    webkit_settings_set_enable_mediasource                      (g_settings, TRUE);
    webkit_settings_set_enable_encrypted_media                  (g_settings, TRUE);
    webkit_settings_set_enable_webaudio                         (g_settings, TRUE);
    webkit_settings_set_media_playback_requires_user_gesture    (g_settings, FALSE);
    webkit_settings_set_javascript_can_access_clipboard         (g_settings, TRUE);
    /* A chatty page writes these synchronously from the web process, and
     * to a terminal that is six times dearer than to a file. */
    webkit_settings_set_enable_write_console_messages_to_stdout (
        g_settings, !g_quiet && !g_no_console);

    features_apply (g_settings);

    if (g_no_gpu) {
        webkit_settings_set_hardware_acceleration_policy (
            g_settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
        LOG ("gpu: hardware acceleration policy = NEVER\n");
    }

    if (g_app->settings_ready)
        g_app->settings_ready (g_settings);
}

/* ------------------------------------------------------------ start page */

/*
 * With no address to open, the window would otherwise come up empty. This
 * is drawn from the same palette as everything else, so the browser looks
 * like one thing from the first frame. It is served from load_html with no
 * base URI, which makes it about:blank as far as the rest of the code is
 * concerned - so it stays out of the history by itself.
 */

/*
 * The swatches. Pure white and pure black are both in, at the two ends,
 * with soft flat tones between them - nothing saturated, since this is a
 * background and not a subject.
 */
static const char *const START_SWATCHES[] = {
    "#ffffff",   /* pure white */
    "#f3f0e9",   /* paper, the default */
    "#ebe4d6",   /* sand */
    "#e3ebe2",   /* sage */
    "#e2e9f1",   /* sky */
    "#e9e4ef",   /* lilac */
    "#f2e4e4",   /* rose */
    "#cfd6dd",   /* stone */
    "#2b3138",   /* slate */
    "#0d1117",   /* ink, the panel palette */
    "#000000",   /* pure black */
};

static char *
color_hex6 (Color c)
{
    return g_strdup_printf ("#%02x%02x%02x",
                            (int) (c.r * 255 + 0.5),
                            (int) (c.g * 255 + 0.5),
                            (int) (c.b * 255 + 0.5));
}

static char *
start_page_html (void)
{
    Theme *t = &g_theme;

    char *c_bg = color_hex6 (t->start_bg);

    /* Both ends of the palette have to read, so the text colour comes
     * from the background rather than from the panel palette. */
    double lum = 0.2126 * t->start_bg.r
               + 0.7152 * t->start_bg.g
               + 0.0722 * t->start_bg.b;
    gboolean light = lum > 0.5;

    const char *c_text = light ? "#16191d" : "#e9ecef";
    const char *c_sub  = light ? "#5a6470" : "#aab4c0";
    const char *c_dim  = light ? "#8a939e" : "#6e7a87";
    /* the panel accent is a mustard: it needs deepening on a light
     * background and stays as it is on a dark one */
    char       *c_hl     = css_rgba (t->hl);
    char       *c_hl_lo  = css_rgba ((Color) { t->hl.r * 0.72, t->hl.g * 0.62,
                                               t->hl.b * 0.30, 1.0 });
    const char *c_line = light ? "rgba(0,0,0,.14)" : "rgba(255,255,255,.16)";

    /* "browser-mini" reads better as browser + mini */
    const char *name = g_app->default_app_id;
    const char *dash = strchr (name, '-');
    char       *head = dash ? g_strndup (name, (gsize) (dash - name)) : g_strdup (name);
    const char *tail = dash ? dash + 1 : NULL;

    GString *sw = g_string_new (NULL);
    for (gsize i = 0; i < G_N_ELEMENTS (START_SWATCHES); i++)
        g_string_append_printf (sw, "<i data-c=\"%s\" style=\"background:%s\"></i>",
                                START_SWATCHES[i], START_SWATCHES[i]);

    char *html = g_strdup_printf (
"<!doctype html><meta charset=\"utf-8\"><title>%s</title><style>"
":root{--bg:%s;--fg:%s;--sub:%s;--dim:%s;--line:%s;--hl:%s}"
"html,body{margin:0}"
"body{min-height:100vh;box-sizing:border-box;padding:8vh 2rem;"
"display:flex;align-items:center;justify-content:center;"
"background:var(--bg);font-family:%s,sans-serif;color:var(--fg);"
"transition:background .18s ease;-webkit-font-smoothing:antialiased}"
".c{text-align:center;transform:translateY(-5vh)}"
"h1{margin:0;font-size:11vmin;font-weight:200;letter-spacing:.18em;color:var(--fg)}"
"h1 b{color:var(--hl);font-weight:600}"
".r{width:9em;height:2px;margin:1.1em auto 1.4em;"
"background:linear-gradient(90deg,transparent,var(--line),transparent)}"
".v{font-family:%s,monospace;font-size:.85rem;color:var(--dim);letter-spacing:.1em}"
".k{margin-top:2.6em;font-family:%s,monospace;font-size:.95rem;color:var(--sub)}"
".k b{color:var(--hl);font-weight:600}"
/* the palette, top centre */
"#p{position:fixed;top:0;left:0;right:0;display:flex;flex-direction:column;"
"align-items:center;gap:.5rem;padding:.9rem 1rem;user-select:none}"
"#t{width:14px;height:14px;padding:0;border-radius:50%%;cursor:pointer;"
"border:1px solid var(--line);background:var(--fg);opacity:.22;"
"transition:opacity .18s ease}"
"#t:hover,#p.on #t{opacity:.6}"
"#s{display:flex;gap:.45rem;opacity:0;pointer-events:none;"
"transform:translateY(-4px);transition:opacity .18s ease,transform .18s ease}"
"#p.on #s{opacity:1;pointer-events:auto;transform:none}"
"#s i{width:20px;height:20px;border-radius:50%%;cursor:pointer;"
"box-shadow:inset 0 0 0 1px rgba(128,128,128,.45);"
"transition:transform .12s ease}"
"#s i:hover{transform:scale(1.25)}"
"#s i.on{box-shadow:inset 0 0 0 1px rgba(128,128,128,.45),0 0 0 2px var(--fg)}"
"</style>"
"<div id=p><button id=t title=\"background colour\"></button><div id=s>%s</div></div>"
"<div class=c><h1>%s%s<b>%s</b></h1><div class=r></div>"
"<div class=v>%s &middot; build %s</div>"
"<div class=k><b>%s+O</b> address &nbsp;&nbsp; <b>%s+H</b> history"
" &nbsp;&nbsp; <b>F1</b> keys</div></div>"
"<script>"
"var p=document.getElementById('p'),s=document.getElementById('s'),"
"r=document.documentElement;"
"document.getElementById('t').onclick=function(){p.classList.toggle('on')};"
"function lum(h){return(0.2126*parseInt(h.substr(1,2),16)"
"+0.7152*parseInt(h.substr(3,2),16)+0.0722*parseInt(h.substr(5,2),16))/255}"
"function mark(h){var a=s.children;for(var i=0;i<a.length;i++)"
"a[i].classList.toggle('on',a[i].dataset.c===h)}"
"function put(h,save){var L=lum(h)>0.5;"
"r.style.setProperty('--bg',h);"
"r.style.setProperty('--fg',L?'#16191d':'#e9ecef');"
"r.style.setProperty('--sub',L?'#5a6470':'#aab4c0');"
"r.style.setProperty('--dim',L?'#8a939e':'#6e7a87');"
"r.style.setProperty('--line',L?'rgba(0,0,0,.14)':'rgba(255,255,255,.16)');"
"r.style.setProperty('--hl',L?'%s':'%s');"
"mark(h);"
"if(save){try{webkit.messageHandlers.wkStart.postMessage(h)}catch(e){}}}"
"s.onclick=function(e){var h=e.target.dataset&&e.target.dataset.c;"
"if(h)put(h,true)};"
"mark('%s');"
"</script>",
        name,
        c_bg, c_text, c_sub, c_dim, c_line, light ? c_hl_lo : c_hl,
        *t->font ? t->font : "system-ui",
        t->font_mono,
        t->font_mono,
        sw->str,
        head, tail ? " " : "", tail ? tail : "",
        BROWSER_VERSION, BROWSER_BUILD,
        g_mod_name, g_mod_name,
        c_hl_lo, c_hl,
        c_bg);

    g_string_free (sw, TRUE);
    g_free (head);
    g_free (c_bg);
    g_free (c_hl);
    g_free (c_hl_lo);
    return html;
}

/* ----------------------------------------------------------------- main */

int
browser_main (int argc, char **argv, const BrowserApp *app)
{
    const char *url_arg       = NULL;
    gboolean    open_devtools = FALSE;
    const char *cfg_path      = NULL;
    gboolean    use_cfg       = TRUE;

    g_app = app;
    theme_defaults ();

    /* config first, so anything on the command line still wins */
    for (int i = 1; i < argc; i++) {
        if ((!strcmp (argv[i], "-c") || !strcmp (argv[i], "--config")) && i + 1 < argc)
            cfg_path = argv[++i];
        else if (!strcmp (argv[i], "-n") || !strcmp (argv[i], "--no-config"))
            use_cfg = FALSE;
    }

    if (use_cfg) {
        if (cfg_path)
            cfg_load_file (cfg_path);
        else
            cfg_load_default_chain (app->default_app_id);
    }

    /* the colour last picked from the start page's palette outranks the
     * start_bg key, because it is the more recent thing the person said */
    startbg_load ();

    /* answered here, like -h, so it needs no display and no config */
    for (int i = 1; i < argc; i++) {
        if (strcmp (argv[i], "--list-features"))
            continue;

        const char *filter = (i + 1 < argc && argv[i + 1][0] != '-')
                           ? argv[i + 1] : NULL;
        features_list (filter);
        return 0;
    }

    /*
     * Answered after the config is read, so the paths and values printed
     * are the ones that would actually be used, but before the main loop,
     * so they cannot be lost behind a mistyped option later on the line
     * or a front-end that exits while parsing its own flags.
     */
    for (int i = 1; i < argc; i++) {
        if (!strcmp (argv[i], "-h") || !strcmp (argv[i], "--help")) {
            usage (argv[0], TRUE);
            return 0;
        }
        if (!strcmp (argv[i], "-V") || !strcmp (argv[i], "--version")) {
            g_print ("%s %s (build %s)\n", app->default_app_id,
                     BROWSER_VERSION, BROWSER_BUILD);
            return 0;
        }
        if (!strcmp (argv[i], "--paths")) {
            /* --profile and --private decide what the answer is, and they
             * are parsed further down, so read them here: --paths works
             * wherever it sits on the line. */
            for (int j = 1; j < argc; j++) {
                if (!strcmp (argv[j], "--private"))
                    g_private = TRUE;
                else if (!strcmp (argv[j], "--profile") && j + 1 < argc) {
                    g_free (g_profile);
                    g_profile = g_strdup (argv[++j]);
                }
                else if (!strcmp (argv[j], "--mod") && j + 1 < argc)
                    parse_mod (argv[++j]);      /* the report names a key */
            }
            paths_report ();
            return 0;
        }
    }

#define NEED_ARG(opt) \
    do { if (i + 1 >= argc) { \
             g_printerr ("%s: %s needs an argument\n", argv[0], opt); \
             return 1; \
         } } while (0)

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp (a, "-h") || !strcmp (a, "--help")) {
            usage (argv[0], TRUE);
            return 0;
        } else if (!strcmp (a, "-V") || !strcmp (a, "--version")) {
            g_print ("%s %s (build %s)\n", app->default_app_id,
                     BROWSER_VERSION, BROWSER_BUILD);
            return 0;
        } else if (!strcmp (a, "-q") || !strcmp (a, "--quiet")) {
            g_quiet = TRUE;
        } else if (!strcmp (a, "--title")) {
            NEED_ARG ("--title");
            g_free (g_title);
            g_title = g_strdup (argv[++i]);
        } else if (!strcmp (a, "--app-id")) {
            NEED_ARG ("--app-id");
            g_free (g_app_id);
            g_app_id = g_strdup (argv[++i]);
        } else if (!strcmp (a, "--page-title")) {
            g_want_page_title = TRUE;
        } else if (!strcmp (a, "-c") || !strcmp (a, "--config")) {
            NEED_ARG ("--config");
            i++;                              /* already applied above */
        } else if (!strcmp (a, "-n") || !strcmp (a, "--no-config")) {
            /* already applied above */
        } else if (!strcmp (a, "-s") || !strcmp (a, "--set")) {
            NEED_ARG ("--set");
            cfg_set_line (argv[++i]);
        } else if (!strcmp (a, "--mod")) {
            NEED_ARG ("--mod");
            if (!parse_mod (argv[++i])) {
                g_printerr ("%s: unknown --mod %s (ctrl|alt|super|meta)\n", argv[0], argv[i]);
                return 1;
            }
        } else if (!strcmp (a, "--clip-cmd")) {
            NEED_ARG ("--clip-cmd");
            g_free (g_clip_cmd);
            g_clip_cmd = g_strdup (argv[++i]);
        } else if (!strcmp (a, "--player")) {
            NEED_ARG ("--player");
            cfg_set ("player", argv[++i]);
            g_media_mode = TRUE;          /* asked for on the line: start in it */
        } else if (!strcmp (a, "--image-viewer")) {
            NEED_ARG ("--image-viewer");
            cfg_set ("image_viewer", argv[++i]);
        } else if (!strcmp (a, "--media")) {
            g_media_mode = TRUE;
        } else if (!strcmp (a, "--player-match")) {
            NEED_ARG ("--player-match");
            cfg_set ("player_match", argv[++i]);
        } else if (!strcmp (a, "--zoom")) {
            NEED_ARG ("--zoom");
            g_zoom = CLAMP (g_ascii_strtod (argv[++i], NULL), ZOOM_MIN, ZOOM_MAX);
        } else if (!strcmp (a, "--css")) {
            NEED_ARG ("--css");
            g_css_path = argv[++i];
        } else if (!strcmp (a, "--devtools")) {
            open_devtools = TRUE;
        } else if (!strcmp (a, "--forget-permissions")) {
            g_forget_perms = TRUE;
        } else if (!strcmp (a, "--allow-media")) {
            g_media_policy = MEDIA_ALLOW;
        } else if (!strcmp (a, "--deny-media") || !strcmp (a, "--no-media")) {
            g_media_policy = MEDIA_DENY;
            g_deny_media   = TRUE;
        } else if (!strcmp (a, "--no-load-bar")) {
            g_load_bar = FALSE;
        } else if (!strcmp (a, "--no-sandbox")) {
            g_no_sandbox = TRUE;
        } else if (!strcmp (a, "--list-features")) {
            g_list_features = (i + 1 < argc && argv[i + 1][0] != '-')
                            ? argv[++i] : "\001";
        } else if (!strcmp (a, "--shared-array-buffer")) {
            g_shared_ab = TRUE;
        } else if (!strcmp (a, "--no-shared-array-buffer")) {
            g_shared_ab = FALSE;
        } else if (!strcmp (a, "--jsc")) {
            NEED_ARG ("--jsc");
            if (!g_jsc_opts)
                g_jsc_opts = g_ptr_array_new_with_free_func (g_free);
            g_ptr_array_add (g_jsc_opts, g_strdup (argv[++i]));
        } else if (!strcmp (a, "--gl-info")) {
            g_gl_info = TRUE;
        } else if (!strcmp (a, "--feature")) {
            NEED_ARG ("--feature");
            if (!g_features)
                g_features = g_ptr_array_new_with_free_func (g_free);
            g_ptr_array_add (g_features, g_strdup (argv[++i]));
        } else if (!strcmp (a, "--gsk")) {
            NEED_ARG ("--gsk");
            g_gsk_renderer = argv[++i];
        } else if (!strcmp (a, "--no-proc-watch")) {
            g_proc_watch = FALSE;
        } else if (!strcmp (a, "--no-console")) {
            g_no_console = TRUE;
        } else if (!strcmp (a, "--all-messages")) {
            g_all_messages = TRUE;
        } else if (!strcmp (a, "--no-devtools")) {
            g_no_devtools = TRUE;
        } else if (!strcmp (a, "--no-jit")) {
            g_no_jit = TRUE;
        } else if (!strcmp (a, "--no-gpu")) {
            g_no_gpu = TRUE;
        } else if (!strcmp (a, "--no-dmabuf")) {
            g_no_dmabuf = TRUE;
        } else if (!strcmp (a, "--no-compositing")) {
            g_no_compositing = TRUE;
        } else if (!strcmp (a, "--no-hw-decode")) {
            g_no_hw_decode = TRUE;
        } else if (!strcmp (a, "--enable-middle-click-paste")) {
            g_no_middle_paste = FALSE;
        } else if (!strcmp (a, "--download-dir")) {
            NEED_ARG ("--download-dir");
            g_free (g_download_dir);
            g_download_dir = g_strdup (argv[++i]);
        } else if (!strcmp (a, "--private")) {
            g_private = TRUE;
        } else if (!strcmp (a, "--profile")) {
            NEED_ARG ("--profile");
            g_free (g_profile);
            g_profile = g_strdup (argv[++i]);
        } else if (!strcmp (a, "--clear-data")) {
            g_clear_data = TRUE;
        } else if (!strcmp (a, "--user-agent")) {
            NEED_ARG ("--user-agent");
            g_free (g_user_agent);
            g_user_agent = g_strdup (argv[++i]);
        } else if (!strcmp (a, "--ua-chrome")) {
            g_free (g_user_agent);
            g_user_agent = g_strdup ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
        } else if (!strcmp (a, "--ua-win-chrome")) {
            g_free (g_user_agent);
            g_user_agent = g_strdup ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/127.0.0.0 Safari/537.36");
        } else if (!strcmp (a, "--ua-win-edge")) {
            g_free (g_user_agent);
            g_user_agent = g_strdup ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/127.0.0.0 Safari/537.36 Edg/127.0.0.0");
        } else if (!strcmp (a, "--ua-firefox")) {
            g_free (g_user_agent);
            g_user_agent = g_strdup ("Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
        } else if (!strcmp (a, "--ua-safari")) {
            g_free (g_user_agent);
            g_user_agent = g_strdup ("Mozilla/5.0 (Macintosh; Intel Mac OS X 14_6) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.6 Safari/605.1.15");
        } else if (app->parse_arg && app->parse_arg (argc, argv, &i)) {
            /* consumed by the front-end */
        } else if (g_str_has_prefix (a, "--") && looks_like_setting (a + 2)) {
            cfg_set_line (a + 2);             /* --pad=12 */
        } else if (a[0] != '-' && looks_like_setting (a) && !strstr (a, "://")) {
            cfg_set_line (a);                 /* pad=12 */
        } else if (a[0] != '-' && !url_arg) {
            url_arg = a;
        } else if (a[0] != '-') {
            g_printerr ("%s: a second address: %s\n", argv[0], a);
            g_printerr ("%s: already opening: %s\n", argv[0], url_arg);
            usage_short (argv[0]);
            return 1;
        } else {
            g_printerr ("%s: unknown option: %s\n", argv[0], a);
            usage_short (argv[0]);
            return 1;
        }
    }
#undef NEED_ARG

    /* The page <title> drives the window title by default. An explicit
     * --title pins it instead, unless --page-title is also given - then
     * --title is just the fallback for pages with no title of their own. */
    g_follow_page_title = g_want_page_title || (g_title == NULL);

    if (!g_title)
        g_title = g_strdup (app->default_title);
    if (!g_app_id)
        g_app_id = g_strdup (app->default_app_id);

    /* must happen before the first toplevel is created: GTK reads the
     * prgname when it builds the xdg_toplevel / sets WM_CLASS */
    g_set_prgname (g_app_id);
    g_set_application_name (g_title);

    if (!g_download_dir) {
        const char *d = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
        g_download_dir = g_strdup (d ? d : ".");
    }

    if (g_private && (g_profile || g_clear_data))
        LOG ("note: --private ignores --profile/--clear-data\n");

    /* Must be set before any process is spawned, so the web and GPU
     * processes inherit them. */
    /*
     * GL by default. GTK 4.20 picks Vulkan where the driver claims support,
     * and a Vulkan stack that deadlocks recreating its swapchain freezes
     * the whole browser on a resize. GL is GTK's fallback everywhere, and
     * WebKit composites the page itself, so nothing is given up. "auto"
     * leaves the choice to GTK.
     */
    if (g_gsk_renderer && g_ascii_strcasecmp (g_gsk_renderer, "auto") != 0) {
        g_setenv ("GSK_RENDERER", g_gsk_renderer, TRUE);
        LOG ("gsk: renderer = %s\n", g_gsk_renderer);
    }

    if (g_no_sandbox) {
        /* The sandboxed web process reaches capture devices through the
         * desktop portal. Without a portal there are none, and this is the
         * way round it - at the cost of the sandbox, so it says so. */
        g_setenv ("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", TRUE);
        g_printerr ("sandbox: DISABLED for this run (--no-sandbox)\n");
    }

    /*
     * JavaScriptCore reads its own options from the environment, and they
     * have to be set before the web process starts. SharedArrayBuffer is
     * off by default here: a site using threaded WebAssembly needs it and
     * hangs without it rather than reporting anything.
     */
    if (g_shared_ab)
        g_setenv ("JSC_useSharedArrayBuffer", "1", TRUE);

    if (g_jsc_opts) {
        for (guint k = 0; k < g_jsc_opts->len; k++) {
            char *spec = g_ptr_array_index (g_jsc_opts, k);
            char *eq   = strchr (spec, '=');
            char *name = g_strconcat ("JSC_", spec, NULL);

            if (eq) {
                name[4 + (eq - spec)] = '\0';
                g_setenv (name, eq + 1, TRUE);
                LOG ("jsc: %s = %s\n", name, eq + 1);
            } else {
                g_setenv (name, "1", TRUE);
                LOG ("jsc: %s = 1\n", name);
            }
            g_free (name);
        }
    }

    if (g_no_jit) {
        /* JavaScriptCore reads these itself; they have to be in the
         * environment before the web process is spawned. */
        g_setenv ("JSC_useJIT", "0", TRUE);
        g_setenv ("JSC_useBaselineJIT", "0", TRUE);
        g_setenv ("JSC_useDFGJIT", "0", TRUE);
        g_setenv ("JSC_useFTLJIT", "0", TRUE);
    }
    if (g_no_dmabuf)      g_setenv ("WEBKIT_DISABLE_DMABUF_RENDERER", "1", TRUE);
    if (g_no_compositing) g_setenv ("WEBKIT_DISABLE_COMPOSITING_MODE", "1", TRUE);
    /* WebKitGTK 2.54 has no switch of its own for this (the old
     * WEBKIT_GST_ENABLE_HW_DECODERS is gone). GStreamer's documented way
     * is the feature rank: a decoder at NONE is never autoplugged. */
    if (g_no_hw_decode)
        gst_rank_env_add ("vah264dec:NONE,vah265dec:NONE,vavp8dec:NONE,vavp9dec:NONE,"
                          "vaav1dec:NONE,vampeg2dec:NONE,vajpegdec:NONE,"
                          "vaapih264dec:NONE,vaapih265dec:NONE,vaapivp8dec:NONE,"
                          "vaapivp9dec:NONE,vaapiav1dec:NONE,vaapidecodebin:NONE,"
                          "v4l2slh264dec:NONE,v4l2slh265dec:NONE,v4l2slvp8dec:NONE,"
                          "v4l2slvp9dec:NONE,v4l2slav1dec:NONE,v4l2h264dec:NONE,"
                          "v4l2h265dec:NONE,v4l2vp8dec:NONE,v4l2vp9dec:NONE");

    if (app->pre_gtk)
        app->pre_gtk ();

    noise_filter_start ();

    /* line-buffer, so our log interleaves sensibly with the web process
     * writing to the same stderr */
    setvbuf (stderr, NULL, _IOLBF, 0);
    setvbuf (stdout, NULL, _IOLBF, 0);

    /* a clipboard helper that dies early must not take us with it */
    signal (SIGPIPE, SIG_IGN);

    g_downloads = g_ptr_array_new_with_free_func (dl_free);
    g_wins      = g_ptr_array_new ();
    g_ucms      = g_ptr_array_new ();

    gtk_init ();
    ui_css_install ();

    if (app->startup)
        app->startup ();

    setup_session ();
    g_signal_connect (g_session, "download-started",
                      G_CALLBACK (on_session_download_started), NULL);

    history_setup (g_data_dir);
    dlrules_setup (g_data_dir);      /* app wide; the profile is only migrated from */
    searches_setup (g_data_dir);

    setup_settings ();
    css_reload ();

    WebKitWebView *view = view_new (NULL);

    if (open_devtools)
        g_signal_connect (view, "load-changed", G_CALLBACK (on_load_changed_devtools), NULL);

    if (g_gl_info) {
        gl_info ();
        return 0;
    }

    GtkWidget *win  = window_new (view, TRUE);
    GMainLoop *loop = g_main_loop_new (NULL, FALSE);

    g_loop = loop;
    frame_watch_attach (win);
    if (g_proc_watch) {
        g_timeout_add_seconds (5, proc_watch_tick, NULL);
        g_timeout_add (500, heartbeat_tick, NULL);
        g_thread_unref (g_thread_new ("ui-watch", ui_watch_thread, NULL));
    }
    g_unix_signal_add (SIGINT,  on_quit_signal, NULL);
    g_unix_signal_add (SIGTERM, on_quit_signal, NULL);
    g_object_set_data (G_OBJECT (win), "loop", loop);

    /* No address given: the start page, rather than an empty window. */
    if (!url_arg) {
        char *html = start_page_html ();
        webkit_web_view_load_html (view, html, NULL);
        g_free (html);
        gtk_window_present (GTK_WINDOW (win));
    } else {
        /* the front-end may claim the address, e.g. browser-big's "diag" */
        if (!(app->load_uri && app->load_uri (view, url_arg))) {
            char *url = normalize_uri (url_arg);
            if (!url) {
                usage_short (argv[0]);
                return 1;
            }
            LOG ("load: %s\n", url);
            webkit_web_view_load_uri (view, url);
            g_free (url);
        }
        gtk_window_present (GTK_WINDOW (win));
    }

    g_main_loop_run (loop);

    if (app->cleanup)
        app->cleanup ();

    noise_summary ();

    g_main_loop_unref (loop);
    g_free (g_css_data);
    g_free (g_data_dir);
    g_free (g_hist_path);
    g_free (g_title);
    g_free (g_app_id);
    g_free (g_clip_cmd);
    g_free (g_download_dir);
    g_free (g_profile);
    g_free (g_user_agent);
    g_free (g_css_owned);
    g_free (g_theme.font);
    g_free (g_theme.font_mono);
    return 0;
}
