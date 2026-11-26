#ifndef STILLMETER_VIEW_H
#define STILLMETER_VIEW_H

#include "monitor.h"
#include <string.h>

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

enum {
    STRIP_NETWORK = 1, STRIP_SYSTEM = 2, STRIP_GRAPHICS = 4, STRIP_TOTAL = 8,
    STRIP_ALL_GROUPS = 15
};

typedef struct {
    DWORD font_size;
    DWORD column_gap;
    DWORD row_gap;
    DWORD groups;
    DWORD rows;
    DWORD align_right;
    DWORD weight;
} StripAppearance;

_Static_assert(sizeof(StripAppearance) == 7 * sizeof(DWORD), "Appearance format must be stable");

static inline bool strip_appearance_equal(const StripAppearance *a, const StripAppearance *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

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
    int column_width[7];
} StripLayout;

typedef struct {
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ original;
    DWORD *pixels;
} StripBitmap;

typedef struct {
    StripBitmap mask, image;
    int width, height;
    DWORD palette[256];
    COLORREF background;
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
                    const wchar_t *strip_status);
bool view_strip(HWND window, const POINT *position, const StripLayout *layout,
                StripSurface *surface, const Monitor *monitor, bool paused);
void view_strip_preview(HDC dc, RECT bounds, const StripLayout *layout, StripSurface *surface);
void view_tooltip(const Monitor *monitor, bool paused, wchar_t output[TOOLTIP_LENGTH]);

#endif
