/*
 * browser_core - the shared half of minibrowser and bigbrowser.
 *
 * See browser_core.h for the front-end interface. Nothing in here knows
 * anything about cameras, GStreamer or diagnostics; that is bigbrowser's
 * business and it hangs off the BrowserApp hooks.
 */

#include "browser_core.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
#define HIST_KEEP  2000           /* lines kept when the file is trimmed */
#define PICK_ROWS  200            /* most matches the picker will build   */
#define HIST_SETTLE_MS 400        /* wait for the page <title> to arrive */

/* -------------------------------------------------------------- globals */

WebKitSettings       *g_settings;
WebKitNetworkSession *g_session;

gboolean    g_quiet;
gboolean    g_deny_media;
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
static int         g_windows;

static char       *g_download_dir;
static char       *g_data_dir;     /* profile data dir, NULL when private */
static char       *g_profile;      /* NULL -> "default" */
static gboolean    g_clear_data;
static char       *g_user_agent;
static gboolean    g_follow_page_title = TRUE; /* see --title / --page-title */
static char       *g_clip_cmd;     /* --clip-cmd, NULL -> wl-copy  */
static gboolean    g_no_middle_paste = TRUE;  /* --enable-middle-click-paste */
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

void
settings_set_bool_if_exists (WebKitSettings *s, const char *prop, gboolean value)
{
    if (g_object_class_find_property (G_OBJECT_GET_CLASS (s), prop))
        g_object_set (G_OBJECT (s), prop, value, NULL);
    else
        LOG ("note: WebKitSettings property not supported: %s\n", prop);
}

void
object_set_string_if_exists (GObject *o, const char *prop, const char *value)
{
    if (g_object_class_find_property (G_OBJECT_GET_CLASS (o), prop))
        g_object_set (o, prop, value, NULL);
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
    object_set_string_if_exists (G_OBJECT (g_session), "downloads-directory", g_download_dir);
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
    while (g_hash_table_iter_next (&it, &k, &v))
        g_string_append_printf (s, "%s\t%s\n", (char *) k, (char *) v);

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

    if (!data_dir)
        return;

    char *path = g_build_filename (data_dir, SEARCH_FILE, NULL);
    char *data = NULL;

    if (g_file_get_contents (path, &data, NULL, NULL)) {
        char **lines = g_strsplit (data, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!*lines[i])
                continue;
            char **f = g_strsplit (lines[i], "\t", 2);
            if (g_strv_length (f) == 2 && *f[0] && *f[1])
                g_hash_table_insert (g_searches, g_strdup (f[0]), g_strdup (f[1]));
            g_strfreev (f);
        }
        g_strfreev (lines);
        g_free (data);
        LOG ("search: %u keywords from %s\n", g_hash_table_size (g_searches), path);
    }

    g_free (path);
}

static void
search_set (const char *key, const char *url)
{
    if (!g_searches)
        g_searches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

    g_hash_table_insert (g_searches, g_strdup (key), g_strdup (url));
    searches_save ();
    LOG ("search: %s -> %s\n", key, url);
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
    Color bg, tile, tile_sel, tile_hover, card, card_hover;
    Color text, subtext, dim, accent, hl, hltext, hint, urgent, outline;

    /* geometry */
    double radius, border, pad, gap, win_gap, ui_scale;

    /* text */
    int   label_px, title_px, hint_px, ws_px;

    /* how wide a popup wants to be, before the window has its say */
    int   popup_w;
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
    t->card       = rgba_hex (0x33404ff7);
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

    t->radius   = 14.0;
    t->border   = 3.0;
    t->pad      = 10.0;
    t->gap      = 14.0;
    t->win_gap  = 5.0;
    t->ui_scale = 1.0;

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
    if (key_is (k, "tile") || key_is (k, "panel"))
                                  return parse_color (v, &t->tile);
    if (key_is (k, "tile_sel"))   return parse_color (v, &t->tile_sel);
    if (key_is (k, "tile_hover")) return parse_color (v, &t->tile_hover);
    if (key_is (k, "card"))       return parse_color (v, &t->card);
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

    /* ---- geometry, shared with swov ---- */
    if (key_is (k, "radius") || key_is (k, "corner")) { t->radius = g_ascii_strtod (v, NULL); return TRUE; }
    if (key_is (k, "border"))   { t->border  = g_ascii_strtod (v, NULL); return TRUE; }
    if (key_is (k, "pad"))      { t->pad     = g_ascii_strtod (v, NULL); return TRUE; }
    if (key_is (k, "gap"))      { t->gap     = g_ascii_strtod (v, NULL); return TRUE; }
    if (key_is (k, "win_gap") || key_is (k, "window_gap")) { t->win_gap = g_ascii_strtod (v, NULL); return TRUE; }
    if (key_is (k, "ui_scale") || key_is (k, "font_scale") || key_is (k, "text_scale")) {
        t->ui_scale = g_ascii_strtod (v, NULL);
        if (t->ui_scale <= 0.1) t->ui_scale = 1.0;
        return TRUE;
    }

    /* ---- text, shared with swov ---- */
    if (key_is (k, "popup_w") || key_is (k, "popup_width")) { t->popup_w = atoi (v); return TRUE; }
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
    if (key_is (k, "download_dir"))  { set_str (&g_download_dir, v); return TRUE; }
    if (key_is (k, "profile"))       { set_str (&g_profile, v); return TRUE; }
    if (key_is (k, "user_agent"))    { set_str (&g_user_agent, v); return TRUE; }
    if (key_is (k, "css"))           { set_str (&g_css_owned, v); g_css_path = g_css_owned; return TRUE; }
    if (key_is (k, "zoom"))          { g_zoom = CLAMP (g_ascii_strtod (v, NULL), ZOOM_MIN, ZOOM_MAX); return TRUE; }
    if (key_is (k, "private"))       { g_private = truthy (v); return TRUE; }
    if (key_is (k, "no_media"))      { g_deny_media = truthy (v); return TRUE; }
    if (key_is (k, "quiet"))         { g_quiet = truthy (v); return TRUE; }
    if (key_is (k, "page_title"))    { g_want_page_title = truthy (v); return TRUE; }
    if (key_is (k, "middle_click_paste")) { g_no_middle_paste = !truthy (v); return TRUE; }
    if (key_is (k, "always_overwrite")) {
        g_always_overwrite     = truthy (v);
        g_always_overwrite_set = TRUE;      /* the file must not undo this */
        return TRUE;
    }
    if (key_is (k, "mod"))           return parse_mod (v);

    /* search_s = https://example.com/?q={} */
    if (g_ascii_strncasecmp (k, "search_", 7) == 0 && k[7]) {
        search_set (k + 7, v);
        return TRUE;
    }

    if (g_app->cfg_set && g_app->cfg_set (k, v))
        return TRUE;

    return FALSE;
}

static void
cfg_set_line (const char *pair)
{
    const char *eq = strchr (pair, '=');
    if (!eq)
        return;

    char *k = g_strstrip (g_strndup (pair, (gsize) (eq - pair)));
    char *v = g_strstrip (g_strdup (eq + 1));

    if (*k && !cfg_set (k, v))
        LOG ("config: unknown key '%s' (ignored)\n", k);

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
    LOG ("config: read %s\n", path);
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

    cfg_load_file (shared);
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
            "  Every address that came up is appended to\n"
            "    %s\n"
            "  as a tab separated line: timestamp, URL, page title.\n", hpath);
    else
        hblock = g_strdup ("  Off, because --private was given.\n");
    const char *arg   = g_app->usage_arg ? g_app->usage_arg : "URL";

    g_string_append_printf (s, "%s - %s\n\n", argv0, g_app->tagline);
    g_string_append_printf (s, "usage: %s [%s] [options]\n"
"\n"
"  With no address the window comes up blank with the history open.\n"
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
"\n"
"page:\n"
"  --css FILE          inject FILE as a user stylesheet\n"
"  --devtools          open the inspector once the first page commits\n"
"  --no-media          deny camera / microphone / screen-share requests\n"
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
"                      are per profile, in searches.tsv. A directory that does not exist is\n"
"                      created group writable (0770)\n"
"  --profile NAME      named profile, persists cookies (default: \"default\")\n"
"  --clear-data        wipe the profile's data and cache before starting\n"
"  --private           ephemeral session, no stored history, ignores\n"
"                      --profile/--clear-data\n"
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
"  swov's file and drive both. Keys either program does not know are\n"
"  ignored with a note on stderr.\n"
"\n"
"  look:    popup_width (how wide the panels want to be, %d)\n"
"           bg tile tile_sel tile_hover card card_hover text subtext dim\n"
"           accent hl hltext hint urgent outline\n"
"           radius border pad gap win_gap ui_scale\n"
"           font font_mono label_px title_px hint_px\n"
"  ours:    title app_id zoom mod clip_cmd download_dir profile private\n"
"           no_media page_title middle_click_paste css user_agent quiet\n"
"           always_overwrite\n"
"           search_KEY (e.g. search_s = https://google.com/search?q={})\n"
"\n"
"misc:\n"
"  -q, --quiet         silence the diagnostic output on stderr\n"
"  -h, --help          this text\n"
"  -V, --version       version and build id, to tell two builds apart\n"
"\n"
"key bindings (<mod> = %s):\n"
"  <mod>+O  or <mod>+L type an address. Tab or Down brings up the matching\n"
"                      history, and with nothing typed yet, all of it.\n"
"                      A search keyword in front runs a search instead:\n"
"                      \"s tree\" with s = https://google.com/search?q={}\n"
"  <mod>+H             the same popup, opened on the history\n"
"  <mod>+F             find in the page. Enter for the next hit,\n"
"                      Shift+Enter for the previous, Esc to stop\n"
"  <mod>+S             download directory. The select decides how far it\n"
"                      reaches: this address only, everything on the site,\n"
"                      or everything. The narrower rule wins, and a rule\n"
"                      set on a page also covers the files it hands out.\n"
"                      If something else already points at that directory\n"
"                      you are asked: Enter drops the older rule, Esc lets\n"
"                      both use it. Tick \"always replace existing files\"\n"
"                      and files covered by that rule take the name they\n"
"                      ask for, with no question. \"everything\" lasts for\n"
"                      this run only; put download_dir in a config file to\n"
"                      keep it\n"
"  <mod>+K             add a search keyword, as \"g URL\" with {} where the\n"
"                      words go\n"
"  F1  or  <mod>+/     the key list, on screen\n"
"  <mod>+J             back: one page back, then on into the stored history\n"
"  <mod>+Shift+J       forward, the same way round\n"
"\n"
"  Anything the popup can save shows a Save and a Cancel button with the\n"
"  keys named on them, so Enter is never the only way in.\n"
"\n"
"  In the popup: Tab and Down walk forward through the matches, Shift+Tab\n"
"  and Up back, the wheel scrolls, a click opens, Delete removes the entry\n"
"  from the history. Whatever is selected is written into the input, so\n"
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
"  <mod>+P             show the current URL (top right) for %d seconds\n"
"  <mod>+plus          zoom in\n"
"  <mod>+minus         zoom out\n"
"  <mod>+0             reset zoom to 100%%\n"
"  <mod>+Y             copy the current URL, and show what was copied\n",
        hblock, HIST_KEEP, cfg_shared, cfg_own, g_theme.popup_w,
        g_mod_name, DL_HISTORY_SECONDS, URL_TOAST_SECONDS);

    if (g_app->usage_keys)
        g_app->usage_keys (s);

    g_string_append_printf (s,
"\n"
"  Esc closes whatever is on screen - popup, key list, download list or a\n"
"  message - and reaches the page only when nothing of ours is up.\n"
"\n"
"  Only the combinations listed above are intercepted; everything else\n"
"  (<mod>+A, <mod>+C, <mod>+V, ...) goes straight to the page, so select-all\n"
"  and copy/paste keep working inside input fields. While the URL bar is\n"
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
static void
css_apply_one (WebKitUserContentManager *ucm)
{
    webkit_user_content_manager_remove_all_style_sheets (ucm);
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

    for (guint i = 0; i < g_ucms->len; i++)
        css_apply_one (g_ptr_array_index (g_ucms, i));
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
        "box.br-dl, box.br-omni, box.br-keys, label.br-toast, label.br-urltoast {"
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
        "box.br-omni {"
        "  border-radius: 0 0 %.0fpx %.0fpx;"
        "  border-top-width: 0;"
        "}"
        /* clear of every edge: all four corners are rounded */
        "box.br-keys { border-radius: %.0fpx; }"
        /* top left corner: only the bottom right is free */
        "label.br-urltoast {"
        "  border-radius: 0 0 %.0fpx 0;"
        "  border-left-width: 0; border-top-width: 0;"
        "}",
        c_tile, t->border, c_outline, t->pad,
        t->radius, t->radius, t->radius, t->radius, t->radius);

    /* ---- primary text ---- */
    g_string_append_printf (css,
        "label.br-toast, label.br-urltoast, label.br-dl-done, label.br-pick-main,"
        "progressbar.br-dl-bar > text {"
        "  color: %s; %s font-size: %.0fpx;"
        "}",
        c_text, ui_font, t->label_px * s);

    /* URLs and file names: fixed width, so they line up and elide sanely */
    g_string_append_printf (css,
        "entry.br-omni-entry, entry.br-omni-entry > text,"
        "label.br-urltoast, label.br-dl-done, label.br-dl-failed,"
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
        c_card, t->radius / 3.0, 6.0 * s,
        c_hl,   t->radius / 3.0, 6.0 * s);

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

static gboolean
on_decide_policy (WebKitWebView            *view,
                  WebKitPolicyDecision     *decision,
                  WebKitPolicyDecisionType  type,
                  gpointer                  u)
{
    (void) view; (void) u;

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
    if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST (req))         return "device-info";
    if (WEBKIT_IS_POINTER_LOCK_PERMISSION_REQUEST (req))        return "pointer-lock";
    if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST (req))         return "geolocation";
    if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST (req))        return "notification";
    if (WEBKIT_IS_CLIPBOARD_PERMISSION_REQUEST (req))           return "clipboard";
    if (WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST (req))    return "media-key-system";
    if (WEBKIT_IS_WEBSITE_DATA_ACCESS_PERMISSION_REQUEST (req)) return "website-data-access";
    return "other";
}

static gboolean
media_permission (WebKitPermissionRequest *req)
{
    if (g_deny_media)
        webkit_permission_request_deny (req);
    else
        webkit_permission_request_allow (req);
    return TRUE;
}

static gboolean
on_permission (WebKitWebView *view, WebKitPermissionRequest *req, gpointer u)
{
    (void) view; (void) u;
    const char *type = perm_type_name (req);

    /* Answered synchronously and without a prompt: a permission dialog here
     * is time the page's getUserMedia() timeout is already counting. */
    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST (req)) {
        WebKitUserMediaPermissionRequest *m = WEBKIT_USER_MEDIA_PERMISSION_REQUEST (req);
        LOG ("perm: %s audio=%d video=%d screen=%d -> %s\n", type,
             webkit_user_media_permission_is_for_audio_device (m),
             webkit_user_media_permission_is_for_video_device (m),
             webkit_user_media_permission_is_for_display_device (m),
             g_deny_media ? "deny" : "allow");
        return media_permission (req);
    }

    /* needed so enumerateDevices() returns real labels, which most
     * conferencing sites depend on */
    if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST (req)) {
        LOG ("perm: %s -> %s\n", type, g_deny_media ? "deny" : "allow");
        return media_permission (req);
    }

    if (WEBKIT_IS_POINTER_LOCK_PERMISSION_REQUEST (req)) {
        webkit_permission_request_allow (req);
        return TRUE;
    }

    LOG ("perm: %s -> deny\n", type);
    webkit_permission_request_deny (req);
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

    GString *kept = g_string_new (NULL);

    for (guint i = first; i < n; i++) {
        char **f = g_strsplit (lines[i], "\t", 3);
        if (g_strv_length (f) >= 2 && history_uri_ok (f[1])) {
            hist_add (f[1], g_strv_length (f) >= 3 ? f[2] : "");
            g_string_append (kept, lines[i]);
            g_string_append_c (kept, '\n');
        }
        g_strfreev (f);
    }

    if (first > 0)
        g_file_set_contents (g_hist_path, kept->str, -1, NULL);

    g_string_free (kept, TRUE);
    g_strfreev (lines);
    g_free (data);

    LOG ("history: %u entries from %s\n", g_hist->len, g_hist_path);
}

static void
history_append (const char *uri, const char *title)
{
    if (!g_hist_path)
        return;

    /* reloads and in-page jumps should not pile up */
    if (g_hist->len && !g_strcmp0 (hist_at (g_hist->len - 1)->uri, uri))
        return;

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

    LOG ("history: removed %s\n", hist_at (pos)->uri);
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
        history_note (w);
    }
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

static void
omni_rebuild (Win *w)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child (w->omnilist)))
        gtk_list_box_remove (GTK_LIST_BOX (w->omnilist), child);

    char *needle = g_utf8_casefold (w->omni_needle ? w->omni_needle : "", -1);
    guint shown  = 0;

    /* newest first: what you want in a history list */
    for (guint i = g_hist->len; i > 0 && shown < PICK_ROWS; i--) {
        HistEntry *e = hist_at (i - 1);
        if (!omni_matches (e, needle))
            continue;

        GtkWidget *row = gtk_list_box_row_new ();
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

        GtkWidget *main_l = gtk_label_new (*e->title ? e->title : e->uri);
        gtk_widget_add_css_class (main_l, "br-pick-main");
        gtk_label_set_xalign (GTK_LABEL (main_l), 0.0);
        gtk_label_set_ellipsize (GTK_LABEL (main_l), PANGO_ELLIPSIZE_END);
        gtk_box_append (GTK_BOX (box), main_l);

        if (*e->title) {
            GtkWidget *sub = gtk_label_new (e->uri);
            gtk_widget_add_css_class (sub, "br-pick-sub");
            gtk_label_set_xalign (GTK_LABEL (sub), 0.0);
            gtk_label_set_ellipsize (GTK_LABEL (sub), PANGO_ELLIPSIZE_MIDDLE);
            gtk_box_append (GTK_BOX (box), sub);
        }

        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
        g_object_set_data (G_OBJECT (row), "pos", GINT_TO_POINTER ((int) i - 1));
        gtk_list_box_append (GTK_LIST_BOX (w->omnilist), row);
        shown++;
    }

    if (shown == 0) {
        GtkWidget *row = gtk_list_box_row_new ();
        GtkWidget *l   = gtk_label_new (g_hist->len ? "no match" : "no history yet");
        gtk_widget_add_css_class (l, "br-pick-sub");
        gtk_label_set_xalign (GTK_LABEL (l), 0.0);
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), l);
        gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
        gtk_list_box_append (GTK_LIST_BOX (w->omnilist), row);
    }

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

/* Delete in the popup: drop the selected row and keep the place. */
static void
omni_delete_selected (Win *w)
{
    GtkListBox    *list = GTK_LIST_BOX (w->omnilist);
    GtkListBoxRow *row  = gtk_list_box_get_selected_row (list);

    if (!row || !gtk_list_box_row_get_selectable (row))
        return;

    int idx = gtk_list_box_row_get_index (row);
    history_delete (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "pos")));

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

    int pos = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "pos"));
    if (pos < 0 || pos >= (int) g_hist->len)
        return;

    w->omni_setting = TRUE;
    gtk_editable_set_text (GTK_EDITABLE (w->omnientry), hist_at (pos)->uri);
    gtk_editable_set_position (GTK_EDITABLE (w->omnientry), -1);
    w->omni_setting = FALSE;
}

/* The list is worth as much of the window as it can decently have: a
 * fixed cap looked cramped on anything but a small screen. */
static void
omni_size_list (Win *w)
{
    int h = gtk_widget_get_height (w->win);

    if (h <= 0)
        h = 800;                        /* not mapped yet, assume the default */

    gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (w->omniscroll),
                                                MIN (320, (int) (h * 0.35)));
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (w->omniscroll),
                                                (int) (h * 0.68));
}

static void
omni_list_show (Win *w)
{
    if (w->omni_list)
        return;

    omni_size_list (w);
    omni_rebuild (w);
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

    if (w->omni_mode == OMNI_FIND)
        webkit_find_controller_search_finish (webkit_web_view_get_find_controller (w->view));

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
    else if (mode == OMNI_HISTORY)
        omni_list_show (w);
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

    /* the front-end may claim it, e.g. bigbrowser's "diag" */
    if (g_app->load_uri && g_app->load_uri (w->view, raw)) {
        g_free (raw);
        return;
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

/* WebKit scrolls the hit into view for us. */
static void
find_run (Win *w)
{
    WebKitFindController *fc = webkit_web_view_get_find_controller (w->view);
    const char           *s  = w->omni_needle ? w->omni_needle : "";

    if (!*s) {
        webkit_find_controller_search_finish (fc);
        gtk_label_set_text (GTK_LABEL (w->omnihint), "");
        return;
    }

    webkit_find_controller_count_matches (fc, s, FIND_OPTS, G_MAXUINT);
    webkit_find_controller_search (fc, s, FIND_OPTS, G_MAXUINT);
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

    char *s = count ? g_strdup_printf ("%u", count) : g_strdup ("no match");
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

    search_set (line, url);
    omni_hide (w);

    char *msg = g_strdup_printf ("search  %s  %s", line, url);
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
        gtk_widget_set_halign (w->omnihint, GTK_ALIGN_START);
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

    if (w->omni_list)
        omni_rebuild (w);
}

static void
on_omni_row_activated (GtkListBox *list, GtkListBoxRow *row, gpointer u)
{
    (void) list;
    Win *w = u;

    omni_reflect (w, row);
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

void
toast_show (Win *w, const char *text, guint seconds)
{
    gtk_label_set_text (GTK_LABEL (w->toast), text);
    gtk_widget_set_visible (w->toast, TRUE);

    if (w->toast_id)
        g_source_remove (w->toast_id);
    w->toast_id = g_timeout_add_seconds (seconds, toast_timeout, w);
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
static void
overlay_hover_update (Win *w, double x, double y)
{
    graphene_rect_t  bounds;
    graphene_point_t point = GRAPHENE_POINT_INIT ((float) x, (float) y);
    double           want  = 1.0;

    if (gtk_widget_compute_bounds (w->topright, w->win, &bounds)) {
        graphene_rect_inset (&bounds, -8.0f, -8.0f);      /* a little margin */
        if (bounds.size.width > 1 && graphene_rect_contains_point (&bounds, &point))
            want = 0.0;
    }

    /* a download that just appeared wins over the fade */
    if (want == 0.0 && w->dl_reveal_us &&
        g_get_monotonic_time () - w->dl_reveal_us < (gint64) DL_REVEAL_MS * 1000)
        want = 1.0;

    if (gtk_widget_get_opacity (w->topright) != want)
        gtk_widget_set_opacity (w->topright, want);
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
    gtk_widget_set_opacity (((Win *) u)->topright, 1.0);
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
        if ((mod && ((key == GDK_KEY_h && w->omni_mode == OMNI_HISTORY) ||
                     (key == GDK_KEY_f && w->omni_mode == OMNI_FIND) ||
                     (key == GDK_KEY_s && w->omni_mode == OMNI_DLDIR) ||
                     (key == GDK_KEY_k && w->omni_mode == OMNI_SEARCH)))) {
            omni_hide (w);
            return TRUE;
        }

        /* finding: Enter walks the hits, there is no list to steer */
        if (w->omni_mode == OMNI_FIND) {
            if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
                find_step (w, shift ? -1 : +1);
                return TRUE;
            }
            return FALSE;
        }

        /* settings modes: no list to steer, Enter applies */
        if (w->omni_mode == OMNI_DLDIR || w->omni_mode == OMNI_SEARCH)
            return FALSE;

        if (keyval == GDK_KEY_Delete || keyval == GDK_KEY_KP_Delete) {
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
    if (keyval == GDK_KEY_F12) {
        inspector_toggle (w->view);
        return TRUE;
    }
    if (keyval == GDK_KEY_F5) {
        webkit_web_view_reload (w->view);
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

    case GDK_KEY_p: {
        const char *uri = webkit_web_view_get_uri (w->view);
        toast_show (w, uri ? uri : "(no url)", URL_TOAST_SECONDS);
        return TRUE;
    }

    case GDK_KEY_y:
        clipboard_copy (w, webkit_web_view_get_uri (w->view));
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
    { "Delete",       "remove the entry from the history" },
    { "Esc",          "close" },

    { NULL, "page" },
    { "%s+F",         "find, Enter next, Shift+Enter previous" },
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

/*
 * Place our overlays ourselves, so they survive a window that is narrower
 * or shorter than they would like: everything is clamped to what the
 * window actually has, and the popup is centered in whatever is left.
 */
static gboolean
on_overlay_position (GtkOverlay *ov, GtkWidget *child, GdkRectangle *alloc, gpointer u)
{
    Win *w      = u;
    int  W      = gtk_widget_get_width (GTK_WIDGET (ov));
    int  H      = gtk_widget_get_height (GTK_WIDGET (ov));
    int  margin = (int) g_theme.gap;
    int  want, min_h, nat_h;

    if (W <= 0 || H <= 0)
        return FALSE;

    /* One width for every panel, whatever it holds: the address popup,
     * the history, the directory popup and the key list are all the same
     * object as far as the eye is concerned. */
    if (child == w->omni || child == w->keys)
        want = (int) (g_theme.popup_w * g_theme.ui_scale);
    else if (child == w->topright)
        want = (int) (360 * g_theme.ui_scale);
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
    } else if (child == w->omni) {
        alloc->x = (W - width) / 2;     /* centered, hanging off the top */
        alloc->y = 0;
    } else {
        alloc->x = (W - width) / 2;     /* centered both ways */
        alloc->y = (H - height) / 2;
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

    /* The address of the page being opened, top left. It touches the top
     * and the left, so only its bottom right corner is rounded. */
    w->urltoast = gtk_label_new ("");
    gtk_widget_add_css_class (w->urltoast, "br-urltoast");
    gtk_widget_set_halign (w->urltoast, GTK_ALIGN_START);
    gtk_widget_set_valign (w->urltoast, GTK_ALIGN_START);
    gtk_widget_set_can_target (w->urltoast, FALSE);
    gtk_widget_set_visible (w->urltoast, FALSE);
    gtk_label_set_ellipsize (GTK_LABEL (w->urltoast), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars (GTK_LABEL (w->urltoast), 70);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), w->urltoast);

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
    g_signal_connect (view, "create",             G_CALLBACK (on_create), NULL);
    g_signal_connect (view, "load-changed",       G_CALLBACK (on_load_core), NULL);
    g_signal_connect (view, "load-failed",        G_CALLBACK (on_load_failed_core), NULL);

    WebKitFindController *fc = webkit_web_view_get_find_controller (view);
    g_signal_connect (fc, "counted-matches",     G_CALLBACK (on_found_count), NULL);
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

    /* Cloudflare/Turnstile sometimes correlates missing GPU features with bots */
    settings_set_bool_if_exists (g_settings, "enable-webgl", TRUE);
    settings_set_bool_if_exists (g_settings, "enable-accelerated-2d-canvas", TRUE);

    /* lets a player pick a codec the build actually has, instead of
     * negotiating one it cannot decode and then stalling */
    settings_set_bool_if_exists (g_settings, "enable-media-capabilities", TRUE);

    if (g_user_agent && *g_user_agent) {
        webkit_settings_set_user_agent (g_settings, g_user_agent);
        LOG ("ua: %s\n", g_user_agent);
    }

    webkit_settings_set_enable_developer_extras                 (g_settings, TRUE);
    webkit_settings_set_enable_media_stream                     (g_settings, TRUE);
    webkit_settings_set_enable_webrtc                           (g_settings, TRUE);
    webkit_settings_set_enable_mediasource                      (g_settings, TRUE);
    webkit_settings_set_enable_encrypted_media                  (g_settings, TRUE);
    webkit_settings_set_enable_webaudio                         (g_settings, TRUE);
    webkit_settings_set_media_playback_requires_user_gesture    (g_settings, FALSE);
    webkit_settings_set_javascript_can_access_clipboard         (g_settings, TRUE);
    webkit_settings_set_enable_write_console_messages_to_stdout (g_settings, !g_quiet);

    if (g_app->settings_ready)
        g_app->settings_ready (g_settings);
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
        } else if (!strcmp (a, "--zoom")) {
            NEED_ARG ("--zoom");
            g_zoom = CLAMP (g_ascii_strtod (argv[++i], NULL), ZOOM_MIN, ZOOM_MAX);
        } else if (!strcmp (a, "--css")) {
            NEED_ARG ("--css");
            g_css_path = argv[++i];
        } else if (!strcmp (a, "--devtools")) {
            open_devtools = TRUE;
        } else if (!strcmp (a, "--no-media")) {
            g_deny_media = TRUE;
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
        } else {
            g_printerr ("%s: unknown argument: %s\n", argv[0], a);
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

    if (app->pre_gtk)
        app->pre_gtk ();

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
    object_set_string_if_exists (G_OBJECT (g_session), "downloads-directory", g_download_dir);

    history_setup (g_data_dir);
    dlrules_setup (g_data_dir);      /* app wide; the profile is only migrated from */
    searches_setup (g_data_dir);

    setup_settings ();
    css_reload ();

    WebKitWebView *view = view_new (NULL);

    if (open_devtools)
        g_signal_connect (view, "load-changed", G_CALLBACK (on_load_changed_devtools), NULL);

    GtkWidget *win  = window_new (view, TRUE);
    GMainLoop *loop = g_main_loop_new (NULL, FALSE);
    g_object_set_data (G_OBJECT (win), "loop", loop);

    /* No address given: come up blank with the history open, so there is
     * something to pick from rather than an empty window. */
    if (!url_arg) {
        webkit_web_view_load_uri (view, "about:blank");
        gtk_window_present (GTK_WINDOW (win));
        omni_show (win_of (view), OMNI_HISTORY);
    } else {
        /* the front-end may claim the address, e.g. bigbrowser's "diag" */
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
