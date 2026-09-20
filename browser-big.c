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
static gboolean g_shim_debug;      /* the shim reports calls; not the rest */
static int      g_prewarm_video;   /* cameras GStreamer found for us */
static int      g_prewarm_audio;   /* real microphones, monitors excluded */
static gboolean g_gum_hint_done;
static gboolean g_capture_started;   /* something actually began capturing */
static guint    g_capture_watch;

/* camera / media behaviour */
static gboolean g_cam_fix;                 /* install the getUserMedia shim */
static gboolean g_cam_relax     = TRUE;    /* loosen the page's constraints */
static gboolean g_cam_keepalive = TRUE;    /* reuse and hold the capture stream */
static gboolean g_cam_drop_audio = TRUE;   /* video-only rather than nothing */
static int      g_cam_retries   = 3;
static int      g_cam_max_w     = 1280;
static int      g_cam_max_h     = 720;
static int      g_cam_max_fps   = 30;
static int      g_cam_force_w, g_cam_force_h, g_cam_force_fps;
static gboolean g_cam_force_exact;
static gboolean g_web_compat;              /* fill in APIs WebKit lacks */
static gboolean g_cam_scale;
static int      g_cam_scale_fps = 15;      /* ceiling for the scaled output */               /* give the page the size it asked */
static gboolean g_rtc_params_fix;          /* fill in the required codecs */
static gboolean g_rtc_trace;               /* report each WebRTC step */
static gboolean g_sdp_ssrc_fix;            /* add the missing a=ssrc cname */
static char    *g_video_codecs;            /* preferred order, e.g. "VP8" */
static int      g_cam_hold_ms = 3000;      /* how long a shared capture lingers */
static char    *g_cam_match;               /* prefer this camera label */
static gboolean g_prewarm;                 /* probe capture devices at startup */
static gboolean g_warm_cam;                /* open the camera before page one */
static int      g_warm_timeout  = 10;

static gboolean g_media_watchdog;
static int      g_stall_timeout  = 6;      /* seconds of frozen playback */
static gboolean g_auto_reload;
static int      g_load_timeout   = 25;     /* seconds to commit a load, 0 = off */
static int      g_max_reloads    = 3;

static char    *g_shim_js;                 /* built once at startup */
static char    *g_pending_url;             /* held back while warming up */
static guint    g_warm_timeout_id;
static gboolean g_first_load_done;

/* rendering / debugging switches that only touch the environment */
static gboolean    g_no_webrtc, g_no_mediastream;
static const char *g_gst_debug, *g_gst_dbgfile, *g_webkit_dbg;
static const char *g_gst_rank;             /* GST_PLUGIN_FEATURE_RANK */
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
static gboolean g_list_cameras;
static void cam_list (void);

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
"var served=[],lastServe=0,lastDevices='';"
"function now(){return (window.performance&&performance.now)?performance.now():Date.now();}"
"function safe(o){try{return JSON.parse(JSON.stringify(o));}catch(e){return String(o);}}"
"function num(v){if(v==null)return null;if(typeof v==='number')return v;"
"  return v.exact!=null?v.exact:v.ideal!=null?v.ideal:v.max!=null?v.max:v.min!=null?v.min:null;}"
"function relaxVideo(v,level){"
"  if(v===true||v==null)return true;"
"  if(typeof v!=='object')return v;"
"  var o={},k;for(k in v)o[k]=v[k];"
"  if(level>=1){"
"    if(o.aspectRatio!=null){var ar=num(o.aspectRatio);"
"      if(ar==null)delete o.aspectRatio;else o.aspectRatio={ideal:ar};}"
"    ['width','height','frameRate'].forEach(function(k){"
"      if(o[k]==null)return;var n=num(o[k]);if(n==null){delete o[k];return;}o[k]={ideal:n};});"
"    if(o.width&&C.maxW&&o.width.ideal>C.maxW)o.width={ideal:C.maxW};"
"    if(o.height&&C.maxH&&o.height.ideal>C.maxH)o.height={ideal:C.maxH};"
"    if(o.frameRate&&C.maxFps&&o.frameRate.ideal>C.maxFps)o.frameRate={ideal:C.maxFps};"
"    if(o.deviceId&&o.deviceId.exact)o.deviceId={ideal:o.deviceId.exact};"
"    if(o.facingMode&&o.facingMode.exact)o.facingMode={ideal:o.facingMode.exact};"
"  }"
/* Giving up on the resolution should not give up on the shape: the
 * aspect ratio the page asked for is carried over as an ideal. */
"  if(level>=2){"
"    if(o.aspectRatio==null&&o.width&&o.height&&o.width.ideal&&o.height.ideal)"
"      o.aspectRatio={ideal:o.width.ideal/o.height.ideal};"
"    delete o.width;delete o.height;delete o.frameRate;delete o.facingMode;}"
"  if(level>=3)return true;"
"  return Object.keys(o).length?o:true;"
"}"
"function relax(c,level){"
"  if(!c)return c;var o={},k;for(k in c)o[k]=c[k];"
"  if(o.video)o.video=relaxVideo(o.video,level);"
"  if(level>=2&&o.audio&&typeof o.audio==='object')o.audio=true;"
"  return o;"
"}"
/* --cam-force: the size and shape are ours, whatever the page asked. */
"function withForce(c){"
"  if(!C.forceW||!c||!c.video)return c;"
"  var o={},k;for(k in c)o[k]=c[k];"
"  var v=(o.video===true||typeof o.video!=='object')?{}:o.video;"
"  var nv={};for(k in v)nv[k]=v[k];"
"  var k2=C.forceExact?'exact':'ideal',w={},h={},ar={},fr={};"
"  w[k2]=C.forceW;h[k2]=C.forceH;ar[k2]=C.forceW/C.forceH;"
"  nv.width=w;nv.height=h;nv.aspectRatio=ar;"
"  if(C.forceFps){fr[k2]=C.forceFps;nv.frameRate=fr;}"
"  o.video=nv;return o;"
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
/* honour what the page asked for unless relaxing was requested */
"      if(typeof c.video==='object')try{"
"        v.applyConstraints(C.relax?relaxVideo(c.video,1):c.video).catch(function(){});"
"      }catch(e){}"
"      out.addTrack(v);"
"    }else ok=false;"
"  }"
"  if(wants(c,'audio')){"
"    if(cachedA&&cachedA.readyState==='live')out.addTrack(cachedA.clone());else ok=false;"
"  }"
"  return (ok&&out.getTracks().length)?note(out):null;"
"}"
/* Remember the clones handed to the page, so we can tell when they have
 * all finished and the held source is no longer doing anybody any good. */
"function note(st){"
"  lastServe=now();"
"  st.getTracks().forEach(function(t){served.push(t);});"
"  return st;"
"}"
"function anyLive(){"
"  served=served.filter(function(t){return t.readyState==='live';});"
"  return served.length>0;"
"}"
"function keep(st){"
"  if(!C.cache)return st;"
"  var v=st.getVideoTracks()[0],a=st.getAudioTracks()[0];"
"  if(v)cachedV=v;if(a)cachedA=a;"
"  var out=new MediaStream();"
"  if(v)out.addTrack(v.clone());"
"  if(a)out.addTrack(a.clone());"
"  return out.getTracks().length?note(out):st;"
"}"
/* Which requested kind the browser has no device for, if any. */
/*
 * WebKit does not scale a capture: asked for 320x240 it picks the nearest
 * native camera mode and hands that over instead, so a site that needs
 * the size it asked for gets a stream it rejects. Chrome scales. This does
 * the same, by drawing the camera into a canvas of the requested size and
 * capturing that.
 */
"function scaleStream(st,c){"
"  if(!C.scale||!st){return Promise.resolve(st);}"
"  var wv=(c&&typeof c.video==='object')?c.video:null;"
"  var wantW=wv?num(wv.width):null,wantH=wv?num(wv.height):null,"
"      wantF=wv?num(wv.frameRate):null;"
"  var vt=st.getVideoTracks()[0];"
"  if(!wantW||!wantH||!vt){"
"    post({ev:'cam-scale-skip',why:!vt?'no video track':'no size asked'});"
"    return Promise.resolve(st);}"
"  var s={};try{s=vt.getSettings?vt.getSettings():{};}catch(e){}"
"  if(s.width===wantW&&s.height===wantH){"
"    post({ev:'cam-scale-skip',why:'already the right size'});"
"    return Promise.resolve(st);}"
"  try{"
"    var v=document.createElement('video');"
"    v.muted=true;v.defaultMuted=true;v.autoplay=true;v.playsInline=true;"
"    v.setAttribute('playsinline','');"
/* It has to be in the document, and not display:none, or some engines
 * never produce frames to draw from. Parked off-screen instead. */
"    v.style.cssText='position:fixed;left:-10000px;top:0;width:4px;height:4px;"
"opacity:0;pointer-events:none';"
"    (document.body||document.documentElement).appendChild(v);"
"    v.srcObject=new MediaStream([vt]);"
"    var cv=document.createElement('canvas');"
"    cv.width=wantW;cv.height=wantH;"
"    var ctx=cv.getContext('2d');"
"    if(!cv.captureStream){"
"      post({ev:'cam-scale-error',why:'no captureStream'});"
"      return Promise.resolve(st);}"
"    var fps=Math.min(wantF||C.scaleFps||15,C.scaleFps||15);"
/* play() is fired and not awaited: if it never settles the page's own
 * getUserMedia would hang forever, which is worse than a late first frame. */
"    try{var p=v.play();if(p&&p.catch)p.catch(function(){});}catch(e){}"
"    return new Promise(function(resolve){"
"      var done=false;"
"      function go(){"
"        if(done)return;done=true;"
/*
 * Drawing on a timer burns CPU whether or not a new frame arrived, and
 * this runs on the page's own main thread - enough to freeze a heavy
 * conference app that is already software-rendering. requestVideoFrameCallback
 * fires once per actual frame, which is both cheaper and in step.
 */
"        var stopped=false;"
"        function draw(){"
"          try{if(v.videoWidth)ctx.drawImage(v,0,0,cv.width,cv.height);}catch(e){}"
"        }"
"        var timer=null;"
"        if(v.requestVideoFrameCallback){"
"          var last=0,minGap=1000/fps;"
"          (function loop(now){"
"            if(stopped)return;"
"            if(!last||now-last>=minGap-2){last=now||0;draw();}"
"            try{v.requestVideoFrameCallback(loop);}catch(e){stopped=true;}"
"          })(0);"
"          post({ev:'cam-scale-mode',mode:'frame-callback',fps:fps});"
"        }else{"
"          timer=setInterval(draw,Math.max(33,Math.round(1000/fps)));"
"          post({ev:'cam-scale-mode',mode:'timer',fps:fps});"
"        }"
"        var out,nt;"
"        try{out=cv.captureStream(fps);nt=out.getVideoTracks()[0];}catch(e){}"
"        if(!nt){clearInterval(timer);"
"          post({ev:'cam-scale-error',why:'captureStream gave no track'});"
"          resolve(st);return;}"
"        var oStop=nt.stop.bind(nt);"
"        nt.stop=function(){stopped=true;if(timer)clearInterval(timer);"
"          try{vt.stop();}catch(e){}"
"          try{v.pause();v.srcObject=null;}catch(e){}"
"          try{v.remove();}catch(e){}"
"          oStop();};"
"        var ms=new MediaStream();"
"        ms.addTrack(nt);"
"        st.getAudioTracks().forEach(function(a){ms.addTrack(a);});"
"        ms.__bbScale={v:v,cv:cv,src:vt,timer:timer};"
"        post({ev:'cam-scaled',from:{w:s.width||0,h:s.height||0},"
"              to:{w:cv.width,h:cv.height},fps:fps});"
"        resolve(ms);"
"      }"
"      v.addEventListener('loadedmetadata',go);"
"      v.addEventListener('playing',go);"
"      setTimeout(go,400);"        /* never leave the page waiting */
"    });"
"  }catch(e){post({ev:'cam-scale-error',why:String((e&&e.message)||e)});"
"    return Promise.resolve(st);}"
"}"
"function countKinds(c){"
"  if(!origEnum)return Promise.resolve(null);"
"  return origEnum().then(function(l){"
"    var v=0,a=0,i;"
"    for(i=0;i<l.length;i++){"
"      if(l[i].kind==='videoinput')v++;"
"      else if(l[i].kind==='audioinput')a++;"
"    }"
"    if(wants(c,'video')&&!v)return 'video';"
"    if(wants(c,'audio')&&!a)return 'audio';"
"    return null;"
"  }).catch(function(){return null;});"
"}"
"function retryable(e){"
"  var n=e&&e.name;"
"  return n==='NotReadableError'||n==='AbortError'||n==='OverconstrainedError'||"
"         n==='TimeoutError'||n==='NotFoundError'||n==='TypeError'||!n;"
"}"
"md.getUserMedia=function(c){"
"  var t0=now();"
"  post({ev:'gum-call',frame:location.href.slice(0,80),constraints:safe(c)});"
"  var hit=serve(c);"
"  if(hit){post({ev:'gum-cache-hit'});return scaleStream(hit,c);}"
"  return pinDevice().then(function(){"
"    var start=C.relax?1:0,levels=[],l;"
"    for(l=start;l<=3&&levels.length<=C.retries;l++)levels.push(l);"
"    if(!levels.length)levels=[start];"
"    var i=0,audioDropped=false;"
"    function attempt(){"
"      var cc=withForce(withPin(relax(c,levels[i])));"
"      post({ev:'gum-try',level:levels[i],constraints:safe(cc)});"
"      return origGUM(cc).then(function(st){"
"        post({ev:'gum-ok',ms:Math.round(now()-t0),level:levels[i]});"
"        st.getTracks().forEach(dumpTrack);"
"        return scaleStream(keep(st),c);"
"      }).catch(function(err){"
"        post({ev:'gum-error',name:err&&err.name,message:err&&err.message,"
"              constraint:err&&err.constraint,level:levels[i],ms:Math.round(now()-t0)});"
"        i++;"
/* No device at all is not an over-tight constraint: loosening it three
 * times cannot help, it just costs the page 700ms and floods the log. */
"        return countKinds(c).then(function(missing){"
/* No microphone, but a camera: a site asking for both gets video rather
 * than nothing. It did not ask for that, so it is a choice, not a
 * default - see --no-cam-drop-audio. */
"          if(missing==='audio'&&C.dropAudio&&wants(c,'video')&&!audioDropped){"
"            audioDropped=true;"
"            post({ev:'gum-drop-audio'});"
"            var vo={},k;for(k in c)if(k!=='audio')vo[k]=c[k];"
"            c=vo;i=0;"
"            return attempt();"
"          }"
"          if(missing){"
"            post({ev:'gum-nodevices',kind:missing});"
"            throw err;"
"          }"
"          if(i<levels.length&&retryable(err))"
"            return new Promise(function(r){setTimeout(r,250);}).then(attempt);"
"          throw err;"
"        });"
"      });"
"    }"
"    return attempt();"
"  });"
"};"
"if(origEnum)md.enumerateDevices=function(){"
"  return origEnum().then(function(l){"
"    if(C.debug){"
"      var lst=l.map(function(d){return {kind:d.kind,label:d.label};});"
"      var key=JSON.stringify(lst);"
"      if(key!==lastDevices){lastDevices=key;post({ev:'devices',list:lst});}"
"    }"
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
/*
 * Holding the capture open is the whole point of sharing, but holding it
 * after the page has stopped every clone leaves the camera light on with
 * nothing watching. So the hold expires once no handed-out track is live.
 */
"if(C.cache&&C.holdMs>0)setInterval(function(){"
"  if(!cachedV&&!cachedA)return;"
"  if(anyLive()){lastServe=now();return;}"
"  if(now()-lastServe<C.holdMs)return;"
"  window.__bbRelease();post({ev:'cam-hold-expired'});"
"},1000);"
/*
 * WebKitGTK's WebRTC is GStreamer-based and its offers differ from the
 * ones a site tested against Chrome. Two of those differences stop a
 * publish dead, and both are repaired here rather than in the site:
 *
 *   - no a=ssrc:<n> cname:<v> lines, so a parser looking for the CNAME
 *     throws "CNAME value not found" and never reaches the connection
 *   - a codec order offering something this machine has no encoder for
 */
"function rtcCname(){"
"  return 'bb' + Math.floor(Math.random()*1e9).toString(36);"
"}"
"function sdpAddSsrc(sdp,cname){"
"  if(!sdp)return sdp;"
"  var parts=sdp.split(/(?=[\\r\\n]m=)/),i,changed=false;"
"  for(i=0;i<parts.length;i++){"
"    var s=parts[i];"
"    if(!/[\\r\\n]m=(audio|video)/.test(s)&&!/^m=(audio|video)/.test(s))continue;"
"    if(/a=recvonly/.test(s))continue;"
"    if(/a=ssrc:\\d+ cname:/.test(s))continue;"
"    var ssrc=Math.floor(Math.random()*4294967294)+1;"
"    var add='a=ssrc:'+ssrc+' cname:'+cname+'\\r\\n';"
"    var ms=/a=msid:(\\S+)\\s+(\\S+)/.exec(s);"
"    if(ms)add+='a=ssrc:'+ssrc+' msid:'+ms[1]+' '+ms[2]+'\\r\\n';"
"    parts[i]=s.replace(/[\\r\\n]*$/,'\\r\\n')+add;"
"    changed=true;"
"  }"
"  if(changed)post({ev:'sdp-ssrc-added'});"
"  return parts.join('');"
"}"
"function codecPrefs(pc){"
"  if(!C.codecs||!C.codecs.length)return;"
"  try{"
"    var caps=window.RTCRtpSender&&RTCRtpSender.getCapabilities?"
"             RTCRtpSender.getCapabilities('video'):null;"
"    if(!caps||!caps.codecs)return;"
"    var want=C.codecs,groups=[],rest=[],i,j;"
"    for(i=0;i<want.length;i++)groups.push([]);"
"    caps.codecs.forEach(function(c){"
"      var m=(c.mimeType||'').toLowerCase(),hit=-1;"
"      for(j=0;j<want.length;j++){"
"        var w=want[j].toLowerCase();"
"        if(m===w||m==='video/'+w)hit=j;"
"      }"
"      if(hit>=0)groups[hit].push(c);else rest.push(c);"
"    });"
"    var flat=[];"
"    groups.forEach(function(g){g.forEach(function(c){flat.push(c);});});"
"    rest.forEach(function(c){flat.push(c);});"
"    if(!flat.length)return;"
"    pc.getTransceivers().forEach(function(t){"
"      if(t.setCodecPreferences&&t.sender&&t.sender.track&&"
"         t.sender.track.kind==='video')"
"        try{t.setCodecPreferences(flat);}catch(e){}"
"    });"
"    post({ev:'codec-pref',order:flat.slice(0,4).map(function(c){return c.mimeType;})});"
"  }catch(e){}"
"}"
/*
 * Where does a publish actually stop? The offer rewriting only helps if an
 * offer is ever made, so every step before it is reported too: the
 * connection being constructed, tracks being attached, the remote answer
 * arriving. The last event seen is the step that failed.
 */
/*
 * screen.orientation is absent in WebKitGTK, and a media library reading
 * screen.orientation.type throws before it ever gets going. Supplying a
 * plausible read-only value is enough.
 */
"if(C.compat&&window.screen&&!window.screen.orientation){(function(){"
"  try{"
"    var angle=0,type='landscape-primary';"
"    var o={get angle(){return angle;},get type(){return type;},"
"           onchange:null,"
"           addEventListener:function(){},removeEventListener:function(){},"
"           dispatchEvent:function(){return false;},"
"           lock:function(){return Promise.reject(new Error('not supported'));},"
"           unlock:function(){}};"
"    try{Object.defineProperty(window.screen,'orientation',"
"        {value:o,configurable:true,enumerable:true});}"
"    catch(e){window.screen.orientation=o;}"
"    post({ev:'compat-orientation',type:type});"
"  }catch(e){post({ev:'compat-error',why:String((e&&e.message)||e)});}"
"})();}"
/* Reported whether or not WebRTC exists: its absence is the point. */
"if(C.rtcTrace){"
"  post({ev:'rtc-caps',"
"    ua:navigator.userAgent,"
"    pc:!!window.RTCPeerConnection,"
"    addTransceiver:!!((window.RTCPeerConnection&&window.RTCPeerConnection.prototype)&&(window.RTCPeerConnection&&window.RTCPeerConnection.prototype).addTransceiver),"
"    senderCaps:!!(window.RTCRtpSender&&RTCRtpSender.getCapabilities),"
"    receiverCaps:!!(window.RTCRtpReceiver&&RTCRtpReceiver.getCapabilities),"
"    dataChannel:!!((window.RTCPeerConnection&&window.RTCPeerConnection.prototype)&&(window.RTCPeerConnection&&window.RTCPeerConnection.prototype).createDataChannel),"
"    getStats:!!((window.RTCPeerConnection&&window.RTCPeerConnection.prototype)&&(window.RTCPeerConnection&&window.RTCPeerConnection.prototype).getStats),"
"    mediaRecorder:!!window.MediaRecorder,"
"    insertableStreams:!!(window.RTCRtpSender&&RTCRtpSender.prototype&&"
"                         RTCRtpSender.prototype.createEncodedStreams),"
/*
 * A conferencing web client needs more than WebRTC: it processes frames in
 * WebAssembly, often in a worker, and wants WebGL for rendering and
 * SharedArrayBuffer for zero-copy handoff. Any of these missing sends it
 * down a slow path or stops it dead, and the failure shows up as a freeze
 * rather than as an error.
 */
"    webgl:(function(){try{var c=document.createElement('canvas');"
"      return !!(c.getContext('webgl')||c.getContext('experimental-webgl'));}"
"      catch(e){return false;}})(),"
"    webgl2:(function(){try{return !!document.createElement('canvas')"
"      .getContext('webgl2');}catch(e){return false;}})(),"
"    offscreenCanvas:typeof OffscreenCanvas!=='undefined',"
"    offscreenWebgl:(function(){try{return typeof OffscreenCanvas!=='undefined'&&"
"      !!(new OffscreenCanvas(2,2).getContext('webgl'));}catch(e){return false;}})(),"
"    wasm:typeof WebAssembly!=='undefined',"
"    sharedArrayBuffer:typeof SharedArrayBuffer!=='undefined',"
"    crossOriginIsolated:(typeof crossOriginIsolated!=='undefined')?crossOriginIsolated:null,"
"    webcodecs:typeof VideoEncoder!=='undefined',"
"    worker:typeof Worker!=='undefined',"
"    rvfc:!!(document.createElement('video').requestVideoFrameCallback),"
"    vcodecs:(function(){try{return (RTCRtpSender.getCapabilities('video')"
"      .codecs||[]).map(function(c){return c.mimeType;});}catch(e){return null;}})()"
"  });""}"
"if(C.rtcTrace){(function(){"
"  var O=window.RTCPeerConnection;"
"  if(!O){post({ev:'rtc-missing'});return;}"
"  function W(){"
"    var pc=new O(arguments[0],arguments[1]);"
"    post({ev:'rtc-new'});"
"    ['icegatheringstatechange','iceconnectionstatechange',"
"     'connectionstatechange','signalingstatechange'].forEach(function(e){"
"      try{pc.addEventListener(e,function(){"
"        post({ev:'rtc-state',on:e,ice:pc.iceConnectionState,"
"              conn:pc.connectionState,sig:pc.signalingState});});}catch(x){}"
"    });"
"    return pc;"
"  }"
"  W.prototype=O.prototype;"
"  ['generateCertificate'].forEach(function(m){if(O[m])W[m]=O[m].bind(O);});"
"  window.RTCPeerConnection=W;"
"  if(window.webkitRTCPeerConnection)window.webkitRTCPeerConnection=W;"
"  ['addTrack','addTransceiver','addStream','setRemoteDescription',"
"   'createOffer','createAnswer','setLocalDescription'].forEach(function(m){"
"    var f=O.prototype[m];"
"    if(!f)return;"
"    O.prototype[m]=function(){"
"      var a0=arguments[0];"
"      var d=(m==='setRemoteDescription'&&a0&&a0.type)?a0.type:"
"            (a0&&a0.kind)?a0.kind:undefined;"
"      post({ev:'rtc-call',fn:m,arg:d});"
"      return f.apply(this,arguments);"
"    };"
"  });"
/*
 * A publisher library decides up front whether it supports this browser,
 * from the user agent and a handful of API checks. When it decides not to,
 * it never builds a connection and there is nothing further to trace - so
 * report what it would have looked at.
 */

"  post({ev:'rtc-trace-on'});"
"})();}"
/*
 * WebKit insists RTCRtpSendParameters.codecs be present; Firefox lets it
 * be omitted, so a library taking its Firefox path calls setParameters
 * without it and WebKit throws
 *
 *   TypeError: Member RTCRtpSendParameters.codecs is required
 *
 * Filling it in from the sender's current parameters is what the other
 * engines effectively do.
 */
"if(C.paramsFix&&window.RTCRtpSender&&RTCRtpSender.prototype&&"
"   RTCRtpSender.prototype.setParameters){(function(){"
"  var oSP=RTCRtpSender.prototype.setParameters;"
"  RTCRtpSender.prototype.setParameters=function(p){"
"    try{"
"      if(p&&typeof p==='object'&&!p.codecs){"
"        var cur=null;"
"        try{cur=this.getParameters?this.getParameters():null;}catch(e){}"
"        var q={},k;for(k in p)q[k]=p[k];"
"        q.codecs=(cur&&cur.codecs)?cur.codecs:[];"
"        post({ev:'send-params-fixed',had:Object.keys(p),codecs:q.codecs.length});"
"        p=q;"
"      }"
"    }catch(e){}"
"    return oSP.call(this,p);"
"  };"
"  post({ev:'params-fix-on'});"
"})();}"
"if(C.ssrcFix||(C.codecs&&C.codecs.length)){(function(){"
"  var P=window.RTCPeerConnection;"
"  if(!P){post({ev:'rtc-missing'});return;}"
"  var cname=rtcCname();"
"  var oCO=P.prototype.createOffer,oCA=P.prototype.createAnswer,"
"      oSLD=P.prototype.setLocalDescription;"
"  function patch(d){"
"    if(!C.ssrcFix||!d||!d.sdp)return d;"
"    var s=sdpAddSsrc(d.sdp,cname);"
"    return s===d.sdp?d:{type:d.type,sdp:s};"
"  }"
"  P.prototype.createOffer=function(){"
"    var pc=this,a=arguments;"
"    codecPrefs(pc);"
"    return oCO.apply(pc,a).then(patch);"
"  };"
"  P.prototype.createAnswer=function(){"
"    var pc=this,a=arguments;"
"    codecPrefs(pc);"
"    return oCA.apply(pc,a).then(patch);"
"  };"
/*
 * Injecting the lines is not the same as them surviving: WebKit reparses
 * and re-serialises the description, and a library reads them back from
 * pc.localDescription. So check afterwards and say which it was - if they
 * are stripped, this approach cannot work and it is better to know.
 */
"  P.prototype.setLocalDescription=function(d){"
"    var pc=this;"
"    return Promise.resolve(oSLD.call(pc,patch(d))).then(function(r){"
"      if(C.ssrcFix)try{"
"        var sdp=(pc.localDescription&&pc.localDescription.sdp)||'';"
"        var n=(sdp.match(/a=ssrc:\\d+ cname:/g)||[]).length;"
"        post({ev:'sdp-ssrc-verify',cnameLines:n,kept:n>0});"
"      }catch(e){}"
"      return r;"
"    });"
"  };"
"  post({ev:'rtc-wrapped',ssrcFix:!!C.ssrcFix,codecs:C.codecs||[]});"
"})();}"
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
"<title>browser-big media diagnostics</title>"
"<style>"
"body{font:14px/1.4 system-ui,sans-serif;margin:16px;background:#111;color:#eee}"
"button{font:inherit;margin:2px;padding:6px 10px}"
"video{width:640px;max-width:100%;background:#000;display:block;margin:8px 0}"
"pre{white-space:pre-wrap;background:#000;color:#8f8;padding:8px;max-height:40vh;overflow:auto}"
"h1{font-size:16px}"
"</style>"
"<h1>browser-big media diagnostics</h1>"
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
    mlog ("browser-big starting (pid %d)", (int) getpid ());
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
    g_prewarm_video = 0;
    g_prewarm_audio = 0;
    for (GList *l = devs; l; l = l->next, n++) {
        GstDevice *dev   = l->data;
        char      *name  = gst_device_get_display_name (dev);
        char      *klass = gst_device_get_device_class (dev);
        GstCaps   *caps  = gst_device_get_caps (dev);

        mlog ("prewarm: %-12s %-32s %u caps", klass ? klass : "?",
              name ? name : "?", caps ? gst_caps_get_size (caps) : 0);

        if (klass && strstr (klass, "Video"))
            g_prewarm_video++;

        /* A PulseAudio monitor records what is being played, not what a
         * microphone hears - getUserMedia cannot use it. */
        if (klass && strstr (klass, "Audio") && name) {
            if (strstr (name, "Monitor of"))
                mlog ("prewarm:   ^ a monitor, not a microphone");
            else
                g_prewarm_audio++;
        }

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

    mlog ("prewarm: %d capture device(s) probed in %d ms  (%d camera, %d microphone)",
          n, (int) ((g_get_monotonic_time () - t0) / 1000),
          g_prewarm_video, g_prewarm_audio);

    if (g_prewarm_audio == 0) {
        GstDeviceProviderFactory *alsa =
            gst_device_provider_factory_find ("alsadeviceprovider");

        mlog ("prewarm: no microphone here.");
        mlog ("prewarm: alsadeviceprovider is %s",
              alsa ? "installed" : "MISSING - only PulseAudio can enumerate,"
                                   " and a dummy sink means no devices");
        if (alsa)
            gst_object_unref (alsa);
    }
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

    if (st != WEBKIT_MEDIA_CAPTURE_STATE_NONE)
        g_capture_started = TRUE;
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
    if (g_media_debug || g_shim_debug ||
        ev_is (s, "gum-ok")     || ev_is (s, "gum-error") ||
        ev_is (s, "gum-cache-hit") || ev_is (s, "gum-drop-audio") ||
        ev_is (s, "sdp-ssrc-added") || ev_is (s, "codec-pref") ||
        ev_is (s, "rtc-wrapped")   || ev_is (s, "rtc-missing") ||
        ev_is (s, "rtc-new")       || ev_is (s, "rtc-call") ||
        ev_is (s, "rtc-state")     || ev_is (s, "rtc-trace-on") ||
        ev_is (s, "rtc-caps")      || ev_is (s, "send-params-fixed") ||
        ev_is (s, "params-fix-on") || ev_is (s, "sdp-ssrc-verify") ||
        ev_is (s, "cam-scaled")    || ev_is (s, "cam-scale-error") ||
        ev_is (s, "cam-scale-skip")|| ev_is (s, "compat-orientation") ||
        ev_is (s, "cam-scale-mode")||
        ev_is (s, "compat-error")  ||
        ev_is (s, "media-stall")|| ev_is (s, "media-reload") ||
        ev_is (s, "media-dump") || ev_is (s, "warm-ok") ||
        ev_is (s, "warm-error") || ev_is (s, "cam-pin"))
        mlog ("js: %s", s);

    /*
     * We probe the capture devices in this process with GStreamer, and
     * the page asks for them in the web process. When we found cameras
     * and the page found none, the devices are not the problem: the web
     * process could not reach them, which on a desktop means the portal,
     * and the portal needs a working D-Bus.
     */
    if (ev_is (s, "gum-drop-audio"))
        mlog ("camera: no microphone here, retrying for video alone");

    if (ev_is (s, "gum-nodevices") && !g_gum_hint_done) {
        g_gum_hint_done = TRUE;
        mlog ("camera: the page asked for a device the web process cannot see");
    }

    if ((ev_is (s, "gum-error") || ev_is (s, "gum-nodevices")) && !g_gum_hint_done) {
        g_gum_hint_done = TRUE;
        if (g_prewarm_video > 0)
            mlog ("camera: %d camera(s) are present here, but the page was given none",
                  g_prewarm_video);
        else
            mlog ("camera: --prewarm will say whether the devices are visible here");

        mlog ("camera: the web process reaches devices through the desktop portal;");
        mlog ("camera: with no portal, --no-sandbox lets it open them directly.");
        mlog ("camera: a portal needs D-Bus, which needs a machine id:");
        mlog ("camera:   dbus-uuidgen --ensure=/etc/machine-id   (no systemd needed)");
        mlog ("camera:   dbus-run-session -- browser-big URL");
    }

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

    /* ["VP8","VP9"] from a comma separated list */
    GString *cj = g_string_new ("[");
    if (g_video_codecs) {
        char **v = g_strsplit (g_video_codecs, ",", -1);
        for (int i = 0; v[i]; i++) {
            char *t = g_strstrip (g_strdup (v[i]));
            if (*t)
                g_string_append_printf (cj, "%s\"%s\"", cj->len > 1 ? "," : "", t);
            g_free (t);
        }
        g_strfreev (v);
    }
    g_string_append_c (cj, ']');
    char *codecs_js = g_string_free (cj, FALSE);
    char *cfg   = g_strdup_printf (
        "window.__bbCfg={camfix:%s,relax:%s,cache:%s,dropAudio:%s,retries:%d,"
        "maxW:%d,maxH:%d,maxFps:%d,forceW:%d,forceH:%d,forceFps:%d,holdMs:%d,"
        "forceExact:%s,ssrcFix:%s,codecs:%s,rtcTrace:%s,paramsFix:%s,scale:%s,"
        "compat:%s,scaleFps:%d,"
        "match:%s,watchdog:%s,stall:%d,debug:%s};",
        g_cam_fix        ? "true" : "false",
        g_cam_relax      ? "true" : "false",
        g_cam_keepalive  ? "true" : "false",
        g_cam_drop_audio ? "true" : "false",
        g_cam_retries,
        g_cam_max_w, g_cam_max_h, g_cam_max_fps,
        g_cam_force_w, g_cam_force_h, g_cam_force_fps, g_cam_hold_ms,
        g_cam_force_exact ? "true" : "false",
        g_sdp_ssrc_fix    ? "true" : "false",
        codecs_js,
        g_rtc_trace       ? "true" : "false",
        g_rtc_params_fix  ? "true" : "false",
        g_cam_scale       ? "true" : "false",
        g_web_compat      ? "true" : "false",
        g_cam_scale_fps,
        match,
        g_media_watchdog ? "true" : "false",
        g_stall_timeout,
        g_shim_debug     ? "true" : "false");

    char *js = g_strconcat (cfg, SHIM_JS, NULL);
    g_free (match);
    g_free (codecs_js);
    g_free (cfg);
    return js;
}

/*
 * "Sandbox disabled" is only a claim about an environment variable. This
 * checks the result: a sandboxed web process is put in its own mount
 * namespace by bwrap, so comparing namespaces answers the question for
 * real, and a bwrap in the ancestry confirms it.
 */
static gboolean
sandbox_report (gpointer u)
{
    (void) u;

    char  *ours = g_file_read_link ("/proc/self/ns/mnt", NULL);
    GDir  *d    = g_dir_open ("/proc", 0, NULL);
    pid_t  self = getpid ();

    /* Only our own web process counts. Without this it would happily
     * report on another browser's, which is worse than saying nothing. */
    GHashTable *parent = g_hash_table_new (g_direct_hash, g_direct_equal);

    if (!d) {
        g_free (ours);
        return G_SOURCE_REMOVE;
    }

    const char *name;
    while ((name = g_dir_read_name (d))) {
        if (!g_ascii_isdigit (name[0]))
            continue;

        pid_t pid = (pid_t) atoi (name);
        pid_t ppid = 0;
        char  comm[64];

        if (!proc_ppid_comm (pid, &ppid, comm, sizeof comm))
            continue;

        g_hash_table_insert (parent, GINT_TO_POINTER (pid), GINT_TO_POINTER (ppid));

        /* /proc/<pid>/status truncates Name: to 15 characters, so
         * "WebKitWebProcess" never appears in full. */
        if (!strstr (comm, "WebKitWebProc"))
            continue;
        if (ppid != self && !is_descendant (parent, pid, self))
            continue;                  /* somebody else's browser */

        char *theirs = NULL;
        char *link   = g_strdup_printf ("/proc/%d/ns/mnt", (int) pid);
        theirs = g_file_read_link (link, NULL);
        g_free (link);

        char  pcomm[64] = "";
        pid_t pppid = 0;
        proc_ppid_comm (ppid, &pppid, pcomm, sizeof pcomm);

        gboolean same = ours && theirs && !g_strcmp0 (ours, theirs);

        mlog ("sandbox: web process %d, parent %s, mount namespace %s ours"
              "  ->  %s",
              (int) pid, *pcomm ? pcomm : "?", same ? "same as" : "differs from",
              same ? "NOT sandboxed" : "sandboxed");

        g_free (theirs);
        break;
    }

    g_dir_close (d);
    g_hash_table_destroy (parent);
    g_free (ours);
    return G_SOURCE_REMOVE;
}

/*
 * A page can be allowed a camera and still get nothing, and without the
 * shim there is no gum-error to tell us. So when permission is granted,
 * wait a moment: if no device ever reports capturing, the request failed
 * somewhere below the page and that is worth saying plainly.
 */
static gboolean
capture_check (gpointer u)
{
    (void) u;
    g_capture_watch = 0;

    if (g_capture_started)
        return G_SOURCE_REMOVE;

    mlog ("capture: permission was granted but nothing started capturing.");
    mlog ("capture: run --list-cameras to see the devices this build can reach;");
    mlog ("capture: an empty list means the web process cannot get at them -");
    mlog ("capture:   --no-sandbox      opens them without the desktop portal");
    mlog ("capture:   --prewarm         says what GStreamer can see from here");
    mlog ("capture: a camera but no microphone is usually a sound server with");
    mlog ("capture: no card; audio = alsa in the config bypasses it.");
    return G_SOURCE_REMOVE;
}

static void
big_media_asked (gboolean audio, gboolean video)
{
    (void) audio; (void) video;

    if (g_capture_watch)
        g_source_remove (g_capture_watch);

    g_capture_started = FALSE;
    g_capture_watch   = g_timeout_add_seconds (5, capture_check, NULL);
}

/* ---------------------------------------------------------------- hooks */

static void
big_usage_options (GString *s)
{
    g_string_append_printf (s,
"camera and video:\n"
"  Nothing in this section is on unless you ask for it. By default the\n"
"  browser is plain WebKitGTK: no shim over getUserMedia, no device probe,\n"
"  no watchdogs, no reloads of its own. Reach for these when a site\n"
"  misbehaves, not before.\n"
"\n"
"  --fix-media         --cam-fix, --prewarm, --media-watchdog and\n"
"                      --auto-reload together\n"
"  --cam-share         hand a repeated request the stream already open,\n"
"                      and change nothing else. For a page that asks for\n"
"                      the same camera twice: the second open yields a\n"
"                      track that never delivers a frame. cam_share = yes\n"
"                      in a config file makes it permanent\n"
"  --cam-fix           relax over-tight getUserMedia constraints, retry\n"
"                      with looser ones, and hold the capture stream so a\n"
"                      second call is instant\n"
"  --list-cameras      print every format, size and frame rate each camera\n"
"                      offers, with the aspect ratio of each, then exit.\n"
"                      Use it to check a site is asking for a shape the\n"
"                      camera actually has\n"
"  --prewarm           probe the capture devices at startup, so the page's\n"
"                      own getUserMedia does not pay for it\n"
"  --media-watchdog    notice a stalled <video> and try to recover it\n"
"  --auto-reload       reload a page that never commits\n"
"  --warm-cam          open and release the camera before the first page\n"
"                      loads, so the page's getUserMedia() is not the one\n"
"                      paying for device probing (the LED blinks once)\n"
"  --warm-timeout SEC  give up warming after SEC seconds (default: %d)\n"
"  --no-cam-relax      with --cam-fix, pass the page's constraints through\n"
"                      unchanged\n"
"  --no-cam-keepalive  with --cam-fix, do not hold on to the capture stream\n"
"  --no-cam-drop-audio with --cam-fix, fail instead of handing a site\n"
"                      video only when the machine has no microphone\n"
"  --cam-max WxH@FPS   cap relaxed constraints (default: %dx%d@%d)\n"
"  --web-compat        supply APIs WebKitGTK does not implement, currently\n"
"                      screen.orientation. Zoom's media code reads\n"
"                      screen.orientation.type and throws without it\n"
"  --cam-scale         deliver the size the page asked for by scaling the\n"
"                      camera through a canvas. WebKit picks the nearest\n"
"                      native mode instead of scaling, so a site that\n"
"                      needs 320x240 is handed 848x480 and refuses it.\n"
"                      Scaling happens on the page's own thread, so the\n"
"                      rate is capped (default 15)\n"
"  --cam-scale-fps N   raise or lower that cap\n"
"  --cam-exact WxH@FPS demand this size exactly, whatever the page asked.\n"
"                      A bare width in a page's constraints is only a\n"
"                      preference and WebKit may answer with another size;\n"
"                      this refuses anything else. Fails outright if the\n"
"                      camera cannot do it, which is the honest answer\n"
"  --cam-force WxH@FPS ask for this size and aspect ratio whatever the page\n"
"                      requested, e.g. 640x480@30 for 4:3. Implies\n"
"                      --cam-fix. Use when a site negotiates a shape the\n"
"                      camera answers badly\n"
"  --cam-match TEXT    prefer the camera whose label contains TEXT\n"
"  --cam-hold SEC      how long a shared capture is held after the page\n"
"                      has stopped using it (default: 3). The camera light\n"
"                      stays on for this long; 0 releases at once\n"
"  --cam-retries N     retries with looser constraints (default: %d)\n"
"\n"
"video:\n"

"  --stall-timeout SEC frozen playback counts as stalled after SEC (default: %d)\n"

"  --load-timeout SEC  reload if a page does not commit in SEC (0 = off,\n"
"                      default: %d)\n"
"  --max-reloads N     auto-reloads per URL (default: %d)\n"

"  --no-webrtc         disable WebRTC only\n"
"  --no-mediastream    disable MediaStream / getUserMedia only\n"
"\n"
"webrtc:\n"
"  --fix-webrtc        --sdp-ssrc-fix and --rtc-params-fix together\n"
"  --rtc-params-fix    supply RTCRtpSendParameters.codecs when a library\n"
"                      omits it. WebKit requires it and Firefox does not,\n"
"                      so a library on its Firefox path throws\n"
"                      \"Member RTCRtpSendParameters.codecs is required\"\n"
"  --rtc-trace         report every WebRTC step - the connection being\n"
"                      made, tracks attached, offer created, answer set -\n"
"                      so the last one logged is the step that failed.\n"
"                      Included in --media-trace\n"
"  --sdp-ssrc-fix      add the a=ssrc cname lines WebKit leaves out, which\n"
"                      a site parsing the offer needs - without them it\n"
"                      throws \"CNAME value not found\" and never connects\n"
"  --video-codecs LIST preferred codec order for sending, e.g. VP8 or\n"
"                      VP8,VP9. Use it when WebKit offers something this\n"
"                      machine has no encoder for\n"
"\n"
"diagnostics:\n"
"  --media-trace       log every getUserMedia call, the constraints it\n"
"                      asked for and how it ended, without changing any of\n"
"                      them. The first thing to reach for when a site says\n"
"                      access was denied and the devices look fine\n"
"  --media-debug       verbose media, permission and capture logging\n"
"                      (a missing GStreamer plugin is always reported,\n"
"                      since pages fail in confusing ways without one)\n"
"  --audio-pulse       undo audio=alsa from a config file for this run\n"
"  --audio-alsa        play and record through ALSA rather than PulseAudio,\n"
"                      by ranking the pulse elements out. Fixes browser\n"
"                      audio when the Pulse server has a dummy sink.\n"
"                      Enumeration is a separate matter: device provider\n"
"                      ranks cannot be overridden, so a microphone still\n"
"                      needs alsadeviceprovider installed - check with\n"
"                      gst-inspect-1.0 alsadeviceprovider\n"
"                      Put audio = alsa in a config file to have this\n"
"                      every run, on a machine with no sound server\n"
"  --gst-rank SPEC     set GST_PLUGIN_FEATURE_RANK yourself, e.g.\n"
"                      'pulsedeviceprovider:NONE'\n"
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
    if (!strcmp (a, "--list-cameras"))        { g_list_cameras = TRUE; return TRUE; }
    if (!strcmp (a, "--warm-cam"))            { g_warm_cam = TRUE; return TRUE; }
    if (!strcmp (a, "--cam-fix"))             { g_cam_fix = TRUE; return TRUE; }
    if (!strcmp (a, "--cam-share")) {
        /*
         * Hand a repeated request the stream that is already open, and
         * nothing else - no relaxing, no retries. A camera cannot be
         * opened twice: the second open returns a live track that never
         * produces a frame, and the page waits for it forever.
         */
        g_cam_fix       = TRUE;
        g_cam_relax     = FALSE;
        g_cam_keepalive = TRUE;
        g_cam_retries   = 0;
        return TRUE;
    }
    if (!strcmp (a, "--prewarm"))             { g_prewarm = TRUE; return TRUE; }
    if (!strcmp (a, "--media-watchdog"))      { g_media_watchdog = TRUE; return TRUE; }
    if (!strcmp (a, "--auto-reload"))         { g_auto_reload = TRUE; return TRUE; }
    if (!strcmp (a, "--fix-media")) {         /* everything at once */
        g_cam_fix = g_prewarm = g_media_watchdog = g_auto_reload = TRUE;
        return TRUE;
    }
    if (!strcmp (a, "--no-prewarm"))          { g_prewarm = FALSE; return TRUE; }
    if (!strcmp (a, "--no-cam-fix"))          { g_cam_fix = FALSE; return TRUE; }
    if (!strcmp (a, "--no-cam-relax"))        { g_cam_relax = FALSE; return TRUE; }
    if (!strcmp (a, "--no-cam-keepalive"))    { g_cam_keepalive = FALSE; return TRUE; }
    if (!strcmp (a, "--no-cam-drop-audio"))  { g_cam_drop_audio = FALSE; return TRUE; }

    if (!strcmp (a, "--warm-timeout")) {
        NEXT ("--warm-timeout");
        g_warm_timeout = MAX (1, atoi (argv[++(*i)]));
        return TRUE;
    }
    if (!strcmp (a, "--web-compat")) {
        g_web_compat = TRUE;
        g_cam_fix    = TRUE;           /* it rides in the same shim */
        g_cam_relax  = FALSE;
        return TRUE;
    }
    if (!strcmp (a, "--cam-scale-fps")) {
        NEXT ("--cam-scale-fps");
        g_cam_scale_fps = CLAMP (atoi (argv[++(*i)]), 1, 60);
        g_cam_scale = g_cam_fix = TRUE;
        g_cam_relax = FALSE;
        return TRUE;
    }
    if (!strcmp (a, "--cam-scale")) {
        g_cam_scale = TRUE;
        g_cam_fix   = TRUE;
        g_cam_relax = FALSE;           /* the page's size is the point */
        return TRUE;
    }
    if (!strcmp (a, "--cam-exact")) {
        /*
         * exact, not ideal. A bare width/height in a page's constraints is
         * only a preference and WebKit is free to answer with another
         * size - which it does, so a site that publishes at the size it
         * asked for gets a stream it cannot use.
         */
        NEXT ("--cam-exact");
        int sw = g_cam_max_w, sh = g_cam_max_h, sf = g_cam_max_fps;
        if (!parse_cam_max (argv[++(*i)])) {
            g_printerr ("%s: bad --cam-exact %s (want WxH@FPS)\n", argv[0], argv[*i]);
            exit (1);
        }
        g_cam_force_w = g_cam_max_w;  g_cam_force_h = g_cam_max_h;
        g_cam_force_fps = g_cam_max_fps;
        g_cam_max_w = sw; g_cam_max_h = sh; g_cam_max_fps = sf;
        g_cam_force_exact = TRUE;
        g_cam_fix = TRUE;
        g_cam_relax = FALSE;           /* relaxing would undo the point */
        return TRUE;
    }
    if (!strcmp (a, "--cam-force")) {
        NEXT ("--cam-force");
        int sw = g_cam_max_w, sh = g_cam_max_h, sf = g_cam_max_fps;
        if (!parse_cam_max (argv[++(*i)])) {
            g_printerr ("%s: bad --cam-force %s (want WxH@FPS)\n", argv[0], argv[*i]);
            exit (1);
        }
        g_cam_force_w = g_cam_max_w;  g_cam_force_h = g_cam_max_h;
        g_cam_force_fps = g_cam_max_fps;
        g_cam_max_w = sw; g_cam_max_h = sh; g_cam_max_fps = sf;
        g_cam_fix = TRUE;              /* it is applied by the shim */
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
    if (!strcmp (a, "--cam-hold")) {
        NEXT ("--cam-hold");
        g_cam_hold_ms = MAX (0, atoi (argv[++(*i)])) * 1000;
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
    if (!strcmp (a, "--rtc-params-fix")) {
        g_rtc_params_fix = TRUE;
        g_cam_fix        = TRUE;
        g_cam_relax      = FALSE;
        return TRUE;
    }
    if (!strcmp (a, "--fix-webrtc")) {     /* both offer repairs at once */
        g_rtc_params_fix = TRUE;
        g_sdp_ssrc_fix   = TRUE;
        g_cam_fix        = TRUE;
        g_cam_relax      = FALSE;
        return TRUE;
    }
    if (!strcmp (a, "--rtc-trace")) {
        g_rtc_trace = TRUE;
        g_cam_fix   = TRUE;            /* it rides in the same shim */
        g_cam_relax = FALSE;
        g_shim_debug = TRUE;
        return TRUE;
    }
    if (!strcmp (a, "--sdp-ssrc-fix")) {
        g_sdp_ssrc_fix = TRUE;
        g_cam_fix      = TRUE;         /* it rides in the same shim */
        g_cam_relax    = FALSE;
        return TRUE;
    }
    if (!strcmp (a, "--video-codecs")) {
        NEXT ("--video-codecs");
        g_free (g_video_codecs);
        g_video_codecs = g_strdup (argv[++(*i)]);
        g_cam_fix   = TRUE;
        g_cam_relax = FALSE;
        return TRUE;
    }
    if (!strcmp (a, "--media-debug")) {
        g_media_debug = g_shim_debug = TRUE;
        return TRUE;
    }

    if (!strcmp (a, "--media-trace")) {
        /*
         * Watch without touching: the shim is installed purely to log
         * every getUserMedia call and its result. Relaxing, caching,
         * retrying and the audio fallback are all off, so the page gets
         * exactly the behaviour it would have had without us - which is
         * the point of a trace.
         */
        g_cam_fix        = TRUE;
        g_cam_relax      = FALSE;
        g_cam_keepalive  = FALSE;
        g_cam_drop_audio = FALSE;
        g_cam_retries    = 0;
        g_shim_debug     = TRUE;
        g_rtc_trace      = TRUE;       /* a trace should cover WebRTC too */
        return TRUE;
    }

    if (!strcmp (a, "--audio-pulse")) { g_gst_rank = NULL; return TRUE; }
    if (!strcmp (a, "--audio-alsa")) {
        /*
         * Rank PulseAudio's elements out so autoaudiosink/autoaudiosrc
         * pick ALSA. This fixes playback on a machine whose Pulse server
         * came up with a dummy sink.
         *
         * It does NOT change which devices are enumerated:
         * GST_PLUGIN_FEATURE_RANK is honoured for elements but ignored for
         * device providers, so the microphone still depends on
         * alsadeviceprovider being installed. Checked, not assumed.
         */
        g_gst_rank = "pulsesink:NONE,pulsesrc:NONE,"
                     "alsasink:PRIMARY,alsasrc:PRIMARY,"
                     "pulsedeviceprovider:NONE,alsadeviceprovider:PRIMARY";
        return TRUE;
    }
    if (!strcmp (a, "--gst-rank")) {
        NEXT ("--gst-rank");
        g_gst_rank = argv[++(*i)];
        return TRUE;
    }

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
/*
 * audio = alsa | pulse
 *
 * Not defaulted to alsa in the binary on purpose: on a machine where
 * PipeWire or PulseAudio is healthy, ranking pulsesink out makes WebKit
 * open the card through alsasink directly, which without a dmix setup
 * takes it exclusively and silences everything else. It is the right
 * setting for a machine with no working sound server, and that is a
 * property of the machine, so it belongs in that machine's config.
 */
/* the core keeps its own truthy(); this is browser-big's copy */
static gboolean
truthy_value (const char *v)
{
    return !(!g_ascii_strcasecmp (v, "0")  || !g_ascii_strcasecmp (v, "no") ||
             !g_ascii_strcasecmp (v, "off")|| !g_ascii_strcasecmp (v, "false"));
}

static gboolean
big_cfg_set (const char *key, const char *value)
{
    /*
     * cam_share = yes
     *
     * WebKitGTK does not multiplex one camera across two getUserMedia
     * calls the way Chrome and Firefox do, so a page that asks twice gets
     * a second track that never delivers a frame. Sharing hands the
     * repeated request a clone of the live track over the single open,
     * which is what the other engines do internally.
     */
    if (!g_ascii_strcasecmp (key, "cam_share")) {
        if (truthy_value (value)) {
            g_cam_fix       = TRUE;
            g_cam_relax     = FALSE;
            g_cam_keepalive = TRUE;
            g_cam_retries   = 0;
        }
        return TRUE;
    }

    if (g_ascii_strcasecmp (key, "audio") != 0)
        return FALSE;

    if (!g_ascii_strcasecmp (value, "alsa")) {
        g_gst_rank = "pulsesink:NONE,pulsesrc:NONE,"
                     "alsasink:PRIMARY,alsasrc:PRIMARY,"
                     "pulsedeviceprovider:NONE,alsadeviceprovider:PRIMARY";
    } else if (!g_ascii_strcasecmp (value, "pulse") ||
               !g_ascii_strcasecmp (value, "auto")) {
        g_gst_rank = NULL;
    } else {
        g_printerr ("config: audio must be alsa, pulse or auto\n");
    }
    return TRUE;
}

static void
big_pre_gtk (void)
{
    /* Before gtk_init, so asking what the camera can do does not need a
     * display - this has to work over ssh and from a script. */
    if (g_list_cameras) {
        gst_init (NULL, NULL);
        cam_list ();
        exit (0);
    }

    if (g_gst_debug) {
        g_setenv ("GST_DEBUG", g_gst_debug, TRUE);
    } else if (g_gst_level >= 0) {
        char *s = g_strdup_printf ("%d", g_gst_level);
        g_setenv ("GST_DEBUG", s, TRUE);
        g_free (s);
    }
    if (g_gst_rank) {
        g_setenv ("GST_PLUGIN_FEATURE_RANK", g_gst_rank, TRUE);
        mlog ("gstreamer: GST_PLUGIN_FEATURE_RANK=%s", g_gst_rank);
    }
    if (g_gst_dbgfile)    g_setenv ("GST_DEBUG_FILE", g_gst_dbgfile, TRUE);
    if (g_webkit_dbg)     g_setenv ("WEBKIT_DEBUG", g_webkit_dbg, TRUE);
}

/*
 * WebKit plays media and talks WebRTC through GStreamer, so a missing
 * plugin looks like a broken site rather than a missing package. Silent
 * when everything needed is present.
 */
static void
codec_check (void)
{
    /*
     * webrtcbin alone is not enough: a call also needs ICE from libnice,
     * SRTP, DTLS and the RTP manager. Naming each one separately turns
     * "WebRTC does not work" into a shopping list.
     */
    static const struct { const char *element; const char *what; } need[] = {
        { "webrtcbin",   "WebRTC             gst-plugins-bad, built with libnice" },
        { "nicesrc",     "ICE                libnice, built with GStreamer support" },
        { "srtpenc",     "SRTP               gst-plugins-bad, needs libsrtp2" },
        { "dtlssrtpenc", "DTLS               gst-plugins-bad, needs OpenSSL" },
        { "rtpbin",      "RTP                gst-plugins-good" },
        { "vp8dec",      "VP8 video          gst-plugins-good, needs libvpx" },
        { "opusdec",     "Opus audio         gst-plugins-base, needs libopus" },
        { "avdec_h264",  "H.264 video        gst-libav" },
        { "avdec_aac",   "AAC audio          gst-libav" },
    };
    GString *missing = g_string_new (NULL);

    for (gsize i = 0; i < G_N_ELEMENTS (need); i++) {
        GstElementFactory *f = gst_element_factory_find (need[i].element);
        if (f) {
            gst_object_unref (f);
            continue;
        }
        g_string_append_printf (missing, "  %-12s %s\n", need[i].element, need[i].what);
    }

    /* Publishing to most platforms needs H.264 out, not just in. Either
     * encoder serves, so neither belongs in the list above. */
    GstElementFactory *x264 = gst_element_factory_find ("x264enc");
    GstElementFactory *oh   = gst_element_factory_find ("openh264enc");

    if (!x264 && !oh)
        g_string_append (missing,
                         "  x264enc      H.264 encoding     gst-plugins-ugly,"
                         " or openh264enc from gst-plugins-bad\n");
    if (x264) gst_object_unref (x264);
    if (oh)   gst_object_unref (oh);

    if (missing->len) {
        mlog ("gstreamer: these are missing, and pages will fail without them:");
        g_printerr ("%s", missing->str);
    }

    /*
     * Not fatal, but WebKit complains about each one at the moment it
     * needs it, which reads like a fault. Listed once, up front, marked
     * for what they are.
     */
    static const struct { const char *element; const char *what; } nice_to_have[] = {
        { "audiornnoise", "noise suppression   gst-plugins-rs" },
        { "rtpgccbwe",    "RTP bandwidth est.  gst-plugins-rs" },
    };
    GString *optional = g_string_new (NULL);

    for (gsize i = 0; i < G_N_ELEMENTS (nice_to_have); i++) {
        GstElementFactory *f = gst_element_factory_find (nice_to_have[i].element);
        if (f) {
            gst_object_unref (f);
            continue;
        }
        g_string_append_printf (optional, "  %-12s %s\n",
                                nice_to_have[i].element, nice_to_have[i].what);
    }

    if (optional->len) {
        mlog ("gstreamer: these are optional - pages work without them:");
        g_printerr ("%s", optional->str);
    }

    g_string_free (optional, TRUE);
    g_string_free (missing, TRUE);
}

/* 640x480 -> "4:3". Only meaningful when both are fixed numbers. */
static char *
aspect_of (int w, int h)
{
    if (w <= 0 || h <= 0)
        return g_strdup ("");

    int a = w, b = h, t;
    while (b) { t = b; b = a % b; a = t; }      /* gcd */

    return g_strdup_printf ("%d:%d", w / a, h / a);
}

static void
cam_list_caps (GstCaps *caps)
{
    guint n = caps ? gst_caps_get_size (caps) : 0;

    for (guint i = 0; i < n; i++) {
        GstStructure *s = gst_caps_get_structure (caps, i);
        const GValue *vw = gst_structure_get_value (s, "width");
        const GValue *vh = gst_structure_get_value (s, "height");
        const GValue *vf = gst_structure_get_value (s, "framerate");
        const char   *fmt = gst_structure_get_string (s, "format");

        char *w  = vw ? gst_value_serialize (vw) : g_strdup ("?");
        char *h  = vh ? gst_value_serialize (vh) : g_strdup ("?");
        char *fr = vf ? gst_value_serialize (vf) : g_strdup ("?");

        /* the serializer annotates list members, which reads as noise */
        if (strstr (fr, "(fraction)")) {
            char **bits = g_strsplit (fr, "(fraction)", -1);
            char  *clean = g_strjoinv ("", bits);
            g_strfreev (bits);
            g_free (fr);
            fr = clean;
        }

        int iw = 0, ih = 0;
        gst_structure_get_int (s, "width", &iw);
        gst_structure_get_int (s, "height", &ih);
        char *ar = aspect_of (iw, ih);

        g_print ("    %-12s %-6s %11s x %-11s %-22s %s\n",
                 gst_structure_get_name (s), fmt ? fmt : "-", w, h, fr, ar);

        g_free (ar); g_free (fr); g_free (h); g_free (w);
    }
}

/* --list-cameras: print and stop, so it can be read without a window. */
static void
cam_list (void)
{
    GstDeviceMonitor *mon = gst_device_monitor_new ();
    GstCaps          *any = gst_caps_new_any ();

    gst_device_monitor_add_filter (mon, "Video/Source", any);
    gst_caps_unref (any);

    if (!gst_device_monitor_start (mon)) {
        g_printerr ("cameras: the device monitor would not start\n");
        gst_object_unref (mon);
        return;
    }

    GList *devs = gst_device_monitor_get_devices (mon);

    if (!devs)
        g_print ("no cameras found\n");

    for (GList *l = devs; l; l = l->next) {
        GstDevice    *dev  = l->data;
        char         *name = gst_device_get_display_name (dev);
        GstStructure *props = gst_device_get_properties (dev);
        const char   *path  = props ? gst_structure_get_string (props, "device.path") : NULL;
        GstCaps      *caps  = gst_device_get_caps (dev);

        g_print ("%s%s%s\n", name ? name : "?",
                 path ? "   " : "", path ? path : "");
        g_print ("    %-12s %-6s %11s   %-11s %-22s %s\n",
                 "media type", "format", "width", "height", "framerate", "aspect");
        cam_list_caps (caps);
        g_print ("\n");

        if (caps)  gst_caps_unref (caps);
        if (props) gst_structure_free (props);
        g_free (name);
    }

    if (devs)
        g_print ("to ask for one of these whatever the page requests:\n"
                 "    browser-big URL --cam-force 640x480@30\n");

    g_list_free_full (devs, gst_object_unref);
    gst_device_monitor_stop (mon);
    gst_object_unref (mon);
}

static void
big_startup (void)
{
    gst_init (NULL, NULL);
    codec_check ();

    print_versions ();
    if (g_media_debug)
        dump_gstreamer_env ();

    if (g_cam_fix || g_media_watchdog)
        g_shim_js = build_shim ();

    /* runs once the main loop starts, i.e. after the window is up */
    if (g_prewarm)
        g_idle_add (capture_prewarm, NULL);

    /* once the web process exists, say plainly whether it is sandboxed */
    g_timeout_add_seconds (3, sandbox_report, NULL);
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
    .cfg_set        = big_cfg_set,
    .media_asked    = big_media_asked,
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
