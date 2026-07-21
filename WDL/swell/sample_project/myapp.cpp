/*
    swell_myapp

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
*/

#ifdef _WIN32
#include <windows.h>
#include "../WDL/win32_utf8.h"
#endif

#include "../WDL/swell/swell.h"

#include "../WDL/wingui/wndsize.h"

#include "resource.h"

#if !defined(_WIN32) && !defined(__APPLE__)
bool g_quit;
#endif

HINSTANCE g_hInstance;
HWND g_hwnd;

WDL_DLGRET mainProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  static WDL_WndSizer resize;
  switch (uMsg)
  {
    case WM_INITDIALOG:
      g_hwnd=hwndDlg;
#ifdef _WIN32
      {
        HICON icon=LoadIcon(g_hInstance,MAKEINTRESOURCE(IDI_ICON1));
        SetClassLongPtr(hwndDlg,GCLP_HICON,(LPARAM)icon);
      }
#endif

      resize.init(hwndDlg);
      resize.init_item(IDCANCEL,0,1,0,1);

      {
        // populate the demo controls
        HWND combo = GetDlgItem(hwndDlg,IDC_COMBO1);
        SendMessage(combo,CB_ADDSTRING,0,(LPARAM)"Apple");
        SendMessage(combo,CB_ADDSTRING,0,(LPARAM)"Banana");
        SendMessage(combo,CB_ADDSTRING,0,(LPARAM)"Cherry");
        SendMessage(combo,CB_SETCURSEL,0,0);

        HWND list = GetDlgItem(hwndDlg,IDC_LIST1);
        SendMessage(list,LB_ADDSTRING,0,(LPARAM)"First item");
        SendMessage(list,LB_ADDSTRING,0,(LPARAM)"Second item");
        SendMessage(list,LB_ADDSTRING,0,(LPARAM)"Third item");
        SendMessage(list,LB_SETCURSEL,0,0);

        HWND slider = GetDlgItem(hwndDlg,IDC_SLIDER1);
        SendMessage(slider,TBM_SETRANGE,0,MAKELONG(0,100));
        SendMessage(slider,TBM_SETPOS,1,42);

        HWND progress = GetDlgItem(hwndDlg,IDC_PROGRESS1);
        SendMessage(progress,PBM_SETRANGE,0,MAKELONG(0,100));
        SendMessage(progress,PBM_SETPOS,66,0);

        HWND lv = GetDlgItem(hwndDlg,IDC_LISTVIEW1);
        LVCOLUMN col;
        memset(&col,0,sizeof(col));
        col.mask = LVCF_TEXT|LVCF_WIDTH;
        col.cx = 120;
        col.pszText = (char*)"File";
        ListView_InsertColumn(lv,0,&col);
        col.pszText = (char*)"Size";
        col.cx = 60;
        ListView_InsertColumn(lv,1,&col);
        for (int i = 0; i < 3; i ++)
        {
          char tmp[64];
          snprintf(tmp,sizeof(tmp),"file%d.wav",i+1);
          LVITEM item;
          memset(&item,0,sizeof(item));
          item.mask = LVIF_TEXT;
          item.iItem = i;
          item.pszText = tmp;
          ListView_InsertItem(lv,&item);
          snprintf(tmp,sizeof(tmp),"%d kb",(i+1)*100);
          ListView_SetItemText(lv,i,1,tmp);
        }

        HWND tree = GetDlgItem(hwndDlg,IDC_TREE1);
        TVINSERTSTRUCT tvis;
        memset(&tvis,0,sizeof(tvis));
        tvis.item.mask = TVIF_TEXT;
        tvis.item.pszText = (char*)"Fruits";
        HTREEITEM root1 = TreeView_InsertItem(tree,&tvis);
        tvis.hParent = root1;
        tvis.item.pszText = (char*)"Apple";
        TreeView_InsertItem(tree,&tvis);
        tvis.item.pszText = (char*)"Banana";
        TreeView_InsertItem(tree,&tvis);
        tvis.hParent = NULL;
        tvis.item.pszText = (char*)"Vegetables";
        TreeView_InsertItem(tree,&tvis);

        HWND tab = GetDlgItem(hwndDlg,IDC_TAB1);
        TCITEM tci;
        memset(&tci,0,sizeof(tci));
        tci.mask = TCIF_TEXT;
        tci.pszText = (char*)"General";
        TabCtrl_InsertItem(tab,0,&tci);
        tci.pszText = (char*)"Advanced";
        TabCtrl_InsertItem(tab,1,&tci);
      }
    return 1;
    case WM_CONTEXTMENU:
      {
        HMENU m = CreatePopupMenu();
        AddMenuItem(m,0,"First action",40001);
        AddMenuItem(m,1,"Second action",40002);
        AddMenuItem(m,2,"Third action",40003);
        POINT p;
        GetCursorPos(&p);
        TrackPopupMenu(m,0,p.x,p.y,0,hwndDlg,NULL);
        DestroyMenu(m);
      }
    return 1;
    case WM_CLOSE:
      DestroyWindow(hwndDlg);
    return 1;
    case WM_DESTROY:
      g_hwnd=NULL;
#ifdef __APPLE__
      SWELL_PostQuitMessage(0);
#elif defined(_WIN32)
      PostQuitMessage(0);
#else
      g_quit = true;
#endif
    break;
    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED)
        resize.onResize();
    break;
    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case ID_QUIT:
        case IDCANCEL:
          DestroyWindow(hwndDlg);
        break;
      }
    break;
  }
  return 0;
}

INT_PTR SWELLAppMain(int msg, INT_PTR parm1, INT_PTR parm2)
{
  switch (msg)
  {
    case SWELLAPP_ONLOAD:
      {
      }
    break;
    case SWELLAPP_LOADED:
      {
        HWND h=CreateDialog(NULL,MAKEINTRESOURCE(IDD_DIALOG1),NULL,mainProc);
        ShowWindow(h,SW_SHOW);
      }
    break;
    case SWELLAPP_DESTROY:
      if (g_hwnd) DestroyWindow(g_hwnd);
    break;
    case SWELLAPP_ONCOMMAND:
      // this is to catch commands coming from the system menu etc
      if (g_hwnd && parm1) SendMessage(g_hwnd,WM_COMMAND,parm1,0);
    break;

  }
  return 0;
}



#ifdef _WIN32

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
  g_hInstance = hInstance;

  SWELLAppMain(SWELLAPP_ONLOAD,0,0);
  SWELLAppMain(SWELLAPP_LOADED,0,0);

  for(;;)
  {
    MSG msg={0,};
    int vvv = GetMessage(&msg,NULL,0,0);
    if (!vvv) break;

    if (vvv<0)
    {
      Sleep(10);
      continue;
    }
    if (!msg.hwnd)
    {
      DispatchMessage(&msg);
      continue;
    }
    if (SWELLAppMain(SWELLAPP_PROCESSMESSAGE, (INT_PTR) &msg, 0)) continue;

    if (g_hwnd && IsDialogMessage(g_hwnd,&msg)) continue;

    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  SWELLAppMain(SWELLAPP_DESTROY,0,0);

  ExitProcess(0);
  
  return 0;
}

#else

/************** SWELL stuff ********** */

#ifdef __APPLE__
extern "C" {
#endif

const char **g_argv;
int g_argc;

#ifdef __APPLE__
};
#endif


#ifndef __APPLE__

int main(int argc, const char **argv)
{
  g_argc=argc;
  g_argv=argv;
  SWELL_initargs(&argc,(char***)&argv);
  SWELL_Internal_PostMessage_Init();
  SWELL_ExtendedAPI("APPNAME",(void*)"MyApp");
  SWELLAppMain(SWELLAPP_ONLOAD,0,0);
  SWELLAppMain(SWELLAPP_LOADED,0,0);
  while (!g_quit) {
    SWELL_RunMessageLoop();
    Sleep(10);
  }
  SWELLAppMain(SWELLAPP_DESTROY,0,0);
  return 0;
}

#endif


#include "../WDL/swell/swell-dlggen.h"
#include "res.rc_mac_dlg"
#undef BEGIN
#undef END
#include "../WDL/swell/swell-menugen.h"
#include "res.rc_mac_menu"

#endif
