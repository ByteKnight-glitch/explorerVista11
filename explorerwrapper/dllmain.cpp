#define INITGUID

#pragma warning(disable:4302)
#pragma warning(disable:4309)
#pragma warning(disable:4311)
#pragma warning(disable:4312)
#pragma warning(disable:4700) // this one in particular because it fires erroneously

#include "util.h"
#include "common.h"
#include "forwards.h"
#include "StartMenuResolver.h"
#include "TrayObject.h"
#include "dbgprint.h"
#include "ImmersiveShell.h"
#include "TrayNotify.h"
#include "AuthUI.h"
#include "StartMenuPin.h"
#include "ImmersiveFactory.h"
#include "ProjectionFactory.h"
#include "OSVersion.h"
#include "PinnedList.h"
#include "DestinationList.h"
#include "resource.h"
#include "ThemeManager.h"
#include "MinHook.h"
#include "ShellTaskScheduler.h"
#include "RegistryManager.h"
#include "NscTree.h"
#include "RegTreeOptions.h"
#include "shellapi.h"
#include "AutoPlay.h"
#include "StartMenuItemFilter.h"
#include "shell32_wrappers.h"
#include "ShellURL.h"
#include "OptionConfig.h"
#include "AddressImports.h"
#include "PatternImports.h"
#include "MinhookImports.h"
#include "TypeDefinitions.h"

#define WINX_HOTKEY_ID 0x5758
#define WM_SHOW_CUSTOM_WINX (WM_USER + 0x575)
#define WM_CUSTOM_SNAP (WM_USER + 0x576)
static HWND g_hwndTrayWinX = NULL;
static HHOOK g_hWinXKeyHook = NULL;
static HWINEVENTHOOK g_hSnapWinEventHook = NULL;
static HWND g_snapDragHwnd = NULL;
static RECT g_snapDragStartRect = { 0 };
static HWND g_lastSnappedHwnd = NULL;
static RECT g_lastSnappedRestoreRect = { 0 };
static int g_lastSnappedSide = 0; // 0 = none, 1 = left, 2 = right

static void AppendDiagLog(LPCWSTR line)
{
	WCHAR path[MAX_PATH];
	DWORD len = GetTempPathW(MAX_PATH, path);
	if (!len || len >= MAX_PATH - 32) return;
	lstrcatW(path, L"explorerVista_com.log");

	HANDLE hFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return;

	char utf8[512];
	int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8) - 1, NULL, NULL);
	if (n > 1)
	{
		DWORD written = 0;
		WriteFile(hFile, utf8, (DWORD)(n - 1), &written, NULL);
	}
	CloseHandle(hFile);
}

static void LogFailedComCall(REFCLSID rclsid, REFIID riid, HRESULT hr)
{
	static LONG s_logCount = 0;
	if (InterlockedIncrement(&s_logCount) > 300) return;

	WCHAR clsidStr[64] = { 0 };
	WCHAR iidStr[64] = { 0 };
	StringFromGUID2(rclsid, clsidStr, 64);
	StringFromGUID2(riid, iidStr, 64);

	WCHAR line[256];
	wsprintfW(line, L"[COM FAIL] CLSID=%s IID=%s HR=0x%08X\r\n", clsidStr, iidStr, (unsigned int)hr);
	AppendDiagLog(line);
}

static int ClampPad(LONG v)
{
	if (v < 0) return 0;
	if (v > 32) return 32;
	return (int)v;
}

static bool IsSnappableWindow(HWND hwnd, bool requireThickFrame)
{
	if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
		return false;

	if (GetAncestor(hwnd, GA_ROOT) != hwnd)
		return false;

	LONG style = GetWindowLongW(hwnd, GWL_STYLE);
	LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
	if ((style & WS_CHILD) || (exStyle & WS_EX_NOACTIVATE) || (exStyle & WS_EX_TOOLWINDOW))
		return false;

	if (requireThickFrame)
	{
		if ((style & WS_THICKFRAME) == 0)
			return false;
	}
	else
	{
		if ((style & (WS_THICKFRAME | WS_MAXIMIZEBOX)) == 0)
			return false;
	}

	WCHAR cls[64] = { 0 };
	if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)))
	{
		static const LPCWSTR s_excludedClasses[] = {
			L"Shell_TrayWnd",
			L"Shell_SecondaryTrayWnd",
			L"Progman",
			L"WorkerW",
			L"DV2ControlHost",
			L"TaskListThumbnailWnd",
			L"#32768"
		};
		for (size_t i = 0; i < ARRAYSIZE(s_excludedClasses); ++i)
		{
			if (lstrcmpW(cls, s_excludedClasses[i]) == 0)
				return false;
		}
	}

	return true;
}

static void ApplySnappedRect(HWND hwnd, const RECT& target)
{
	if (IsZoomed(hwnd) || IsIconic(hwnd))
		ShowWindow(hwnd, SW_RESTORE);

	RECT winRect = { 0 };
	RECT frameRect = { 0 };
	GetWindowRect(hwnd, &winRect);

	int padL = 0, padT = 0, padR = 0, padB = 0;
	if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameRect, sizeof(frameRect))))
	{
		padL = ClampPad(frameRect.left - winRect.left);
		padT = ClampPad(frameRect.top - winRect.top);
		padR = ClampPad(winRect.right - frameRect.right);
		padB = ClampPad(winRect.bottom - frameRect.bottom);
	}

	int x = target.left - padL;
	int y = target.top - padT;
	int w = (target.right - target.left) + padL + padR;
	int h = (target.bottom - target.top) + padT + padB;
	SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static bool RestoreSnappedWindow(HWND hwnd)
{
	if (hwnd != g_lastSnappedHwnd || g_lastSnappedSide == 0)
		return false;
	if (g_lastSnappedRestoreRect.right <= g_lastSnappedRestoreRect.left)
		return false;

	SetWindowPos(
		hwnd,
		NULL,
		g_lastSnappedRestoreRect.left,
		g_lastSnappedRestoreRect.top,
		g_lastSnappedRestoreRect.right - g_lastSnappedRestoreRect.left,
		g_lastSnappedRestoreRect.bottom - g_lastSnappedRestoreRect.top,
		SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
	);
	g_lastSnappedHwnd = NULL;
	g_lastSnappedSide = 0;
	return true;
}

static void SnapWindowToEdge(HWND hwnd, int side, HMONITOR hMonOverride, const RECT* pPreSnapRect)
{
	if (!IsSnappableWindow(hwnd, true))
		return;

	if (hwnd != g_lastSnappedHwnd || g_lastSnappedSide == 0)
	{
		if (pPreSnapRect)
			g_lastSnappedRestoreRect = *pPreSnapRect;
		else if (!IsZoomed(hwnd) && !IsIconic(hwnd))
			GetWindowRect(hwnd, &g_lastSnappedRestoreRect);
	}

	HMONITOR hMon = hMonOverride ? hMonOverride : MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = { sizeof(mi) };
	if (!GetMonitorInfoW(hMon, &mi))
		return;

	RECT wa = mi.rcWork;
	int midX = wa.left + (wa.right - wa.left) / 2;
	RECT target;
	if (side == 1)
		target = { wa.left, wa.top, midX, wa.bottom };
	else
		target = { midX, wa.top, wa.right, wa.bottom };

	ApplySnappedRect(hwnd, target);
	g_lastSnappedHwnd = hwnd;
	g_lastSnappedSide = side;
}

static void HandleSnapHotkey(UINT vkCode)
{
	HWND hwnd = GetForegroundWindow();
	if (!IsSnappableWindow(hwnd, false))
		return;

	switch (vkCode)
	{
	case VK_UP:
		if (IsIconic(hwnd))
		{
			ShowWindow(hwnd, SW_RESTORE);
		}
		else if (GetWindowLongW(hwnd, GWL_STYLE) & WS_MAXIMIZEBOX)
		{
			ShowWindow(hwnd, SW_MAXIMIZE);
			if (hwnd == g_lastSnappedHwnd)
				g_lastSnappedSide = 0;
		}
		break;

	case VK_DOWN:
		if (IsZoomed(hwnd))
		{
			ShowWindow(hwnd, SW_RESTORE);
		}
		else if (!RestoreSnappedWindow(hwnd) && (GetWindowLongW(hwnd, GWL_STYLE) & WS_MINIMIZEBOX))
		{
			ShowWindow(hwnd, SW_MINIMIZE);
		}
		break;

	case VK_LEFT:
		if (g_lastSnappedSide == 2 && RestoreSnappedWindow(hwnd))
			break;
		SnapWindowToEdge(hwnd, 1, NULL, NULL);
		break;

	case VK_RIGHT:
		if (g_lastSnappedSide == 1 && RestoreSnappedWindow(hwnd))
			break;
		SnapWindowToEdge(hwnd, 2, NULL, NULL);
		break;
	}
}

static VOID CALLBACK SnapWinEventProc(
	HWINEVENTHOOK hWinEventHook,
	DWORD event,
	HWND hwnd,
	LONG idObject,
	LONG idChild,
	DWORD dwEventThread,
	DWORD dwmsEventTime)
{
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd)
		return;

	if (event == EVENT_SYSTEM_MOVESIZESTART)
	{
		if (IsSnappableWindow(hwnd, true) && !IsZoomed(hwnd) && !IsIconic(hwnd))
		{
			g_snapDragHwnd = hwnd;
			GetWindowRect(hwnd, &g_snapDragStartRect);
		}
		else
		{
			g_snapDragHwnd = NULL;
		}
		return;
	}

	if (event == EVENT_SYSTEM_MOVESIZEEND)
	{
		if (hwnd != g_snapDragHwnd)
			return;
		g_snapDragHwnd = NULL;

		if (!IsSnappableWindow(hwnd, true) || IsZoomed(hwnd) || IsIconic(hwnd))
			return;

		RECT endRect = { 0 };
		GetWindowRect(hwnd, &endRect);

		int startW = g_snapDragStartRect.right - g_snapDragStartRect.left;
		int startH = g_snapDragStartRect.bottom - g_snapDragStartRect.top;
		int endW = endRect.right - endRect.left;
		int endH = endRect.bottom - endRect.top;
		int dw = (startW > endW) ? (startW - endW) : (endW - startW);
		int dh = (startH > endH) ? (startH - endH) : (endH - startH);

		if ((endRect.left == g_snapDragStartRect.left && endRect.top == g_snapDragStartRect.top) || dw > 4 || dh > 4)
			return;

		POINT pt = { 0 };
		GetCursorPos(&pt);
		HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi = { sizeof(mi) };
		if (!GetMonitorInfoW(hMon, &mi))
			return;

		const int edge = 8;
		if (pt.x <= mi.rcMonitor.left + edge)
		{
			SnapWindowToEdge(hwnd, 1, hMon, &g_snapDragStartRect);
		}
		else if (pt.x >= mi.rcMonitor.right - edge - 1)
		{
			SnapWindowToEdge(hwnd, 2, hMon, &g_snapDragStartRect);
		}
		else if (pt.y <= mi.rcMonitor.top + edge)
		{
			if (GetWindowLongW(hwnd, GWL_STYLE) & WS_MAXIMIZEBOX)
			{
				ShowWindow(hwnd, SW_MAXIMIZE);
				if (hwnd == g_lastSnappedHwnd)
					g_lastSnappedSide = 0;
			}
		}
		else if (hwnd == g_lastSnappedHwnd)
		{
			g_lastSnappedSide = 0;
		}
	}
}

LRESULT CALLBACK WinXKeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
	{
		KBDLLHOOKSTRUCT* pKbd = (KBDLLHOOKSTRUCT*)lParam;
		bool winHeld = ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000));
		if (pKbd && winHeld)
		{
			if (pKbd->vkCode == 'X')
			{
				if (g_hwndTrayWinX && IsWindow(g_hwndTrayWinX))
				{
					PostMessageW(g_hwndTrayWinX, WM_SHOW_CUSTOM_WINX, 0, 0);
					return 1;
				}
			}
			else if ((pKbd->vkCode == VK_LEFT || pKbd->vkCode == VK_RIGHT || pKbd->vkCode == VK_UP || pKbd->vkCode == VK_DOWN) &&
				!(GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
				!(GetAsyncKeyState(VK_MENU) & 0x8000) &&
				!(GetAsyncKeyState(VK_SHIFT) & 0x8000))
			{
				if (g_hwndTrayWinX && IsWindow(g_hwndTrayWinX))
				{
					PostMessageW(g_hwndTrayWinX, WM_CUSTOM_SNAP, (WPARAM)pKbd->vkCode, 0);
					return 1;
				}
			}
		}
	}
	return CallNextHookEx(g_hWinXKeyHook, nCode, wParam, lParam);
}

void ShowCustomWinXMenu(HWND hwndTray)
{
	HMENU hMenu = CreatePopupMenu();
	if (!hMenu) return;

	AppendMenuW(hMenu, MF_STRING, 101, L"Programs and Features");
	AppendMenuW(hMenu, MF_STRING, 102, L"Power Options");
	AppendMenuW(hMenu, MF_STRING, 103, L"Event Viewer");
	AppendMenuW(hMenu, MF_STRING, 104, L"System");
	AppendMenuW(hMenu, MF_STRING, 105, L"Device Manager");
	AppendMenuW(hMenu, MF_STRING, 106, L"Network Connections");
	AppendMenuW(hMenu, MF_STRING, 107, L"Disk Management");
	AppendMenuW(hMenu, MF_STRING, 108, L"Computer Management");
	AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(hMenu, MF_STRING, 109, L"Windows PowerShell");
	AppendMenuW(hMenu, MF_STRING, 110, L"Command Prompt");
	AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(hMenu, MF_STRING, 111, L"Task Manager");
	AppendMenuW(hMenu, MF_STRING, 112, L"Settings");
	AppendMenuW(hMenu, MF_STRING, 113, L"File Explorer");
	AppendMenuW(hMenu, MF_STRING, 114, L"Run");
	AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

	HMENU hPowerMenu = CreatePopupMenu();
	if (hPowerMenu)
	{
		AppendMenuW(hPowerMenu, MF_STRING, 201, L"Sign out");
		AppendMenuW(hPowerMenu, MF_STRING, 202, L"Restart");
		AppendMenuW(hPowerMenu, MF_STRING, 203, L"Shut down");
		AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hPowerMenu, L"Shut down or sign out");
	}

	RECT rc = { 0 };
	GetWindowRect(hwndTray, &rc);

	SetForegroundWindow(hwndTray);
	UINT cmd = TrackPopupMenuEx(
		hMenu,
		TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON,
		rc.left + 4,
		rc.top,
		hwndTray,
		NULL
	);
	PostMessageW(hwndTray, WM_NULL, 0, 0);

	DestroyMenu(hMenu);

	switch (cmd)
	{
	case 101: ShellExecuteW(NULL, L"open", L"appwiz.cpl", NULL, NULL, SW_SHOWNORMAL); break;
	case 102: ShellExecuteW(NULL, L"open", L"powercfg.cpl", NULL, NULL, SW_SHOWNORMAL); break;
	case 103: ShellExecuteW(NULL, L"open", L"eventvwr.msc", NULL, NULL, SW_SHOWNORMAL); break;
	case 104: ShellExecuteW(NULL, L"open", L"control.exe", L"/name Microsoft.System", NULL, SW_SHOWNORMAL); break;
	case 105: ShellExecuteW(NULL, L"open", L"devmgmt.msc", NULL, NULL, SW_SHOWNORMAL); break;
	case 106: ShellExecuteW(NULL, L"open", L"ncpa.cpl", NULL, NULL, SW_SHOWNORMAL); break;
	case 107: ShellExecuteW(NULL, L"open", L"diskmgmt.msc", NULL, NULL, SW_SHOWNORMAL); break;
	case 108: ShellExecuteW(NULL, L"open", L"compmgmt.msc", NULL, NULL, SW_SHOWNORMAL); break;
	case 109: ShellExecuteW(NULL, L"open", L"powershell.exe", NULL, NULL, SW_SHOWNORMAL); break;
	case 110: ShellExecuteW(NULL, L"open", L"cmd.exe", NULL, NULL, SW_SHOWNORMAL); break;
	case 111: ShellExecuteW(NULL, L"open", L"taskmgr.exe", NULL, NULL, SW_SHOWNORMAL); break;
	case 112: ShellExecuteW(NULL, L"open", L"ms-settings:", NULL, NULL, SW_SHOWNORMAL); break;
	case 113: ShellExecuteW(NULL, L"open", L"C:\\Windows\\explorer.exe", L"/separate", NULL, SW_SHOWNORMAL); break;
	case 114: ShellExecuteW(NULL, L"open", L"rundll32.exe", L"shell32.dll,#61", NULL, SW_SHOWNORMAL); break;
	case 201: ShellExecuteW(NULL, L"open", L"shutdown.exe", L"/l", NULL, SW_HIDE); break;
	case 202: ShellExecuteW(NULL, L"open", L"shutdown.exe", L"/r /t 0", NULL, SW_HIDE); break;
	case 203: ShellExecuteW(NULL, L"open", L"shutdown.exe", L"/s /t 0", NULL, SW_HIDE); break;
	}
}

LRESULT CALLBACK NewTrayProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (!g_dwTrayThreadId)
	{
		g_dwTrayThreadId = GetCurrentThreadId();
		dbgprintf(L"set g_dwTrayThreadId from NewTrayProc to %u", g_dwTrayThreadId);
	}
	if (!g_hWinXKeyHook && hwnd == GetTaskbarWnd())
	{
		g_hwndTrayWinX = hwnd;
		g_hWinXKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, WinXKeyboardHookProc, g_hInstance, 0);
		if (!g_hSnapWinEventHook)
		{
			g_hSnapWinEventHook = SetWinEventHook(
				EVENT_SYSTEM_MOVESIZESTART,
				EVENT_SYSTEM_MOVESIZEEND,
				NULL,
				SnapWinEventProc,
				0,
				0,
				WINEVENT_OUTOFCONTEXT
			);
		}
		BOOL hkOk = RegisterHotKey(hwnd, WINX_HOTKEY_ID, MOD_WIN, 'X');
		WCHAR msg[128];
		wsprintfW(msg, L"[WINX] TrayThread=%u Hook=%p RegisterHotKey=%d\r\n", GetCurrentThreadId(), g_hWinXKeyHook, hkOk);
		AppendDiagLog(msg);
	}
	if ((uMsg == WM_HOTKEY && wParam == WINX_HOTKEY_ID) || uMsg == WM_SHOW_CUSTOM_WINX)
	{
		ShowCustomWinXMenu(hwnd);
		return 0;
	}
	if (uMsg == WM_CUSTOM_SNAP)
	{
		HandleSnapHotkey((UINT)wParam);
		return 0;
	}
	if (uMsg == WM_DESTROY)
	{
		UnregisterHotKey(hwnd, WINX_HOTKEY_ID);
		if (g_hWinXKeyHook)
		{
			UnhookWindowsHookEx(g_hWinXKeyHook);
			g_hWinXKeyHook = NULL;
		}
		if (g_hSnapWinEventHook)
		{
			UnhookWinEvent(g_hSnapWinEventHook);
			g_hSnapWinEventHook = NULL;
		}
	}
	if (uMsg == 0x56D) return 0;
	if (uMsg == ThemeChangeMessage) //reinit thememanager on themechanged, so that inactive msstyles is updated
	{
		for (int i = 0; i < themeHandles->size; ++i)
		{
			CloseThemeData(themeHandles->data[i]);
		}
		realloc(themeHandles->data, 0);
		themeHandles->size = 0;

		ThemeManagerInitialize();
		EnumWindows(RefreshWindows, (LPARAM)hwnd);

		uMsg = WM_THEMECHANGED;
		return CallWindowProc(g_prevTrayProc, hwnd, uMsg, wParam, lParam);
	}

	if (uMsg == WM_DISPLAYCHANGE || uMsg == WM_WINDOWPOSCHANGED)
	{
		RemoveProp(hwnd, L"TaskbarMonitor");
		SetProp(hwnd, L"TaskbarMonitor", (HANDLE)MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY));
		//send displaychanged to desktop
		if (uMsg == WM_DISPLAYCHANGE) PostMessage(hwnd_desktop, 0x44B, 0, 0);
	}

	if (uMsg == 0x574) //handledelayboot
	{
		if (lParam == 3)
			return CallWindowProc(g_prevTrayProc, hwnd, 0x5B5, wParam, lParam); //fire ShellDesktopSwitch event
		if (lParam == 1)
			SetEvent(hEvent_DesktopVisible);
		return 0;
	}

	if (uMsg == WM_THEMECHANGED)
	{
		EnsureWindowColorization(); // Ittr: Correct colorization enablement setting for Win10/11
	}

	if (uMsg == WM_SETTINGCHANGE || uMsg == WM_ERASEBKGND || uMsg == WM_WININICHANGE) // Ittr: Fix taskbar colorization for non-legacy
	{
		if ((IsThemeActive() && !s_ClassicTheme && IsCompositionActive() && !s_DisableComposition) && hwnd == GetTaskbarWnd() && s_ColorizationOptions != 0) // Ittr: Only taskbar needs updating now, start menu and new thumbnail algo correct for themselves
		{
			SetWindowCompositionAttribute(hwnd, &GetTrayAccentProperties(false));
		}
	}

	return CallWindowProc(g_prevTrayProc, hwnd, uMsg, wParam, lParam);
}

// Ittr: Subclass the thumbnail so we can update its colorization as needed
LRESULT CALLBACK NewThumbnailProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_SETTINGCHANGE || uMsg == WM_ERASEBKGND || uMsg == WM_WININICHANGE) // Ittr: Fix thumbnail colorization for non-legacy
	{
		if ((IsThemeActive() && !s_ClassicTheme && IsCompositionActive() && !s_DisableComposition) && (g_osVersion.BuildNumber() >= 10074 && hwnd == GetThumbnailWnd()) && s_ColorizationOptions != 0) // Ittr: Only taskbar needs updating now, start menu and new thumbnail algo correct for themselves
		{
			SetWindowCompositionAttribute(hwnd, &GetTrayAccentProperties(true));
		}
	}

	return CallWindowProc(g_prevThumbnailProc, hwnd, uMsg, wParam, lParam);
}

void ShimDesktop()
{
	static int InitOnce = FALSE;
	if (InitOnce) return;
	hwnd_desktop = FindWindow(L"Progman", L"Program Manager");
	HWND hwndTray = GetTaskbarWnd();
	HWND hwndThumbnail = GetThumbnailWnd(); // thumbnail hwnd not being present should not stop the shim
	if (!hwnd_desktop || !hwndTray) return;
	InitOnce = TRUE;
	if (!g_dwTrayThreadId)
	{
		g_dwTrayThreadId = GetWindowThreadProcessId(hwndTray, NULL);
		dbgprintf(L"set g_dwTrayThreadId from Shell_TrayWnd to %u", g_dwTrayThreadId);
	}
	//hook tray
	g_prevTrayProc = (WNDPROC)GetWindowLongPtr(hwndTray, GWLP_WNDPROC);
	g_prevThumbnailProc = (WNDPROC)GetWindowLongPtr(hwndThumbnail, GWLP_WNDPROC);
	SetWindowLongPtr(hwndTray, GWLP_WNDPROC, (LONG_PTR)NewTrayProc);
	SetWindowLongPtr(hwndThumbnail, GWLP_WNDPROC, (LONG_PTR)NewThumbnailProc);
	//set monitor (doh!)
	SetProp(hwndTray, L"TaskbarMonitor", (HANDLE)MonitorFromWindow(hwndTray, MONITOR_DEFAULTTOPRIMARY));
	//init desktop	
	PostMessage(hwnd_desktop, 0x45C, 1, 1); //wallpaper
	PostMessage(hwnd_desktop, 0x45E, 0, 2); //wallpaper host
	PostMessage(hwnd_desktop, 0x45C, 2, 3); //wallpaper & icons
	PostMessage(hwnd_desktop, 0x45B, 0, 0); //final init
	PostMessage(hwnd_desktop, 0x40B, 0, 0); //pins
}

PVOID WINAPI SHCreateDesktopNEW(PVOID p1)
{
	PVOID ret = SHCreateDesktopOrig(p1);
	ShimDesktop();
	return ret;
}

PVOID WINAPI SHDesktopMessageLoopNEW(PVOID p1)
{
	PVOID ret = SHDesktopMessageLoop(p1);
	SHPtrParamAPI SHCloseDesktopHandle;
	SHCloseDesktopHandle = (SHPtrParamAPI)GetProcAddress(GetModuleHandle(L"shell32.dll"),(LPSTR)206);
	SHCloseDesktopHandle(p1);
	return ret;
}

DWORD WINAPI CTray__SyncThreadProc_hook(LPVOID lpParameter)
{
	if (!g_dwTrayThreadId)
	{
		g_dwTrayThreadId = GetCurrentThreadId();
		dbgprintf(L"set g_dwTrayThreadId to %u", g_dwTrayThreadId);
	}

	return CTray__SyncThreadProc_orig(lpParameter);
}

void HookTrayThread(void)
{
	CTray__SyncThreadProc_orig = (LPTHREAD_START_ROUTINE)FindPattern(
		(uintptr_t)GetModuleHandle(NULL),
		"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 81 EC 00 03 00 00 48 8B"
	);

	// Ensure File Explorer behaves correctly where certain explorer versions are concerned
	if (!CTray__SyncThreadProc_orig)
	{
		CTray__SyncThreadProc_orig = (LPTHREAD_START_ROUTINE)FindPattern(
			(uintptr_t)GetModuleHandle(NULL),
			"48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC 00 03 00 00"
		);
	}

	if (CTray__SyncThreadProc_orig)
	{
		MH_CreateHook(
			(void*)CTray__SyncThreadProc_orig,
			(void*)CTray__SyncThreadProc_hook,
			(void**)&CTray__SyncThreadProc_orig
		);
	}
}

void GetOrbDPIAndPos(LPWSTR fName)
{
	APPBARDATA abd;
	abd.cbSize = sizeof(APPBARDATA);
	SHAppBarMessage(ABM_GETTASKBARPOS, &abd);

	HDC screen = GetDC(NULL);
	double hPixelsPerInch = GetDeviceCaps(screen, LOGPIXELSX);
	double vPixelsPerInch = GetDeviceCaps(screen, LOGPIXELSY);
	ReleaseDC(NULL, screen);
	double dpi = (hPixelsPerInch + vPixelsPerInch) * 0.5;

	if (dpi >= 120)
	{
		if (dpi >= 144)
		{
			if (dpi >= 192)
			{
				if (abd.uEdge == ABE_LEFT || abd.uEdge == ABE_RIGHT) StringCchCopyW(fName, MAX_PATH, L"6808");
				else if (abd.uEdge == ABE_TOP) StringCchCopyW(fName, MAX_PATH, L"6812");
				else StringCchCopyW(fName, MAX_PATH, L"6804");
			}
			else
			{
				if (abd.uEdge == ABE_LEFT || abd.uEdge == ABE_RIGHT) StringCchCopyW(fName, MAX_PATH, L"6807");
				else if (abd.uEdge == ABE_TOP) StringCchCopyW(fName, MAX_PATH, L"6811");
				else StringCchCopyW(fName, MAX_PATH, L"6803");
			}
		}
		else
		{
			if (abd.uEdge == ABE_LEFT || abd.uEdge == ABE_RIGHT) StringCchCopyW(fName, MAX_PATH, L"6806");
			else if (abd.uEdge == ABE_TOP) StringCchCopyW(fName, MAX_PATH, L"6810");
			else StringCchCopyW(fName, MAX_PATH, L"6802");
		}
	}
	else
	{
		if (abd.uEdge == ABE_LEFT || abd.uEdge == ABE_RIGHT) StringCchCopyW(fName, MAX_PATH, L"6805");
		else if (abd.uEdge == ABE_TOP) StringCchCopyW(fName, MAX_PATH, L"6809");
		else StringCchCopyW(fName, MAX_PATH, L"6801");
	}
}

HANDLE __stdcall LoadImageW_CallHook(HINSTANCE hInst, LPCWSTR name, UINT type, int cx, int cy, UINT fuLoad)
{
	dbgprintf(L"LoadImageW_CallHook has been called!");

	WCHAR szExeDir[MAX_PATH];
	GetModuleFileNameW(NULL, szExeDir, MAX_PATH);
	WCHAR* backslash = StrRChrW(szExeDir, NULL, L'\\');
	if (*backslash == L'\\')
		*backslash = L'\0';

	WCHAR szOrbDir[MAX_PATH];
	LSTATUS res = g_registry.QueryValue(L"OrbDirectory", (LPBYTE)szOrbDir, sizeof(szOrbDir));

	if (!*szOrbDir || ERROR_SUCCESS != res)
		return LoadImageW(hInst, name, type, cx, cy, fuLoad);

	WCHAR szOrbFile[MAX_PATH];
	GetOrbDPIAndPos(szOrbFile);

	WCHAR szOrbPath[MAX_PATH * 3];
	wsprintfW(
		szOrbPath,
		L"%s\\orbs\\%s\\%s.bmp",
		szExeDir,
		szOrbDir,
		szOrbFile
	);

	if (FileExists(szOrbPath) == FALSE)
		return LoadImageW(hInst, name, type, cx, cy, fuLoad);
	else
		return LoadImageW(NULL, szOrbPath, IMAGE_BITMAP, 0, 0, fuLoad | LR_LOADFROMFILE);
}

void HookLoadImageForSizeAndFont()
{
	auto callLoadImage = (uintptr_t)FindPattern((uintptr_t)GetModuleHandle(0), "FF 15 ?? ?? ?? ?? 48 89 43 ?? 48 85 C0 74 ?? 4C 8D ?? ?? ?? BA ?? ?? ?? ?? 48 8B C8 FF 15");
	if (callLoadImage)
	{
		//write a nop
		DWORD old;
		VirtualProtect((void*)callLoadImage, 1, PAGE_EXECUTE_READWRITE, &old);
		*reinterpret_cast<char*>(callLoadImage) = 0x90;
		VirtualProtect((void*)callLoadImage, 1, old, 0);

		callLoadImage += 1;

		// write a call to our function
		DetourCall((void*)callLoadImage, LoadImageW_CallHook);
	}

	char* callDrawExtended = (char*)FindPattern((uintptr_t)GetModuleHandle(0), "48 89 5C 24 08 57 48 83 EC 30 33 DB 48 8B F9 48 39 59 40");
	if (!callDrawExtended) return;

	if (callDrawExtended)
	{
		unsigned char bytes[] = { 0xB0,0x01,0xC3 };
		ChangeImportedPattern(callDrawExtended, bytes, sizeof(bytes));
	}
}

void ModifyDesktopHwnd()
{
	uintptr_t desktopHwnd = FindPattern((uintptr_t)GetModuleHandle(0), "74 ?? 48 3B 3D ?? ?? ?? ?? 8D 43 01 0F 45 D8");
	if (desktopHwnd)
	{
		desktopHwnd += 2;
		v_hwndDesktop = (HWND*)(desktopHwnd + 7 + *reinterpret_cast<signed int*>(desktopHwnd + 3));
	}
}

void HookShell32();
void HookAPIs() // largely a legacy function now
{
	// 24H2+ - W32PTP
	if (g_osVersion.BuildNumber() >= 26100)
	{
		HMODULE twinui_pcshell = LoadLibrary(L"twinui.pcshell.dll");

		if (twinui_pcshell)
		{
			CTaskbandPin_CreateInstance = (CTaskbandPin_CreateInstance_t)FindPattern((uintptr_t)twinui_pcshell, "40 53 48 83 EC 20 48 8B D9 48 8D 15 ?? ?? ?? ?? B9 80 00 00 00 E8 ?? ?? ?? ?? 48 85 C0");
		}
	}

	// Change and fix core desktop components
	hEvent_DesktopVisible = CreateEvent(NULL, TRUE, FALSE, L"ShellDesktopVisibleEvent");
	SHCreateDesktopOrig = (SHCreateDesktopAPI)GetProcAddress(GetModuleHandle(L"shell32.dll"), (LPSTR)200);
	ChangeImportedAddress(GetModuleHandle(NULL), "shell32.dll", SHCreateDesktopOrig, SHCreateDesktopNEW);
	SHDesktopMessageLoop = (SHCreateDesktopAPI)GetProcAddress(GetModuleHandle(L"shell32.dll"), (LPSTR)201);
	ChangeImportedAddress(GetModuleHandle(NULL), "shell32.dll", SHDesktopMessageLoop, SHDesktopMessageLoopNEW);

	// ???
	ModifyDesktopHwnd();

	// We run the Minhook patches here
	ChangeMinhookImports();

	// Prevent theme overrides applying to file explorer *VERY IMPORTANT*
	HookTrayThread();

	// 1. shell32.dll - hack created startmenupin instance
	// 2. shell32.dll - patch delayload stuff
	StartMenuPin_PatchShell32();
	HookShell32();

	// Handle custom start orb feature
	HookLoadImageForSizeAndFont();

	// Enable MinHook hooks at the end
	MH_EnableHook(MH_ALL_HOOKS);
}

BOOL WINAPI GetUserObjectInformationNew(HANDLE hObj, int nIndex, PVOID pvInfo, DWORD nLength, LPDWORD lpnLengthNeeded)
{
	lstrcpy(LPWSTR(pvInfo), L"Winlogon");
	return TRUE;
}

BOOL WINAPI GetWindowBandNew(HWND hwnd, DWORD* out)
{
	BOOL ret = GetWindowBandOrig(hwnd, out);
	DWORD origband = (DWORD)GetProp(GetAncestor(hwnd, GA_ROOTOWNER), L"explorer7.WindowBand");
	//dbgprintf(L"GetWindowBand %p %p %p",hwnd,*out,origband);
	if (origband && out) *out = origband;
	return ret;
}

UINT_PTR WINAPI SetTimer_WUI(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc)
{
	if (nIDEvent == 0x2252CE37)
		ShowWindow(hWnd, SW_HIDE);
	return SetTimer(hWnd, nIDEvent, uElapse, lpTimerFunc);
}

// Used even when immersive UI is not active in some cases..?
void HookImmersive()
{
	HMODULE immersiveui = LoadLibrary(L"Windows.UI.Immersive.dll");
	HMODULE hUser32 = GetModuleHandle(L"user32.dll");
	CreateWindowInBandOrig = (CreateWindowInBandAPI)GetProcAddress(hUser32, "CreateWindowInBand");

	if (s_EnableImmersiveShellStack == 1)
		CreateWindowInBandExOrig = (CreateWindowInBandExAPI)GetProcAddress(hUser32, "CreateWindowInBand");

	GetWindowBandOrig = (GetWindowBandAPI)GetProcAddress(hUser32, "GetWindowBand");
	ChangeImportedAddress(immersiveui, "user32.dll", CreateWindowInBandOrig, CreateWindowInBandNew);
	ChangeImportedAddress(immersiveui, "user32.dll", GetWindowBandOrig, GetWindowBandNew);
	ChangeImportedAddress(immersiveui, "user32.dll", GetUserObjectInformation, GetUserObjectInformationNew);
	ChangeImportedAddress(immersiveui, "user32.dll", SetTimer, SetTimer_WUI);

	if (!s_EnableImmersiveShellStack || g_osVersion.BuildNumber() < 10074) // Ittr: If user *either* has UWP disabled, or they are NOT on Windows 10, run legacy window band code
	{
		//bugbug!!!
		ChangeImportedAddress(GetModuleHandle(L"twinui.dll"), "user32.dll", CreateWindowInBandOrig, CreateWindowInBandNew);
		ChangeImportedAddress(GetModuleHandle(L"authui.dll"), "user32.dll", CreateWindowInBandOrig, CreateWindowInBandNew);
		ChangeImportedAddress(GetModuleHandle(L"shell32.dll"), "user32.dll", CreateWindowInBandOrig, CreateWindowInBandNew);

		ChangeImportedAddress(GetModuleHandle(L"twinapi.dll"), "user32.dll", CreateWindowInBandOrig, CreateWindowInBandNew);
		ChangeImportedAddress(GetModuleHandle(L"Windows.UI.dll"), "user32.dll", CreateWindowInBandOrig, CreateWindowInBandNew);
	}
}

// Basically this allows explorer to actually work on builds >9200
void PatchShunimpl()
{
	uintptr_t shunImpl = (uintptr_t)GetModuleHandle(L"shunimpl.dll");
	if (!shunImpl) return;
	char* dllmainSHUNIMPL = (char*)FindPattern(shunImpl, "48 83 EC 28 83 FA 01");

	if (dllmainSHUNIMPL)
	{
		unsigned char bytes[] = { 0xB0,0x01,0xC3 };
		ChangeImportedPattern(dllmainSHUNIMPL, bytes, sizeof(bytes));
	}
}

// Where we need to close explorer silently (such as to block people from using awful, horrendous software...)
void ExitExplorerSilently()
{
	// we do these blocks of code like this, so that the 0xc0000142 error doesn't appear
	LPDWORD exitCode;
	GetExitCodeProcess(L"explorer.exe", exitCode); // compiler warning is wrong here - the variable is supplied the exit code by this function
	ExitProcess((UINT)exitCode); // exit explorer
}

// Initialize the inactive theme engine
void ThemeHandlesInit()
{
	themeHandles = new wiktorArray<HTHEME>();
	themeHandles->data = 0;
	themeHandles->size = 0;
}

// Terminate inactive theme engine when needed
void EndThemeHandles()
{
	realloc(themeHandles->data, 0);
	themeHandles->size = 0;
	delete themeHandles;
}

// WINDOWS 11
void InitPinnedListHack()
{
	// == CPINNEDLIST HACK ==

	HMODULE twinui_pcshell = LoadLibrary(L"twinui.pcshell.dll");

	// CTaskbandPin_CreateInstance
	if (twinui_pcshell)
	{
		// Method 1: Direct function preamble (dangerous; breaks if they modify the fields of CTaskbandPin or its superclass(es))
		// 40 53 48 83 EC 20 48 8B D9 48 8D 15 ?? ?? ?? ?? B9 80 00 00 00 E8 ?? ?? ?? ?? 48 85 C0
		/*matchCTaskbandPinCreateInstance = (PBYTE)FindPattern(
			pFile,
			dwSize,
			"\x40\x53\x48\x83\xEC\x20\x48\x8B\xD9\x48\x8D\x15\x00\x00\x00\x00\xB9\x80\x00\x00\x00\xE8\x00\x00\x00\x00\x48\x85\xC0",
			"xxxxxxxxxxxx????xxxxxx????xxx",
			&numMatchesCTaskbandPinCreateInstance
		);*/

		// Method 2: winrt::Windows::Internal::Shell::implementation::PinManager::IsItemPinned
		// 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 48 83 64 24 ?? ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 48 8B 8D ?? ?? ?? ?? 85 C0
		//                                                                   ^^^^^^^^^^^
		PBYTE matchCTaskbandPinCreateInstance = (PBYTE)FindPattern((uintptr_t)twinui_pcshell, "48 8D 4C 24 ?? E8 ?? ?? ?? ?? 48 83 64 24 ?? ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 48 8B 8D ?? ?? ?? ?? 85 C0");

		if (matchCTaskbandPinCreateInstance)
		{
			matchCTaskbandPinCreateInstance += 21;
			matchCTaskbandPinCreateInstance += 5 + *(int*)(matchCTaskbandPinCreateInstance + 1);
		}

		if (!matchCTaskbandPinCreateInstance)
		{
			// wil::out_param() destructor inlined
			// 0F 1F 44 00 00 48 83 64 24 ?? ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 48 8B 8D ?? ?? ?? ?? 85 C0
			//                                                    ^^^^^^^^^^^
			matchCTaskbandPinCreateInstance = (PBYTE)FindPattern((uintptr_t)twinui_pcshell, "0F 1F 44 00 00 48 83 64 24 ?? ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 48 8B 8D ?? ?? ?? ?? 85 C0");

			if (matchCTaskbandPinCreateInstance)
			{
				matchCTaskbandPinCreateInstance += 16;
				matchCTaskbandPinCreateInstance += 5 + *(int*)(matchCTaskbandPinCreateInstance + 1);
			}
		}

		if (matchCTaskbandPinCreateInstance)
		{
			CTaskbandPin_CreateInstance = (CTaskbandPin_CreateInstance_t)matchCTaskbandPinCreateInstance;
		}
	}
}

BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved)
{
	// Ittr: We initialise values for closing program if incompatible software is present
	WCHAR programPath[MAX_PATH] = L"\\Stardock\\WindowBlinds 11\\unins000.exe";
	WCHAR blacklistPath[MAX_PATH];
	ExpandEnvironmentStringsW(L"%ProgramFiles%", (LPWSTR)blacklistPath, sizeof(blacklistPath));
	lstrcat(blacklistPath, programPath);

	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		PatchShunimpl();

		if (GetFileAttributesW((LPCWSTR)blacklistPath) != INVALID_FILE_ATTRIBUTES) // Windowblinds blockage part 1 - create user-facing error
			CrashError(); // The user-facing crash message - we do these blocks of code like this, so that the 0xc0000142 error doesn't appear

		/*if (g_osVersion.BuildNumber() >= 26100)
		{
			InitPinnedListHack();
		}*/

		CreateShellFolder(); // Fix shell folder for 1607+...
		EnsureWindowColorization(); // Correct colorization enablement setting for Win10/11
		FirstRunCompatibilityWarning(); // Warn users on Windows 11 24H2+ and Server 2022 of potential problems
		FirstRunPrereleaseWarning(); // Warn users if this is a pre-release build that this is the case on first run ONLY
		ThemeHandlesInit(); // Basically start the inactive theme management process

		dbgprintf(L"Dll Attach\n");

		// Ittr: Load user configuration from the registry, important that we do this first before applying API hooks
		InitializeConfiguration();

		// Ittr: Handle pattern byte replacement patches, usually for disabling or fixing features
		ChangePatternImports();

		// Ittr: Handle address import changes, usually for rewriting or modifying results from API
		ChangeAddressImports();

		g_hInstance = hModule;
		if (GetModuleHandle(L"DisplaySwitch.exe"))
		{
			dbgprintf(L"loaded into displayswitch %p %s!", GetCurrentProcessId(), GetCommandLine());
			HookImmersive();
		}
		else
		{
			HookAPIs();
		}
	}
	break;
	case DLL_THREAD_ATTACH:
	{
		if (!g_alttabhooked && GetModuleHandle(L"alttab.dll"))
		{
			CreateWindowInBandOrig = (CreateWindowInBandAPI)GetProcAddress(GetModuleHandle(L"user32.dll"), "CreateWindowInBand");
			ChangeImportedAddress(GetModuleHandle(L"alttab.dll"), "user32.dll", CreateWindowInBandOrig, CreateWindowInBandNew);
			g_alttabhooked = TRUE;
		}

		if (GetFileAttributes((LPCWSTR)blacklistPath) != INVALID_FILE_ATTRIBUTES) // Windowblinds blockage part 2 - actually stops the program from running
			ExitExplorerSilently(); //byebye WB users

	}
	break;
	case DLL_THREAD_DETACH:
		break;
	case DLL_PROCESS_DETACH:
		EndThemeHandles();
		break;
	}
	return TRUE;
}

extern "C" HRESULT WINAPI Explorer_CoCreateInstance(
	__in   REFCLSID rclsid,
	__in   LPUNKNOWN pUnkOuter,
	__in   DWORD dwClsContext,
	__in   REFIID riid,
	__out  LPVOID* ppv
)
{
	HRESULT result;
	result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);

	if (rclsid == CLSID_PersonalStartMenu && riid == IID_IShellItemFilter && result != S_OK && g_osVersion.BuildNumber() >= 10074) //Ittr: as far as im aware doesnt cause crashing on 1507/11. needs further checking when im awake
	{
		auto shellItemFilter = new CStartMenuItemFilter();
		result = shellItemFilter->QueryInterface(riid, ppv);
	}

	if (rclsid == CLSID_SysTray) //create Metro before tray
	{
		dbgprintf(L"create Metro before tray\n");
		HookImmersive();

		if (s_EnableImmersiveShellStack == 1) // Ittr: Only create TWinUI UWP mode here if we are going to use it
			CreateTwinUI_UWP();

	}
	if (rclsid == CLSID_RegTreeOptions && riid == IID_IRegTreeOptions7) //upgrading RegTreeOptions interface
	{
		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IRegTreeOptions8, ppv);
		*ppv = new CRegTreeOptionsWrapper((IRegTreeOptions8*)*ppv);
	}

	if (riid == IID_IAuthUILogonSound7 && result != S_OK)
	{
		dbgprintf(L"Wrap authuilogonsound7\n");
		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IAuthUILogonSound10, ppv);
	}

	if (rclsid == CLSID_UserAssist && result != S_OK)
	{
		if (riid == IID_IUserAssist7)
			result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IUserAssist10, ppv);
		else if (riid == IID_IUserAssist72)
			result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IUserAssist102, ppv);
		else
		{
			// Windows Vista IUserAssist fallback
			result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IUserAssist10, ppv);
		}
	}

	if (rclsid == CLSID_StartMenuCacheAndAppResolver && result != S_OK)
	{
		if (riid == IID_IAppResolver7)
		{
			//dbgprintf(L"Explorer_CoCreateInstance: Resolver7 using iappresolver8\n");
			PVOID rslvr8 = NULL;
			CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IAppResolver8, &rslvr8);
			//create our object

			CStartMenuResolver* resolver7 = new CStartMenuResolver((IAppResolver8*)rslvr8);
			result = resolver7->QueryInterface(riid, ppv);
			//if (result == S_OK)
				//dbgprintf(L"Explorer_CoCreateInstance: Resolver7 using iappresolver8 IS OK!!\n");
		}
		else if (riid == IID_IStartMenuItemsCache7)
		{
			int build = g_osVersion.BuildNumber();
			IID iid = IID_IStartMenuItemsCache8;
			if (build >= 14393)
				iid = IID_IStartMenuItemsCache10;

			void* newcache = nullptr;
			CoCreateInstance(rclsid, pUnkOuter, dwClsContext, iid, &newcache);

			CStartMenuResolver* resolver7 = nullptr;
			if (build >= 14393)
				resolver7 = new CStartMenuResolver((IStartMenuItemsCache10*)newcache);
			else
				resolver7 = new CStartMenuResolver((IStartMenuItemsCache8*)newcache);

			result = resolver7->QueryInterface(riid, ppv);
			if (result == S_OK)
				dbgprintf(L"Explorer_CoCreateInstance: Cache7 using IStartMenuItemsCache8/10 is OK!!\n");
		}
	}
	if ((rclsid == CLSID_StartMenuPin || rclsid == CLSID_TaskbarPin) /* && riid == IID_IPinnedList2*/ && result != S_OK)
	{
		// In Windows Vista shell mode or when taskbar pinning is disabled,
		// avoid probing CLSID_TaskbarPin which prevents hangs/crashes on Windows 11 24H2+
		if (!s_UseTaskbarPinning && rclsid == CLSID_TaskbarPin)
		{
			return E_FAIL;
		}

		int build = g_osVersion.BuildNumber();
		IID id = IID_IPinnedList25;

		if (build >= 14393 && build < 17763)
		{
			id = IID_IFlexibleTaskbarPinnedList;
		}
		else if (build >= 17763)
		{
			id = IID_IPinnedList3;
		}

		//if (rclsid == CLSID_TaskbarPin && CTaskbandPin_CreateInstance && build >= 26100) // Windows 11...
		//{
		//	CTaskbandPin_W32PTP* pTaskbandPin;
		//	result = CTaskbandPin_CreateInstance(&pTaskbandPin);
		//	dbgprintf(L"CTaskbandPin_CreateInstance result: %p", result);
		//	if (SUCCEEDED(result))
		//	{
		//		result = ((IUnknown*)pTaskbandPin)->QueryInterface(id, ppv);
		//		dbgprintf(L"CTaskbandPin_CreateInstance result 2: %p", result);
		//		((IUnknown*)pTaskbandPin)->Release();
		//	}
		//}
		//else
		{
			result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, id, ppv);
		}

		if (SUCCEEDED(result))
		{
			if (riid == IID_IPinnedListVista)
				*ppv = new CPinnedListVistaWrapper((IUnknown*)*ppv, build);
			else
				*ppv = new CPinnedListWrapper((IUnknown*)*ppv, build);
		}

	}

	if (riid == IID_AutoDestList && result != S_OK)
	{
		dbgprintf(L"USE 10 AUTODESTLIST!!!!\n");
		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_AutoDestList10, ppv);
		*ppv = new CAutoDestWrapper((IAutoDestinationList10*)*ppv);
	}
	if (riid == IID_CustomDestList && result != S_OK)
	{
		dbgprintf(L"CUSTOMDESTLIST!!!!\n");
		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_CustomDestList10, ppv);
		if (result != S_OK || !*ppv)
		{
			result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_CustomDestList1507, ppv);
			*ppv = new CCustomDestWrapper((IInternalCustomDestList1507*)*ppv);
		}
		else
			*ppv = new CCustomDestWrapper((IInternalCustomDestList10*)*ppv);
	}
	if (riid == IID_IShellTaskScheduler7)
	{
		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
		dbgprintf(L"wrap IID_IShellTaskScheduler7\n");
		*ppv = new CShellTaskSchedulerWrapper((IShellTaskScheduler7*)*ppv);
	}
	if (result == S_OK && rclsid == CLSID_SysTray) //wrap stobject
	{
		dbgprintf(L"wrap stobject\n");
		*ppv = new CSysTrayWrapper((IOleCommandTarget*)*ppv);
	}
	if (rclsid == CLSID_AuthUIShutdownChoices && result != S_OK) //wrap authui
	{
		dbgprintf(L"wrap authui\n");
		int build = g_osVersion.BuildNumber();
		if (*ppv)
		{
			dbgprintf(L"good\n");
			*ppv = new CAuthUIWrapper((IUnknown*)*ppv, build);
		}
		else
		{
			IID dk = IID_IShutdownChoices8;
			if (build >= 10074)
				dk = IID_IShutdownChoices10;

			result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, dk, ppv);
			if (*ppv)
			{
				dbgprintf(L"good 2\n");
				*ppv = new CAuthUIWrapper((IUnknown*)*ppv, build);
			}
		}
	}
	if (riid == IID_TrayClock7 && result != S_OK)
		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_TrayClock8, ppv);

	if (FAILED(result))
	{
		LogFailedComCall(rclsid, riid, result);
	}

	return result;
}

extern "C" HRESULT WINAPI Explorer_CoRegisterClassObject(
	REFCLSID rclsid,     //Class identifier (CLSID) to be registered
	IUnknown* pUnk,     //Pointer to the class object
	DWORD dwClsContext,  //Context for running executable code
	DWORD flags,         //How to connect to the class object
	LPDWORD  lpdwRegister
)
{
	if (rclsid == CLSID_TrayNotify)
	{
		pUnk = new CTrayNotifyFactory((IClassFactory*)pUnk);
		if (g_osVersion.BuildNumber() < 10074 || s_EnableImmersiveShellStack == 2) // Ittr: gate fakeimmersive to 8.1 due to functional issues (e.g. hanging) with 10 - restoring this on 10 is now seemingly unnecessary
		{
			//register immersive shell fake too
			RegisterFakeImmersive();
			//and projection
			RegisterProjection();
		}
	}

	HRESULT rslt = CoRegisterClassObject(rclsid, pUnk, dwClsContext, flags, lpdwRegister);

	if (rclsid == CLSID_TrayNotify)
		dwRegisterNotify = *lpdwRegister;

	return rslt;
}

extern "C" HRESULT WINAPI Explorer_CoRevokeClassObject(DWORD dwRegister)
{
	if (dwRegister == dwRegisterNotify)
	{
		if (g_osVersion.BuildNumber() < 10074 || s_EnableImmersiveShellStack == 2) // Ittr: gate fakeimmersive to 8.1 due to functional issues (e.g. hanging) with 10
		{
			UnregisterFakeImmersive();
			UnregisterProjection();
		}
	}
	return CoRevokeClassObject(dwRegister);
}
