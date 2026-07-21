/* Cockos SWELL (Simple/Small Win32 Emulation Layer for Linux/OSX)
   Copyright (C) 2006 and later, Cockos, Inc.

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
       claim that you wrote the original software. If you use this software
       in a product, an acknowledgment in the product documentation would be
       appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.


    AT-SPI accessibility for the generic (GDK/Linux) SWELL implementation,
    via ATK + at-spi2-atk (atk-bridge). Every HWND gets a lazily-created
    AtkObject wrapper (stored in HWND__::m_atspi, holding a Retain()ed HWND
    reference); an AtkUtil root exposes SWELL_topwindows; state transitions
    are observed centrally from SendMessage via the SWELL_ATSPI_* hooks in
    swell-atspi-internal.h. The atk-bridge D-Bus machinery attaches to the
    default GMainContext, which SWELL_RunEvents already iterates, so
    everything runs on the UI thread.

  */

#ifndef SWELL_PROVIDED_BY_APP

#ifdef SWELL_TARGET_ATSPI
#ifdef SWELL_TARGET_GDK

#ifndef WDL_NO_DEFINE_MINMAX
#define WDL_NO_DEFINE_MINMAX
#endif
#include "swell.h"
#include "swell-internal.h"
#include "swell-atspi-internal.h"
#include "../wdlcstring.h"

#include <dlfcn.h>
#include <atk/atk.h>
#include <atk-bridge.h>

bool swell_atspi_active;

static bool swell_atspi_debug;
#define ATSPI_DEBUG(...) do { if (swell_atspi_debug) fprintf(stderr,"swell-atspi: " __VA_ARGS__); } while(0)

bool IsModalDialogBox(HWND hwnd); // swell-dlg-generic.cpp
HWND GetFocusIncludeMenus();      // swell-wnd-generic.cpp

AtkObject *swell_atspi_wrapper(HWND h, bool create);

static bool wantWrapper(HWND h)
{
  return h && !h->m_hashaddestroy &&
    (!h->m_classname || strcmp(h->m_classname,"__SWELL_MENU")); // menus get wrapped in a later pass
}

static HWND toplevelOf(HWND h)
{
  while (h && h->m_parent) h = h->m_parent;
  return h;
}

/////////////// base wrapper object (any HWND)

#define SWELL_TYPE_ATK_BASE (swell_atk_base_get_type())
#define SWELL_ATK_BASE(o) (G_TYPE_CHECK_INSTANCE_CAST((o),SWELL_TYPE_ATK_BASE,SwellAtkBase))
#define SWELL_IS_ATK_BASE(o) (G_TYPE_CHECK_INSTANCE_TYPE((o),SWELL_TYPE_ATK_BASE))

typedef struct {
  AtkObject parent;
  HWND hwnd;         // Retain()ed for the wrapper's lifetime, so the pointer stays valid after destroy
  gchar *name_cache; // owned storage backing get_name()
} SwellAtkBase;
typedef struct { AtkObjectClass parent; } SwellAtkBaseClass;

G_DEFINE_TYPE(SwellAtkBase, swell_atk_base, ATK_TYPE_OBJECT)

static HWND swell_atk_hwnd(AtkObject *o)
{
  if (!o || !SWELL_IS_ATK_BASE(o)) return NULL;
  HWND h = SWELL_ATK_BASE(o)->hwnd;
  return h && !h->m_hashaddestroy ? h : NULL;
}

static const gchar *swell_atk_base_get_name(AtkObject *o)
{
  SwellAtkBase *b = SWELL_ATK_BASE(o);
  HWND h = swell_atk_hwnd(o);
  if (!h) return b->name_cache;

  g_free(b->name_cache);
  b->name_cache = g_strdup(h->m_title.Get());
  return b->name_cache;
}

static AtkRole swell_atk_base_get_role(AtkObject *o)
{
  HWND h = swell_atk_hwnd(o);
  if (!h) return ATK_ROLE_INVALID;
  return ATK_ROLE_PANEL; // per-class roles arrive with the control wrappers
}

static AtkStateSet *swell_atk_base_ref_state_set(AtkObject *o)
{
  AtkStateSet *ss = ATK_OBJECT_CLASS(swell_atk_base_parent_class)->ref_state_set(o);
  HWND h = SWELL_IS_ATK_BASE(o) ? SWELL_ATK_BASE(o)->hwnd : NULL;
  if (!h || h->m_hashaddestroy)
  {
    atk_state_set_add_state(ss,ATK_STATE_DEFUNCT);
    return ss;
  }

  if (h->m_visible) atk_state_set_add_state(ss,ATK_STATE_VISIBLE);
  if (IsWindowVisible(h)) atk_state_set_add_state(ss,ATK_STATE_SHOWING);
  if (IsWindowEnabled(h))
  {
    atk_state_set_add_state(ss,ATK_STATE_ENABLED);
    atk_state_set_add_state(ss,ATK_STATE_SENSITIVE);
  }
  if (h->m_wantfocus) atk_state_set_add_state(ss,ATK_STATE_FOCUSABLE);
  if (h == GetFocusIncludeMenus()) atk_state_set_add_state(ss,ATK_STATE_FOCUSED);

  if (!h->m_parent)
  {
    if (swell_is_app_inactive()<=0 &&
        h == swell_oswindow_to_hwnd(SWELL_focused_oswindow))
      atk_state_set_add_state(ss,ATK_STATE_ACTIVE);
    if (IsModalDialogBox(h)) atk_state_set_add_state(ss,ATK_STATE_MODAL);
    if (h->m_style & WS_THICKFRAME) atk_state_set_add_state(ss,ATK_STATE_RESIZABLE);
  }
  return ss;
}

static AtkObject *swell_atk_root(void);

static AtkObject *swell_atk_base_get_parent(AtkObject *o)
{
  HWND h = SWELL_IS_ATK_BASE(o) ? SWELL_ATK_BASE(o)->hwnd : NULL;
  if (!h) return NULL;
  if (!h->m_parent) return swell_atk_root();
  return swell_atspi_wrapper(h->m_parent,true);
}

static gint swell_atk_base_get_n_children(AtkObject *o)
{
  return 0; // child descent lands with the control wrappers
}

static AtkObject *swell_atk_base_ref_child(AtkObject *o, gint i)
{
  return NULL;
}

static gint swell_atk_base_get_index_in_parent(AtkObject *o)
{
  HWND h = swell_atk_hwnd(o);
  if (!h) return -1;
  int idx = 0;
  if (!h->m_parent)
  {
    HWND w = SWELL_topwindows;
    while (w && w != h)
    {
      if (wantWrapper(w)) idx++;
      w = w->m_next;
    }
    return w ? idx : -1;
  }
  HWND w = h->m_parent->m_children;
  while (w && w != h)
  {
    if (wantWrapper(w)) idx++;
    w = w->m_next;
  }
  return w ? idx : -1;
}

static void swell_atk_base_finalize(GObject *o)
{
  SwellAtkBase *b = SWELL_ATK_BASE(o);
  if (b->hwnd) { b->hwnd->Release(); b->hwnd = NULL; }
  g_free(b->name_cache);
  b->name_cache = NULL;
  G_OBJECT_CLASS(swell_atk_base_parent_class)->finalize(o);
}

static void swell_atk_base_class_init(SwellAtkBaseClass *klass)
{
  AtkObjectClass *oc = ATK_OBJECT_CLASS(klass);
  oc->get_name = swell_atk_base_get_name;
  oc->get_role = swell_atk_base_get_role;
  oc->ref_state_set = swell_atk_base_ref_state_set;
  oc->get_parent = swell_atk_base_get_parent;
  oc->get_n_children = swell_atk_base_get_n_children;
  oc->ref_child = swell_atk_base_ref_child;
  oc->get_index_in_parent = swell_atk_base_get_index_in_parent;
  G_OBJECT_CLASS(klass)->finalize = swell_atk_base_finalize;
}

static void swell_atk_base_init(SwellAtkBase *b)
{
  b->hwnd = NULL;
  b->name_cache = NULL;
}

/////////////// top level windows (frames/dialogs)

#define SWELL_TYPE_ATK_TOPLEVEL (swell_atk_toplevel_get_type())

typedef struct { SwellAtkBase parent; } SwellAtkTopLevel;
typedef struct { SwellAtkBaseClass parent; } SwellAtkTopLevelClass;

static void swell_atk_window_iface_init(AtkWindowIface *iface) { }

G_DEFINE_TYPE_WITH_CODE(SwellAtkTopLevel, swell_atk_toplevel, SWELL_TYPE_ATK_BASE,
    G_IMPLEMENT_INTERFACE(ATK_TYPE_WINDOW, swell_atk_window_iface_init))

static AtkRole swell_atk_toplevel_get_role(AtkObject *o)
{
  HWND h = swell_atk_hwnd(o);
  if (!h) return ATK_ROLE_INVALID;
  if (IsModalDialogBox(h) || (h->m_dlgproc && h->m_owner)) return ATK_ROLE_DIALOG;
  return ATK_ROLE_FRAME;
}

static void swell_atk_toplevel_class_init(SwellAtkTopLevelClass *klass)
{
  ATK_OBJECT_CLASS(klass)->get_role = swell_atk_toplevel_get_role;
}

static void swell_atk_toplevel_init(SwellAtkTopLevel *tl) { }

/////////////// application root

#define SWELL_TYPE_ATK_ROOT (swell_atk_root_obj_get_type())

typedef struct {
  AtkObject parent;
  gchar *name_cache;
} SwellAtkRoot;
typedef struct { AtkObjectClass parent; } SwellAtkRootClass;

G_DEFINE_TYPE(SwellAtkRoot, swell_atk_root_obj, ATK_TYPE_OBJECT)

static const gchar *swell_atk_root_get_name(AtkObject *o)
{
  SwellAtkRoot *r = (SwellAtkRoot *)o;
  if (g_swell_appname && *g_swell_appname)
  {
    g_free(r->name_cache);
    r->name_cache = g_strdup(g_swell_appname);
  }
  else if (!r->name_cache)
  {
    char buf[1024];
    buf[0]=0;
    GetModuleFileName(NULL,buf,sizeof(buf));
    const char *p = WDL_get_filepart(buf);
    r->name_cache = g_strdup(p && *p ? p : "swell");
  }
  return r->name_cache;
}

static gint swell_atk_root_get_n_children(AtkObject *o)
{
  gint n = 0;
  HWND w = SWELL_topwindows;
  while (w)
  {
    if (wantWrapper(w)) n++;
    w = w->m_next;
  }
  return n;
}

static AtkObject *swell_atk_root_ref_child(AtkObject *o, gint i)
{
  HWND w = SWELL_topwindows;
  while (w)
  {
    if (wantWrapper(w) && i-- == 0)
    {
      AtkObject *c = swell_atspi_wrapper(w,true);
      if (c) g_object_ref(c);
      return c;
    }
    w = w->m_next;
  }
  return NULL;
}

static void swell_atk_root_obj_class_init(SwellAtkRootClass *klass)
{
  AtkObjectClass *oc = ATK_OBJECT_CLASS(klass);
  oc->get_name = swell_atk_root_get_name;
  oc->get_n_children = swell_atk_root_get_n_children;
  oc->ref_child = swell_atk_root_ref_child;
}

static void swell_atk_root_obj_init(SwellAtkRoot *r)
{
  r->name_cache = NULL;
  atk_object_set_role(ATK_OBJECT(r),ATK_ROLE_APPLICATION);
}

static AtkObject *swell_atk_root(void)
{
  static AtkObject *s_root;
  if (!s_root) s_root = (AtkObject *)g_object_new(SWELL_TYPE_ATK_ROOT,NULL);
  return s_root;
}

/////////////// wrapper management

AtkObject *swell_atspi_wrapper(HWND h, bool create)
{
  if (!h) return NULL;
  if (h->m_atspi) return (AtkObject *)h->m_atspi;
  if (!create || !wantWrapper(h)) return NULL;
  if (h->m_parent) return NULL; // controls arrive in a later pass

  SwellAtkBase *b = (SwellAtkBase *)g_object_new(SWELL_TYPE_ATK_TOPLEVEL,NULL);
  h->Retain();
  b->hwnd = h;
  h->m_atspi = b;
  return (AtkObject *)b;
}

// current window-activation state, tracked so activate/deactivate pairs stay balanced
static AtkObject *s_active_frame;

static void set_active_frame(AtkObject *frame)
{
  if (frame == s_active_frame) return;
  ATSPI_DEBUG("set_active_frame %p -> %p\n",(void*)s_active_frame,(void*)frame);
  if (s_active_frame)
  {
    AtkObject *old = s_active_frame;
    s_active_frame = NULL;
    if (swell_atk_hwnd(old)) // skip the signal on defunct objects
    {
      g_signal_emit_by_name(old,"deactivate");
      atk_object_notify_state_change(old,ATK_STATE_ACTIVE,FALSE);
    }
    g_object_unref(old);
  }
  if (frame)
  {
    s_active_frame = (AtkObject *)g_object_ref(frame);
    g_signal_emit_by_name(frame,"activate");
    atk_object_notify_state_change(frame,ATK_STATE_ACTIVE,TRUE);
  }
}

static void wrapper_notify_destroyed(HWND h)
{
  AtkObject *o = (AtkObject *)h->m_atspi;
  if (!o) return;

  if (o == s_active_frame)
  {
    s_active_frame = NULL;
    g_object_unref(o);
  }

  if (!h->m_parent)
  {
    g_signal_emit_by_name(o,"destroy");
    gint idx = swell_atk_base_get_index_in_parent(o);
    g_signal_emit_by_name(swell_atk_root(),"children-changed::remove",
                          idx >= 0 ? idx : 0, o);
  }
  atk_object_notify_state_change(o,ATK_STATE_DEFUNCT,TRUE);

  h->m_atspi = NULL;
  g_object_unref(o); // drop the HWND's owned ref; ATs may keep the wrapper alive
}

/////////////// hooks called from swell-wnd-generic.cpp / swell-generic-gdk.cpp

void swell_atspi_msg_pre(HWND h, UINT m, WPARAM w, LPARAM l)
{
}

void swell_atspi_msg_post(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT r)
{
  switch (m)
  {
    case WM_SETFOCUS:
      {
        ATSPI_DEBUG("WM_SETFOCUS hwnd=%p class=%s\n",(void*)h,h->m_classname);
        HWND tl = toplevelOf(h);
        if (!wantWrapper(tl)) break;
        AtkObject *frame = swell_atspi_wrapper(tl,true);
        if (frame) set_active_frame(frame);
        if (h == tl && frame)
          atk_object_notify_state_change(frame,ATK_STATE_FOCUSED,TRUE);
      }
    break;
    case WM_DESTROY:
      if (h->m_hashaddestroy == 2) wrapper_notify_destroyed(h);
    break;
  }
}

void swell_atspi_show_window(HWND h, bool wasVisible)
{
  ATSPI_DEBUG("show_window hwnd=%p wasvis=%d vis=%d toplevel=%d\n",
              (void*)h,wasVisible,h->m_visible,!h->m_parent);
  if (wasVisible == !!h->m_visible) return;
  if (h->m_parent)
  {
    AtkObject *o = swell_atspi_wrapper(h,false);
    if (o)
    {
      atk_object_notify_state_change(o,ATK_STATE_VISIBLE,h->m_visible);
      atk_object_notify_state_change(o,ATK_STATE_SHOWING,IsWindowVisible(h));
    }
    return;
  }

  if (h->m_visible)
  {
    AtkObject *o = swell_atspi_wrapper(h,true);
    if (!o) return;
    g_signal_emit_by_name(o,"create");
    gint idx = swell_atk_base_get_index_in_parent(o);
    g_signal_emit_by_name(swell_atk_root(),"children-changed::add",
                          idx >= 0 ? idx : 0, o);
    atk_object_notify_state_change(o,ATK_STATE_VISIBLE,TRUE);
    atk_object_notify_state_change(o,ATK_STATE_SHOWING,TRUE);
  }
  else
  {
    AtkObject *o = swell_atspi_wrapper(h,false);
    if (!o) return;
    if (o == s_active_frame) set_active_frame(NULL);
    atk_object_notify_state_change(o,ATK_STATE_VISIBLE,FALSE);
    atk_object_notify_state_change(o,ATK_STATE_SHOWING,FALSE);
  }
}

void swell_atspi_enable_window(HWND h)
{
  AtkObject *o = swell_atspi_wrapper(h,false);
  if (!o) return;
  const bool en = IsWindowEnabled(h);
  atk_object_notify_state_change(o,ATK_STATE_ENABLED,en);
  atk_object_notify_state_change(o,ATK_STATE_SENSITIVE,en);
}

void swell_atspi_app_active(int active)
{
  if (!active)
  {
    set_active_frame(NULL);
    return;
  }
  HWND h = swell_oswindow_to_hwnd(SWELL_focused_oswindow);
  if (wantWrapper(h)) set_active_frame(swell_atspi_wrapper(h,true));
}

bool swell_atspi_on_key(void *gdkEventKey)
{
  return false; // AT key listener support comes with a later pass
}

/////////////// AtkUtil integration + bridge init

static AtkObject *swell_atk_util_get_root(void) { return swell_atk_root(); }
static const gchar *swell_atk_util_get_toolkit_name(void) { return "swell"; }
static const gchar *swell_atk_util_get_toolkit_version(void) { return "1.0"; }

void swell_atspi_init(void)
{
#ifdef SWELL_SUPPORT_GTK
  // gtk_init installs GTK's own AtkUtil implementation and initializes
  // atk-bridge itself, assuming all accessible content is GtkWidgets --
  // SWELL windows are not exposed in GTK-enabled builds for now.
  return;
#else
  swell_atspi_debug = getenv("SWELL_ATSPI_DEBUG") != NULL;
  ATSPI_DEBUG("init\n");
  if (getenv("SWELL_NO_ATSPI") || getenv("NO_AT_BRIDGE")) return;

#ifdef SWELL_PRELOAD
  // mirror the GDK preload: no hard link dependency, resolve lazily after dlopen
  if (!dlopen("libatk-1.0.so.0",RTLD_LAZY|RTLD_GLOBAL) ||
      !dlopen("libatk-bridge-2.0.so.0",RTLD_LAZY|RTLD_GLOBAL)) return;
#endif

  AtkUtilClass *uc = ATK_UTIL_CLASS(g_type_class_ref(ATK_TYPE_UTIL));
  if (!uc) return;
  uc->get_root = swell_atk_util_get_root;
  uc->get_toolkit_name = swell_atk_util_get_toolkit_name;
  uc->get_toolkit_version = swell_atk_util_get_toolkit_version;

  const int br = atk_bridge_adaptor_init(NULL,NULL);
  ATSPI_DEBUG("atk_bridge_adaptor_init returned %d, root=%s\n",
              br, G_OBJECT_TYPE_NAME(atk_get_root()));
  if (br == 0)
    swell_atspi_active = true;
#endif
}

#endif // SWELL_TARGET_GDK
#endif // SWELL_TARGET_ATSPI

#endif // !SWELL_PROVIDED_BY_APP
