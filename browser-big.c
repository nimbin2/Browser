/*
 * browser-big - WebKitGTK 6.0 (GTK4) page viewer for camera and streaming
 *
 * The window, URL bar, downloads, profiles, history and keys all come
 * from browser_core.c. What is left here is the part that makes camera
 * capture and video playback actually come up on the first try:
 *
 *   - a getUserMedia() shim that relaxes over-tight constraints, retries
 *     with progressively looser ones, and keeps the capture stream alive
 *     so the second and third call return instantly
 *   - a startup warm-up of the GStreamer/V4L2 capture stack, so the
 *     page's own getUserMedia() does not pay for device probing
 *   - a media watchdog that notices a stalled <video> and recovers it,
 *     and a load watchdog that reloads a page that never commits
 *   - a built-in diagnostics page, reachable as "diag"
 *
 * Run with -h for the full option and key list.
 */

#include "browser_core.h"

#include <gst/gst.h>

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

/* watchdogs */
#define RELOAD_MIN_GAP_SECONDS 5  /* never auto-reload faster than this */

/* -------------------------------------------------------------- globals */

static gboolean g_media_debug;

/* camera / media behaviour */
static gboolean g_cam_fix       = TRUE;    /* install the getUserMedia shim */
static gboolean g_cam_relax     = TRUE;    /* loosen the page's constraints */
static gboolean g_cam_keepalive = TRUE;    /* reuse and hold the capture stream */
static int      g_cam_retries   = 3;
static int      g_cam_max_w     = 1280;
static int      g_cam_max_h     = 720;
static int      g_cam_max_fps   = 30;
static char    *g_cam_match;               /* prefer this camera label */
static gboolean g_prewarm       = TRUE;    /* probe capture devices at startup */
static gboolean g_warm_cam;                /* open the camera before page one */
static int      g_warm_timeout  = 10;

static gboolean g_media_watchdog = TRUE;
static int      g_stall_timeout  = 6;      /* seconds of frozen playback */
static gboolean g_auto_reload    = TRUE;
static int      g_load_timeout   = 25;     /* seconds to commit a load, 0 = off */
static int      g_max_reloads    = 3;

static char    *g_shim_js;                 /* built once at startup */
static char    *g_pending_url;             /* held back while warming up */
static guint    g_warm_timeout_id;
static gboolean g_first_load_done;

/* rendering / debugging switches that only touch the environment */
static gboolean    g_no_gpu, g_no_webrtc, g_no_mediastream;
static gboolean    g_no_hw_decode, g_no_dmabuf, g_no_compositing;
static const char *g_gst_debug, *g_gst_dbgfile, *g_webkit_dbg;
static int         g_gst_level = -1;

/* Per-window watchdog bookkeeping, hung off Win.ext. */
typedef struct {
    guint   load_wd_id;      /* "load did not commit" timer      */
    char   *reload_uri;      /* URI the reload budget belongs to */
    int     reloads;         /* auto-reloads spent on that URI   */
    gint64  last_reload_us;
} BigWin;

#define EXT(w) ((BigWin *) (w)->ext)

static void load_diag_page (WebKitWebView *view);

/* Wall-clock stamp, so the UI process, the web process and GStreamer logs
 * can be lined up against each other. */
static void
tstamp (char *buf, gsize n)
{
    GDateTime *dt  = g_date_time_new_now_local ();
    char      *hms = g_date_time_format (dt, "%H:%M:%S");

    g_snprintf (buf, n, "%s.%03d", hms, g_date_time_get_microsecond (dt) / 1000);
    g_free (hms);
    g_date_time_unref (dt);
}

static void G_GNUC_PRINTF (1, 2)
mlog (const char *fmt, ...)
{
    if (g_quiet)
        return;

    char ts[32];
    tstamp (ts, sizeof ts);

    va_list ap;
    va_start (ap, fmt);
    char *msg = g_strdup_vprintf (fmt, ap);
    va_end (ap);

    g_printerr ("[%s] %s\n", ts, msg);
    g_free (msg);
}

static gboolean
is_diag_request (const char *s)
{
    return !g_strcmp0 (s, "diag") || !g_strcmp0 (s, "about:diag");
}

/* "1280x720@30", "640x480", "@15" - every part optional. */
static gboolean
parse_cam_max (const char *spec)
{
    int w = 0, h = 0, fps = 0;

    if (sscanf (spec, "%dx%d@%d", &w, &h, &fps) >= 2) {
        g_cam_max_w = w;
        g_cam_max_h = h;
        if (fps > 0)
            g_cam_max_fps = fps;
        return TRUE;
    }
    if (sscanf (spec, "@%d", &fps) == 1 && fps > 0) {
        g_cam_max_fps = fps;
        return TRUE;
    }
    return FALSE;
}

/* ================================================================== */
/*                    injected JavaScript                             */
/* ================================================================== */

/*
 * The shim runs at document-start in every frame. It does four things:
 *
 *  1. relaxes the constraints a page asks for. Exact 1280x720@30 forces the
 *     V4L2 source into one specific mode; if the camera only offers it as
 *     MJPEG the pipeline has to bring up a decoder before the promise can
 *     resolve, which is a large part of the "my cam is too slow" delay.
 *     Turning exact into ideal lets the source pick a mode it already has.
 *
 *  2. retries with progressively looser constraints instead of handing the
 *     page a NotReadableError it will usually turn into a permanent failure.
 *
 *  3. keeps the acquired tracks and hands the page *clones*. The page can
 *     stop its clone (leave a call, switch view) without closing the device,
 *     so the next getUserMedia() resolves in milliseconds instead of seconds.
 *
 *  4. watches <video>/<audio> for playback that has stopped advancing and
 *     tries play() -> seek -> load() -> reload before giving up.
 *
 * Configuration arrives as window.__bbCfg, built in main().
 */
static const char *SHIM_JS =
"(function(){"
"if(window.__bbShim)return; window.__bbShim=true;"
"var C=window.__bbCfg||{};"
"function post(o){try{o.t=Date.now();"
"  var h=window.webkit&&webkit.messageHandlers&&webkit.messageHandlers.bbEvent;"
"  if(h)h.postMessage(JSON.stringify(o));else if(C.debug)console.log('[bb]',JSON.stringify(o));"
"}catch(e){}}"
"window.__bbPost=post;"

/* ---- media watchdog ---- */
"function watchdog(){"
"  var seen=new WeakMap();"
"  function attach(v){"
"    ['error','stalled','abort','emptied','suspend'].forEach(function(n){"
"      v.addEventListener(n,function(){"
"        var code=(v.error&&v.error.code)||0;"
"        post({ev:'media-event',name:n,code:code,readyState:v.readyState,networkState:v.networkState});"
"        if(n==='error'&&(code===3||code===4))post({ev:'media-reload',reason:'media-error-'+code});"
"      });});"
"    if(C.debug)['waiting','playing','canplay','loadeddata'].forEach(function(n){"
"      v.addEventListener(n,function(){post({ev:'media-event',name:n,readyState:v.readyState});});});"
"  }"
"  function recover(v,s){"
"    post({ev:'media-stall',tries:s.tries,readyState:v.readyState,networkState:v.networkState,"
"          w:v.videoWidth||0,src:(v.currentSrc||'').slice(0,140)});"
"    try{"
"      if(s.tries===1){if(v.play)v.play().catch(function(){});return;}"
"      if(s.tries===2){"
"        if(v.seekable&&v.seekable.length){"
"          try{v.currentTime=Math.max(0,v.seekable.end(v.seekable.length-1)-0.5);}catch(e){}}"
"        if(v.play)v.play().catch(function(){});return;}"
"      if(s.tries===3&&v.currentSrc&&v.currentSrc.indexOf('blob:')!==0){"
"        v.load();if(v.play)v.play().catch(function(){});return;}"
"    }catch(e){}"
"    if(s.tries>=4){post({ev:'media-reload',reason:'stalled'});s.tries=0;}"
"  }"
"  function check(){"
"    var l=document.querySelectorAll('video,audio'),now=Date.now();"
"    for(var i=0;i<l.length;i++){var v=l[i],s=seen.get(v);"
"      if(!s){s={ct:-1,since:now,tries:0};seen.set(v,s);attach(v);}"
"      if(v.paused||v.ended||!v.currentSrc&&!v.srcObject){s.ct=v.currentTime;s.since=now;continue;}"
"      if(v.currentTime!==s.ct){s.ct=v.currentTime;s.since=now;s.tries=0;continue;}"
"      if(now-s.since<C.stall*1000)continue;"
"      s.since=now;s.tries++;recover(v,s);"
"    }"
"  }"
"  setInterval(check,1000);"
"}"
"if(C.watchdog)watchdog();"

/* ---- getUserMedia ---- */
"var md=navigator.mediaDevices;"
"if(!C.camfix||!md||!md.getUserMedia){post({ev:'shim',camfix:false});return;}"
"var origGUM=md.getUserMedia.bind(md);"
"var origEnum=md.enumerateDevices?md.enumerateDevices.bind(md):null;"
"var cachedV=null,cachedA=null,pinned=null,pinTried=false;"
"function now(){return (window.performance&&performance.now)?performance.now():Date.now();}"
"function safe(o){try{return JSON.parse(JSON.stringify(o));}catch(e){return String(o);}}"
"function num(v){if(v==null)return null;if(typeof v==='number')return v;"
"  return v.exact!=null?v.exact:v.ideal!=null?v.ideal:v.max!=null?v.max:v.min!=null?v.min:null;}"
"function relaxVideo(v,level){"
"  if(v===true||v==null)return true;"
"  if(typeof v!=='object')return v;"
"  var o={},k;for(k in v)o[k]=v[k];"
"  if(level>=1){"
"    if(o.aspectRatio!=null)delete o.aspectRatio;"
"    ['width','height','frameRate'].forEach(function(k){"
"      if(o[k]==null)return;var n=num(o[k]);if(n==null){delete o[k];return;}o[k]={ideal:n};});"
"    if(o.width&&C.maxW&&o.width.ideal>C.maxW)o.width={ideal:C.maxW};"
"    if(o.height&&C.maxH&&o.height.ideal>C.maxH)o.height={ideal:C.maxH};"
"    if(o.frameRate&&C.maxFps&&o.frameRate.ideal>C.maxFps)o.frameRate={ideal:C.maxFps};"
"    if(o.deviceId&&o.deviceId.exact)o.deviceId={ideal:o.deviceId.exact};"
"    if(o.facingMode&&o.facingMode.exact)o.facingMode={ideal:o.facingMode.exact};"
"  }"
"  if(level>=2){delete o.width;delete o.height;delete o.frameRate;delete o.facingMode;}"
"  if(level>=3)return true;"
"  return Object.keys(o).length?o:true;"
"}"
"function relax(c,level){"
"  if(!c)return c;var o={},k;for(k in c)o[k]=c[k];"
"  if(o.video)o.video=relaxVideo(o.video,level);"
"  if(level>=2&&o.audio&&typeof o.audio==='object')o.audio=true;"
"  return o;"
"}"
"function withPin(c){"
"  if(!pinned||!c||!c.video)return c;"
"  var o={},k;for(k in c)o[k]=c[k];"
"  var v=(o.video===true||typeof o.video!=='object')?{}:o.video;"
"  var nv={};for(k in v)nv[k]=v[k];"
"  if(!nv.deviceId)nv.deviceId={ideal:pinned};"
"  o.video=nv;return o;"
"}"
"function pinDevice(){"
"  if(pinTried||!C.match||!origEnum)return Promise.resolve(null);"
"  pinTried=true;"
"  return origEnum().then(function(l){"
"    var m=C.match.toLowerCase();"
"    for(var i=0;i<l.length;i++)"
"      if(l[i].kind==='videoinput'&&(l[i].label||'').toLowerCase().indexOf(m)>=0){pinned=l[i].deviceId;break;}"
"    post({ev:'cam-pin',match:C.match,found:!!pinned});return pinned;"
"  }).catch(function(){return null;});"
"}"
"function dumpTrack(t){"
"  var s={};try{s=t.getSettings?t.getSettings():{};}catch(e){}"
"  post({ev:'track',kind:t.kind,label:t.label,readyState:t.readyState,settings:s});"
"}"
"function wants(c,k){return !!(c&&c[k]);}"
"function deviceOk(track,c){"
"  if(!track||!c||typeof c.video!=='object'||!c.video.deviceId)return true;"
"  var want=c.video.deviceId.exact||c.video.deviceId;"
"  if(typeof want!=='string')return true;"
"  try{return track.getSettings().deviceId===want;}catch(e){return true;}"
"}"
"function serve(c){"
"  if(!C.cache)return null;"
"  var out=new MediaStream(),ok=true;"
"  if(wants(c,'video')){"
"    if(cachedV&&cachedV.readyState==='live'&&deviceOk(cachedV,c)){"
"      var v=cachedV.clone();"
"      if(typeof c.video==='object')try{v.applyConstraints(relaxVideo(c.video,1)).catch(function(){});}catch(e){}"
"      out.addTrack(v);"
"    }else ok=false;"
"  }"
"  if(wants(c,'audio')){"
"    if(cachedA&&cachedA.readyState==='live')out.addTrack(cachedA.clone());else ok=false;"
"  }"
"  return (ok&&out.getTracks().length)?out:null;"
"}"
"function keep(st){"
"  if(!C.cache)return st;"
"  var v=st.getVideoTracks()[0],a=st.getAudioTracks()[0];"
"  if(v)cachedV=v;if(a)cachedA=a;"
"  var out=new MediaStream();"
"  if(v)out.addTrack(v.clone());"
"  if(a)out.addTrack(a.clone());"
"  return out.getTracks().length?out:st;"
"}"
"function retryable(e){"
"  var n=e&&e.name;"
"  return n==='NotReadableError'||n==='AbortError'||n==='OverconstrainedError'||"
"         n==='TimeoutError'||n==='NotFoundError'||n==='TypeError'||!n;"
"}"
"md.getUserMedia=function(c){"
"  var t0=now();"
"  post({ev:'gum-call',constraints:safe(c)});"
"  var hit=serve(c);"
"  if(hit){post({ev:'gum-cache-hit'});return Promise.resolve(hit);}"
"  return pinDevice().then(function(){"
"    var start=C.relax?1:0,levels=[],l;"
"    for(l=start;l<=3&&levels.length<=C.retries;l++)levels.push(l);"
"    if(!levels.length)levels=[start];"
"    var i=0;"
"    function attempt(){"
"      var cc=withPin(relax(c,levels[i]));"
"      post({ev:'gum-try',level:levels[i],constraints:safe(cc)});"
"      return origGUM(cc).then(function(st){"
"        post({ev:'gum-ok',ms:Math.round(now()-t0),level:levels[i]});"
"        st.getTracks().forEach(dumpTrack);"
"        return keep(st);"
"      }).catch(function(err){"
"        post({ev:'gum-error',name:err&&err.name,message:err&&err.message,"
"              constraint:err&&err.constraint,level:levels[i],ms:Math.round(now()-t0)});"
"        i++;"
"        if(i<levels.length&&retryable(err))"
"          return new Promise(function(r){setTimeout(r,250);}).then(attempt);"
"        throw err;"
"      });"
"    }"
"    return attempt();"
"  });"
"};"
"if(origEnum)md.enumerateDevices=function(){"
"  return origEnum().then(function(l){"
"    if(C.debug)post({ev:'devices',list:l.map(function(d){return {kind:d.kind,label:d.label};})});"
"    return l;});};"
"window.__bbWarm=function(){"
"  return md.getUserMedia({video:true}).then(function(s){"
"    s.getTracks().forEach(function(t){t.stop();});"   /* stops the clone only */
"    post({ev:'warm-ok'});return true;"
"  }).catch(function(e){post({ev:'warm-error',name:e&&e.name,message:e&&e.message});return false;});"
"};"
"window.__bbRelease=function(){"
"  [cachedV,cachedA].forEach(function(t){if(t)try{t.stop();}catch(e){}});"
"  cachedV=cachedA=null;post({ev:'released'});return true;"
"};"
"window.__bbHeld=function(){"
"  return !!(cachedV&&cachedV.readyState==='live')||!!(cachedA&&cachedA.readyState==='live');"
"};"
"post({ev:'shim',camfix:true,url:location.href});"
"})();";

/* Dumps the state of every media element; used by <mod>+Shift+V. */
static const char *MEDIA_DUMP_JS =
"(function(){var o=[],l=document.querySelectorAll('video,audio');"
"for(var i=0;i<l.length;i++){var v=l[i];o.push({i:i,tag:v.tagName,readyState:v.readyState,"
"networkState:v.networkState,paused:v.paused,muted:v.muted,"
"currentTime:+(v.currentTime||0).toFixed(2),"
"duration:isFinite(v.duration)?+v.duration.toFixed(2):null,"
"w:v.videoWidth||0,h:v.videoHeight||0,err:(v.error&&v.error.code)||0,"
"src:(v.currentSrc||(v.srcObject?'[srcObject]':'')).slice(0,140)});}"
"if(window.__bbPost)window.__bbPost({ev:'media-dump',held:!!(window.__bbHeld&&__bbHeld()),elements:o});"
"else console.log('[bb] media-dump',JSON.stringify(o));})();";

/* Loaded before the real page when --warm-cam is on. Its only job is to make
 * the capture stack pay its startup cost here instead of inside the site. */
static const char *WARM_HTML =
"<!doctype html><meta charset='utf-8'><title>warming camera</title>"
"<style>body{font:14px system-ui,sans-serif;background:#111;color:#ccc;margin:24px}</style>"
"<p>warming up the camera…</p>"
"<script>"
"function done(r){try{webkit.messageHandlers.bbEvent.postMessage("
"  JSON.stringify({ev:'warm-page-done',result:r}));}catch(e){}}"
"if(navigator.mediaDevices&&navigator.mediaDevices.getUserMedia){"
"  navigator.mediaDevices.getUserMedia({video:true}).then(function(s){"
"    s.getTracks().forEach(function(t){t.stop();});done('ok');"
"  }).catch(function(e){done('error:'+e.name);});"
"}else done('unsupported');"
"</script>";

/* Built-in getUserMedia diagnostics, loaded with an https base URI so it
 * counts as a secure context. */
static const char *DIAG_HTML =
"<!doctype html><meta charset='utf-8'>"
"<title>bigbrowser media diagnostics</title>"
"<style>"
"body{font:14px/1.4 system-ui,sans-serif;margin:16px;background:#111;color:#eee}"
"button{font:inherit;margin:2px;padding:6px 10px}"
"video{width:640px;max-width:100%;background:#000;display:block;margin:8px 0}"
"pre{white-space:pre-wrap;background:#000;color:#8f8;padding:8px;max-height:40vh;overflow:auto}"
"h1{font-size:16px}"
"</style>"
"<h1>bigbrowser media diagnostics</h1>"
"<div>"
"<button id='enum'>enumerateDevices</button>"
"<button id='v'>video: true</button>"
"<button id='vhd'>video 1280x720@30 (ideal)</button>"
"<button id='vexact'>video 1280x720@30 (exact)</button>"
"<button id='ar'>video aspectRatio 16:9 (exact)</button>"
"<button id='a'>audio: true</button>"
"<button id='warm'>warm</button>"
"<button id='rel'>release</button>"
"<button id='stop'>stop</button>"
"<button id='clr'>clear log</button>"
"</div>"
"<video id='vid' autoplay playsinline muted></video>"
"<pre id='log'></pre>"
"<script>"
"var L=document.getElementById('log');"
"var V=document.getElementById('vid');"
"var cur=null,fpsTimer=null;"
"function log(){var a=[].slice.call(arguments).map(function(x){"
"  return (typeof x==='string')?x:JSON.stringify(x);}).join(' ');"
"  L.textContent+=(new Date().toISOString().substr(11,12))+' '+a+'\\n';"
"  L.scrollTop=L.scrollHeight;}"
"function stop(){if(cur){cur.getTracks().forEach(function(t){t.stop();});cur=null;}"
"  if(fpsTimer){clearInterval(fpsTimer);fpsTimer=null;}V.srcObject=null;log('stopped');}"
"function dumpTrack(t){var s={},c={};"
"  try{s=t.getSettings?t.getSettings():{};}catch(e){}"
"  try{c=t.getCapabilities?t.getCapabilities():{};}catch(e){}"
"  log('track',t.kind,'label='+JSON.stringify(t.label),'readyState='+t.readyState);"
"  log('  settings',s);log('  capabilities',c);"
"  t.onended=function(){log('track ended',t.kind);};"
"  t.onmute=function(){log('track mute',t.kind);};"
"  t.onunmute=function(){log('track unmute',t.kind);};}"
"function watchFps(){"
"  if(!('requestVideoFrameCallback' in HTMLVideoElement.prototype)){"
"    log('note: requestVideoFrameCallback unavailable, cannot measure FPS');return;}"
"  var n=0,last=performance.now();"
"  function cb(){n++;V.requestVideoFrameCallback(cb);}V.requestVideoFrameCallback(cb);"
"  fpsTimer=setInterval(function(){var t=performance.now();"
"    log('measured FPS ~'+(n*1000/(t-last)).toFixed(1));n=0;last=t;},2000);}"
"function go(c){stop();var t0=performance.now();log('getUserMedia',c);"
"  navigator.mediaDevices.getUserMedia(c).then(function(st){"
"    cur=st;V.srcObject=st;"
"    log('OK in '+Math.round(performance.now()-t0)+'ms, tracks='+st.getTracks().length);"
"    st.getTracks().forEach(dumpTrack);if(c.video)watchFps();"
"  }).catch(function(e){log('ERROR',e.name,e.message||'',e.constraint?('constraint='+e.constraint):'',"
"    'after '+Math.round(performance.now()-t0)+'ms');});}"
"document.getElementById('enum').onclick=function(){"
"  navigator.mediaDevices.enumerateDevices().then(function(ds){"
"    log('devices:');ds.forEach(function(d){log('  ',d.kind,JSON.stringify(d.label));});"
"    try{log('supportedConstraints',navigator.mediaDevices.getSupportedConstraints());}catch(e){}});};"
"document.getElementById('v').onclick=function(){go({video:true});};"
"document.getElementById('vhd').onclick=function(){go({video:{width:{ideal:1280},height:{ideal:720},frameRate:{ideal:30}}});};"
"document.getElementById('vexact').onclick=function(){go({video:{width:{exact:1280},height:{exact:720},frameRate:{exact:30}}});};"
"document.getElementById('ar').onclick=function(){go({video:{aspectRatio:{exact:1.7777777778}}});};"
"document.getElementById('a').onclick=function(){go({audio:true});};"
"document.getElementById('warm').onclick=function(){"
"  if(window.__bbWarm)__bbWarm().then(function(r){log('warm',r);});else log('shim disabled');};"
"document.getElementById('rel').onclick=function(){"
"  if(window.__bbRelease){__bbRelease();log('released held tracks');}else log('shim disabled');};"
"document.getElementById('stop').onclick=stop;"
"document.getElementById('clr').onclick=function(){L.textContent='';};"
"V.addEventListener('error',function(){log('video element error');});"
"log('ready. origin='+location.origin+' secureContext='+window.isSecureContext"
"    +' shim='+(window.__bbShim?'on':'off'));"
"</script>";

/* ------------------------------------------------- process inventory */

/* WebKitGTK 6.0 exposes no UI-process API for the auxiliary process PIDs, so
 * read them from /proc: descendant PIDs of ours whose comm starts with
 * "WebKit" (comm is truncated to 15 chars, e.g. "WebKitGPUProces"). */
static gboolean
proc_ppid_comm (pid_t pid, pid_t *ppid, char *comm, gsize commlen)
{
    char *p    = g_strdup_printf ("/proc/%d/status", (int) pid);
    char *data = NULL;
    gboolean ok = g_file_get_contents (p, &data, NULL, NULL);
    g_free (p);
    if (!ok)
        return FALSE;

    *ppid = 0;
    comm[0] = '\0';
    for (char *line = data; line && *line; ) {
        char *nl = strchr (line, '\n');
        if (nl) *nl = '\0';
        if (g_str_has_prefix (line, "Name:"))
            g_strlcpy (comm, g_strchomp (line + 5 + strspn (line + 5, " \t")), commlen);
        else if (g_str_has_prefix (line, "PPid:"))
            *ppid = (pid_t) atoi (line + 5);
        if (!nl) break;
        line = nl + 1;
    }
    g_free (data);
    return TRUE;
}

static gboolean
is_descendant (GHashTable *parent, pid_t pid, pid_t ancestor)
{
    for (int guard = 0; pid > 1 && guard < 128; guard++) {
        gpointer pp = g_hash_table_lookup (parent, GINT_TO_POINTER ((int) pid));
        if (!pp)
            return FALSE;
        pid_t par = (pid_t) GPOINTER_TO_INT (pp);
        if (par == ancestor)
            return TRUE;
        pid = par;
    }
    return FALSE;
}

static void
dump_child_processes (const char *why)
{
    DIR *d = opendir ("/proc");
    if (!d) {
        mlog ("proc: cannot open /proc (%s)", g_strerror (errno));
        return;
    }

    GHashTable *parent = g_hash_table_new (g_direct_hash, g_direct_equal);
    GHashTable *comm   = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                NULL, g_free);
    struct dirent *e;
    while ((e = readdir (d))) {
        char *end;
        long pid = strtol (e->d_name, &end, 10);
        if (*end != '\0' || pid <= 0)
            continue;
        pid_t ppid;
        char  cm[64];
        if (!proc_ppid_comm ((pid_t) pid, &ppid, cm, sizeof cm))
            continue;
        g_hash_table_insert (parent, GINT_TO_POINTER ((int) pid),
                             GINT_TO_POINTER ((int) ppid));
        g_hash_table_insert (comm, GINT_TO_POINTER ((int) pid), g_strdup (cm));
    }
    closedir (d);

    pid_t self = getpid ();
    GHashTableIter it;
    gpointer k, v;
    int found = 0;
    g_hash_table_iter_init (&it, comm);
    while (g_hash_table_iter_next (&it, &k, &v)) {
        pid_t pid = (pid_t) GPOINTER_TO_INT (k);
        const char *cm = v;
        if (!g_str_has_prefix (cm, "WebKit"))
            continue;
        if (pid == self || !is_descendant (parent, pid, self))
            continue;
        pid_t ppid = (pid_t) GPOINTER_TO_INT (
            g_hash_table_lookup (parent, GINT_TO_POINTER ((int) pid)));
        mlog ("proc: %-16s pid=%d ppid=%d", cm, (int) pid, (int) ppid);
        found++;
    }
    if (!found)
        mlog ("proc: no WebKit auxiliary processes found yet (%s)", why);

    g_hash_table_destroy (parent);
    g_hash_table_destroy (comm);
}

/* --------------------------------------------------- capture diagnostics */

static void
dump_env (const char *name)
{
    const char *v = g_getenv (name);
    mlog ("env: %-28s %s", name, v ? v : "(unset)");
}

static void
print_versions (void)
{
    char *gstv = gst_version_string ();
    mlog ("bigbrowser starting (pid %d)", (int) getpid ());
    mlog ("versions: GTK %u.%u.%u | GLib %u.%u.%u | WebKitGTK %u.%u.%u | %s",
          gtk_get_major_version (), gtk_get_minor_version (), gtk_get_micro_version (),
          glib_major_version, glib_minor_version, glib_micro_version,
          webkit_get_major_version (), webkit_get_minor_version (),
          webkit_get_micro_version (),
          gstv ? gstv : "GStreamer ?");
    g_free (gstv);
}

static void
dump_gstreamer_env (void)
{
    mlog ("---- GStreamer environment ----");
    dump_env ("GST_PLUGIN_PATH");
    dump_env ("GST_PLUGIN_SYSTEM_PATH_1_0");
    dump_env ("GST_PLUGIN_SCANNER");
    dump_env ("GST_REGISTRY_1_0");
    dump_env ("GST_DEBUG");
    dump_env ("GST_DEBUG_FILE");
    dump_env ("WEBKIT_DEBUG");
    dump_env ("WEBKIT_GST_ENABLE_HW_DECODERS");
    dump_env ("WEBKIT_DISABLE_DMABUF_RENDERER");
    dump_env ("WEBKIT_DISABLE_COMPOSITING_MODE");

    mlog ("---- capture / WebRTC element check ----");
    static const char *els[] = {
        "v4l2src", "pipewiresrc", "videoconvert", "videoscale", "videorate",
        "capsfilter", "jpegdec", "vah264dec",
        "vp8enc", "vp8dec", "vp9enc", "vp9dec",
        "openh264enc", "openh264dec", "x264enc", "avdec_h264", "avdec_aac",
        "webrtcbin", "rtpvp8pay", "opusenc", "opusdec",
        "matroskademux", "qtdemux", "hlsdemux", "dashdemux", NULL
    };
    for (int i = 0; els[i]; i++) {
        GstElementFactory *f = gst_element_factory_find (els[i]);
        mlog ("gst-element: %-16s %s", els[i], f ? "present" : "MISSING");
        if (f) gst_object_unref (f);
    }
    mlog ("---------------------------------------");
}

/*
 * Startup warm-up, step one: enumerate the capture devices ourselves.
 *
 * This builds the GStreamer plugin registry, walks udev and asks every
 * /dev/video* node for its caps - exactly the work WebKit would otherwise do
 * inside the page's first getUserMedia() call, where a site's own timeout is
 * already running. Doing it here moves that cost off the critical path and,
 * as a bonus, prints which nodes are real cameras and which are the metadata
 * nodes UVC devices expose next to them.
 *
 * Runs in an idle callback so the window is up first.
 */
static gboolean
capture_prewarm (gpointer u)
{
    (void) u;

    gint64 t0 = g_get_monotonic_time ();
    GstDeviceMonitor *mon = gst_device_monitor_new ();

    /* NULL caps: a camera that only offers MJPEG must not be filtered out */
    gst_device_monitor_add_filter (mon, "Video/Source", NULL);
    gst_device_monitor_add_filter (mon, "Audio/Source", NULL);

    if (!gst_device_monitor_start (mon)) {
        mlog ("prewarm: device monitor failed to start");
        gst_object_unref (mon);
        return G_SOURCE_REMOVE;
    }

    GList *devs = gst_device_monitor_get_devices (mon);
    int n = 0;
    for (GList *l = devs; l; l = l->next, n++) {
        GstDevice *dev   = l->data;
        char      *name  = gst_device_get_display_name (dev);
        char      *klass = gst_device_get_device_class (dev);
        GstCaps   *caps  = gst_device_get_caps (dev);

        mlog ("prewarm: %-12s %-32s %u caps", klass ? klass : "?",
              name ? name : "?", caps ? gst_caps_get_size (caps) : 0);

        if (caps && g_media_debug) {
            char *s = gst_caps_to_string (caps);
            mlog ("prewarm:   %s", s);
            g_free (s);
        }
        if (caps) gst_caps_unref (caps);
        g_free (name);
        g_free (klass);
    }
    g_list_free_full (devs, gst_object_unref);

    gst_device_monitor_stop (mon);
    gst_object_unref (mon);

    mlog ("prewarm: %d capture device(s) probed in %d ms", n,
          (int) ((g_get_monotonic_time () - t0) / 1000));
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------- capture state signals */

static const char *
capture_state_name (WebKitMediaCaptureState s)
{
    switch (s) {
        case WEBKIT_MEDIA_CAPTURE_STATE_NONE:   return "none";
        case WEBKIT_MEDIA_CAPTURE_STATE_ACTIVE: return "active";
        case WEBKIT_MEDIA_CAPTURE_STATE_MUTED:  return "muted";
        default:                                return "?";
    }
}

static void
on_capture_notify (GObject *obj, GParamSpec *ps, gpointer u)
{
    (void) u;
    WebKitWebView *view = WEBKIT_WEB_VIEW (obj);
    const char *name = g_param_spec_get_name (ps);
    WebKitMediaCaptureState st = WEBKIT_MEDIA_CAPTURE_STATE_NONE;

    if (!g_strcmp0 (name, "camera-capture-state"))
        st = webkit_web_view_get_camera_capture_state (view);
    else if (!g_strcmp0 (name, "microphone-capture-state"))
        st = webkit_web_view_get_microphone_capture_state (view);
    else if (!g_strcmp0 (name, "display-capture-state"))
        st = webkit_web_view_get_display_capture_state (view);

    mlog ("capture: %s -> %s", name, capture_state_name (st));
    if (g_media_debug)
        dump_child_processes ("capture change");
}

/* ------------------------------------------------------------ watchdogs */

/* Spend one auto-reload from this window's budget, if there is one left. */
static gboolean
reload_budget_take (Win *w, const char *why)
{
    if (!g_auto_reload || !w)
        return FALSE;

    const char *uri = webkit_web_view_get_uri (w->view);
    if (g_strcmp0 (uri, EXT (w)->reload_uri) != 0) {
        g_free (EXT (w)->reload_uri);
        EXT (w)->reload_uri = g_strdup (uri);
        EXT (w)->reloads    = 0;
    }

    gint64 nowus = g_get_monotonic_time ();
    if (nowus - EXT (w)->last_reload_us < (gint64) RELOAD_MIN_GAP_SECONDS * G_USEC_PER_SEC)
        return FALSE;

    if (EXT (w)->reloads >= g_max_reloads) {
        mlog ("watchdog: %s, but the reload budget for this page is spent", why);
        return FALSE;
    }

    EXT (w)->reloads++;
    EXT (w)->last_reload_us = nowus;
    return TRUE;
}

static void
auto_reload (Win *w, const char *why)
{
    if (!reload_budget_take (w, why))
        return;

    mlog ("watchdog: %s -> reload %d/%d", why, EXT (w)->reloads, g_max_reloads);
    toast_show (w, "reloading (watchdog)", 2);
    webkit_web_view_reload (w->view);
}

static gboolean
load_watchdog_fire (gpointer u)
{
    Win *w = u;
    EXT (w)->load_wd_id = 0;

    if (!webkit_web_view_is_loading (w->view))
        return G_SOURCE_REMOVE;

    auto_reload (w, "page did not finish loading in time");
    return G_SOURCE_REMOVE;
}

static void
load_watchdog_arm (Win *w)
{
    if (EXT (w)->load_wd_id) {
        g_source_remove (EXT (w)->load_wd_id);
        EXT (w)->load_wd_id = 0;
    }
    if (g_load_timeout > 0 && g_auto_reload)
        EXT (w)->load_wd_id = g_timeout_add_seconds (g_load_timeout, load_watchdog_fire, w);
}

static void
load_watchdog_disarm (Win *w)
{
    if (EXT (w)->load_wd_id) {
        g_source_remove (EXT (w)->load_wd_id);
        EXT (w)->load_wd_id = 0;
    }
}

static void
on_load_changed (WebKitWebView *view, WebKitLoadEvent ev, gpointer u)
{
    (void) u;
    Win *w = win_of (view);

    const char *n = "?";
    switch (ev) {
        case WEBKIT_LOAD_STARTED:    n = "started";    break;
        case WEBKIT_LOAD_REDIRECTED: n = "redirected"; break;
        case WEBKIT_LOAD_COMMITTED:  n = "committed";  break;
        case WEBKIT_LOAD_FINISHED:   n = "finished";   break;
    }

    if (g_media_debug) {
        const char *uri = webkit_web_view_get_uri (view);
        mlog ("load: %-10s %s", n, uri ? uri : "");
    }

    if (!w)
        return;

    if (ev == WEBKIT_LOAD_STARTED)
        load_watchdog_arm (w);
    else if (ev == WEBKIT_LOAD_FINISHED)
        load_watchdog_disarm (w);
}

static gboolean
on_load_failed (WebKitWebView *view, WebKitLoadEvent ev,
                gchar *uri, GError *error, gpointer u)
{
    (void) ev; (void) u;

    /* a user-cancelled load is not a failure worth reloading */
    if (error && error->domain == WEBKIT_NETWORK_ERROR &&
        error->code == WEBKIT_NETWORK_ERROR_CANCELLED)
        return FALSE;

    mlog ("load: FAILED %s: %s", uri ? uri : "",
          error ? error->message : "(no message)");

    Win *w = win_of (view);
    if (w) {
        load_watchdog_disarm (w);
        auto_reload (w, "load failed");
    }
    return FALSE;   /* let WebKit show its default error page */
}

static void
on_web_process_terminated (WebKitWebView *view,
                           WebKitWebProcessTerminationReason reason, gpointer u)
{
    (void) u;
    const char *r = "unknown";
    switch (reason) {
        case WEBKIT_WEB_PROCESS_CRASHED:                r = "crashed"; break;
        case WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT:  r = "exceeded-memory-limit"; break;
        case WEBKIT_WEB_PROCESS_TERMINATED_BY_API:      r = "terminated-by-api"; break;
    }
    mlog ("WEB PROCESS TERMINATED: %s", r);

    Win *w = win_of (view);
    if (w && reason != WEBKIT_WEB_PROCESS_TERMINATED_BY_API)
        auto_reload (w, "web process died");
}

/* ------------------------------------------------- script message routing */

static gboolean
ev_is (const char *json, const char *ev)
{
    char    *needle = g_strdup_printf ("\"ev\":\"%s\"", ev);
    gboolean hit    = json && strstr (json, needle) != NULL;
    g_free (needle);
    return hit;
}

static void
warm_finish (const char *why);

static void
on_script_message (WebKitUserContentManager *ucm, JSCValue *value, gpointer u)
{
    (void) u;

    char *s = jsc_value_to_string (value);
    if (!s)
        return;

    WebKitWebView *view = g_object_get_data (G_OBJECT (ucm), "view");
    Win           *w    = view ? win_of (view) : NULL;

    /* Everything is interesting under --media-debug; without it only the
     * events that say something went wrong or slow. */
    if (g_media_debug ||
        ev_is (s, "gum-ok")     || ev_is (s, "gum-error") ||
        ev_is (s, "gum-cache-hit") ||
        ev_is (s, "media-stall")|| ev_is (s, "media-reload") ||
        ev_is (s, "media-dump") || ev_is (s, "warm-ok") ||
        ev_is (s, "warm-error") || ev_is (s, "cam-pin"))
        mlog ("js: %s", s);

    if (ev_is (s, "warm-page-done")) {
        warm_finish ("camera warm-up finished");
    } else if (ev_is (s, "media-reload")) {
        if (w)
            auto_reload (w, "video stalled");
    } else if (ev_is (s, "media-stall") && w) {
        toast_show (w, "video stalled - recovering", 2);
    }

    g_free (s);
}

/* ------------------------------------------------------------ diag page */

static void
load_diag_page (WebKitWebView *view)
{
    mlog ("loading built-in media diagnostics page");
    webkit_web_view_load_html (view, DIAG_HTML, "https://bigbrowser.diag/");
}

/* ------------------------------------------------------------ warm-up */

/* Called when the warm-up page reports back, or when it takes too long. */
static void
warm_finish (const char *why)
{
    if (!g_pending_url)
        return;

    if (g_warm_timeout_id) {
        g_source_remove (g_warm_timeout_id);
        g_warm_timeout_id = 0;
    }

    char *url = g_pending_url;
    g_pending_url = NULL;

    mlog ("warm: %s -> loading %s", why, url);

    if (g_wins->len > 0) {
        Win *w = g_ptr_array_index (g_wins, 0);
        if (is_diag_request (url))
            load_diag_page (w->view);
        else
            webkit_web_view_load_uri (w->view, url);
    }
    g_free (url);
}

static gboolean
warm_timeout (gpointer u)
{
    (void) u;
    g_warm_timeout_id = 0;
    warm_finish ("warm-up timed out");
    return G_SOURCE_REMOVE;
}

/* JSON string literal for the shim config. */
static char *
js_quote (const char *s)
{
    if (!s)
        return g_strdup ("null");

    GString *out = g_string_new ("\"");
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_c (out, '\\');
        if ((unsigned char) *p < 0x20)
            continue;
        g_string_append_c (out, *p);
    }
    g_string_append_c (out, '"');
    return g_string_free (out, FALSE);
}

static char *
build_shim (void)
{
    char *match = js_quote (g_cam_match);
    char *cfg   = g_strdup_printf (
        "window.__bbCfg={camfix:%s,relax:%s,cache:%s,retries:%d,"
        "maxW:%d,maxH:%d,maxFps:%d,match:%s,watchdog:%s,stall:%d,debug:%s};",
        g_cam_fix        ? "true" : "false",
        g_cam_relax      ? "true" : "false",
        g_cam_keepalive  ? "true" : "false",
        g_cam_retries,
        g_cam_max_w, g_cam_max_h, g_cam_max_fps,
        match,
        g_media_watchdog ? "true" : "false",
        g_stall_timeout,
        g_media_debug    ? "true" : "false");

    char *js = g_strconcat (cfg, SHIM_JS, NULL);
    g_free (match);
    g_free (cfg);
    return js;
}

/* ---------------------------------------------------------------- hooks */

static void
big_usage_options (GString *s)
{
    g_string_append_printf (s,
"camera:\n"
"  --warm-cam          open and release the camera before the first page\n"
"                      loads, so the page's getUserMedia() is not the one\n"
"                      paying for device probing (the LED blinks once)\n"
"  --warm-timeout SEC  give up warming after SEC seconds (default: %d)\n"
"  --no-prewarm        skip the GStreamer capture-device probe at startup\n"
"  --no-cam-fix        do not touch getUserMedia() at all\n"
"  --no-cam-relax      pass the page's constraints through unchanged\n"
"  --no-cam-keepalive  do not reuse or hold on to the capture stream\n"
"  --cam-max WxH@FPS   cap relaxed constraints (default: %dx%d@%d)\n"
"  --cam-match TEXT    prefer the camera whose label contains TEXT\n"
"  --cam-retries N     retries with looser constraints (default: %d)\n"
"\n"
"video:\n"
"  --no-media-watchdog do not watch <video> elements for stalls\n"
"  --stall-timeout SEC frozen playback counts as stalled after SEC (default: %d)\n"
"  --no-auto-reload    never reload a page on our own\n"
"  --load-timeout SEC  reload if a page does not commit in SEC (0 = off,\n"
"                      default: %d)\n"
"  --max-reloads N     auto-reloads per URL (default: %d)\n"
"  --no-gpu            hardware acceleration policy NEVER\n"
"  --no-hw-decode      WEBKIT_GST_ENABLE_HW_DECODERS=0\n"
"  --no-dmabuf         WEBKIT_DISABLE_DMABUF_RENDERER=1\n"
"  --no-compositing    WEBKIT_DISABLE_COMPOSITING_MODE=1\n"
"  --no-webrtc         disable WebRTC only\n"
"  --no-mediastream    disable MediaStream / getUserMedia only\n"
"\n"
"diagnostics:\n"
"  --media-debug       verbose media, permission and capture logging\n"
"  --gst-debug SPEC    set GST_DEBUG (e.g. 'v4l2*:6,webrtc*:5')\n"
"  --gst-debug-level N set GST_DEBUG to a global level\n"
"  --gst-debug-file P  set GST_DEBUG_FILE\n"
"  --webkit-debug SPEC set WEBKIT_DEBUG (e.g. 'Media' or 'all')\n"
"\n",
        g_warm_timeout, g_cam_max_w, g_cam_max_h, g_cam_max_fps, g_cam_retries,
        g_stall_timeout, g_load_timeout, g_max_reloads);
}

static void
big_usage_keys (GString *s)
{
    g_string_append (s,
"  <mod>+Shift+M       open the built-in media diagnostics page\n"
"  <mod>+Shift+C       warm the camera now (before you press \"join\")\n"
"  <mod>+Shift+X       release the held capture stream\n"
"  <mod>+Shift+V       dump the state of every <video> to stderr\n");
}

/* Returns TRUE when argv[*i] belonged to us. */
static gboolean
big_parse_arg (int argc, char **argv, int *i)
{
    const char *a = argv[*i];

#define NEXT(opt) \
    do { if (*i + 1 >= argc) { \
             g_printerr ("%s: %s needs an argument\n", argv[0], opt); \
             exit (1); \
         } } while (0)

    /* ---- camera ---- */
    if (!strcmp (a, "--warm-cam"))            { g_warm_cam = TRUE; return TRUE; }
    if (!strcmp (a, "--no-prewarm"))          { g_prewarm = FALSE; return TRUE; }
    if (!strcmp (a, "--no-cam-fix"))          { g_cam_fix = FALSE; return TRUE; }
    if (!strcmp (a, "--no-cam-relax"))        { g_cam_relax = FALSE; return TRUE; }
    if (!strcmp (a, "--no-cam-keepalive"))    { g_cam_keepalive = FALSE; return TRUE; }

    if (!strcmp (a, "--warm-timeout")) {
        NEXT ("--warm-timeout");
        g_warm_timeout = MAX (1, atoi (argv[++(*i)]));
        return TRUE;
    }
    if (!strcmp (a, "--cam-max")) {
        NEXT ("--cam-max");
        if (!parse_cam_max (argv[++(*i)])) {
            g_printerr ("%s: bad --cam-max %s (want WxH@FPS)\n", argv[0], argv[*i]);
            exit (1);
        }
        return TRUE;
    }
    if (!strcmp (a, "--cam-match")) {
        NEXT ("--cam-match");
        g_free (g_cam_match);
        g_cam_match = g_strdup (argv[++(*i)]);
        return TRUE;
    }
    if (!strcmp (a, "--cam-retries")) {
        NEXT ("--cam-retries");
        g_cam_retries = CLAMP (atoi (argv[++(*i)]), 0, 3);
        return TRUE;
    }

    /* ---- video / rendering ---- */
    if (!strcmp (a, "--no-media-watchdog"))   { g_media_watchdog = FALSE; return TRUE; }
    if (!strcmp (a, "--no-auto-reload"))      { g_auto_reload = FALSE; return TRUE; }
    if (!strcmp (a, "--no-gpu"))              { g_no_gpu = TRUE; return TRUE; }
    if (!strcmp (a, "--no-hw-decode"))        { g_no_hw_decode = TRUE; return TRUE; }
    if (!strcmp (a, "--no-dmabuf"))           { g_no_dmabuf = TRUE; return TRUE; }
    if (!strcmp (a, "--no-compositing"))      { g_no_compositing = TRUE; return TRUE; }
    if (!strcmp (a, "--no-webrtc"))           { g_no_webrtc = TRUE; return TRUE; }
    if (!strcmp (a, "--no-mediastream"))      { g_no_mediastream = TRUE; return TRUE; }

    if (!strcmp (a, "--stall-timeout")) {
        NEXT ("--stall-timeout");
        g_stall_timeout = MAX (2, atoi (argv[++(*i)]));
        return TRUE;
    }
    if (!strcmp (a, "--load-timeout")) {
        NEXT ("--load-timeout");
        g_load_timeout = MAX (0, atoi (argv[++(*i)]));
        return TRUE;
    }
    if (!strcmp (a, "--max-reloads")) {
        NEXT ("--max-reloads");
        g_max_reloads = MAX (0, atoi (argv[++(*i)]));
        return TRUE;
    }

    /* ---- diagnostics ---- */
    if (!strcmp (a, "--media-debug"))         { g_media_debug = TRUE; return TRUE; }

    if (!strcmp (a, "--gst-debug")) {
        NEXT ("--gst-debug");
        g_gst_debug = argv[++(*i)];
        return TRUE;
    }
    if (g_str_has_prefix (a, "--gst-debug=")) {
        g_gst_debug = a + strlen ("--gst-debug=");
        return TRUE;
    }
    if (!strcmp (a, "--gst-debug-level")) {
        NEXT ("--gst-debug-level");
        g_gst_level = atoi (argv[++(*i)]);
        return TRUE;
    }
    if (g_str_has_prefix (a, "--gst-debug-level=")) {
        g_gst_level = atoi (a + strlen ("--gst-debug-level="));
        return TRUE;
    }
    if (!strcmp (a, "--gst-debug-file")) {
        NEXT ("--gst-debug-file");
        g_gst_dbgfile = argv[++(*i)];
        return TRUE;
    }
    if (g_str_has_prefix (a, "--gst-debug-file=")) {
        g_gst_dbgfile = a + strlen ("--gst-debug-file=");
        return TRUE;
    }
    if (!strcmp (a, "--webkit-debug")) {
        NEXT ("--webkit-debug");
        g_webkit_dbg = argv[++(*i)];
        return TRUE;
    }
    if (g_str_has_prefix (a, "--webkit-debug=")) {
        g_webkit_dbg = a + strlen ("--webkit-debug=");
        return TRUE;
    }

#undef NEXT
    return FALSE;
}

/* Environment has to be set before any process is spawned, so the web and
 * GPU processes - where GStreamer actually runs - inherit it. */
static void
big_pre_gtk (void)
{
    if (g_gst_debug) {
        g_setenv ("GST_DEBUG", g_gst_debug, TRUE);
    } else if (g_gst_level >= 0) {
        char *s = g_strdup_printf ("%d", g_gst_level);
        g_setenv ("GST_DEBUG", s, TRUE);
        g_free (s);
    }
    if (g_gst_dbgfile)    g_setenv ("GST_DEBUG_FILE", g_gst_dbgfile, TRUE);
    if (g_webkit_dbg)     g_setenv ("WEBKIT_DEBUG", g_webkit_dbg, TRUE);
    if (g_no_hw_decode)   g_setenv ("WEBKIT_GST_ENABLE_HW_DECODERS", "0", TRUE);
    if (g_no_dmabuf)      g_setenv ("WEBKIT_DISABLE_DMABUF_RENDERER", "1", TRUE);
    if (g_no_compositing) g_setenv ("WEBKIT_DISABLE_COMPOSITING_MODE", "1", TRUE);
}

static void
big_startup (void)
{
    gst_init (NULL, NULL);

    print_versions ();
    if (g_media_debug)
        dump_gstreamer_env ();

    if (g_cam_fix || g_media_watchdog)
        g_shim_js = build_shim ();

    /* runs once the main loop starts, i.e. after the window is up */
    if (g_prewarm)
        g_idle_add (capture_prewarm, NULL);
}

static void
big_settings_ready (WebKitSettings *s)
{
    if (g_no_mediastream) {
        webkit_settings_set_enable_media_stream (s, FALSE);
        LOG ("media: MediaStream disabled\n");
    }
    if (g_no_webrtc) {
        webkit_settings_set_enable_webrtc (s, FALSE);
        LOG ("media: WebRTC disabled\n");
    }
    if (g_no_gpu) {
        webkit_settings_set_hardware_acceleration_policy (
            s, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
        LOG ("gpu: hardware acceleration policy = NEVER\n");
    }
}

static void
big_ucm_ready (WebKitUserContentManager *ucm)
{
    /* connect before registering, to avoid a race (per WebKit docs) */
    g_signal_connect (ucm, "script-message-received::bbEvent",
                      G_CALLBACK (on_script_message), NULL);
    webkit_user_content_manager_register_script_message_handler (ucm, "bbEvent", NULL);

    if (g_shim_js) {
        WebKitUserScript *script =
            webkit_user_script_new (g_shim_js,
                                    WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                    NULL, NULL);
        webkit_user_content_manager_add_script (ucm, script);
        webkit_user_script_unref (script);
    }
}

static void
big_view_ready (WebKitWebView *view)
{
    g_signal_connect (view, "load-changed", G_CALLBACK (on_load_changed), NULL);
    g_signal_connect (view, "load-failed",  G_CALLBACK (on_load_failed), NULL);
    g_signal_connect (view, "web-process-terminated",
                      G_CALLBACK (on_web_process_terminated), NULL);

    g_signal_connect (view, "notify::camera-capture-state",
                      G_CALLBACK (on_capture_notify), NULL);
    g_signal_connect (view, "notify::microphone-capture-state",
                      G_CALLBACK (on_capture_notify), NULL);
    g_signal_connect (view, "notify::display-capture-state",
                      G_CALLBACK (on_capture_notify), NULL);
}

static void
big_win_ready (Win *w)
{
    w->ext = g_new0 (BigWin, 1);
}

static void
big_win_gone (Win *w)
{
    BigWin *e = EXT (w);
    if (!e)
        return;

    if (e->load_wd_id)
        g_source_remove (e->load_wd_id);
    g_free (e->reload_uri);
    g_free (e);
    w->ext = NULL;
}

static gboolean
big_key (Win *w, guint key, gboolean shift)
{
    if (!shift)
        return FALSE;

    switch (key) {
    case GDK_KEY_m:
        load_diag_page (w->view);
        return TRUE;
    case GDK_KEY_c:
        toast_show (w, "warming camera", 2);
        view_eval (w->view, "window.__bbWarm&&window.__bbWarm();");
        return TRUE;
    case GDK_KEY_x:
        toast_show (w, "releasing camera", 2);
        view_eval (w->view, "window.__bbRelease&&window.__bbRelease();");
        return TRUE;
    case GDK_KEY_v:
        toast_show (w, "media state -> stderr", 2);
        view_eval (w->view, MEDIA_DUMP_JS);
        return TRUE;
    default:
        return FALSE;
    }
}

/* "diag" is ours, and --warm-cam holds the first address back until the
 * camera has been opened once. */
static gboolean
big_load_uri (WebKitWebView *view, const char *raw)
{
    gboolean first = !g_first_load_done;
    g_first_load_done = TRUE;

    if (first && g_warm_cam && !g_deny_media) {
        g_pending_url = is_diag_request (raw) ? g_strdup ("diag") : normalize_uri (raw);
        g_warm_timeout_id = g_timeout_add_seconds (g_warm_timeout, warm_timeout, NULL);
        mlog ("warm: opening the camera before %s", g_pending_url);
        webkit_web_view_load_html (view, WARM_HTML, "https://bigbrowser.warm/");
        return TRUE;
    }

    if (is_diag_request (raw)) {
        load_diag_page (view);
        return TRUE;
    }

    return FALSE;
}

/* our own pages are served from load_html, they are not links */
static gboolean
big_history_skip (const char *uri)
{
    return g_str_has_prefix (uri, "https://bigbrowser.diag/") ||
           g_str_has_prefix (uri, "https://bigbrowser.warm/");
}

static void
big_cleanup (void)
{
    g_free (g_shim_js);
    g_free (g_cam_match);
}

/* ----------------------------------------------------------------- main */

static const BrowserApp browser_big = {
    .tagline        = "WebKitGTK 6.0 / GTK4 page viewer with camera and streaming fixes",
    .default_title  = "Browser Big",
    .default_app_id = "browser-big",
    .usage_arg      = "URL|PATH|diag",

    .usage_options  = big_usage_options,
    .usage_keys     = big_usage_keys,
    .parse_arg      = big_parse_arg,
    .pre_gtk        = big_pre_gtk,
    .startup        = big_startup,
    .settings_ready = big_settings_ready,
    .ucm_ready      = big_ucm_ready,
    .view_ready     = big_view_ready,
    .win_ready      = big_win_ready,
    .win_gone       = big_win_gone,
    .key            = big_key,
    .load_uri       = big_load_uri,
    .history_skip   = big_history_skip,
    .cleanup        = big_cleanup,
};

int
main (int argc, char **argv)
{
    return browser_main (argc, argv, &browser_big);
}
