#ifndef STILLMETER_TASKBAR_H
#define STILLMETER_TASKBAR_H

#include <stdbool.h>
#include <windows.h>

#define TASKBAR_LAYOUT_EVENT (WM_APP + 3)

bool taskbar_layout_start(HWND recipient);
void taskbar_layout_request(void);
void taskbar_layout_stop(void);
bool taskbar_layout_place(HWND window, const RECT *bounds, const RECT *visible,
                          int width, int height, int padding, bool align_right, RECT *slot);

bool taskbar_find_gap(const RECT *visible, const RECT *occupied, unsigned count,
                      int width, int height, int padding, bool align_right, RECT *slot);

#endif
