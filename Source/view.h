#ifndef STILLMETER_VIEW_H
#define STILLMETER_VIEW_H

#include "monitor.h"
#include <string.h>

#define DASHBOARD_WIDTH 476
#define DASHBOARD_HEIGHT 512
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
    STRIP_UPLOAD = 1u << 0,
    STRIP_DOWNLOAD = 1u << 1,
    STRIP_CPU = 1u << 2,
    STRIP_MEMORY = 1u << 3,
    STRIP_GPU = 1u << 4,
    STRIP_CPU_FREQUENCY = 1u << 5,
    STRIP_TOTAL = 1u << 6,
    STRIP_METRIC_COUNT = 7,
    STRIP_ALL_METRICS = (1u << STRIP_METRIC_COUNT) - 1
};

typedef struct {
    DWORD font_size;
    DWORD column_gap;
    DWORD row_gap;
    DWORD metrics;
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
    int x;
    int width;
    int row;
} StripCell;

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
    StripCell cells[STRIP_METRIC_COUNT];
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
