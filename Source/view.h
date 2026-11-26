#ifndef STILLMETER_VIEW_H
#define STILLMETER_VIEW_H

#include "monitor.h"

#define DASHBOARD_WIDTH 476
#define DASHBOARD_HEIGHT 474
#define TOOLTIP_LENGTH 128
#define STATUS_LENGTH 100

typedef struct {
    HFONT heading;
    HFONT regular;
    HFONT label;
    HFONT value;
    HFONT strip;
    UINT dpi;
} ViewFonts;

void view_fonts_init(ViewFonts *fonts, UINT dpi);
void view_fonts_close(ViewFonts *fonts);
bool view_high_contrast(void);
void view_dashboard(HWND window, const ViewFonts *fonts, const Monitor *monitor, bool paused);
void view_strip(HWND window, const ViewFonts *fonts, const Monitor *monitor, bool paused);
void view_tooltip(const Monitor *monitor, bool paused, wchar_t output[TOOLTIP_LENGTH]);

#endif
