#include "view.h"
#include "taskbar.h"
#include "resource.h"

#include <shellapi.h>
#include <stdlib.h>
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
#define FOREGROUND_CHANGED_EVENT (WM_APP + 4)
#define LAYOUT_POLL_MS 2000
#define SETTINGS_KEY L"Software\\Stillmeter"
#define APPEARANCE_VALUE L"StripAppearanceV3"
#define ADAPTER_VALUE L"NetworkAdapter"

enum { SHOW_DASHBOARD = 1001, TOGGLE_STRIP, TOGGLE_PAUSE, EXIT_APP, STRIP_APPEARANCE, NETWORK_ADAPTER };

typedef struct {
    HINSTANCE instance;
    HWND dashboard;
    HWND strip;
    HWND strip_taskbar;
    bool strip_docked;
    bool layout_started;
    HWND strip_button;
    HWND pause_button;
    HWND appearance_button;
    HWND adapter_button;
    ViewFonts dashboard_fonts;
    StripAppearance appearance;
    StripLayout strip_layouts[2];
    const StripLayout *placed_layout;
    StripSurface strip_surface;
    ULONGLONG last_layout_request;
    ULONGLONG panel_closed_at;
    HWINEVENTHOOK foreground_hook;
    bool panel_open;
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

static const StripAppearance appearance_presets[] = {
    {12, 6, 1, STRIP_ALL_METRICS, 2, 0, 1},
    {11, 3, 0, STRIP_UPLOAD | STRIP_DOWNLOAD | STRIP_CPU | STRIP_MEMORY, 2, 0, 1},
    {16, 6, 2, STRIP_UPLOAD | STRIP_DOWNLOAD | STRIP_CPU | STRIP_MEMORY, 1, 0, 2}
};

_Static_assert(IDC_STRIP_TOTAL - IDC_STRIP_UPLOAD + 1 == STRIP_METRIC_COUNT,
               "Metric control IDs must cover the metric mask");

static bool appearance_valid(const StripAppearance *value)
{
    return value->font_size >= 9 && value->font_size <= 20 &&
           value->column_gap <= 24 && value->row_gap <= 12 &&
           value->metrics > 0 && value->metrics <= STRIP_ALL_METRICS &&
           (value->rows == 1 || value->rows == 2) && value->align_right <= 1 &&
           value->weight <= 2;
}

static StripAppearance load_appearance(void)
{
    StripAppearance saved;
    DWORD bytes = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, APPEARANCE_VALUE,
                     RRF_RT_REG_BINARY, NULL, &saved, &bytes) == ERROR_SUCCESS &&
        bytes == sizeof(saved) && appearance_valid(&saved))
        return saved;

    // V2 stored four group bits in the same field as the V3 metric mask.
    bytes = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"StripAppearanceV2",
                     RRF_RT_REG_BINARY, NULL, &saved, &bytes) == ERROR_SUCCESS &&
        bytes == sizeof(saved) && saved.metrics <= 15) {
        static const DWORD group_metrics[] = {
            STRIP_UPLOAD | STRIP_DOWNLOAD, STRIP_CPU | STRIP_MEMORY,
            STRIP_GPU | STRIP_CPU_FREQUENCY, STRIP_TOTAL
        };
        const DWORD groups = saved.metrics;
        saved.metrics = 0;
        for (unsigned i = 0; i < sizeof(group_metrics) / sizeof(*group_metrics); ++i)
            if (groups & (1u << i)) saved.metrics |= group_metrics[i];
        if (appearance_valid(&saved)) return saved;
    }

    // Preserve the three appearance fields stored by versions 1.3 and 1.4.
    DWORD legacy[3];
    bytes = sizeof(legacy);
    saved = appearance_presets[0];
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"StripAppearance",
                     RRF_RT_REG_BINARY, NULL, legacy, &bytes) == ERROR_SUCCESS &&
        bytes == sizeof(legacy)) {
        saved.font_size = legacy[0];
        saved.column_gap = legacy[1];
        saved.row_gap = legacy[2];
    }
    return appearance_valid(&saved) ? saved : appearance_presets[0];
}

static AdapterSelection load_adapter(void)
{
    AdapterSelection saved;
    DWORD bytes = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, ADAPTER_VALUE, RRF_RT_REG_BINARY,
                     NULL, &saved, &bytes) == ERROR_SUCCESS && bytes == sizeof(saved) &&
        saved.mode <= ADAPTER_SINGLE &&
        (saved.mode != ADAPTER_SINGLE || !IsEqualGUID(&saved.guid, &(GUID){0})))
        return saved;
    return (AdapterSelection){.mode = ADAPTER_ALL};
}

static bool save_setting(const wchar_t *name, const void *value, DWORD size)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return false;
    const LSTATUS status = RegSetValueExW(key, name, 0, REG_BINARY, value, size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

static void request_layout(bool force)
{
    if (!app.strip_enabled || app.suspended) return;
    const ULONGLONG now = GetTickCount64();
    if (force || !app.last_layout_request || now - app.last_layout_request >= LAYOUT_POLL_MS) {
        app.last_layout_request = now;
        taskbar_layout_request();
    }
}

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
                (app.layout_started ? MF_ENABLED : MF_GRAYED),
                TOGGLE_STRIP, L"Show taskbar strip");
    AppendMenuW(menu, MF_STRING, STRIP_APPEARANCE, L"Strip appearance…");
    AppendMenuW(menu, MF_STRING, NETWORK_ADAPTER, L"Network adapter…");
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

static bool fullscreen_on(HWND foreground, HMONITOR monitor, const RECT *screen)
{
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

static const StripLayout *strip_bounds(HWND taskbar, bool retain_layout, RECT *target)
{
    if (!taskbar || !IsWindowVisible(taskbar)) return NULL;
    const HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONULL);
    MONITORINFO monitor_info = {.cbSize = sizeof(monitor_info)};
    RECT position, visible;
    if (!monitor || !GetMonitorInfoW(monitor, &monitor_info) || !GetWindowRect(taskbar, &position) ||
        !IntersectRect(&visible, &position, &monitor_info.rcMonitor))
        return NULL;

    const int taskbar_height = visible.bottom - visible.top;
    const int taskbar_width = visible.right - visible.left;
    const UINT native_dpi = window_dpi(taskbar);

    if (taskbar_width < taskbar_height || taskbar_height < scale(28, native_dpi))
        return NULL;

    const int padding = scale(4, native_dpi);
    const UINT layout_dpi[] = {native_dpi, native_dpi * 87 / 100};
    for (unsigned i = 0; i < 2; ++i) {
        StripLayout *layout = &app.strip_layouts[i];
        if (view_strip_prepare(layout, &app.appearance, layout_dpi[i], taskbar_height - 2) &&
            taskbar_layout_place(taskbar, &position, &visible, layout->width,
                                 layout->height, padding, app.appearance.align_right != 0,
                                 retain_layout, target))
            return layout;
    }
    return NULL;
}

static bool taskbar_can_host_strip(HWND taskbar)
{
    // Only attach to the window owned by the current desktop shell.
    const HWND shell = GetShellWindow();
    if (!shell) return false;
    DWORD shell_process = 0, taskbar_process = 0;
    GetWindowThreadProcessId(shell, &shell_process);
    GetWindowThreadProcessId(taskbar, &taskbar_process);
    return shell_process && shell_process == taskbar_process;
}

static HWND create_strip_window(HWND taskbar, bool docked)
{
    DPI_AWARENESS_CONTEXT previous = NULL;
    if (docked) {
        const DPI_AWARENESS_CONTEXT host_dpi = GetWindowDpiAwarenessContext(taskbar);
        if (host_dpi) previous = SetThreadDpiAwarenessContext(host_dpi);
    }
    const DWORD styles = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT;
    HWND window = CreateWindowExW(styles | (docked ? 0 : WS_EX_TOPMOST),
                                 STRIP_CLASS, L"Stillmeter taskbar strip",
                                 docked ? WS_CHILD : WS_POPUP,
                                 0, 0, 0, 0, docked ? taskbar : NULL,
                                 NULL, app.instance, NULL);
    if (previous) SetThreadDpiAwarenessContext(previous);
    return window;
}

static bool ensure_strip_window(HWND taskbar)
{
    if (app.strip_taskbar == taskbar && IsWindow(app.strip)) return true;
    if (app.strip) DestroyWindow(app.strip);

    app.strip_taskbar = taskbar;
    app.strip_docked = false;
    if (taskbar_can_host_strip(taskbar)) {
        app.strip = create_strip_window(taskbar, true);
        app.strip_docked = app.strip != NULL;
    }
    if (!app.strip) app.strip = create_strip_window(taskbar, false);
    return app.strip != NULL;
}

static bool show_docked_strip(HWND taskbar, const RECT *desired, bool move, bool shown)
{
    if (move) {
        RECT relative = *desired;
        MapWindowPoints(HWND_DESKTOP, taskbar, (POINT *)&relative, 2);
        if (!SetWindowPos(app.strip, HWND_TOP, relative.left, relative.top,
                          relative.right - relative.left, relative.bottom - relative.top,
                          SWP_NOACTIVATE | SWP_NOOWNERZORDER))
            return false;
        if (!view_strip(app.strip, NULL, app.placed_layout, &app.strip_surface,
                        &app.monitor, app.paused))
            return false;
    }
    // This is the top of Explorer's child windows, not the desktop's topmost band.
    if (!shown || GetWindow(app.strip, GW_HWNDPREV))
        return SetWindowPos(app.strip, HWND_TOP, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                            SWP_SHOWWINDOW | SWP_NOOWNERZORDER) != FALSE;
    return true;
}

static void show_overlay_strip(HWND taskbar, HWND foreground, const RECT *desired,
                               bool move, bool shown)
{
    MONITORINFO screen = {.cbSize = sizeof(screen)};
    const HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONULL);
    if (!app.panel_open && monitor && GetMonitorInfoW(monitor, &screen) &&
        fullscreen_on(foreground, monitor, &screen.rcMonitor)) {
        if (shown) ShowWindow(app.strip, SW_HIDE);
        return;
    }

    bool raise_strip = !shown;
    if (shown)
        for (HWND other = GetWindow(app.strip, GW_HWNDPREV); other; other = GetWindow(other, GW_HWNDPREV))
            if (other == taskbar || (app.panel_open && other == foreground)) {
                raise_strip = true;
                break;
            }

    if (move) {
        const POINT position = {desired->left, desired->top};
        if (!view_strip(app.strip, &position, app.placed_layout, &app.strip_surface,
                        &app.monitor, app.paused)) {
            if (shown) ShowWindow(app.strip, SW_HIDE);
            return;
        }
    }
    if (raise_strip)
        SetWindowPos(app.strip, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
}

static void position_strip(void)
{
    const bool shown = app.strip && IsWindowVisible(app.strip);
    if (!app.strip_enabled || app.suspended) {
        app.placed_layout = NULL;
        app.panel_open = false;
        app.panel_closed_at = 0;
        if (shown) ShowWindow(app.strip, SW_HIDE);
        return;
    }

    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", NULL);
    const HWND foreground = GetForegroundWindow();
    const bool panel_open = taskbar_shell_panel(foreground, taskbar);
    const ULONGLONG now = GetTickCount64();
    if (app.panel_open && !panel_open) {
        app.panel_closed_at = now;
        request_layout(true);
    }
    app.panel_open = panel_open;
    const bool retain_layout = shown && (panel_open || (app.panel_closed_at &&
                                       now - app.panel_closed_at < LAYOUT_POLL_MS));
    RECT desired;
    app.placed_layout = strip_bounds(taskbar, retain_layout, &desired);
    if (!app.placed_layout) {
        if (shown) ShowWindow(app.strip, SW_HIDE);
        return;
    }
    if (!ensure_strip_window(taskbar)) return;

    RECT current;
    const bool current_shown = IsWindowVisible(app.strip);
    const bool move = !current_shown || !GetWindowRect(app.strip, &current) ||
                      !EqualRect(&current, &desired);
    if (app.strip_docked) {
        if (show_docked_strip(taskbar, &desired, move, current_shown)) return;
        DestroyWindow(app.strip);
        app.strip_docked = false;
        app.strip = create_strip_window(taskbar, false);
        if (app.strip) show_overlay_strip(taskbar, foreground, &desired, true, false);
    } else {
        show_overlay_strip(taskbar, foreground, &desired, move, current_shown);
    }
}

static void repaint(void)
{
    if (IsWindowVisible(app.dashboard) && !IsIconic(app.dashboard))
        InvalidateRect(app.dashboard, NULL, FALSE);
    if (IsWindowVisible(app.strip))
        view_strip(app.strip, NULL, app.placed_layout, &app.strip_surface, &app.monitor, app.paused);
}

static void refresh(void)
{
    if (!app.paused && !app.suspended) monitor_sample(&app.monitor);
    if (!app.tray_added) add_tray(app.dashboard);
    update_tray();
    request_layout(false);
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
    SendMessageW(app.adapter_button, WM_SETFONT, (WPARAM)app.dashboard_fonts.regular, TRUE);
    MoveWindow(app.adapter_button, scale(22, dpi), scale(431, dpi),
               scale(156, dpi), scale(29, dpi), TRUE);
    SendMessageW(app.strip_button, WM_SETFONT, (WPARAM)app.dashboard_fonts.regular, TRUE);
    SendMessageW(app.pause_button, WM_SETFONT, (WPARAM)app.dashboard_fonts.regular, TRUE);
    MoveWindow(app.strip_button, scale(22, dpi), scale(469, dpi),
               scale(156, dpi), scale(29, dpi), TRUE);
    MoveWindow(app.pause_button, scale(187, dpi), scale(469, dpi),
               scale(141, dpi), scale(29, dpi), TRUE);
    SendMessageW(app.appearance_button, WM_SETFONT, (WPARAM)app.dashboard_fonts.regular, TRUE);
    MoveWindow(app.appearance_button, scale(337, dpi), scale(469, dpi),
               scale(117, dpi), scale(29, dpi), TRUE);
}

typedef struct {
    bool filling;
    StripLayout preview_layout;
    StripSurface preview_surface;
    const StripLayout *preview;
} AppearanceDialog;

static void fill_appearance_dialog(HWND dialog, AppearanceDialog *state, const StripAppearance *value)
{
    state->filling = true;
    SetDlgItemInt(dialog, IDC_STRIP_FONT_SIZE, value->font_size, FALSE);
    SetDlgItemInt(dialog, IDC_STRIP_COLUMN_GAP, value->column_gap, FALSE);
    SetDlgItemInt(dialog, IDC_STRIP_ROW_GAP, value->row_gap, FALSE);
    for (unsigned i = 0; i < STRIP_METRIC_COUNT; ++i)
        CheckDlgButton(dialog, IDC_STRIP_UPLOAD + (int)i,
                       value->metrics & (1u << i) ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dialog, IDC_STRIP_ROWS, CB_SETCURSEL, value->rows - 1, 0);
    SendDlgItemMessageW(dialog, IDC_STRIP_ALIGNMENT, CB_SETCURSEL, value->align_right, 0);
    SendDlgItemMessageW(dialog, IDC_STRIP_WEIGHT, CB_SETCURSEL, value->weight, 0);
    WPARAM preset = 0;
    for (unsigned i = 0; i < sizeof(appearance_presets) / sizeof(*appearance_presets); ++i) {
        if (strip_appearance_equal(value, &appearance_presets[i])) {
            preset = i + 1;
            break;
        }
    }
    SendDlgItemMessageW(dialog, IDC_STRIP_PRESET, CB_SETCURSEL, preset, 0);
    state->filling = false;
}

static bool read_appearance_field(HWND dialog, int id, DWORD *value)
{
    wchar_t input[8];
    const int length = GetDlgItemTextW(dialog, id, input, 8);
    if (length < 1 || length > 2 || wcsspn(input, L"0123456789") != (size_t)length)
        return false;
    *value = wcstoul(input, NULL, 10);
    return true;
}

static bool read_appearance_dialog(HWND dialog, StripAppearance *value)
{
    *value = (StripAppearance){0};
    value->rows = (DWORD)(SendDlgItemMessageW(dialog, IDC_STRIP_ROWS, CB_GETCURSEL, 0, 0) + 1);
    value->align_right = (DWORD)SendDlgItemMessageW(dialog, IDC_STRIP_ALIGNMENT, CB_GETCURSEL, 0, 0);
    value->weight = (DWORD)SendDlgItemMessageW(dialog, IDC_STRIP_WEIGHT, CB_GETCURSEL, 0, 0);
    for (unsigned i = 0; i < STRIP_METRIC_COUNT; ++i)
        if (IsDlgButtonChecked(dialog, IDC_STRIP_UPLOAD + (int)i) == BST_CHECKED)
            value->metrics |= 1u << i;
    return read_appearance_field(dialog, IDC_STRIP_FONT_SIZE, &value->font_size) &&
           read_appearance_field(dialog, IDC_STRIP_COLUMN_GAP, &value->column_gap) &&
           read_appearance_field(dialog, IDC_STRIP_ROW_GAP, &value->row_gap) &&
           appearance_valid(value);
}

static void update_appearance_preview(HWND dialog, AppearanceDialog *state)
{
    StripAppearance value;
    const bool valid = read_appearance_dialog(dialog, &value);
    EnableWindow(GetDlgItem(dialog, IDOK), valid);
    EnableWindow(GetDlgItem(dialog, IDC_STRIP_APPLY), valid);
    EnableWindow(GetDlgItem(dialog, IDC_STRIP_ROW_GAP), value.rows != 1);
    SetDlgItemTextW(dialog, IDC_STRIP_HINT, valid ? L"" :
                    value.metrics ? L"Font 9–20 px, column gap 0–24 px, row gap 0–12 px."
                                  : L"Select at least one metric.");

    const UINT dpi = window_dpi(dialog);
    state->preview = valid && view_strip_prepare(&state->preview_layout, &value, dpi, scale(48, dpi))
                     ? &state->preview_layout : NULL;
    InvalidateRect(GetDlgItem(dialog, IDC_STRIP_PREVIEW), NULL, FALSE);
}

static void init_appearance_dialog(HWND dialog, AppearanceDialog *state)
{
    static const struct {
        int control;
        const wchar_t *items[5];
    } choices[] = {
        {IDC_STRIP_PRESET, {L"Custom", L"Balanced", L"Compact", L"Large text", NULL}},
        {IDC_STRIP_ROWS, {L"One row", L"Two rows", NULL}},
        {IDC_STRIP_ALIGNMENT, {L"Left", L"Right", NULL}},
        {IDC_STRIP_WEIGHT, {L"Regular", L"Semibold", L"Bold", NULL}}
    };
    for (unsigned i = 0; i < sizeof(choices) / sizeof(*choices); ++i)
        for (unsigned j = 0; choices[i].items[j]; ++j)
            SendDlgItemMessageW(dialog, choices[i].control, CB_ADDSTRING, 0,
                                (LPARAM)choices[i].items[j]);
    SendDlgItemMessageW(dialog, IDC_STRIP_FONT_SIZE, EM_SETLIMITTEXT, 2, 0);
    SendDlgItemMessageW(dialog, IDC_STRIP_COLUMN_GAP, EM_SETLIMITTEXT, 2, 0);
    SendDlgItemMessageW(dialog, IDC_STRIP_ROW_GAP, EM_SETLIMITTEXT, 2, 0);
    view_strip_theme(&state->preview_surface);
    fill_appearance_dialog(dialog, state, &app.appearance);
    update_appearance_preview(dialog, state);
}

static INT_PTR CALLBACK appearance_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_INITDIALOG) {
        AppearanceDialog *state = (AppearanceDialog *)lparam;
        SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)state);
        init_appearance_dialog(dialog, state);
        return TRUE;
    }

    AppearanceDialog *state = (AppearanceDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (!state) return FALSE;
    if (message == WM_DRAWITEM && wparam == IDC_STRIP_PREVIEW) {
        const DRAWITEMSTRUCT *item = (const DRAWITEMSTRUCT *)lparam;
        view_strip_preview(item->hDC, item->rcItem, state->preview, &state->preview_surface);
        return TRUE;
    }
    if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED ||
        message == WM_SYSCOLORCHANGE || message == WM_DPICHANGED) {
        view_strip_theme(&state->preview_surface);
        update_appearance_preview(dialog, state);
        return FALSE;
    }
    if (message != WM_COMMAND || state->filling) return FALSE;

    const UINT command = LOWORD(wparam);
    const UINT notification = HIWORD(wparam);
    if (command == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
    } else if (command == IDC_STRIP_DEFAULTS) {
        fill_appearance_dialog(dialog, state, &appearance_presets[0]);
        update_appearance_preview(dialog, state);
    } else if (command == IDC_STRIP_PRESET && notification == CBN_SELCHANGE) {
        const LRESULT index = SendDlgItemMessageW(dialog, IDC_STRIP_PRESET, CB_GETCURSEL, 0, 0);
        if (index > 0 && index <= (LRESULT)(sizeof(appearance_presets) / sizeof(*appearance_presets)))
            fill_appearance_dialog(dialog, state, &appearance_presets[index - 1]);
        update_appearance_preview(dialog, state);
    } else if ((command >= IDC_STRIP_FONT_SIZE && command <= IDC_STRIP_ROW_GAP && notification == EN_CHANGE) ||
               (command >= IDC_STRIP_UPLOAD && command <= IDC_STRIP_TOTAL && notification == BN_CLICKED) ||
               (command >= IDC_STRIP_ROWS && command <= IDC_STRIP_WEIGHT && notification == CBN_SELCHANGE)) {
        SendDlgItemMessageW(dialog, IDC_STRIP_PRESET, CB_SETCURSEL, 0, 0);
        update_appearance_preview(dialog, state);
    } else if (command == IDOK || command == IDC_STRIP_APPLY) {
        StripAppearance next;
        if (!read_appearance_dialog(dialog, &next)) return TRUE;
        if (!strip_appearance_equal(&next, &app.appearance)) {
            app.appearance = next;
            app.placed_layout = NULL;
            position_strip();
            repaint();
            if (!save_setting(APPEARANCE_VALUE, &next, sizeof(next)))
                MessageBoxW(dialog, L"Appearance was applied for this run. "
                            L"Windows couldn't save it.", APP_NAME, MB_OK | MB_ICONINFORMATION);
        }
        if (command == IDOK) EndDialog(dialog, IDOK);
    } else {
        return FALSE;
    }
    return TRUE;
}

enum { FIRST_ADAPTER_ITEM = ADAPTER_SINGLE + 1 };

static LRESULT add_adapter_choice(HWND combo, const wchar_t *label, LPARAM data)
{
    const LRESULT item = SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)label);
    if (item >= 0) SendMessageW(combo, CB_SETITEMDATA, item, data);
    return item;
}

static void open_adapter_choices(HWND dialog, const MIB_IF_TABLE2 *table)
{
    const HWND combo = GetDlgItem(dialog, IDC_NETWORK_ADAPTER);
    LRESULT selected = add_adapter_choice(combo, L"All adapters", ADAPTER_ALL);
    const LRESULT automatic = add_adapter_choice(combo, L"Auto select", ADAPTER_AUTO);
    if (app.monitor.adapter.mode == ADAPTER_AUTO) selected = automatic;
    bool found = false;
    if (table) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2 *row = &table->Table[i];
            if (!monitor_adapter_supported(row) || IsEqualGUID(&row->InterfaceGuid, &(GUID){0}))
                continue;
            wchar_t label[IF_MAX_STRING_SIZE + 24];
            swprintf(label, sizeof(label) / sizeof(*label), L"%ls%ls", monitor_adapter_name(row),
                     monitor_adapter_active(row) ? L"" : L" (disconnected)");
            const LRESULT item = add_adapter_choice(combo, label, (LPARAM)i + FIRST_ADAPTER_ITEM);
            if (item >= 0 && app.monitor.adapter.mode == ADAPTER_SINGLE &&
                IsEqualGUID(&row->InterfaceGuid, &app.monitor.adapter.guid)) {
                selected = item;
                found = true;
            }
        }
    }
    if (app.monitor.adapter.mode == ADAPTER_SINGLE && !found)
        selected = add_adapter_choice(combo, L"Saved adapter (unavailable)", ADAPTER_SINGLE);
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
    EnableWindow(GetDlgItem(dialog, IDOK), selected >= 0);
}

static INT_PTR CALLBACK adapter_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        open_adapter_choices(dialog, (const MIB_IF_TABLE2 *)lparam);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    if (LOWORD(wparam) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (LOWORD(wparam) != IDOK) return FALSE;

    const HWND combo = GetDlgItem(dialog, IDC_NETWORK_ADAPTER);
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected < 0) return TRUE;
    const LRESULT item = SendMessageW(combo, CB_GETITEMDATA, selected, 0);
    if (item < 0) return TRUE;
    AdapterSelection next = {.mode = (DWORD)item};
    if (item == ADAPTER_SINGLE) {
        next = app.monitor.adapter;
    } else if (item >= FIRST_ADAPTER_ITEM) {
        const MIB_IF_TABLE2 *table = (const MIB_IF_TABLE2 *)GetWindowLongPtrW(dialog, DWLP_USER);
        const size_t row = (size_t)(item - FIRST_ADAPTER_ITEM);
        if (!table || row >= table->NumEntries) return TRUE;
        next = (AdapterSelection){ADAPTER_SINGLE, table->Table[row].InterfaceGuid};
    }
    if (monitor_set_adapter(&app.monitor, &next) &&
        !save_setting(ADAPTER_VALUE, &next, sizeof(next)))
        MessageBoxW(dialog, L"Adapter selection applies for this run. Windows couldn't save it.",
                    APP_NAME, MB_OK | MB_ICONINFORMATION);
    EndDialog(dialog, IDOK);
    return TRUE;
}

static void handle_command(UINT command)
{
    switch (command) {
    case SHOW_DASHBOARD:
        show_dashboard();
        break;
    case STRIP_APPEARANCE: {
        AppearanceDialog dialog = {0};
        DialogBoxParamW(app.instance, MAKEINTRESOURCEW(IDD_STRIP_APPEARANCE),
                        app.dashboard, appearance_proc, (LPARAM)&dialog);
        view_strip_layout_close(&dialog.preview_layout);
        view_strip_surface_close(&dialog.preview_surface);
        break;
    }
    case NETWORK_ADAPTER: {
        PMIB_IF_TABLE2 table = monitor_adapter_table(&app.monitor);
        const INT_PTR result = DialogBoxParamW(app.instance, MAKEINTRESOURCEW(IDD_NETWORK_ADAPTER),
                                               app.dashboard, adapter_proc, (LPARAM)table);
        if (table) app.monitor.release_table(table);
        if (result == IDOK) refresh();
        break;
    }
    case TOGGLE_STRIP:
        app.strip_enabled = !app.strip_enabled;
        if (app.strip_enabled) request_layout(true);
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

static void CALLBACK foreground_changed(HWINEVENTHOOK hook, DWORD event, HWND window,
                                         LONG object, LONG child, DWORD thread, DWORD time)
{
    (void)hook; (void)event; (void)window; (void)object; (void)child; (void)thread; (void)time;
    PostMessageW(app.dashboard, FOREGROUND_CHANGED_EVENT, 0, 0);
}

static LRESULT CALLBACK dashboard_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (app.taskbar_created && message == app.taskbar_created) {
        app.tray_added = false;
        add_tray(window);
        request_layout(true);
        position_strip();
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        app.adapter_button = CreateWindowExW(0, L"BUTTON", L"&Network adapter…",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  0, 0, 0, 0, window, (HMENU)(INT_PTR)NETWORK_ADAPTER, app.instance, NULL);
        app.strip_button = CreateWindowExW(0, L"BUTTON", L"&Hide taskbar strip",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                0, 0, 0, 0, window, (HMENU)(INT_PTR)TOGGLE_STRIP, app.instance, NULL);
        app.pause_button = CreateWindowExW(0, L"BUTTON", L"&Pause sampling",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                0, 0, 0, 0, window, (HMENU)(INT_PTR)TOGGLE_PAUSE, app.instance, NULL);
        app.appearance_button = CreateWindowExW(0, L"BUTTON", L"Strip &appearance…",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                0, 0, 0, 0, window, (HMENU)(INT_PTR)STRIP_APPEARANCE, app.instance, NULL);
        if (!app.adapter_button || !app.strip_button || !app.pause_button || !app.appearance_button)
            return -1;
        layout_buttons(window);
        return 0;
    case WM_PAINT: {
        const wchar_t *strip_status = NULL;
        if (app.strip_enabled) {
            if (!IsWindowVisible(app.strip))
                strip_status = L"Taskbar strip is waiting for a clear space.";
            else if (!app.strip_docked)
                strip_status = L"Taskbar attachment unavailable. Using an overlay.";
        }
        view_dashboard(window, &app.dashboard_fonts, &app.monitor, app.paused, strip_status);
        return 0;
    }
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
    case WM_THEMECHANGED:
        view_strip_theme(&app.strip_surface);
        request_layout(true);
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
                request_layout(true);
                refresh();
            }
        }
        return TRUE;
    case TASKBAR_LAYOUT_EVENT:
    case FOREGROUND_CHANGED_EVENT:
        position_strip();
        return 0;
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
        if (app.foreground_hook) {
            UnhookWinEvent(app.foreground_hook);
            app.foreground_hook = NULL;
        }
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
        ValidateRect(window, NULL);
        return 0;
    case WM_NCDESTROY:
        if (app.strip == window) app.strip = NULL;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
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
    const int y = display.rcWork.bottom - height - edge;
    app.dashboard = CreateWindowExW(ex_style, DASHBOARD_CLASS, APP_NAME, style,
                       x > display.rcWork.left ? x : display.rcWork.left,
                       y > display.rcWork.top ? y : display.rcWork.top,
                       width, height, NULL, NULL, app.instance, NULL);
    if (!app.dashboard) return false;

    app.layout_started = taskbar_layout_start(app.dashboard);
    if (!app.layout_started) {
        app.strip_enabled = false;
        EnableWindow(app.strip_button, FALSE);
        SetWindowTextW(app.strip_button, L"Strip unavailable");
    } else {
        app.foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                             NULL, foreground_changed, 0, 0,
                                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
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
    app.appearance = load_appearance();
    view_strip_theme(&app.strip_surface);
    app.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    monitor_init(&app.monitor);
    const AdapterSelection adapter = load_adapter();
    monitor_set_adapter(&app.monitor, &adapter);

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
    taskbar_layout_stop();
    view_fonts_close(&app.dashboard_fonts);
    view_strip_layout_close(&app.strip_layouts[0]);
    view_strip_layout_close(&app.strip_layouts[1]);
    view_strip_surface_close(&app.strip_surface);
    monitor_close(&app.monitor);
    if (single_instance) CloseHandle(single_instance);
    return exit_code;
}
