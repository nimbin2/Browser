/*
 * browser_core - the shared half of minibrowser and bigbrowser
 *
 * Everything both binaries have in common lives here: window, URL bar,
 * toast, zoom, downloads, profiles, clipboard, history and the key
 * handling. A front-end supplies a BrowserApp and calls browser_main().
 *
 * minibrowser adds nothing. bigbrowser adds the camera / streaming layer.
 */

#ifndef BROWSER_CORE_H
#define BROWSER_CORE_H

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

/* Modifiers we consider "significant" when matching a shortcut.
 * Shift is deliberately absent: it is part of the key, not the combo. */
#define MOD_MASK_ALL (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)

#define ZOOM_STEP 1.1
#define ZOOM_MIN  0.25
#define ZOOM_MAX  5.0

#define URL_TOAST_SECONDS  4
#define DL_HISTORY_SECONDS 8

/* Per-window state. Owned by the GtkWindow (see window_new). */
/* what the popup is doing right now */
typedef enum { OMNI_URL, OMNI_HISTORY, OMNI_FIND, OMNI_DLDIR, OMNI_SEARCH } OmniMode;

typedef struct {
    GtkWidget     *win;
    WebKitWebView *view;
    GtkWidget     *omni;        /* the centered popup, hidden by default  */
    GtkWidget     *omnientry;   /* its input line                         */
    GtkWidget     *omnilist;    /* history matches under the input        */
    GtkWidget     *omniscroll;
    GtkWidget     *omnihint;    /* match count while finding               */
    GtkWidget     *omniscope;   /* page / site / everything, in the dir popup */
    GtkWidget     *omnialways;  /* "always replace existing files"          */
    GtkWidget     *omnibuttons; /* the confirm row, so Enter is not the only way */
    GtkWidget     *omnisave;
    GtkWidget     *omnicancel;
    GtkWidget     *urltoast;    /* the address, top left, on a new page     */
    GtkWidget     *loadbar;     /* a thin line across the top while loading */
    double         load_frac;
    guint          urltoast_id;
    GtkWidget     *keys;        /* key reference, hidden by default         */
    GtkWidget     *perm;        /* "this site wants your camera" prompt     */
    GtkWidget     *permlabel;
    GPtrArray     *permqueue;   /* requests waiting on the prompt's answer  */
    char          *permhost;
    GtkWidget     *topright;    /* box holding the toast and the downloads */
    GtkWidget     *toast;       /* GtkLabel, hidden by default            */
    GtkWidget     *dlpanel;     /* download overlay, rebuilt on refresh   */
    guint          toast_id;
    gboolean       dl_history;  /* <mod>+D list is showing                */
    guint          dl_history_id;
    gboolean       primary;
    gint64         dl_reveal_us;  /* a download just appeared; do not fade   */
    gint64         error_until_us; /* an error toast is up; do not fade      */

    /* stored-history walk, see history.c section in browser_core.c */
    gboolean       hist_walk;    /* past the end of the session list       */
    gboolean       hist_loading; /* a load we started ourselves            */
    int            hist_pos;     /* cursor into the stored history         */
    gboolean       load_failed;  /* current load did not come up           */
    guint          hist_id;      /* debounce before the entry is written   */


    /* omni popup state */
    OmniMode       omni_mode;
    gboolean       omni_list;    /* the match list is showing              */
    gboolean       omni_setting; /* we are writing to the input ourselves  */
    char          *omni_needle;  /* what the user actually typed           */

    /* the download-directory popup, while it is asking about a clash */
    gboolean       dl_conflict;
    char          *dl_pending_dir;
    int            dl_pending_scope;

    /* media mode: the frame that says it is on, and what is under the
     * pointer, so a <mod>+click on an image can go to the image viewer */
    GtkWidget     *mediaframe;
    char          *hover_link;   /* link address under the pointer, or NULL */
    char          *hover_image;  /* image address under the pointer, or NULL */
    char          *hover_media;  /* <video>/<audio> source, or NULL         */

    gpointer       ext;          /* front-end state, see BrowserApp.win_* */
} Win;

/*
 * Extension points. Every one may be NULL; minibrowser leaves almost all
 * of them NULL, which is the whole point of the split.
 */
typedef struct {
    const char *tagline;         /* first line of --help                  */
    const char *default_title;
    const char *default_app_id;
    const char *usage_arg;       /* "URL" or "URL|PATH|diag"              */

    void     (*usage_options) (GString *out);   /* extra option sections  */
    void     (*usage_keys)    (GString *out);   /* extra key bindings     */

    /* Return TRUE if argv[*i] was consumed; advance *i over its value. */
    gboolean (*parse_arg)     (int argc, char **argv, int *i);

    void     (*pre_gtk)       (void);           /* env, before gtk_init() */
    void     (*startup)       (void);           /* after gtk_init()       */
    void     (*settings_ready)(WebKitSettings *s);
    void     (*ucm_ready)     (WebKitUserContentManager *ucm);
    void     (*view_ready)    (WebKitWebView *view);
    void     (*win_ready)     (Win *w);
    void     (*win_gone)      (Win *w);

    /* Extra shortcuts. Called with our modifier already held down. */
    gboolean (*key)           (Win *w, guint key, gboolean shift);

    /* Chance to claim a raw address before it is normalised, e.g. "diag".
     * Return TRUE if the front-end loaded something itself. */
    gboolean (*load_uri)      (WebKitWebView *view, const char *raw);

    /* TRUE for addresses the front-end serves itself and that therefore
     * do not belong in the stored history. */
    gboolean (*history_skip)  (const char *uri);

    /* Front-end config keys, same file as everything else. Return TRUE if
     * the key was ours. */
    gboolean (*cfg_set)       (const char *key, const char *value);

    /* A page asked for a camera or microphone and was allowed it. Lets a
     * front-end notice that nothing ever started capturing. */
    void     (*media_asked)   (gboolean audio, gboolean video);

    void     (*cleanup)       (void);
} BrowserApp;

#ifndef BROWSER_VERSION
#define BROWSER_VERSION "0.0.0"
#endif
#ifndef BROWSER_BUILD
#define BROWSER_BUILD   "unknown"
#endif

int browser_main (int argc, char **argv, const BrowserApp *app);

/* -------------------------------------------------------- shared state */

extern WebKitSettings       *g_settings;
extern WebKitNetworkSession *g_session;

extern gboolean    g_quiet;
extern gboolean    g_deny_media;
extern gboolean    g_private;
extern double      g_zoom;
extern char       *g_title;
extern char       *g_app_id;
extern GPtrArray  *g_wins;             /* Win*, every open window */

extern GdkModifierType g_mod;
extern const char     *g_mod_name;

#define LOG(...) G_STMT_START { if (!g_quiet) g_printerr (__VA_ARGS__); } G_STMT_END

/* ------------------------------------------------------ shared helpers */

Win  *win_of    (WebKitWebView *view);
void  toast_show (Win *w, const char *text, guint seconds);
void  toast_error (Win *w, const char *text);   /* red, wrapped, 10 s, also stderr */
void  css_reload (void);
void  view_eval  (WebKitWebView *view, const char *js);
char *normalize_uri (const char *in);
void  gst_rank_env_add (const char *spec);   /* append to GST_PLUGIN_FEATURE_RANK */
void  feature_request  (const char *spec);   /* --feature NAME[=on|off], from a front-end */

#endif /* BROWSER_CORE_H */
