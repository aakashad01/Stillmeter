#define COBJMACROS
#define INITGUID
#include "taskbar.h"
#include <uiautomationclient.h>

#define MAX_OCCUPIED 256
#define LAYOUT_MAX_AGE_MS 3000

typedef struct {
    HWND taskbar;
    RECT bounds;
    RECT occupied[MAX_OCCUPIED];
    unsigned count;
    bool complete;
    ULONGLONG measured;
} TaskbarLayout;

static SRWLOCK layout_lock = SRWLOCK_INIT;
static TaskbarLayout layout;
static HANDLE worker;
static HANDLE events[2];

bool taskbar_find_gap(const RECT *visible, const RECT *occupied, unsigned count,
                      int width, int height, int padding, RECT *slot)
{
    if (width <= 0 || height <= 0 || visible->bottom - visible->top < height)
        return false;

    const LONG top = visible->top + (visible->bottom - visible->top - height) / 2;
    const LONG right_limit = visible->right - padding;
    LONG left = visible->left + padding;
    while (left + width <= right_limit) {
        LONG next = left;
        for (unsigned i = 0; i < count; ++i) {
            const RECT *control = &occupied[i];
            if (control->top >= top + height || control->bottom <= top) continue;
            if (left + width > control->left - padding && left < control->right + padding)
                if (control->right + padding > next) next = control->right + padding;
        }
        if (next == left) {
            *slot = (RECT){left, top, left + width, top + height};
            return true;
        }
        left = next;
    }
    return false;
}

static bool add_control(TaskbarLayout *state, const RECT *bounds)
{
    RECT clipped;
    if (!IntersectRect(&clipped, &state->bounds, bounds)) return true;
    for (unsigned i = 0; i < state->count; ++i)
        if (EqualRect(&state->occupied[i], &clipped)) return true;
    if (state->count == MAX_OCCUPIED) return false;
    state->occupied[state->count++] = clipped;
    return true;
}

static bool reserves_space(CONTROLTYPEID type, BOOL focusable)
{
    switch (type) {
    case UIA_ButtonControlTypeId:
    case UIA_SplitButtonControlTypeId:
    case UIA_MenuItemControlTypeId:
    case UIA_EditControlTypeId:
    case UIA_CheckBoxControlTypeId:
    case UIA_HyperlinkControlTypeId:
    case UIA_ListItemControlTypeId:
    case UIA_TabItemControlTypeId:
    case UIA_TextControlTypeId:
    case UIA_ImageControlTypeId:
        return true;
    case UIA_CustomControlTypeId:
    case UIA_PaneControlTypeId:
        return focusable != FALSE;
    default:
        return false;
    }
}

static BOOL CALLBACK record_shell_control(HWND child, LPARAM context)
{
    TaskbarLayout *state = (TaskbarLayout *)context;
    wchar_t name[64];
    if (!IsWindowVisible(child) || !GetClassNameW(child, name, 64)) return TRUE;
    if (wcscmp(name, L"Start") && wcscmp(name, L"TrayNotifyWnd")) return TRUE;
    RECT bounds;
    if (GetWindowRect(child, &bounds) && !add_control(state, &bounds)) {
        state->complete = false;
        return FALSE;
    }
    return TRUE;
}

static bool record_automated_controls(IUIAutomation *automation, IUIAutomationCondition *condition,
                                      IUIAutomationCacheRequest *cache, TaskbarLayout *state)
{
    IUIAutomationElement *root = NULL;
    IUIAutomationElementArray *controls = NULL;
    bool complete = false;
    int count = 0;
    if (FAILED(IUIAutomation_ElementFromHandle(automation, state->taskbar, &root)) || !root)
        return false;
    if (SUCCEEDED(IUIAutomationElement_FindAllBuildCache(root, TreeScope_Descendants,
                                                        condition, cache, &controls)) &&
        controls && SUCCEEDED(IUIAutomationElementArray_get_Length(controls, &count))) {
        complete = true;
        for (int i = 0; i < count && complete; ++i) {
            IUIAutomationElement *element = NULL;
            if (FAILED(IUIAutomationElementArray_GetElement(controls, i, &element)) || !element) {
                complete = false;
                break;
            }

            BOOL offscreen = FALSE, focusable = FALSE;
            CONTROLTYPEID type = 0;
            RECT bounds;
            HRESULT type_status = IUIAutomationElement_get_CachedControlType(element, &type);
            IUIAutomationElement_get_CachedIsKeyboardFocusable(element, &focusable);
            IUIAutomationElement_get_CachedIsOffscreen(element, &offscreen);
            if (!offscreen && SUCCEEDED(type_status) && reserves_space(type, focusable)) {
                const HRESULT bounds_status =
                    IUIAutomationElement_get_CachedBoundingRectangle(element, &bounds);
                complete = SUCCEEDED(bounds_status) && add_control(state, &bounds);
            }
            IUIAutomationElement_Release(element);
        }
    }

    if (controls) IUIAutomationElementArray_Release(controls);
    IUIAutomationElement_Release(root);
    return complete && state->count != 0;
}

static TaskbarLayout read_layout(IUIAutomation *automation, IUIAutomationCondition *condition,
                                 IUIAutomationCacheRequest *cache)
{
    TaskbarLayout result = {0};
    result.taskbar = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!result.taskbar || !IsWindowVisible(result.taskbar) ||
        !GetWindowRect(result.taskbar, &result.bounds))
        return result;

    result.complete = record_automated_controls(automation, condition, cache, &result);
    if (result.complete) EnumChildWindows(result.taskbar, record_shell_control, (LPARAM)&result);
    RECT now;
    if (!GetWindowRect(result.taskbar, &now) || !EqualRect(&result.bounds, &now))
        result.complete = false;
    result.measured = GetTickCount64();
    return result;
}

static DWORD WINAPI layout_worker(void *context)
{
    const HWND recipient = context;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 0;
    IUIAutomation *automation = NULL;
    IUIAutomationCondition *condition = NULL;
    IUIAutomationCacheRequest *cache = NULL;

    if (SUCCEEDED(CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IUIAutomation, (void **)&automation)) &&
        SUCCEEDED(IUIAutomation_get_ControlViewCondition(automation, &condition)) &&
        SUCCEEDED(IUIAutomation_CreateCacheRequest(automation, &cache)) &&
        SUCCEEDED(IUIAutomationCacheRequest_AddProperty(cache, UIA_BoundingRectanglePropertyId)) &&
        SUCCEEDED(IUIAutomationCacheRequest_AddProperty(cache, UIA_ControlTypePropertyId)) &&
        SUCCEEDED(IUIAutomationCacheRequest_AddProperty(cache, UIA_IsKeyboardFocusablePropertyId)) &&
        SUCCEEDED(IUIAutomationCacheRequest_AddProperty(cache, UIA_IsOffscreenPropertyId))) {
        while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            TaskbarLayout next = read_layout(automation, condition, cache);
            AcquireSRWLockExclusive(&layout_lock);
            layout = next;
            ReleaseSRWLockExclusive(&layout_lock);
            if (WaitForSingleObject(events[0], 0) == WAIT_OBJECT_0) break;
            PostMessageW(recipient, TASKBAR_LAYOUT_EVENT, 0, 0);
        }
    }

    if (cache) IUIAutomationCacheRequest_Release(cache);
    if (condition) IUIAutomationCondition_Release(condition);
    if (automation) IUIAutomation_Release(automation);
    CoUninitialize();
    return 0;
}

bool taskbar_layout_start(HWND recipient)
{
    events[0] = CreateEventW(NULL, TRUE, FALSE, NULL);
    events[1] = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (events[0] && events[1])
        worker = CreateThread(NULL, 0, layout_worker, recipient, 0, NULL);
    if (worker) return true;
    if (events[0]) CloseHandle(events[0]);
    if (events[1]) CloseHandle(events[1]);
    return false;
}

void taskbar_layout_request(void)
{
    if (worker) SetEvent(events[1]);
}

void taskbar_layout_stop(void)
{
    if (!worker) return;
    SetEvent(events[0]);
    if (WaitForSingleObject(worker, 150) == WAIT_OBJECT_0) {
        CloseHandle(events[0]);
        CloseHandle(events[1]);
    }
    // A stalled shell provider may still use these handles until process exit.
    CloseHandle(worker);
    worker = NULL;
}

bool taskbar_layout_place(HWND window, const RECT *bounds, const RECT *visible,
                          int width, int height, int padding, RECT *slot)
{
    AcquireSRWLockShared(&layout_lock);
    const bool placed = layout.complete && layout.taskbar == window &&
                       EqualRect(bounds, &layout.bounds) &&
                       GetTickCount64() - layout.measured <= LAYOUT_MAX_AGE_MS &&
                       taskbar_find_gap(visible, layout.occupied, layout.count,
                                        width, height, padding, slot);
    ReleaseSRWLockShared(&layout_lock);
    return placed;
}
