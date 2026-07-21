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


    Internal hooks for the AT-SPI accessibility bridge (swell-atspi-generic.cpp).
    This header deliberately uses no ATK/GLib types so that including files
    need no extra include paths when SWELL_TARGET_ATSPI is not defined.

  */

#ifndef _SWELL_ATSPI_INTERNAL_H_
#define _SWELL_ATSPI_INTERNAL_H_

#ifdef SWELL_TARGET_ATSPI

extern bool swell_atspi_active;
void swell_atspi_init(void);
void swell_atspi_msg_pre(HWND h, UINT m, WPARAM w, LPARAM l);
void swell_atspi_msg_post(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT r);
void swell_atspi_show_window(HWND h, bool wasVisible);
void swell_atspi_enable_window(HWND h);
void swell_atspi_app_active(int active);
bool swell_atspi_on_key(void *gdkEventKey); // returns true if the event was consumed by an AT

// private-state accessors implemented in swell-wnd-generic.cpp
void swell_atspi_get_edit_state(HWND hwnd, int *caret, int *sel1, int *sel2);

#define SWELL_ATSPI_INIT() swell_atspi_init()
#define SWELL_ATSPI_MSG_PRE(h,m,w,l) do { if (swell_atspi_active) swell_atspi_msg_pre(h,m,w,l); } while(0)
#define SWELL_ATSPI_MSG_POST(h,m,w,l,r) do { if (swell_atspi_active) swell_atspi_msg_post(h,m,w,l,r); } while(0)
#define SWELL_ATSPI_SHOWWINDOW(h,wasvis) do { if (swell_atspi_active) swell_atspi_show_window(h,wasvis); } while(0)
#define SWELL_ATSPI_ENABLE(h) do { if (swell_atspi_active) swell_atspi_enable_window(h); } while(0)
#define SWELL_ATSPI_APP_ACTIVE(a) do { if (swell_atspi_active) swell_atspi_app_active(a); } while(0)
#define SWELL_ATSPI_ON_KEY(k) (swell_atspi_active && swell_atspi_on_key(k))

#else

#define SWELL_ATSPI_INIT() do { } while(0)
#define SWELL_ATSPI_MSG_PRE(h,m,w,l) do { } while(0)
#define SWELL_ATSPI_MSG_POST(h,m,w,l,r) do { } while(0)
#define SWELL_ATSPI_SHOWWINDOW(h,wasvis) do { } while(0)
#define SWELL_ATSPI_ENABLE(h) do { } while(0)
#define SWELL_ATSPI_APP_ACTIVE(a) do { } while(0)
#define SWELL_ATSPI_ON_KEY(k) (false)

#endif

#endif // _SWELL_ATSPI_INTERNAL_H_
