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

enum swellWidgetType {
  WT_GENERIC = 0,
  WT_TOPLEVEL,
  WT_STATIC,
  WT_GROUPBOX,
  WT_PUSHBUTTON,
  WT_CHECKBOX,
  WT_RADIO,
  WT_EDIT,
  WT_COMBO,
  WT_TRACKBAR,
  WT_PROGRESS,
  WT_LISTBOX,
  WT_LISTVIEW,
  WT_TREEVIEW,
  WT_TAB
};

static int classifyHwnd(HWND h)
{
  if (!h) return WT_GENERIC;
  if (!h->m_parent) return WT_TOPLEVEL;
  const char *cn = h->m_classname ? h->m_classname : "";
  if (!strcmp(cn,"Button"))
  {
    if (h->m_style & BS_GROUPBOX) return WT_GROUPBOX; // SWELL defines this as a high bit, not a low-nibble value
    switch (h->m_style & 0xf)
    {
      case BS_AUTOCHECKBOX:
      case BS_AUTO3STATE: return WT_CHECKBOX;
      case BS_AUTORADIOBUTTON: return WT_RADIO;
      default: return WT_PUSHBUTTON;
    }
  }
  if (!strcmp(cn,"Static")) return WT_STATIC;
  if (!strcmp(cn,"Edit")) return WT_EDIT;
  if (!strcmp(cn,"combobox")) return WT_COMBO;
  if (!strcmp(cn,"msctls_trackbar32") || !strcmp(cn,"REAPERhfader")) return WT_TRACKBAR;
  if (!strcmp(cn,"msctls_progress32")) return WT_PROGRESS;
  if (!strcmp(cn,"ListBox")) return WT_LISTBOX;
  if (!strcmp(cn,"SysListView32")) return WT_LISTVIEW;
  if (!strcmp(cn,"SysTreeView32")) return WT_TREEVIEW;
  if (!strcmp(cn,"SysTabControl32")) return WT_TAB;
  return WT_GENERIC;
}

// controls that take their accessible name from a preceding Static label
static bool wantsLabelName(int wt)
{
  switch (wt)
  {
    case WT_EDIT:
    case WT_COMBO:
    case WT_TRACKBAR:
    case WT_PROGRESS:
    case WT_LISTBOX:
    case WT_LISTVIEW:
    case WT_TREEVIEW:
    case WT_TAB:
      return true;
  }
  return false;
}

static HWND findLabelFor(HWND h)
{
  if (!h || !wantsLabelName(classifyHwnd(h))) return NULL;
  HWND w = h->m_prev;
  while (w)
  {
    const int wt = classifyHwnd(w);
    if (wt == WT_STATIC && w->m_title.GetLength()) return w;
    w = w->m_prev;
  }
  return NULL;
}

static HWND findLabelTarget(HWND label) // inverse of findLabelFor
{
  if (classifyHwnd(label) != WT_STATIC || !label->m_title.GetLength()) return NULL;
  HWND w = label->m_next;
  while (w)
  {
    if (wantsLabelName(classifyHwnd(w))) return findLabelFor(w) == label ? w : NULL;
    if (classifyHwnd(w) == WT_STATIC && w->m_title.GetLength()) return NULL;
    w = w->m_next;
  }
  return NULL;
}

// strips '&' accelerator markers ("&&" emits a literal '&')
static gchar *swell_atspi_strip_accel(const char *p)
{
  gchar *s = g_strdup(p ? p : ""), *r = s, *w = s;
  while (*r)
  {
    if (*r == '&' && r[1] && r[1] != '&') { r++; continue; }
    if (*r == '&' && r[1] == '&') r++;
    *w++ = *r++;
  }
  *w = 0;
  return s;
}

/////////////// base wrapper object (any HWND)

#define SWELL_TYPE_ATK_BASE (swell_atk_base_get_type())
#define SWELL_ATK_BASE(o) (G_TYPE_CHECK_INSTANCE_CAST((o),SWELL_TYPE_ATK_BASE,SwellAtkBase))
#define SWELL_IS_ATK_BASE(o) (G_TYPE_CHECK_INSTANCE_TYPE((o),SWELL_TYPE_ATK_BASE))

typedef struct {
  AtkObject parent;
  HWND hwnd;         // Retain()ed for the wrapper's lifetime, so the pointer stays valid after destroy
  gchar *name_cache; // owned storage backing get_name()
  GHashTable *item_cache; // virtual items keyed by index+1 or HTREEITEM (containers only)
} SwellAtkBase;
typedef struct { AtkObjectClass parent; } SwellAtkBaseClass;

static void swell_atk_component_iface_init(AtkComponentIface *iface);

G_DEFINE_TYPE_WITH_CODE(SwellAtkBase, swell_atk_base, ATK_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(ATK_TYPE_COMPONENT, swell_atk_component_iface_init))

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

  const char *src = h->m_title.Get();
  if (wantsLabelName(classifyHwnd(h)))
  {
    HWND label = findLabelFor(h);
    src = label ? label->m_title.Get() : "";
  }
  g_free(b->name_cache);
  b->name_cache = swell_atspi_strip_accel(src);
  return b->name_cache;
}

static AtkRole swell_atk_base_get_role(AtkObject *o)
{
  HWND h = swell_atk_hwnd(o);
  if (!h) return ATK_ROLE_INVALID;
  switch (classifyHwnd(h))
  {
    case WT_STATIC: return ATK_ROLE_LABEL;
    case WT_GROUPBOX: return ATK_ROLE_PANEL;
  }
  return ATK_ROLE_PANEL;
}

static AtkRelationSet *swell_atk_base_ref_relation_set(AtkObject *o)
{
  AtkRelationSet *rs = ATK_OBJECT_CLASS(swell_atk_base_parent_class)->ref_relation_set(o);
  HWND h = swell_atk_hwnd(o);
  if (!h) return rs;

  HWND label = findLabelFor(h);
  if (label)
  {
    AtkObject *lo = swell_atspi_wrapper(label,true);
    if (lo) atk_relation_set_add_relation_by_type(rs,ATK_RELATION_LABELLED_BY,lo);
  }
  else
  {
    HWND target = findLabelTarget(h);
    if (target)
    {
      AtkObject *to = swell_atspi_wrapper(target,true);
      if (to) atk_relation_set_add_relation_by_type(rs,ATK_RELATION_LABEL_FOR,to);
    }
  }
  return rs;
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

  switch (classifyHwnd(h))
  {
    case WT_CHECKBOX:
    case WT_RADIO:
      {
        const LRESULT chk = SendMessage(h,BM_GETCHECK,0,0);
        if (chk == 1) atk_state_set_add_state(ss,ATK_STATE_CHECKED);
        else if (chk == 2) atk_state_set_add_state(ss,ATK_STATE_INDETERMINATE);
      }
    break;
    case WT_EDIT:
      if (!(h->m_style & ES_READONLY)) atk_state_set_add_state(ss,ATK_STATE_EDITABLE);
      atk_state_set_add_state(ss,(h->m_style & ES_MULTILINE) ?
                              ATK_STATE_MULTI_LINE : ATK_STATE_SINGLE_LINE);
    break;
  }

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
  HWND h = swell_atk_hwnd(o);
  if (!h) return 0;
  gint n = 0;
  HWND w = h->m_children;
  while (w)
  {
    if (wantWrapper(w)) n++;
    w = w->m_next;
  }
  return n;
}

static AtkObject *swell_atk_base_ref_child(AtkObject *o, gint i)
{
  HWND h = swell_atk_hwnd(o);
  if (!h) return NULL;
  HWND w = h->m_children;
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
  if (b->item_cache)
  {
    g_hash_table_destroy(b->item_cache);
    b->item_cache = NULL;
  }
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
  oc->ref_relation_set = swell_atk_base_ref_relation_set;
  G_OBJECT_CLASS(klass)->finalize = swell_atk_base_finalize;
}

static void swell_atk_base_init(SwellAtkBase *b)
{
  b->hwnd = NULL;
  b->name_cache = NULL;
  b->item_cache = NULL;
}

/////////////// AtkComponent (geometry, hit testing, focus grab)

static void swell_atk_component_get_extents(AtkComponent *c, gint *x, gint *y,
                                            gint *w, gint *hh, AtkCoordType ct)
{
  if (x) *x = 0;
  if (y) *y = 0;
  if (w) *w = 0;
  if (hh) *hh = 0;
  HWND h = swell_atk_hwnd((AtkObject *)c);
  if (!h) return;

  // computed purely from SWELL's coordinate model (not the window manager's
  // frame origin) so parent/child extents are always mutually consistent
  POINT pt = { 0, 0 };
  ClientToScreen(h,&pt);
  RECT cr;
  GetClientRect(h,&cr);
  RECT r = { pt.x, pt.y, pt.x + cr.right, pt.y + cr.bottom };

  if (ct == ATK_XY_WINDOW)
  {
    HWND tl = toplevelOf(h);
    if (tl)
    {
      POINT tp = { 0, 0 };
      ClientToScreen(tl,&tp);
      r.left -= tp.x; r.right -= tp.x;
      r.top -= tp.y; r.bottom -= tp.y;
    }
  }
  if (x) *x = r.left;
  if (y) *y = r.top;
  if (w) *w = r.right - r.left;
  if (hh) *hh = r.bottom - r.top;
}

static AtkObject *swell_atk_component_ref_accessible_at_point(AtkComponent *c,
                                                              gint x, gint y, AtkCoordType ct)
{
  HWND h = swell_atk_hwnd((AtkObject *)c);
  if (!h) return NULL;

  gint ox, oy, ow, oh;
  swell_atk_component_get_extents(c,&ox,&oy,&ow,&oh,ct);
  if (x < ox || y < oy || x >= ox+ow || y >= oy+oh) return NULL;

  HWND w = h->m_children;
  while (w)
  {
    if (wantWrapper(w) && w->m_visible)
    {
      AtkObject *co = swell_atspi_wrapper(w,true);
      if (co)
      {
        gint cx, cy, cw, ch;
        swell_atk_component_get_extents((AtkComponent *)co,&cx,&cy,&cw,&ch,ct);
        if (x >= cx && y >= cy && x < cx+cw && y < cy+ch)
        {
          AtkObject *sub = swell_atk_component_ref_accessible_at_point((AtkComponent *)co,x,y,ct);
          if (sub) return sub;
          g_object_ref(co);
          return co;
        }
      }
    }
    w = w->m_next;
  }
  g_object_ref((AtkObject *)c);
  return (AtkObject *)c;
}

static gboolean swell_atk_component_grab_focus(AtkComponent *c)
{
  HWND h = swell_atk_hwnd((AtkObject *)c);
  if (!h || !h->m_wantfocus || !IsWindowEnabled(h)) return FALSE;
  SetFocus(h);
  return TRUE;
}

static void swell_atk_component_iface_init(AtkComponentIface *iface)
{
  iface->get_extents = swell_atk_component_get_extents;
  iface->ref_accessible_at_point = swell_atk_component_ref_accessible_at_point;
  iface->grab_focus = swell_atk_component_grab_focus;
}

/////////////// virtual items (list rows, tree items, tabs, combo entries)

#define SWELL_TYPE_ATK_ITEM (swell_atk_item_get_type())
#define SWELL_ATK_ITEM(o) (G_TYPE_CHECK_INSTANCE_CAST((o),SWELL_TYPE_ATK_ITEM,SwellAtkItem))
#define SWELL_IS_ATK_ITEM(o) (G_TYPE_CHECK_INSTANCE_TYPE((o),SWELL_TYPE_ATK_ITEM))

typedef struct {
  AtkObject parent;
  SwellAtkBase *container; // strong ref; liveness of the item follows container->hwnd
  int index;               // index-keyed containers (list/tab/combo); -1 for tree items
  HTREEITEM hti;           // tree items only, validated against the live tree before use
  gchar *name_cache;
} SwellAtkItem;
typedef struct { AtkObjectClass parent; } SwellAtkItemClass;

static bool tree_desc_contains(HTREEITEM par, HTREEITEM needle)
{
  for (int i = 0; i < par->m_children.GetSize(); i ++)
  {
    HTREEITEM c = par->m_children.Get(i);
    if (c == needle || tree_desc_contains(c,needle)) return true;
  }
  return false;
}

static bool tree_item_valid(HWND h, HTREEITEM it)
{
  if (!it) return false;
  HTREEITEM r = TreeView_GetRoot(h);
  while (r)
  {
    if (r == it || tree_desc_contains(r,it)) return true;
    r = TreeView_GetNextSibling(h,r);
  }
  return false;
}

static int cont_item_count(HWND h)
{
  switch (classifyHwnd(h))
  {
    case WT_LISTBOX: return (int)SendMessage(h,LB_GETCOUNT,0,0);
    case WT_LISTVIEW: return ListView_GetItemCount(h);
    case WT_TAB: return TabCtrl_GetItemCount(h);
    case WT_COMBO: return (int)SendMessage(h,CB_GETCOUNT,0,0);
    case WT_TREEVIEW:
      {
        int n = 0;
        HTREEITEM r = TreeView_GetRoot(h);
        while (r) { n++; r = TreeView_GetNextSibling(h,r); }
        return n;
      }
  }
  return 0;
}

// container hwnd if the item is still usable, otherwise NULL
static HWND item_hwnd(SwellAtkItem *it)
{
  if (!it->container) return NULL;
  HWND h = swell_atk_hwnd((AtkObject *)it->container);
  if (!h) return NULL;
  if (it->hti) return tree_item_valid(h,it->hti) ? h : NULL;
  return it->index >= 0 && it->index < cont_item_count(h) ? h : NULL;
}

static bool item_is_selected(SwellAtkItem *it)
{
  HWND h = item_hwnd(it);
  if (!h) return false;
  switch (classifyHwnd(h))
  {
    case WT_LISTBOX: return SendMessage(h,LB_GETSEL,it->index,0) > 0 ||
                            (int)SendMessage(h,LB_GETCURSEL,0,0) == it->index;
    case WT_LISTVIEW: return (ListView_GetItemState(h,it->index,LVIS_SELECTED) & LVIS_SELECTED) != 0;
    case WT_TREEVIEW: return TreeView_GetSelection(h) == it->hti;
    case WT_TAB: return TabCtrl_GetCurSel(h) == it->index;
    case WT_COMBO: return (int)SendMessage(h,CB_GETCURSEL,0,0) == it->index;
  }
  return false;
}

static void item_select(SwellAtkItem *it)
{
  HWND h = item_hwnd(it);
  if (!h) return;
  switch (classifyHwnd(h))
  {
    case WT_LISTBOX:
      SendMessage(h,LB_SETCURSEL,it->index,0);
      if (h->m_parent) SendMessage(h->m_parent,WM_COMMAND,MAKEWPARAM(h->m_id,LBN_SELCHANGE),(LPARAM)h);
    break;
    case WT_LISTVIEW:
      ListView_SetItemState(h,it->index,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
      ListView_EnsureVisible(h,it->index,FALSE);
    break;
    case WT_TREEVIEW: TreeView_SelectItem(h,it->hti); break;
    case WT_TAB:
      TabCtrl_SetCurSel(h,it->index);
      if (h->m_parent)
      {
        NMHDR nm = { h, (UINT_PTR)h->m_id, TCN_SELCHANGE };
        SendMessage(h->m_parent,WM_NOTIFY,h->m_id,(LPARAM)&nm);
      }
    break;
    case WT_COMBO:
      SendMessage(h,CB_SETCURSEL,it->index,0);
      if (h->m_parent) SendMessage(h->m_parent,WM_COMMAND,MAKEWPARAM(h->m_id,CBN_SELCHANGE),(LPARAM)h);
    break;
  }
}

static void item_get_name(SwellAtkItem *it, WDL_FastString *out)
{
  out->Set("");
  HWND h = item_hwnd(it);
  if (!h) return;
  char buf[4096];
  switch (classifyHwnd(h))
  {
    case WT_LISTBOX:
      buf[0] = 0;
      SendMessage(h,LB_GETTEXT,it->index,(LPARAM)buf);
      out->Set(buf);
    break;
    case WT_LISTVIEW:
      {
        int nc = swell_atspi_get_listview_ncols(h);
        if (nc < 1) nc = 1;
        for (int c = 0; c < nc; c ++)
        {
          buf[0] = 0;
          ListView_GetItemText(h,it->index,c,buf,sizeof(buf));
          if (buf[0])
          {
            if (out->GetLength()) out->Append(", ");
            out->Append(buf);
          }
        }
      }
    break;
    case WT_TREEVIEW:
      if (it->hti->m_value) out->Set(it->hti->m_value);
    break;
    case WT_TAB:
      if (swell_atspi_get_tab_text(h,it->index,buf,sizeof(buf))) out->Set(buf);
    break;
    case WT_COMBO:
      buf[0] = 0;
      SendMessage(h,CB_GETLBTEXT,it->index,(LPARAM)buf);
      out->Set(buf);
    break;
  }
}

static AtkObject *swell_atk_container_get_item(SwellAtkBase *cont, int index, HTREEITEM hti);
static void swell_atk_item_component_iface_init(AtkComponentIface *iface);
static void swell_atk_item_action_iface_init(AtkActionIface *iface);

G_DEFINE_TYPE_WITH_CODE(SwellAtkItem, swell_atk_item, ATK_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(ATK_TYPE_COMPONENT, swell_atk_item_component_iface_init)
    G_IMPLEMENT_INTERFACE(ATK_TYPE_ACTION, swell_atk_item_action_iface_init))

static const gchar *swell_atk_item_get_name(AtkObject *o)
{
  SwellAtkItem *it = SWELL_ATK_ITEM(o);
  WDL_FastString s;
  item_get_name(it,&s);
  g_free(it->name_cache);
  it->name_cache = g_strdup(s.Get());
  return it->name_cache;
}

static AtkRole swell_atk_item_get_role(AtkObject *o)
{
  SwellAtkItem *it = SWELL_ATK_ITEM(o);
  HWND h = item_hwnd(it);
  if (!h) return ATK_ROLE_INVALID;
  switch (classifyHwnd(h))
  {
    case WT_TREEVIEW: return ATK_ROLE_TREE_ITEM;
    case WT_TAB: return ATK_ROLE_PAGE_TAB;
  }
  return ATK_ROLE_LIST_ITEM;
}

static AtkStateSet *swell_atk_item_ref_state_set(AtkObject *o)
{
  AtkStateSet *ss = ATK_OBJECT_CLASS(swell_atk_item_parent_class)->ref_state_set(o);
  SwellAtkItem *it = SWELL_ATK_ITEM(o);
  HWND h = item_hwnd(it);
  if (!h)
  {
    atk_state_set_add_state(ss,ATK_STATE_DEFUNCT);
    return ss;
  }
  atk_state_set_add_state(ss,ATK_STATE_SELECTABLE);
  atk_state_set_add_state(ss,ATK_STATE_FOCUSABLE);
  if (h->m_visible) atk_state_set_add_state(ss,ATK_STATE_VISIBLE);
  if (IsWindowVisible(h)) atk_state_set_add_state(ss,ATK_STATE_SHOWING);
  if (IsWindowEnabled(h))
  {
    atk_state_set_add_state(ss,ATK_STATE_ENABLED);
    atk_state_set_add_state(ss,ATK_STATE_SENSITIVE);
  }
  if (item_is_selected(it))
  {
    atk_state_set_add_state(ss,ATK_STATE_SELECTED);
    if (h == GetFocusIncludeMenus()) atk_state_set_add_state(ss,ATK_STATE_FOCUSED);
  }
  if (it->hti)
  {
    if (it->hti->m_haschildren || it->hti->m_children.GetSize())
    {
      atk_state_set_add_state(ss,ATK_STATE_EXPANDABLE);
      if (it->hti->m_state & TVIS_EXPANDED) atk_state_set_add_state(ss,ATK_STATE_EXPANDED);
    }
  }
  return ss;
}

static AtkObject *swell_atk_item_get_parent(AtkObject *o)
{
  SwellAtkItem *it = SWELL_ATK_ITEM(o);
  if (!it->container) return NULL;
  if (it->hti)
  {
    HWND h = item_hwnd(it);
    HTREEITEM par = h ? TreeView_GetParent(h,it->hti) : NULL;
    if (par) return swell_atk_container_get_item(it->container,-1,par);
  }
  return (AtkObject *)it->container;
}

static gint swell_atk_item_get_n_children(AtkObject *o)
{
  SwellAtkItem *it = SWELL_ATK_ITEM(o);
  if (!it->hti) return 0;
  return item_hwnd(it) ? it->hti->m_children.GetSize() : 0;
}

static AtkObject *swell_atk_item_ref_child(AtkObject *o, gint i)
{
  SwellAtkItem *it = SWELL_ATK_ITEM(o);
  if (!it->hti || !item_hwnd(it)) return NULL;
  HTREEITEM c = it->hti->m_children.Get(i);
  if (!c) return NULL;
  AtkObject *co = swell_atk_container_get_item(it->container,-1,c);
  if (co) g_object_ref(co);
  return co;
}

static gint swell_atk_item_get_index_in_parent(AtkObject *o)
{
  SwellAtkItem *it = SWELL_ATK_ITEM(o);
  HWND h = item_hwnd(it);
  if (!h) return -1;
  if (!it->hti) return it->index;
  HTREEITEM par = TreeView_GetParent(h,it->hti);
  if (par) return par->m_children.Find(it->hti);
  int idx = 0;
  HTREEITEM r = TreeView_GetRoot(h);
  while (r && r != it->hti) { idx++; r = TreeView_GetNextSibling(h,r); }
  return r ? idx : -1;
}

static void swell_atk_item_finalize(GObject *o)
{
  SwellAtkItem *it = SWELL_ATK_ITEM(o);
  if (it->container) { g_object_unref(it->container); it->container = NULL; }
  g_free(it->name_cache);
  it->name_cache = NULL;
  G_OBJECT_CLASS(swell_atk_item_parent_class)->finalize(o);
}

static void swell_atk_item_get_extents(AtkComponent *c, gint *x, gint *y,
                                       gint *w, gint *hh, AtkCoordType ct)
{
  if (x) *x = 0;
  if (y) *y = 0;
  if (w) *w = 0;
  if (hh) *hh = 0;
  SwellAtkItem *it = SWELL_ATK_ITEM(c);
  HWND h = item_hwnd(it);
  if (!h || it->hti) return;
  const int wt = classifyHwnd(h);
  if (wt != WT_LISTVIEW && wt != WT_LISTBOX) return;
  RECT r;
  if (!ListView_GetItemRect(h,it->index,&r,LVIR_BOUNDS)) return;
  gint cx, cy, cw, chh;
  swell_atk_component_get_extents((AtkComponent *)it->container,&cx,&cy,&cw,&chh,ct);
  if (x) *x = cx + r.left;
  if (y) *y = cy + r.top;
  if (w) *w = r.right - r.left;
  if (hh) *hh = r.bottom - r.top;
}

static gboolean swell_atk_item_grab_focus(AtkComponent *c)
{
  SwellAtkItem *it = SWELL_ATK_ITEM(c);
  HWND h = item_hwnd(it);
  if (!h) return FALSE;
  SetFocus(h);
  item_select(it);
  return TRUE;
}

static void swell_atk_item_component_iface_init(AtkComponentIface *iface)
{
  iface->get_extents = swell_atk_item_get_extents;
  iface->grab_focus = swell_atk_item_grab_focus;
}

static gboolean swell_atk_item_do_action(AtkAction *a, gint i)
{
  SwellAtkItem *it = SWELL_ATK_ITEM(a);
  if (i != 0 || !item_hwnd(it)) return FALSE;
  item_select(it);
  return TRUE;
}

static gint swell_atk_item_get_n_actions(AtkAction *a) { return 1; }
static const gchar *swell_atk_item_get_action_name(AtkAction *a, gint i)
{
  return i == 0 ? "click" : NULL;
}

static void swell_atk_item_action_iface_init(AtkActionIface *iface)
{
  iface->do_action = swell_atk_item_do_action;
  iface->get_n_actions = swell_atk_item_get_n_actions;
  iface->get_name = swell_atk_item_get_action_name;
}

static void swell_atk_item_class_init(SwellAtkItemClass *klass)
{
  AtkObjectClass *oc = ATK_OBJECT_CLASS(klass);
  oc->get_name = swell_atk_item_get_name;
  oc->get_role = swell_atk_item_get_role;
  oc->ref_state_set = swell_atk_item_ref_state_set;
  oc->get_parent = swell_atk_item_get_parent;
  oc->get_n_children = swell_atk_item_get_n_children;
  oc->ref_child = swell_atk_item_ref_child;
  oc->get_index_in_parent = swell_atk_item_get_index_in_parent;
  G_OBJECT_CLASS(klass)->finalize = swell_atk_item_finalize;
}

static void swell_atk_item_init(SwellAtkItem *it)
{
  it->container = NULL;
  it->index = -1;
  it->hti = NULL;
  it->name_cache = NULL;
}

// items are cached on the container wrapper so the same (index/htreeitem)
// always resolves to the same AtkObject while the container lives
static AtkObject *swell_atk_container_get_item(SwellAtkBase *cont, int index, HTREEITEM hti)
{
  if (!cont) return NULL;
  if (!cont->item_cache)
    cont->item_cache = g_hash_table_new_full(NULL,NULL,NULL,g_object_unref);
  gpointer key = hti ? (gpointer)hti : GINT_TO_POINTER(index + 1);
  SwellAtkItem *it = (SwellAtkItem *)g_hash_table_lookup(cont->item_cache,key);
  if (!it)
  {
    it = (SwellAtkItem *)g_object_new(SWELL_TYPE_ATK_ITEM,NULL);
    it->container = (SwellAtkBase *)g_object_ref(cont);
    it->index = index;
    it->hti = hti;
    g_hash_table_insert(cont->item_cache,key,it); // cache owns this ref
  }
  return (AtkObject *)it;
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

/////////////// buttons (push/check/radio)

#define SWELL_TYPE_ATK_BUTTON (swell_atk_button_get_type())
typedef struct { SwellAtkBase parent; } SwellAtkButton;
typedef struct { SwellAtkBaseClass parent; } SwellAtkButtonClass;

static gboolean swell_atk_button_do_action(AtkAction *a, gint i)
{
  HWND h = swell_atk_hwnd((AtkObject *)a);
  if (!h || i != 0 || !IsWindowEnabled(h)) return FALSE;
  SendMessage(h,WM_KEYDOWN,VK_SPACE,0); // drives the real click path, including BN_CLICKED
  return TRUE;
}

static gint swell_atk_button_get_n_actions(AtkAction *a)
{
  return 1;
}

static const gchar *swell_atk_button_get_action_name(AtkAction *a, gint i)
{
  return i == 0 ? "press" : NULL;
}

static void swell_atk_action_iface_init(AtkActionIface *iface)
{
  iface->do_action = swell_atk_button_do_action;
  iface->get_n_actions = swell_atk_button_get_n_actions;
  iface->get_name = swell_atk_button_get_action_name;
}

G_DEFINE_TYPE_WITH_CODE(SwellAtkButton, swell_atk_button, SWELL_TYPE_ATK_BASE,
    G_IMPLEMENT_INTERFACE(ATK_TYPE_ACTION, swell_atk_action_iface_init))

static AtkRole swell_atk_button_get_role(AtkObject *o)
{
  switch (classifyHwnd(swell_atk_hwnd(o)))
  {
    case WT_CHECKBOX: return ATK_ROLE_CHECK_BOX;
    case WT_RADIO: return ATK_ROLE_RADIO_BUTTON;
    case WT_PUSHBUTTON: return ATK_ROLE_PUSH_BUTTON;
  }
  return ATK_ROLE_INVALID;
}

static void swell_atk_button_class_init(SwellAtkButtonClass *klass)
{
  ATK_OBJECT_CLASS(klass)->get_role = swell_atk_button_get_role;
}
static void swell_atk_button_init(SwellAtkButton *b) { }

/////////////// value controls (trackbar/progress)

#define SWELL_TYPE_ATK_VALUE (swell_atk_value_get_type())
#define SWELL_ATK_VALUE(o) (G_TYPE_CHECK_INSTANCE_CAST((o),SWELL_TYPE_ATK_VALUE,SwellAtkValue))
typedef struct {
  SwellAtkBase parent;
  gint64 last_emit;   // monotonic us of last value-changed emission (drag throttling)
  double last_value;  // last emitted value, to drop duplicate notifications
} SwellAtkValue;
typedef struct { SwellAtkBaseClass parent; } SwellAtkValueClass;

static void swell_atk_value_iface_init(AtkValueIface *iface);

G_DEFINE_TYPE_WITH_CODE(SwellAtkValue, swell_atk_value, SWELL_TYPE_ATK_BASE,
    G_IMPLEMENT_INTERFACE(ATK_TYPE_VALUE, swell_atk_value_iface_init))

static AtkRole swell_atk_value_get_role(AtkObject *o)
{
  switch (classifyHwnd(swell_atk_hwnd(o)))
  {
    case WT_TRACKBAR: return ATK_ROLE_SLIDER;
    case WT_PROGRESS: return ATK_ROLE_PROGRESS_BAR;
  }
  return ATK_ROLE_INVALID;
}

static void swell_atk_value_class_init(SwellAtkValueClass *klass)
{
  ATK_OBJECT_CLASS(klass)->get_role = swell_atk_value_get_role;
}
static void swell_atk_value_init(SwellAtkValue *v)
{
  v->last_emit = 0;
  v->last_value = -1e300;
}

// trackbar and progress state share the layout int[0]=pos,
// int[1]=range packed as MAKELONG(min,max) -- see trackbarWindowProc/progressWindowProc
static bool swell_atk_value_read(HWND h, double *cur, double *lo, double *hi)
{
  const int wt = classifyHwnd(h);
  if ((wt != WT_TRACKBAR && wt != WT_PROGRESS) || !h->m_private_data) return false;
  const int *state = (const int *)h->m_private_data;
  if (cur) *cur = state[0];
  if (lo) *lo = (short)LOWORD(state[1]);
  if (hi) *hi = (short)HIWORD(state[1]);
  return true;
}

static void swell_atk_value_get_value_and_text(AtkValue *v, gdouble *value, gchar **text)
{
  if (value) *value = 0;
  if (text) *text = NULL;
  double cur;
  HWND h = swell_atk_hwnd((AtkObject *)v);
  if (h && swell_atk_value_read(h,&cur,NULL,NULL) && value) *value = cur;
}

static AtkRange *swell_atk_value_get_range(AtkValue *v)
{
  double lo, hi;
  HWND h = swell_atk_hwnd((AtkObject *)v);
  if (!h || !swell_atk_value_read(h,NULL,&lo,&hi)) return NULL;
  return atk_range_new(lo,hi,NULL);
}

static gdouble swell_atk_value_get_increment(AtkValue *v)
{
  return 1;
}

static void swell_atk_value_set_value(AtkValue *v, const gdouble value)
{
  HWND h = swell_atk_hwnd((AtkObject *)v);
  if (!h || classifyHwnd(h) != WT_TRACKBAR || !IsWindowEnabled(h)) return;
  double lo, hi;
  if (!swell_atk_value_read(h,NULL,&lo,&hi)) return;
  double nv = value;
  if (nv < lo) nv = lo;
  else if (nv > hi) nv = hi;
  SendMessage(h,TBM_SETPOS,1,(LPARAM)(int)nv);
  if (h->m_parent) SendMessage(h->m_parent,WM_HSCROLL,SB_ENDSCROLL,(LPARAM)h);
}

// legacy AtkValue API (some ATs still query it)
static void swell_atk_value_get_current_value(AtkValue *v, GValue *gv)
{
  gdouble d = 0;
  swell_atk_value_get_value_and_text(v,&d,NULL);
  g_value_init(gv,G_TYPE_DOUBLE);
  g_value_set_double(gv,d);
}

static void swell_atk_value_get_maximum_value(AtkValue *v, GValue *gv)
{
  double lo = 0, hi = 0;
  HWND h = swell_atk_hwnd((AtkObject *)v);
  if (h) swell_atk_value_read(h,NULL,&lo,&hi);
  g_value_init(gv,G_TYPE_DOUBLE);
  g_value_set_double(gv,hi);
}

static void swell_atk_value_get_minimum_value(AtkValue *v, GValue *gv)
{
  double lo = 0, hi = 0;
  HWND h = swell_atk_hwnd((AtkObject *)v);
  if (h) swell_atk_value_read(h,NULL,&lo,&hi);
  g_value_init(gv,G_TYPE_DOUBLE);
  g_value_set_double(gv,lo);
}

static gboolean swell_atk_value_set_current_value(AtkValue *v, const GValue *gv)
{
  if (!G_VALUE_HOLDS_DOUBLE(gv)) return FALSE;
  swell_atk_value_set_value(v,g_value_get_double(gv));
  return TRUE;
}

static void swell_atk_value_iface_init(AtkValueIface *iface)
{
  iface->get_value_and_text = swell_atk_value_get_value_and_text;
  iface->get_range = swell_atk_value_get_range;
  iface->get_increment = swell_atk_value_get_increment;
  iface->set_value = swell_atk_value_set_value;
  iface->get_current_value = swell_atk_value_get_current_value;
  iface->get_maximum_value = swell_atk_value_get_maximum_value;
  iface->get_minimum_value = swell_atk_value_get_minimum_value;
  iface->set_current_value = swell_atk_value_set_current_value;
}

// emits value-changed, throttled during drags unless force is set
static void notify_value_changed(HWND h, bool force)
{
  AtkObject *o = swell_atspi_wrapper(h,false);
  if (!o || !G_TYPE_CHECK_INSTANCE_TYPE(o,SWELL_TYPE_ATK_VALUE)) return;
  SwellAtkValue *v = SWELL_ATK_VALUE(o);
  double cur;
  if (!swell_atk_value_read(h,&cur,NULL,NULL) || cur == v->last_value) return;
  const gint64 now = g_get_monotonic_time();
  if (!force && now - v->last_emit < 50000) return;
  v->last_emit = now;
  v->last_value = cur;
  g_object_notify(G_OBJECT(o),"accessible-value");
}

/////////////// edit controls

#define SWELL_TYPE_ATK_EDIT (swell_atk_edit_get_type())
#define SWELL_ATK_EDIT(o) (G_TYPE_CHECK_INSTANCE_CAST((o),SWELL_TYPE_ATK_EDIT,SwellAtkEdit))
typedef struct {
  SwellAtkBase parent;
  // last-reported text state, diffed against on every observed change
  gchar *tcache;
  gint ccaret, csel1, csel2;
} SwellAtkEdit;
typedef struct { SwellAtkBaseClass parent; } SwellAtkEditClass;

static void swell_atk_text_iface_init(AtkTextIface *iface);
static void swell_atk_editable_text_iface_init(AtkEditableTextIface *iface);

G_DEFINE_TYPE_WITH_CODE(SwellAtkEdit, swell_atk_edit, SWELL_TYPE_ATK_BASE,
    G_IMPLEMENT_INTERFACE(ATK_TYPE_TEXT, swell_atk_text_iface_init)
    G_IMPLEMENT_INTERFACE(ATK_TYPE_EDITABLE_TEXT, swell_atk_editable_text_iface_init))

static AtkRole swell_atk_edit_get_role(AtkObject *o)
{
  HWND h = swell_atk_hwnd(o);
  if (!h) return ATK_ROLE_INVALID;
  return (h->m_style & ES_PASSWORD) ? ATK_ROLE_PASSWORD_TEXT : ATK_ROLE_TEXT;
}

static void swell_atk_edit_finalize(GObject *o)
{
  g_free(SWELL_ATK_EDIT(o)->tcache);
  SWELL_ATK_EDIT(o)->tcache = NULL;
  G_OBJECT_CLASS(swell_atk_edit_parent_class)->finalize(o);
}

static void swell_atk_edit_class_init(SwellAtkEditClass *klass)
{
  ATK_OBJECT_CLASS(klass)->get_role = swell_atk_edit_get_role;
  G_OBJECT_CLASS(klass)->finalize = swell_atk_edit_finalize;
}

static void swell_atk_edit_init(SwellAtkEdit *e)
{
  e->tcache = NULL;
  e->ccaret = e->csel1 = e->csel2 = -1;
}

// current edit text as the AT should see it (password chars masked)
static gchar *swell_atk_edit_read_text(HWND h)
{
  const char *t = h->m_title.Get();
  if (h->m_style & ES_PASSWORD)
    return g_strnfill(g_utf8_strlen(t,-1),'*');
  return g_strdup(t);
}

static void swell_atk_edit_read_sel(HWND h, gint *caret, gint *s1, gint *s2)
{
  int c = -1, a = -1, b = -1;
  swell_atspi_get_edit_state(h,&c,&a,&b);
  if (a > b && b >= 0) { const int t = a; a = b; b = t; }
  if (a < 0 || b < 0 || a == b) a = b = -1;
  *caret = c >= 0 ? c : 0;
  *s1 = a;
  *s2 = b;
}

// diffs current state against the wrapper cache and emits text events
static void swell_atk_edit_sync(HWND h, bool emit)
{
  AtkObject *o = swell_atspi_wrapper(h,false);
  if (!o || !SWELL_IS_ATK_BASE(o) || !G_TYPE_CHECK_INSTANCE_TYPE(o,SWELL_TYPE_ATK_EDIT)) return;
  SwellAtkEdit *e = SWELL_ATK_EDIT(o);

  gchar *cur = swell_atk_edit_read_text(h);
  gint caret, s1, s2;
  swell_atk_edit_read_sel(h,&caret,&s1,&s2);

  if (e->tcache && strcmp(cur,e->tcache))
  {
    if (emit)
    {
      const gchar *olds = e->tcache, *news = cur;
      const glong oldlen = g_utf8_strlen(olds,-1), newlen = g_utf8_strlen(news,-1);
      glong pre = 0;
      const gchar *op = olds, *np = news;
      while (*op && *np)
      {
        if (g_utf8_get_char(op) != g_utf8_get_char(np)) break;
        op = g_utf8_next_char(op);
        np = g_utf8_next_char(np);
        pre++;
      }
      glong suf = 0;
      {
        const glong maxsuf = wdl_min(oldlen,newlen) - pre;
        const gchar *oe = olds + strlen(olds), *ne = news + strlen(news);
        while (suf < maxsuf)
        {
          const gchar *po = g_utf8_prev_char(oe), *pn = g_utf8_prev_char(ne);
          if (g_utf8_get_char(po) != g_utf8_get_char(pn)) break;
          oe = po; ne = pn;
          suf++;
        }
      }
      const glong ndel = oldlen - pre - suf, nins = newlen - pre - suf;
      if (ndel > 0)
      {
        gchar *seg = g_utf8_substring(olds,pre,pre+ndel);
        g_signal_emit_by_name(o,"text-remove",(gint)pre,(gint)ndel,seg);
        g_free(seg);
      }
      if (nins > 0)
      {
        gchar *seg = g_utf8_substring(news,pre,pre+nins);
        g_signal_emit_by_name(o,"text-insert",(gint)pre,(gint)nins,seg);
        g_free(seg);
      }
    }
    g_free(e->tcache);
    e->tcache = cur;
  }
  else if (!e->tcache) e->tcache = cur;
  else g_free(cur);

  if (caret != e->ccaret)
  {
    e->ccaret = caret;
    if (emit) g_signal_emit_by_name(o,"text-caret-moved",caret);
  }
  if (s1 != e->csel1 || s2 != e->csel2)
  {
    e->csel1 = s1;
    e->csel2 = s2;
    if (emit) g_signal_emit_by_name(o,"text-selection-changed");
  }
}

/////////////// AtkText implementation

static gchar *swell_atk_text_get_text(AtkText *t, gint start, gint end)
{
  HWND h = swell_atk_hwnd((AtkObject *)t);
  if (!h) return NULL;
  gchar *full = swell_atk_edit_read_text(h);
  const glong len = g_utf8_strlen(full,-1);
  if (start < 0) start = 0;
  if (end < 0 || end > len) end = (gint)len;
  gchar *r = start < end ? g_utf8_substring(full,start,end) : g_strdup("");
  g_free(full);
  return r;
}

static gint swell_atk_text_get_character_count(AtkText *t)
{
  HWND h = swell_atk_hwnd((AtkObject *)t);
  return h ? (gint)g_utf8_strlen(h->m_title.Get(),-1) : 0;
}

static gunichar swell_atk_text_get_character_at_offset(AtkText *t, gint offset)
{
  HWND h = swell_atk_hwnd((AtkObject *)t);
  if (!h || offset < 0) return 0;
  gchar *full = swell_atk_edit_read_text(h);
  gunichar r = 0;
  if (offset < g_utf8_strlen(full,-1))
    r = g_utf8_get_char(g_utf8_offset_to_pointer(full,offset));
  g_free(full);
  return r;
}

static gint swell_atk_text_get_caret_offset(AtkText *t)
{
  HWND h = swell_atk_hwnd((AtkObject *)t);
  if (!h) return 0;
  gint caret, s1, s2;
  swell_atk_edit_read_sel(h,&caret,&s1,&s2);
  return caret;
}

static gboolean swell_atk_text_set_caret_offset(AtkText *t, gint offset)
{
  HWND h = swell_atk_hwnd((AtkObject *)t);
  if (!h) return FALSE;
  SendMessage(h,EM_SETSEL,offset,offset);
  return TRUE;
}

static gint swell_atk_text_get_n_selections(AtkText *t)
{
  HWND h = swell_atk_hwnd((AtkObject *)t);
  if (!h) return 0;
  gint caret, s1, s2;
  swell_atk_edit_read_sel(h,&caret,&s1,&s2);
  return s1 >= 0 ? 1 : 0;
}

static gchar *swell_atk_text_get_selection(AtkText *t, gint selnum, gint *start, gint *end)
{
  if (start) *start = 0;
  if (end) *end = 0;
  HWND h = swell_atk_hwnd((AtkObject *)t);
  if (!h || selnum != 0) return NULL;
  gint caret, s1, s2;
  swell_atk_edit_read_sel(h,&caret,&s1,&s2);
  if (s1 < 0) return NULL;
  if (start) *start = s1;
  if (end) *end = s2;
  return swell_atk_text_get_text(t,s1,s2);
}

static gboolean swell_atk_text_set_selection(AtkText *t, gint selnum, gint start, gint end)
{
  HWND h = swell_atk_hwnd((AtkObject *)t);
  if (!h || selnum != 0) return FALSE;
  SendMessage(h,EM_SETSEL,start,end);
  return TRUE;
}

static gboolean swell_atk_text_add_selection(AtkText *t, gint start, gint end)
{
  return swell_atk_text_set_selection(t,0,start,end);
}

static gboolean swell_atk_text_remove_selection(AtkText *t, gint selnum)
{
  HWND h = swell_atk_hwnd((AtkObject *)t);
  if (!h || selnum != 0) return FALSE;
  gint caret, s1, s2;
  swell_atk_edit_read_sel(h,&caret,&s1,&s2);
  SendMessage(h,EM_SETSEL,caret,caret);
  return TRUE;
}

// computes [start,end) character bounds for char/word/line units around offset
static void swell_atk_text_bounds(const gchar *full, gint offset, int unit, gint *start, gint *end)
{
  const glong len = g_utf8_strlen(full,-1);
  if (offset < 0) offset = 0;
  if (offset > len) offset = (gint)len;
  gint s = offset, e = offset;
  switch (unit)
  {
    case 0: // character
      e = offset < len ? offset+1 : offset;
    break;
    case 1: // word: [start of word containing/before offset, start of next word)
      {
        while (s > 0)
        {
          const gunichar c = g_utf8_get_char(g_utf8_offset_to_pointer(full,s-1));
          if (g_unichar_isspace(c)) break;
          s--;
        }
        e = offset;
        while (e < len && !g_unichar_isspace(g_utf8_get_char(g_utf8_offset_to_pointer(full,e)))) e++;
        while (e < len && g_unichar_isspace(g_utf8_get_char(g_utf8_offset_to_pointer(full,e)))) e++;
      }
    break;
    case 2: // line (buffer lines; word-wrap visual lines are not modeled)
      {
        while (s > 0 && g_utf8_get_char(g_utf8_offset_to_pointer(full,s-1)) != '\n') s--;
        e = offset;
        while (e < len && g_utf8_get_char(g_utf8_offset_to_pointer(full,e)) != '\n') e++;
        if (e < len) e++; // include the newline
      }
    break;
  }
  *start = s;
  *end = e;
}

static gchar *swell_atk_text_get_string_at_offset(AtkText *t, gint offset,
    AtkTextGranularity granularity, gint *start, gint *end)
{
  if (start) *start = 0;
  if (end) *end = 0;
  HWND h = swell_atk_hwnd((AtkObject *)t);
  if (!h) return NULL;
  int unit;
  switch (granularity)
  {
    case ATK_TEXT_GRANULARITY_CHAR: unit = 0; break;
    case ATK_TEXT_GRANULARITY_WORD: unit = 1; break;
    case ATK_TEXT_GRANULARITY_LINE:
    case ATK_TEXT_GRANULARITY_SENTENCE:
    case ATK_TEXT_GRANULARITY_PARAGRAPH: unit = 2; break;
    default: return NULL;
  }
  gchar *full = swell_atk_edit_read_text(h);
  gint s, e;
  swell_atk_text_bounds(full,offset,unit,&s,&e);
  gchar *r = g_utf8_substring(full,s,e);
  g_free(full);
  if (start) *start = s;
  if (end) *end = e;
  return r;
}

static gchar *swell_atk_text_get_text_at_offset(AtkText *t, gint offset,
    AtkTextBoundary boundary, gint *start, gint *end)
{
  AtkTextGranularity g;
  switch (boundary)
  {
    case ATK_TEXT_BOUNDARY_CHAR: g = ATK_TEXT_GRANULARITY_CHAR; break;
    case ATK_TEXT_BOUNDARY_WORD_START:
    case ATK_TEXT_BOUNDARY_WORD_END: g = ATK_TEXT_GRANULARITY_WORD; break;
    default: g = ATK_TEXT_GRANULARITY_LINE; break;
  }
  return swell_atk_text_get_string_at_offset(t,offset,g,start,end);
}

static void swell_atk_text_iface_init(AtkTextIface *iface)
{
  iface->get_text = swell_atk_text_get_text;
  iface->get_character_count = swell_atk_text_get_character_count;
  iface->get_character_at_offset = swell_atk_text_get_character_at_offset;
  iface->get_caret_offset = swell_atk_text_get_caret_offset;
  iface->set_caret_offset = swell_atk_text_set_caret_offset;
  iface->get_n_selections = swell_atk_text_get_n_selections;
  iface->get_selection = swell_atk_text_get_selection;
  iface->set_selection = swell_atk_text_set_selection;
  iface->add_selection = swell_atk_text_add_selection;
  iface->remove_selection = swell_atk_text_remove_selection;
  iface->get_string_at_offset = swell_atk_text_get_string_at_offset;
  iface->get_text_at_offset = swell_atk_text_get_text_at_offset;
}

/////////////// AtkEditableText implementation

static void swell_atk_edtext_set_text_contents(AtkEditableText *t, const gchar *s)
{
  HWND h = swell_atk_hwnd((AtkObject *)t);
  if (h && !(h->m_style & ES_READONLY)) SendMessage(h,WM_SETTEXT,0,(LPARAM)(s ? s : ""));
}

static void swell_atk_edtext_insert_text(AtkEditableText *t, const gchar *s, gint len, gint *pos)
{
  HWND h = swell_atk_hwnd((AtkObject *)t);
  if (!h || (h->m_style & ES_READONLY) || !s) return;
  const gint p = pos ? *pos : 0;
  gchar *seg = len >= 0 ? g_strndup(s,len) : g_strdup(s);
  SendMessage(h,EM_SETSEL,p,p);
  SendMessage(h,EM_REPLACESEL,TRUE,(LPARAM)seg);
  if (pos) *pos = p + (gint)g_utf8_strlen(seg,-1);
  g_free(seg);
}

static void swell_atk_edtext_delete_text(AtkEditableText *t, gint start, gint end)
{
  HWND h = swell_atk_hwnd((AtkObject *)t);
  if (!h || (h->m_style & ES_READONLY)) return;
  SendMessage(h,EM_SETSEL,start,end);
  SendMessage(h,EM_REPLACESEL,TRUE,(LPARAM)"");
}

static void swell_atk_editable_text_iface_init(AtkEditableTextIface *iface)
{
  iface->set_text_contents = swell_atk_edtext_set_text_contents;
  iface->insert_text = swell_atk_edtext_insert_text;
  iface->delete_text = swell_atk_edtext_delete_text;
}

/////////////// combo boxes

/////////////// shared container child/selection plumbing (list-likes + combo)

static gint swell_atk_container_get_n_children(AtkObject *o)
{
  HWND h = swell_atk_hwnd(o);
  return h ? cont_item_count(h) : 0;
}

static AtkObject *swell_atk_container_ref_child(AtkObject *o, gint i)
{
  HWND h = swell_atk_hwnd(o);
  if (!h || i < 0 || i >= cont_item_count(h)) return NULL;
  AtkObject *c;
  if (classifyHwnd(h) == WT_TREEVIEW)
  {
    HTREEITEM r = TreeView_GetRoot(h);
    while (r && i-- > 0) r = TreeView_GetNextSibling(h,r);
    c = r ? swell_atk_container_get_item(SWELL_ATK_BASE(o),-1,r) : NULL;
  }
  else c = swell_atk_container_get_item(SWELL_ATK_BASE(o),i,NULL);
  if (c) g_object_ref(c);
  return c;
}

// current "cursor" item of a container, or NULL
static AtkObject *swell_atk_container_current_item(HWND h)
{
  if (!h || !h->m_atspi) return NULL;
  SwellAtkBase *cont = SWELL_ATK_BASE((AtkObject *)h->m_atspi);
  switch (classifyHwnd(h))
  {
    case WT_LISTBOX:
      {
        const int i = (int)SendMessage(h,LB_GETCURSEL,0,0);
        return i >= 0 ? swell_atk_container_get_item(cont,i,NULL) : NULL;
      }
    case WT_LISTVIEW:
      {
        const int n = ListView_GetItemCount(h);
        for (int i = 0; i < n; i ++)
          if (ListView_GetItemState(h,i,LVIS_FOCUSED|LVIS_SELECTED))
            return swell_atk_container_get_item(cont,i,NULL);
        return NULL;
      }
    case WT_TREEVIEW:
      {
        HTREEITEM sel = TreeView_GetSelection(h);
        return sel ? swell_atk_container_get_item(cont,-1,sel) : NULL;
      }
    case WT_TAB:
      {
        const int i = TabCtrl_GetCurSel(h);
        return i >= 0 && i < TabCtrl_GetItemCount(h) ? swell_atk_container_get_item(cont,i,NULL) : NULL;
      }
    case WT_COMBO:
      {
        const int i = (int)SendMessage(h,CB_GETCURSEL,0,0);
        return i >= 0 ? swell_atk_container_get_item(cont,i,NULL) : NULL;
      }
  }
  return NULL;
}

static gint swell_atk_sel_get_selection_count(AtkSelection *s)
{
  HWND h = swell_atk_hwnd((AtkObject *)s);
  if (!h) return 0;
  if (classifyHwnd(h) == WT_LISTVIEW) return ListView_GetSelectedCount(h);
  return swell_atk_container_current_item(h) ? 1 : 0;
}

static AtkObject *swell_atk_sel_ref_selection(AtkSelection *s, gint i)
{
  HWND h = swell_atk_hwnd((AtkObject *)s);
  if (!h) return NULL;
  if (classifyHwnd(h) == WT_LISTVIEW)
  {
    const int n = ListView_GetItemCount(h);
    for (int x = 0; x < n; x ++)
      if (ListView_GetItemState(h,x,LVIS_SELECTED) && i-- == 0)
      {
        AtkObject *c = swell_atk_container_get_item(SWELL_ATK_BASE(s),x,NULL);
        if (c) g_object_ref(c);
        return c;
      }
    return NULL;
  }
  if (i != 0) return NULL;
  AtkObject *c = swell_atk_container_current_item(h);
  if (c) g_object_ref(c);
  return c;
}

static gboolean swell_atk_sel_is_child_selected(AtkSelection *s, gint i)
{
  HWND h = swell_atk_hwnd((AtkObject *)s);
  if (!h || i < 0 || i >= cont_item_count(h)) return FALSE;
  AtkObject *c = swell_atk_container_ref_child((AtkObject *)s,i);
  if (!c) return FALSE;
  const gboolean r = item_is_selected(SWELL_ATK_ITEM(c));
  g_object_unref(c);
  return r;
}

static gboolean swell_atk_sel_add_selection(AtkSelection *s, gint i)
{
  AtkObject *c = swell_atk_container_ref_child((AtkObject *)s,i);
  if (!c) return FALSE;
  item_select(SWELL_ATK_ITEM(c));
  g_object_unref(c);
  return TRUE;
}

static void swell_atk_selection_iface_init(AtkSelectionIface *iface)
{
  iface->get_selection_count = swell_atk_sel_get_selection_count;
  iface->ref_selection = swell_atk_sel_ref_selection;
  iface->is_child_selected = swell_atk_sel_is_child_selected;
  iface->add_selection = swell_atk_sel_add_selection;
}

/////////////// combo boxes

#define SWELL_TYPE_ATK_COMBO (swell_atk_combo_get_type())
typedef struct { SwellAtkBase parent; } SwellAtkCombo;
typedef struct { SwellAtkBaseClass parent; } SwellAtkComboClass;

G_DEFINE_TYPE_WITH_CODE(SwellAtkCombo, swell_atk_combo, SWELL_TYPE_ATK_BASE,
    G_IMPLEMENT_INTERFACE(ATK_TYPE_SELECTION, swell_atk_selection_iface_init))

static AtkRole swell_atk_combo_get_role(AtkObject *o)
{
  return swell_atk_hwnd(o) ? ATK_ROLE_COMBO_BOX : ATK_ROLE_INVALID;
}

static void swell_atk_combo_class_init(SwellAtkComboClass *klass)
{
  AtkObjectClass *oc = ATK_OBJECT_CLASS(klass);
  oc->get_role = swell_atk_combo_get_role;
  oc->get_n_children = swell_atk_container_get_n_children;
  oc->ref_child = swell_atk_container_ref_child;
}
static void swell_atk_combo_init(SwellAtkCombo *c) { }

/////////////// list-like controls (listbox/listview/treeview/tab)

#define SWELL_TYPE_ATK_LIST (swell_atk_list_get_type())
typedef struct { SwellAtkBase parent; } SwellAtkList;
typedef struct { SwellAtkBaseClass parent; } SwellAtkListClass;

G_DEFINE_TYPE_WITH_CODE(SwellAtkList, swell_atk_list, SWELL_TYPE_ATK_BASE,
    G_IMPLEMENT_INTERFACE(ATK_TYPE_SELECTION, swell_atk_selection_iface_init))

static AtkRole swell_atk_list_get_role(AtkObject *o)
{
  switch (classifyHwnd(swell_atk_hwnd(o)))
  {
    case WT_LISTBOX: return ATK_ROLE_LIST_BOX;
    case WT_LISTVIEW: return ATK_ROLE_TREE_TABLE;
    case WT_TREEVIEW: return ATK_ROLE_TREE;
    case WT_TAB: return ATK_ROLE_PAGE_TAB_LIST;
  }
  return ATK_ROLE_INVALID;
}

static void swell_atk_list_class_init(SwellAtkListClass *klass)
{
  AtkObjectClass *oc = ATK_OBJECT_CLASS(klass);
  oc->get_role = swell_atk_list_get_role;
  oc->get_n_children = swell_atk_container_get_n_children;
  oc->ref_child = swell_atk_container_ref_child;
}
static void swell_atk_list_init(SwellAtkList *l) { }

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

  GType t;
  switch (classifyHwnd(h))
  {
    case WT_TOPLEVEL: t = SWELL_TYPE_ATK_TOPLEVEL; break;
    case WT_PUSHBUTTON:
    case WT_CHECKBOX:
    case WT_RADIO: t = SWELL_TYPE_ATK_BUTTON; break;
    case WT_TRACKBAR:
    case WT_PROGRESS: t = SWELL_TYPE_ATK_VALUE; break;
    case WT_EDIT: t = SWELL_TYPE_ATK_EDIT; break;
    case WT_COMBO: t = SWELL_TYPE_ATK_COMBO; break;
    case WT_LISTBOX:
    case WT_LISTVIEW:
    case WT_TREEVIEW:
    case WT_TAB: t = SWELL_TYPE_ATK_LIST; break;
    default: t = SWELL_TYPE_ATK_BASE; break;
  }

  SwellAtkBase *b = (SwellAtkBase *)g_object_new(t,NULL);
  h->Retain();
  b->hwnd = h;
  h->m_atspi = b;
  if (t == SWELL_TYPE_ATK_EDIT) swell_atk_edit_sync(h,false); // seed the text cache silently
  return (AtkObject *)b;
}

// current window-activation state, tracked so activate/deactivate pairs stay balanced
static AtkObject *s_active_frame;
// last object that got a focused=TRUE notification, so FALSE can be paired to it
static AtkObject *s_focus_obj;

static bool focus_obj_live(AtkObject *o)
{
  if (SWELL_IS_ATK_ITEM(o)) return item_hwnd(SWELL_ATK_ITEM(o)) != NULL;
  return swell_atk_hwnd(o) != NULL;
}

// true if o is a virtual item belonging to container hwnd h
static bool is_item_of(AtkObject *o, HWND h)
{
  return o && SWELL_IS_ATK_ITEM(o) && SWELL_ATK_ITEM(o)->container &&
         SWELL_ATK_ITEM(o)->container->hwnd == h;
}

static void set_focus_obj(AtkObject *o)
{
  if (o == s_focus_obj) return;
  if (s_focus_obj)
  {
    AtkObject *old = s_focus_obj;
    s_focus_obj = NULL;
    if (focus_obj_live(old))
      atk_object_notify_state_change(old,ATK_STATE_FOCUSED,FALSE);
    g_object_unref(old);
  }
  if (o)
  {
    s_focus_obj = (AtkObject *)g_object_ref(o);
    atk_object_notify_state_change(o,ATK_STATE_FOCUSED,TRUE);
  }
}

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
  if (o == s_focus_obj || is_item_of(s_focus_obj,h))
  {
    AtkObject *f = s_focus_obj;
    s_focus_obj = NULL;
    g_object_unref(f);
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

static void notify_check_state(HWND h)
{
  AtkObject *o = swell_atspi_wrapper(h,false);
  if (!o) return;
  const LRESULT chk = SendMessage(h,BM_GETCHECK,0,0);
  atk_object_notify_state_change(o,ATK_STATE_CHECKED,chk == 1);
  if (classifyHwnd(h) == WT_CHECKBOX)
    atk_object_notify_state_change(o,ATK_STATE_INDETERMINATE,chk == 2);
}

// a radio click untoggles its group siblings without any observable message,
// so refresh the whole run (same traversal as buttonWindowProc's radio logic)
static void notify_radio_group(HWND h)
{
  notify_check_state(h);
  for (int x = 0; x < 2; x ++)
  {
    HWND nw = x ? h->m_next : h->m_prev;
    while (nw)
    {
      if (classifyHwnd(nw) != WT_RADIO) break;
      if (x && (nw->m_style & WS_GROUP)) break;
      notify_check_state(nw);
      if (nw->m_style & WS_GROUP) break;
      nw = x ? nw->m_next : nw->m_prev;
    }
  }
}

// PRE/POST pairs are strictly nested, so snapshots live on a small stack
static struct { HWND h; UINT msg; LRESULT val; } s_snap[16];
static int s_snap_depth;

void swell_atspi_msg_pre(HWND h, UINT m, WPARAM w, LPARAM l)
{
  switch (m)
  {
    case BM_SETCHECK:
      {
        const int wt = classifyHwnd(h);
        if ((wt == WT_CHECKBOX || wt == WT_RADIO) && h->m_atspi &&
            s_snap_depth < (int) (sizeof(s_snap)/sizeof(s_snap[0])))
        {
          s_snap[s_snap_depth].h = h;
          s_snap[s_snap_depth].msg = m;
          s_snap[s_snap_depth].val = SendMessage(h,BM_GETCHECK,0,0);
          s_snap_depth++;
        }
      }
    break;
  }
}

void swell_atspi_msg_post(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT r)
{
  switch (m)
  {
    case WM_SETFOCUS:
      {
        ATSPI_DEBUG("WM_SETFOCUS hwnd=%p class=%s\n",(void*)h,h->m_classname);
        HWND tl = toplevelOf(h);
        if (!wantWrapper(tl) || !wantWrapper(h)) break;
        AtkObject *frame = swell_atspi_wrapper(tl,true);
        if (frame) set_active_frame(frame);
        AtkObject *o = swell_atspi_wrapper(h,true);
        AtkObject *item = swell_atk_container_current_item(h);
        set_focus_obj(item ? item : o);
      }
    break;
    case WM_KILLFOCUS:
      if (s_focus_obj &&
          ((AtkObject *)h->m_atspi == s_focus_obj || is_item_of(s_focus_obj,h)))
        set_focus_obj(NULL);
    break;
    case WM_DESTROY:
      if (h->m_hashaddestroy == 2) wrapper_notify_destroyed(h);
    break;
    case BM_SETCHECK:
      if (s_snap_depth > 0 && s_snap[s_snap_depth-1].h == h && s_snap[s_snap_depth-1].msg == m)
      {
        s_snap_depth--;
        if (SendMessage(h,BM_GETCHECK,0,0) != s_snap[s_snap_depth].val)
          notify_check_state(h);
      }
    break;
    case WM_COMMAND:
      if (l)
      {
        HWND src = (HWND)l;
        switch (HIWORD(w))
        {
          case BN_CLICKED:
            switch (classifyHwnd(src))
            {
              case WT_CHECKBOX: notify_check_state(src); break;
              case WT_RADIO: notify_radio_group(src); break;
            }
          break;
          case EN_CHANGE:
            if (classifyHwnd(src) == WT_EDIT && src->m_atspi) swell_atk_edit_sync(src,true);
          break;
          case CBN_SELCHANGE: // note: LBN_SELCHANGE has the same value; distinguish by class
            {
              const int swt = classifyHwnd(src);
              if ((swt == WT_COMBO || swt == WT_LISTBOX) && src->m_atspi)
              {
                g_signal_emit_by_name((AtkObject *)src->m_atspi,"selection-changed");
                AtkObject *item = swell_atk_container_current_item(src);
                if (item && src == GetFocusIncludeMenus()) set_focus_obj(item);
                else if (item) atk_object_notify_state_change(item,ATK_STATE_SELECTED,TRUE);
              }
            }
          break;
        }
      }
      if (classifyHwnd(h) == WT_EDIT && h->m_atspi)
        swell_atk_edit_sync(h,true); // context-menu cut/paste arrive as WM_COMMAND on the edit
    break;
    case WM_SETTEXT:
      if (h->m_atspi)
      {
        switch (classifyHwnd(h))
        {
          case WT_EDIT: // name comes from the label; content changes are AtkText's job
            swell_atk_edit_sync(h,true);
          break;
          default:
            g_object_notify(G_OBJECT(h->m_atspi),"accessible-name");
          break;
        }
      }
    break;
    case WM_KEYDOWN:
    case WM_CHAR:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case EM_SETSEL:
    case EM_REPLACESEL:
      if (classifyHwnd(h) == WT_EDIT && h->m_atspi) swell_atk_edit_sync(h,true);
    break;
    case WM_MOUSEMOVE:
      if (classifyHwnd(h) == WT_EDIT && h->m_atspi && GetCapture() == h)
        swell_atk_edit_sync(h,true); // drag-selection
    break;
    case TBM_SETPOS:
    case TBM_SETRANGE:
    case PBM_SETPOS:
    case PBM_SETRANGE:
    case PBM_DELTAPOS:
      if (h->m_atspi) notify_value_changed(h,true);
    break;
    case WM_HSCROLL:
      if (l && classifyHwnd((HWND)l) == WT_TRACKBAR && ((HWND)l)->m_atspi)
        notify_value_changed((HWND)l,w == SB_ENDSCROLL); // drag stream is throttled
    break;
    case WM_NOTIFY:
      {
        NMHDR *nm = (NMHDR *)l;
        if (!nm || !nm->hwndFrom || !nm->hwndFrom->m_atspi) break;
        HWND src = nm->hwndFrom;
        switch (nm->code)
        {
          case LVN_ITEMCHANGED:
          case TVN_SELCHANGED:
          case TCN_SELCHANGE:
            {
              g_signal_emit_by_name((AtkObject *)src->m_atspi,"selection-changed");
              AtkObject *item = swell_atk_container_current_item(src);
              if (item && src == GetFocusIncludeMenus()) set_focus_obj(item);
              else if (item) atk_object_notify_state_change(item,ATK_STATE_SELECTED,TRUE);
            }
          break;
        }
      }
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
