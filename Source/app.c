#include "view.h"

#include <shellapi.h>
#include <windowsx.h>

#define APP_NAME L"Stillmeter"
#define DASHBOARD_CLASS L"Stillmeter.Dashboard.1"
#define STRIP_CLASS L"Stillmeter.Strip.1"
#define INSTANCE_NAME L"Local\\Stillmeter.Dashboard.1"
#define ICON_RESOURCE 101
#define SAMPLE_TIMER 1
#define TIMER_INTERVAL_MS 1000
#define TRAY_ICON 1
#define TRAY_EVENT (WM_APP + 1)
#define SHOW_DASHBOARD_EVENT (WM_APP + 2)
#define STRIP_WIDTH 202
#define STRIP_HEIGHT 43

enum { SHOW_DASHBOARD = 1001, TOGGLE_STRIP, TOGGLE_PAUSE, EXIT_APP };

typedef struct {
    HINSTANCE instance;
    HWND dashboard;
    HWND strip;
    HWND strip_button;
    HWND pause_button;
    ViewFonts dashboard_fonts;
    ViewFonts strip_fonts;
    Monitor monitor;
    UINT taskbar_created;
    bool tray_added;
    bool tray_version_four;
    bool strip_enabled;
    bool paused;
    bool suspended;
    wchar_t tray_tooltip[TOOLTIP_LENGTH];
} Application;

static Application app;

static int scale(int value, UINT dpi)
{
    return MulDiv(value, (int)dpi, USER_DEFAULT_SCREEN_DPI);
}

static UINT window_dpi(HWND window)
{
    const UINT dpi = GetDpiForWindow(window);
    return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
}

static NOTIFYICONDATAW tray_data(HWND dashboard)
{
    return (NOTIFYICONDATAW){.cbSize = sizeof(NOTIFYICONDATAW), .hWnd = dashboard,
                            .uID = TRAY_ICON};
}

static void add_tray(HWND dashboard)
{
    NOTIFYICONDATAW icon = tray_data(dashboard);
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = TRAY_EVENT;
    icon.hIcon = (HICON)LoadImageW(app.instance, MAKEINTRESOURCEW(ICON_RESOURCE), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                 LR_SHARED);
    if (!icon.hIcon) icon.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    view_tooltip(&app.monitor, app.paused, icon.szTip);
    app.tray_added = Shell_NotifyIconW(NIM_ADD, &icon) || Shell_NotifyIconW(NIM_MODIFY, &icon);
    if (!app.tray_added) return;

    wcscpy(app.tray_tooltip, icon.szTip);
    icon.uVersion = NOTIFYICON_VERSION_4;
    app.tray_version_four = Shell_NotifyIconW(NIM_SETVERSION, &icon) != FALSE;
}

static void update_tray(void)
{
    if (!app.tray_added) return;

    NOTIFYICONDATAW icon = tray_data(app.dashboard);
    view_tooltip(&app.monitor, app.paused, icon.szTip);
    if (wcscmp(app.tray_tooltip, icon.szTip) == 0) return;

    icon.uFlags = NIF_TIP | NIF_SHOWTIP;
    if (Shell_NotifyIconW(NIM_MODIFY, &icon)) wcscpy(app.tray_tooltip, icon.szTip);
}

static void show_dashboard(void)
{
    ShowWindow(app.dashboard, IsIconic(app.dashboard) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(app.dashboard);
}

static void tray_menu(POINT position)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, SHOW_DASHBOARD, L"Open dashboard");
    AppendMenuW(menu, MF_STRING | (app.strip_enabled ? MF_CHECKED : MF_UNCHECKED) |
                (app.strip ? MF_ENABLED : MF_GRAYED),
                TOGGLE_STRIP, L"Show taskbar strip");
    AppendMenuW(menu, MF_STRING, TOGGLE_PAUSE, app.paused ? L"Resume sampling" : L"Pause sampling");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, EXIT_APP, L"Exit Stillmeter");
    SetMenuDefaultItem(menu, SHOW_DASHBOARD, FALSE);

    SetForegroundWindow(app.dashboard);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                        position.x, position.y, 0, app.dashboard, NULL);
    DestroyMenu(menu);
    PostMessageW(app.dashboard, WM_NULL, 0, 0);
    if (command) SendMessageW(app.dashboard, WM_COMMAND, command, 0);
    if (app.tray_added) {
        NOTIFYICONDATAW icon = tray_data(app.dashboard);
        Shell_NotifyIconW(NIM_SETFOCUS, &icon);
    }
}

static void handle_tray_event(WPARAM wparam, LPARAM lparam)
{
    const UINT event = LOWORD(lparam);
    if (app.tray_version_four ? HIWORD(lparam) != TRAY_ICON : wparam != TRAY_ICON)
        return;

    if (event == NIN_SELECT || event == NIN_KEYSELECT ||
        (!app.tray_version_four && event == WM_LBUTTONUP)) {
        show_dashboard();
    } else if ((app.tray_version_four && event == WM_CONTEXTMENU) ||
               (!app.tray_version_four && event == WM_RBUTTONUP)) {
        POINT position = {GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)};
        if (!app.tray_version_four || (position.x == -1 && position.y == -1))
            GetCursorPos(&position);
        tray_menu(position);
    }
}

static bool fullscreen_on(HMONITOR monitor, const RECT *screen)
{
    const HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == app.dashboard || foreground == GetDesktopWindow() ||
        foreground == GetShellWindow() || IsIconic(foreground) ||
        MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL) != monitor)
        return false;

    RECT client;
    if (!GetClientRect(foreground, &client)) return false;
    MapWindowPoints(foreground, NULL, (POINT *)&client, 2);
    return client.left <= screen->left && client.top <= screen->top &&
           client.right >= screen->right && client.bottom >= screen->bottom;
}

static bool strip_bounds(RECT *target, UINT *dpi)
{
    if (!app.strip_enabled || app.suspended) return false;
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!taskbar || !IsWindowVisible(taskbar)) return false;
    const HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONULL);
    MONITORINFO monitor_info = {.cbSize = sizeof(monitor_info)};
    RECT position, visible;
    if (!monitor || !GetMonitorInfoW(monitor, &monitor_info) || !GetWindowRect(taskbar, &position) ||
        !IntersectRect(&visible, &position, &monitor_info.rcMonitor))
        return false;

    *dpi = window_dpi(taskbar);
    const RECT screen = monitor_info.rcMonitor;
    const int width = scale(STRIP_WIDTH, *dpi);
    const int height = scale(STRIP_HEIGHT, *dpi);
    const int gap = scale(7, *dpi);
    const int taskbar_width = visible.right - visible.left;
    const int taskbar_height = visible.bottom - visible.top;
    int x, y;

    if (taskbar_width >= taskbar_height) {
        if (taskbar_height < scale(16, *dpi)) return false;
        x = screen.right - width - gap;
        const bool top = visible.top - screen.top < screen.bottom - visible.bottom;
        y = top ? visible.bottom + gap : visible.top - height - gap;
    } else {
        if (taskbar_width < scale(16, *dpi)) return false;
        const bool left = visible.left - screen.left < screen.right - visible.right;
        x = left ? visible.right + gap : visible.left - width - gap;
        y = screen.bottom - height - gap;
    }

    *target = (RECT){x, y, x + width, y + height};
    RECT dashboard, overlap;
    if (GetForegroundWindow() == app.dashboard && IsWindowVisible(app.dashboard) &&
        GetWindowRect(app.dashboard, &dashboard) && IntersectRect(&overlap, &dashboard, target))
        return false;

    return target->left >= screen.left && target->right <= screen.right &&
           target->top >= screen.top && target->bottom <= screen.bottom &&
           !fullscreen_on(monitor, &screen);
}

static void position_strip(void)
{
    if (!app.strip) return;

    const bool shown = IsWindowVisible(app.strip) != FALSE;
    RECT desired;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    if (!strip_bounds(&desired, &dpi)) {
        if (shown) ShowWindow(app.strip, SW_HIDE);
        return;
    }

    if (app.strip_fonts.dpi != dpi) {
        view_fonts_init(&app.strip_fonts, dpi);
        const int radius = scale(13, dpi);
        HRGN region = CreateRoundRectRgn(0, 0, desired.right - desired.left,
                                       desired.bottom - desired.top, radius, radius);
        if (region && !SetWindowRgn(app.strip, region, TRUE)) DeleteObject(region);
    }

    RECT current;
    if (!shown || !GetWindowRect(app.strip, &current) || !EqualRect(&current, &desired)) {
        SetWindowPos(app.strip, HWND_TOPMOST, desired.left, desired.top,
                     desired.right - desired.left, desired.bottom - desired.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    }
}

static void repaint(void)
{
    if (IsWindowVisible(app.dashboard) && !IsIconic(app.dashboard))
        InvalidateRect(app.dashboard, NULL, FALSE);
    if (IsWindowVisible(app.strip)) InvalidateRect(app.strip, NULL, FALSE);
}

static void refresh(void)
{
    if (!app.paused && !app.suspended) monitor_sample(&app.monitor);
    if (!app.tray_added) add_tray(app.dashboard);
    update_tray();
    position_strip();
    repaint();
}

static void layout_buttons(HWND window)
{
    UINT dpi = window_dpi(window);
    RECT client;
    if (GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
        const UINT width_dpi = (UINT)client.right * USER_DEFAULT_SCREEN_DPI / DASHBOARD_WIDTH;
        const UINT height_dpi = (UINT)client.bottom * USER_DEFAULT_SCREEN_DPI / DASHBOARD_HEIGHT;
        if (width_dpi < dpi) dpi = width_dpi;
        if (height_dpi < dpi) dpi = height_dpi;
        if (dpi == 0) dpi = 1;
    }
    view_fonts_init(&app.dashboard_fonts, dpi);
    SendMessageW(app.strip_button, WM_SETFONT, (WPARAM)app.dashboard_fonts.regular, TRUE);
    SendMessageW(app.pause_button, WM_SETFONT, (WPARAM)app.dashboard_fonts.regular, TRUE);
    MoveWindow(app.strip_button, scale(22, dpi), scale(431, dpi),
               scale(156, dpi), scale(29, dpi), TRUE);
    MoveWindow(app.pause_button, scale(187, dpi), scale(431, dpi),
               scale(141, dpi), scale(29, dpi), TRUE);
}

static void handle_command(UINT command)
{
    switch (command) {
    case SHOW_DASHBOARD:
        show_dashboard();
        break;
    case TOGGLE_STRIP:
        app.strip_enabled = !app.strip_enabled;
        SetWindowTextW(app.strip_button, app.strip_enabled ? L"&Hide taskbar strip" : L"&Show taskbar strip");
        position_strip();
        repaint();
        break;
    case TOGGLE_PAUSE:
        app.paused = !app.paused;
        SetWindowTextW(app.pause_button, app.paused ? L"&Resume sampling" : L"&Pause sampling");
        monitor_reset(&app.monitor);
        refresh();
        break;
    case EXIT_APP:
        DestroyWindow(app.dashboard);
        break;
    default:
        break;
    }
}

static LRESULT CALLBACK dashboard_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (app.taskbar_created && message == app.taskbar_created) {
        app.tray_added = false;
        add_tray(window);
        position_strip();
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        app.strip_button = CreateWindowExW(0, L"BUTTON", L"&Hide taskbar strip",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                0, 0, 0, 0, window, (HMENU)(INT_PTR)TOGGLE_STRIP, app.instance, NULL);
        app.pause_button = CreateWindowExW(0, L"BUTTON", L"&Pause sampling",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                0, 0, 0, 0, window, (HMENU)(INT_PTR)TOGGLE_PAUSE, app.instance, NULL);
        if (!app.strip_button || !app.pause_button) return -1;
        layout_buttons(window);
        return 0;
    case WM_PAINT:
        view_dashboard(window, &app.dashboard_fonts, &app.monitor, app.paused);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wparam == SAMPLE_TIMER) refresh();
        return 0;
    case WM_COMMAND:
        handle_command(LOWORD(wparam));
        return 0;
    case WM_DPICHANGED: {
        const RECT *bounds = (const RECT *)lparam;
        SetWindowPos(window, NULL, bounds->left, bounds->top, bounds->right - bounds->left,
                     bounds->bottom - bounds->top, SWP_NOACTIVATE | SWP_NOZORDER);
        layout_buttons(window);
        InvalidateRect(window, NULL, FALSE);
        return 0;
    }
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) layout_buttons(window);
        break;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        position_strip();
        repaint();
        break;
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND) {
            app.suspended = true;
            monitor_reset(&app.monitor);
            position_strip();
        }
        if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
            if (app.suspended) {
                app.suspended = false;
                refresh();
            }
        }
        return TRUE;
    case TRAY_EVENT:
        handle_tray_event(wparam, lparam);
        return 0;
    case SHOW_DASHBOARD_EVENT:
        show_dashboard();
        return 0;
    case WM_CLOSE:
        if (app.tray_added) ShowWindow(window, SW_HIDE);
        else DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, SAMPLE_TIMER);
        if (app.tray_added) {
            NOTIFYICONDATAW icon = tray_data(window);
            Shell_NotifyIconW(NIM_DELETE, &icon);
            app.tray_added = false;
        }
        if (app.strip) DestroyWindow(app.strip);
        app.strip = NULL;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static LRESULT CALLBACK strip_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_PAINT:
        view_strip(window, &app.strip_fonts, &app.monitor, app.paused);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

static bool create_windows(void)
{
    const HICON icon = LoadIconW(app.instance, MAKEINTRESOURCEW(ICON_RESOURCE));
    const WNDCLASSEXW dashboard_class = {
        .cbSize = sizeof(WNDCLASSEXW), .hInstance = app.instance, .lpfnWndProc = dashboard_proc,
        .lpszClassName = DASHBOARD_CLASS, .hIcon = icon, .hIconSm = icon,
        .hCursor = LoadCursorW(NULL, IDC_ARROW)
    };
    const WNDCLASSEXW strip_class = {
        .cbSize = sizeof(WNDCLASSEXW), .hInstance = app.instance, .lpfnWndProc = strip_proc,
        .lpszClassName = STRIP_CLASS
    };
    if (!RegisterClassExW(&dashboard_class) || !RegisterClassExW(&strip_class)) return false;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    const DWORD ex_style = WS_EX_CONTROLPARENT;
    const HMONITOR primary = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO display = {.cbSize = sizeof(display)};
    if (!GetMonitorInfoW(primary, &display)) return false;

    const UINT dpi = GetDpiForSystem();
    RECT bounds = {0, 0, scale(DASHBOARD_WIDTH, dpi), scale(DASHBOARD_HEIGHT, dpi)};
    if (!AdjustWindowRectExForDpi(&bounds, style, FALSE, ex_style, dpi)) return false;
    const int available_width = display.rcWork.right - display.rcWork.left;
    const int available_height = display.rcWork.bottom - display.rcWork.top;
    int width = bounds.right - bounds.left;
    int height = bounds.bottom - bounds.top;
    if (width > available_width) width = available_width;
    if (height > available_height) height = available_height;
    const int edge = scale(13, dpi);
    const int x = display.rcWork.right - width - edge;
    const int strip_space = scale(STRIP_HEIGHT + 14, dpi);
    const int bottom_gap = available_height - height >= strip_space ? strip_space : edge;
    const int y = display.rcWork.bottom - height - bottom_gap;
    app.dashboard = CreateWindowExW(ex_style, DASHBOARD_CLASS, APP_NAME, style,
                       x > display.rcWork.left ? x : display.rcWork.left,
                       y > display.rcWork.top ? y : display.rcWork.top,
                       width, height, NULL, NULL, app.instance, NULL);
    if (!app.dashboard) return false;

    app.strip = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST |
                                WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                STRIP_CLASS, L"Stillmeter taskbar strip", WS_POPUP,
                                0, 0, 0, 0, NULL, NULL, app.instance, NULL);
    if (!app.strip || !SetLayeredWindowAttributes(app.strip, 0, 255, LWA_ALPHA)) {
        if (app.strip) DestroyWindow(app.strip);
        app.strip = NULL;
        app.strip_enabled = false;
        EnableWindow(app.strip_button, FALSE);
        SetWindowTextW(app.strip_button, L"Strip unavailable");
    }

    return SetTimer(app.dashboard, SAMPLE_TIMER, TIMER_INTERVAL_MS, NULL) != 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show)
{
    (void)previous;
    (void)command;
    (void)show;

    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    const HANDLE single_instance = CreateMutexW(NULL, FALSE, INSTANCE_NAME);
    if (single_instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        const HWND window = FindWindowW(DASHBOARD_CLASS, NULL);
        if (window) {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner) AllowSetForegroundWindow(owner);
            PostMessageW(window, SHOW_DASHBOARD_EVENT, 0, 0);
        }
        CloseHandle(single_instance);
        return 0;
    }

    app.instance = instance;
    app.strip_enabled = true;
    app.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    monitor_init(&app.monitor);

    int exit_code = 1;
    if (single_instance && create_windows()) {
        refresh();
        show_dashboard();

        MSG message;
        BOOL result;
        while ((result = GetMessageW(&message, NULL, 0, 0)) > 0) {
            if (!IsWindowVisible(app.dashboard) || !IsDialogMessageW(app.dashboard, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        exit_code = result == 0 ? (int)message.wParam : 1;
    } else {
        MessageBoxW(NULL, L"Stillmeter could not start.", APP_NAME, MB_OK | MB_ICONERROR);
    }

    if (app.dashboard && IsWindow(app.dashboard)) DestroyWindow(app.dashboard);
    view_fonts_close(&app.dashboard_fonts);
    view_fonts_close(&app.strip_fonts);
    monitor_close(&app.monitor);
    if (single_instance) CloseHandle(single_instance);
    return exit_code;
}
