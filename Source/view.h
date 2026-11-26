#ifndef STILLMETER_VIEW_H
#define STILLMETER_VIEW_H

#include "monitor.h"

#define STRIP_COLOR_KEY RGB(255, 0, 128)

#define DASHBOARD_WIDTH 476
#define DASHBOARD_HEIGHT 474
#define TOOLTIP_LENGTH 128
#define STATUS_LENGTH 100

typedef struct {
    HFONT heading;
    HFONT regular;
    HFONT label;
    HFONT value;
    UINT dpi;
} ViewFonts;

typedef struct {
    DWORD font_size;
    DWORD column_gap;
    DWORD row_gap;
} StripAppearance;

typedef struct {
    HFONT font;
    StripAppearance appearance;
    UINT dpi;
    int height_limit;
    int width, height;
    int padding;
    int row_height;
    int row_gap;
    int column_gap;
    int column_width[4];
} StripLayout;

typedef struct {
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ original;
    int width, height;
    COLORREF text, separator;
} StripSurface;

bool view_strip_prepare(StripLayout *layout, const StripAppearance *appearance,
                        UINT dpi, int height_limit);
void view_strip_layout_close(StripLayout *layout);
void view_strip_surface_close(StripSurface *surface);
void view_strip_theme(StripSurface *surface);

void view_fonts_init(ViewFonts *fonts, UINT dpi);
void view_fonts_close(ViewFonts *fonts);
bool view_high_contrast(void);
void view_dashboard(HWND window, const ViewFonts *fonts, const Monitor *monitor, bool paused,
                    bool strip_unplaced);
void view_strip(HWND window, const StripLayout *layout, StripSurface *surface,
                const Monitor *monitor, bool paused);
void view_tooltip(const Monitor *monitor, bool paused, wchar_t output[TOOLTIP_LENGTH]);

#endif
