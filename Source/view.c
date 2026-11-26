#include "view.h"

typedef struct {
    COLORREF background;
    COLORREF surface;
    COLORREF border;
    COLORREF text;
    COLORREF muted;
    COLORREF download;
    COLORREF upload;
} Palette;

typedef struct {
    PAINTSTRUCT paint;
    HDC window_dc;
    HDC canvas;
    HBITMAP bitmap;
    HGDIOBJ old_bitmap;
    RECT bounds;
} PaintBuffer;

static HFONT make_font(int pixels, int weight, UINT dpi, DWORD quality)
{
    HFONT font = CreateFontW(-MulDiv(pixels, (int)dpi, 96), 0, 0, 0, weight,
                            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, quality,
                            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    return font ? font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

void view_fonts_close(ViewFonts *fonts)
{
    const HFONT stock = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    const HFONT handles[] = { fonts->heading, fonts->regular, fonts->label,
                              fonts->value };
    for (unsigned i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i)
        if (handles[i] && handles[i] != stock) DeleteObject(handles[i]);
    *fonts = (ViewFonts){0};
}

void view_fonts_init(ViewFonts *fonts, UINT dpi)
{
    if (fonts->dpi == dpi) return;
    view_fonts_close(fonts);
    fonts->heading = make_font(25, FW_SEMIBOLD, dpi, CLEARTYPE_QUALITY);
    fonts->regular = make_font(13, FW_NORMAL, dpi, CLEARTYPE_QUALITY);
    fonts->label = make_font(12, FW_SEMIBOLD, dpi, CLEARTYPE_QUALITY);
    fonts->value = make_font(26, FW_SEMIBOLD, dpi, CLEARTYPE_QUALITY);
    fonts->dpi = dpi;
}

bool view_high_contrast(void)
{
    HIGHCONTRASTW settings = {.cbSize = sizeof(settings)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, settings.cbSize, &settings, 0) &&
           (settings.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

static Palette palette(bool dark)
{
    if (view_high_contrast()) {
        const COLORREF background = GetSysColor(COLOR_WINDOW);
        const COLORREF text = GetSysColor(COLOR_WINDOWTEXT);
        return (Palette){background, background, text, text, text,
                         text, GetSysColor(COLOR_HOTLIGHT)};
    }
    if (dark) {
        return (Palette){RGB(18, 35, 47), RGB(18, 35, 47), RGB(53, 76, 90),
                         RGB(244, 250, 253), RGB(177, 197, 210),
                         RGB(111, 230, 200), RGB(162, 198, 255)};
    }
    return (Palette){RGB(244, 247, 249), RGB(255, 255, 255), RGB(209, 222, 229),
                     RGB(19, 40, 53), RGB(75, 100, 116),
                     RGB(0, 114, 94), RGB(43, 89, 175)};
}

static void paint_begin(HWND window, PaintBuffer *buffer)
{
    *buffer = (PaintBuffer){0};
    buffer->window_dc = BeginPaint(window, &buffer->paint);
    GetClientRect(window, &buffer->bounds);
    buffer->canvas = CreateCompatibleDC(buffer->window_dc);
    if (buffer->canvas) {
        buffer->bitmap = CreateCompatibleBitmap(buffer->window_dc,
                                                 buffer->bounds.right, buffer->bounds.bottom);
        if (buffer->bitmap)
            buffer->old_bitmap = SelectObject(buffer->canvas, buffer->bitmap);
        if (!buffer->old_bitmap) {
            if (buffer->bitmap) DeleteObject(buffer->bitmap);
            DeleteDC(buffer->canvas);
            buffer->canvas = NULL;
        }
    }
    if (!buffer->canvas) buffer->canvas = buffer->window_dc;
    SetBkMode(buffer->canvas, TRANSPARENT);
}

static void paint_end(HWND window, PaintBuffer *buffer)
{
    if (buffer->canvas != buffer->window_dc) {
        const RECT update = buffer->paint.rcPaint;
        BitBlt(buffer->window_dc, update.left, update.top,
               update.right - update.left, update.bottom - update.top,
               buffer->canvas, update.left, update.top, SRCCOPY);
        SelectObject(buffer->canvas, buffer->old_bitmap);
        DeleteObject(buffer->bitmap);
        DeleteDC(buffer->canvas);
    }
    EndPaint(window, &buffer->paint);
}

static int px(const ViewFonts *fonts, int value)
{
    return MulDiv(value, (int)fonts->dpi, 96);
}

static RECT box(const ViewFonts *fonts, int x, int y, int width, int height)
{
    return (RECT){px(fonts, x), px(fonts, y), px(fonts, x + width), px(fonts, y + height)};
}

static void solid(HDC dc, const RECT *rect, COLORREF color)
{
    SetDCBrushColor(dc, color);
    FillRect(dc, rect, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void card(HDC dc, const ViewFonts *fonts, RECT rect, Palette colors)
{
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCPenColor(dc, colors.border);
    SetDCBrushColor(dc, colors.surface);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, px(fonts, 13), px(fonts, 13));
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
}

static void text_at(HDC dc, RECT rect, HFONT font, COLORREF color,
                    const wchar_t *value, UINT align)
{
    HGDIOBJ old_font = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, value, -1, &rect,
              align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
}

static void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color)
{
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, color);
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
    SelectObject(dc, old_pen);
}

static void percent_text(int percent, wchar_t output[8])
{
    if (percent < 0) {
        wcscpy(output, L"\u2014");
    } else {
        swprintf(output, 8, L"%d%%", percent);
    }
}

static void draw_chart_series(HDC dc, const ViewFonts *fonts, RECT plot,
                              const TrafficHistory *history, double ceiling,
                              bool upload, COLORREF color)
{
    HPEN pen = CreatePen(upload ? PS_DASH : PS_SOLID, upload ? 1 : px(fonts, 2), color);
    if (!pen) return;
    HGDIOBJ old_pen = SelectObject(dc, pen);
    bool connected = false;
    for (unsigned i = 0; i < history->count; ++i) {
        const TrafficRate sample = history_at(history, i);
        if (!sample.available) {
            connected = false;
            continue;
        }
        const int column = (int)(HISTORY_LENGTH - history->count + i);
        const double value = upload ? sample.upload : sample.download;
        const int x = plot.left + MulDiv(column, plot.right - plot.left, HISTORY_LENGTH - 1);
        const int y = plot.bottom - (int)(value * (plot.bottom - plot.top) / ceiling + 0.5);
        if (connected) {
            LineTo(dc, x, y);
        } else {
            SetPixelV(dc, x, y, color);
            MoveToEx(dc, x, y, NULL);
        }
        connected = true;
    }
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

static void draw_traffic(HDC dc, const ViewFonts *fonts, Palette colors,
                         const Monitor *monitor, bool paused)
{
    wchar_t download[RATE_TEXT_LENGTH], upload[RATE_TEXT_LENGTH];
    format_rate(monitor->traffic, false, download);
    format_rate(monitor->traffic, true, upload);
    const RECT bounds = box(fonts, 22, 87, 432, 215);
    card(dc, fonts, bounds, colors);
    text_at(dc, box(fonts, 38, 103, 190, 18), fonts->label, colors.download,
            L"\u2193  DOWNLOAD", DT_LEFT);
    text_at(dc, box(fonts, 256, 103, 180, 18), fonts->label, colors.upload,
            L"\u2191  UPLOAD", DT_LEFT);
    text_at(dc, box(fonts, 38, 120, 192, 37), fonts->value, colors.text, download, DT_LEFT);
    text_at(dc, box(fonts, 256, 120, 180, 37), fonts->value, colors.text, upload, DT_LEFT);

    line(dc, px(fonts, 38), px(fonts, 169), px(fonts, 438), px(fonts, 169), colors.border);
    text_at(dc, box(fonts, 38, 179, 226, 18), fonts->label, colors.text,
            L"NETWORK TRAFFIC", DT_LEFT);
    text_at(dc, box(fonts, 302, 179, 136, 18), fonts->regular, colors.muted,
            L"Last 60 seconds", DT_RIGHT);

    const RECT plot = box(fonts, 38, 208, 319, 75);
    for (int i = 0; i <= 2; ++i) {
        const int y = plot.top + i * (plot.bottom - plot.top) / 2;
        line(dc, plot.left, y, plot.right + 1, y, colors.border);
    }

    double ceiling = 1000.0;
    bool has_samples = false;
    for (unsigned i = 0; i < monitor->history.count; ++i) {
        const TrafficRate rate = history_at(&monitor->history, i);
        if (!rate.available) continue;
        has_samples = true;
        if (rate.download > ceiling) ceiling = rate.download;
        if (rate.upload > ceiling) ceiling = rate.upload;
    }

    if (has_samples) {
        draw_chart_series(dc, fonts, plot, &monitor->history, ceiling, true, colors.upload);
        draw_chart_series(dc, fonts, plot, &monitor->history, ceiling, false, colors.download);
        wchar_t maximum[RATE_TEXT_LENGTH];
        format_rate((TrafficRate){.download = ceiling, .available = true}, false, maximum);
        text_at(dc, box(fonts, 362, 199, 76, 19), fonts->regular, colors.muted, maximum, DT_RIGHT);
        text_at(dc, box(fonts, 362, 274, 76, 19), fonts->regular, colors.muted, L"0 B/s", DT_RIGHT);
    } else {
        const RECT caption = box(fonts, 97, 235, 197, 20);
        solid(dc, &caption, colors.surface);
        const wchar_t *message = paused ? L"Sampling paused" : L"Waiting for readings";
        if (!paused && monitor->network_readable && !monitor->active_adapters)
            message = L"No active Wi-Fi or Ethernet";
        if (!paused && !monitor->network_readable)
            message = L"Network counters unavailable";
        text_at(dc, caption, fonts->regular, colors.muted, message, DT_CENTER);
    }
}

static void draw_load_card(HDC dc, const ViewFonts *fonts, Palette colors, int x,
                           const wchar_t *name, const wchar_t *details, int load,
                           COLORREF accent)
{
    const RECT bounds = box(fonts, x, 314, 210, 82);
    card(dc, fonts, bounds, colors);
    text_at(dc, box(fonts, x + 15, 326, 107, 16), fonts->label, colors.muted, name, DT_LEFT);

    wchar_t percent[8];
    percent_text(load, percent);
    text_at(dc, box(fonts, x + 15, 348, 70, 26), fonts->value, colors.text, percent, DT_LEFT);
    text_at(dc, box(fonts, x + 83, 352, 111, 22), fonts->regular, colors.muted, details, DT_RIGHT);

    const RECT track = box(fonts, x + 15, 380, 180, 4);
    solid(dc, &track, colors.border);
    if (load > 0) {
        RECT filled = track;
        filled.right = filled.left + MulDiv(filled.right - filled.left, load, 100);
        solid(dc, &filled, accent);
    }
}

void view_dashboard(HWND window, const ViewFonts *fonts, const Monitor *monitor, bool paused,
                    bool strip_unplaced)
{
    PaintBuffer buffer;
    paint_begin(window, &buffer);
    HDC dc = buffer.canvas;
    const Palette colors = palette(false);
    solid(dc, &buffer.bounds, colors.background);

    text_at(dc, box(fonts, 22, 16, 325, 36), fonts->heading, colors.text,
            L"System monitor", DT_LEFT);
    text_at(dc, box(fonts, 23, 52, 326, 19), fonts->regular, colors.muted,
            L"Live traffic and system load", DT_LEFT);
    text_at(dc, box(fonts, 331, 23, 123, 26), fonts->label,
            paused ? colors.muted : colors.download,
            paused ? L"\u25CB  PAUSED" : L"\u25CF  SAMPLING", DT_RIGHT);

    draw_traffic(dc, fonts, colors, monitor, paused);

    wchar_t memory_details[32] = L"Physical RAM";
    if (monitor->memory >= 0) {
        const double gib = 1073741824.0;
        swprintf(memory_details, 32, L"%.1f / %.1f GiB",
                 (double)monitor->memory_used / gib, (double)monitor->memory_total / gib);
    }
    draw_load_card(dc, fonts, colors, 22, L"CPU", L"System load", monitor->cpu, colors.download);
    draw_load_card(dc, fonts, colors, 244, L"MEMORY", memory_details, monitor->memory, colors.upload);

    wchar_t status[STATUS_LENGTH] = L"Local Windows counters";
    if (!paused && monitor->network_readable) {
        const unsigned long count = (unsigned long)monitor->active_adapters;
        swprintf(status, STATUS_LENGTH, L"Local counters  \u00b7  %lu active %ls", count,
                 count == 1 ? L"adapter" : L"adapters");
    }
    text_at(dc, box(fonts, 23, 407, 429, 20), fonts->regular, colors.muted,
            strip_unplaced ? L"Taskbar strip is waiting for a clear space." : status, DT_LEFT);
    paint_end(window, &buffer);
}

#define STRIP_SUPERSAMPLE 2

static void delete_strip_font(HFONT font)
{
    if (font && font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(font);
}

void view_strip_layout_close(StripLayout *layout)
{
    delete_strip_font(layout->font);
    *layout = (StripLayout){0};
}

bool view_strip_prepare(StripLayout *layout, const StripAppearance *appearance,
                        UINT dpi, int height_limit)
{
    if (layout->dpi == dpi && layout->height_limit == height_limit &&
        layout->appearance.font_size == appearance->font_size &&
        layout->appearance.column_gap == appearance->column_gap &&
        layout->appearance.row_gap == appearance->row_gap)
        return layout->font != NULL;

    view_strip_layout_close(layout);
    layout->appearance = *appearance;
    layout->dpi = dpi;
    layout->height_limit = height_limit;
    layout->padding = MulDiv(4, (int)dpi, 96);
    const int inset = MulDiv(1, (int)dpi, 96);
    layout->row_gap = MulDiv((int)appearance->row_gap, (int)dpi, 96);
    layout->column_gap = MulDiv((int)appearance->column_gap, (int)dpi, 96);
    HDC dc = GetDC(NULL);
    if (!dc) return false;

    static const wchar_t *const samples[4][3] = {
        {L"\u2191 999.9 MB/s", L"\u2193 999.9 GB/s", L"\u2193 999.9 TB/s"},
        {L"CPU 100%", L"RAM 100%", NULL},
        {L"GPU 100%", L"CPU freq 99.99 GHz", NULL},
        {L"Total traffic", L"999.9 TB", NULL}
    };
    int size = MulDiv((int)appearance->font_size, (int)dpi, 96);
    const int minimum = MulDiv(9, (int)dpi, 96);
    for (; size >= minimum; --size) {
        HFONT font = make_font(size * STRIP_SUPERSAMPLE, FW_SEMIBOLD, 96, ANTIALIASED_QUALITY);
        HGDIOBJ old_font = SelectObject(dc, font);
        TEXTMETRICW metrics;
        if (old_font && GetTextMetricsW(dc, &metrics)) {
            int gap = layout->row_gap;
            const int row_height = (metrics.tmHeight + STRIP_SUPERSAMPLE - 1) / STRIP_SUPERSAMPLE;
            const int remaining = height_limit - 2 * (row_height + inset);
            // Prefer the requested row spacing while keeping text legible.
            if (remaining >= gap || (size == minimum && remaining >= 0)) {
                if (gap > remaining) gap = remaining;
                layout->row_gap = gap;
                layout->row_height = row_height;
                layout->height = 2 * (layout->row_height + inset) + gap;
                layout->width = 2 * layout->padding + 3 * layout->column_gap;
                for (unsigned col = 0; col < 4; ++col) {
                    int width = 0;
                    for (unsigned i = 0; i < 3 && samples[col][i]; ++i) {
                        const wchar_t *sample = samples[col][i];
                        SIZE extent;
                        if (GetTextExtentPoint32W(dc, sample, (int)wcslen(sample), &extent) &&
                            extent.cx > width)
                            width = extent.cx;
                    }
                    layout->column_width[col] = (width + STRIP_SUPERSAMPLE - 1) / STRIP_SUPERSAMPLE + inset * 2;
                    layout->width += layout->column_width[col];
                }
                layout->font = font;
            }
        }
        if (old_font) SelectObject(dc, old_font);
        if (layout->font) break;
        delete_strip_font(font);
    }

    ReleaseDC(NULL, dc);
    return layout->font != NULL;
}

static void strip_bitmap_close(StripBitmap *bitmap)
{
    if (bitmap->original) SelectObject(bitmap->dc, bitmap->original);
    if (bitmap->bitmap) DeleteObject(bitmap->bitmap);
    if (bitmap->dc) DeleteDC(bitmap->dc);
    *bitmap = (StripBitmap){0};
}

void view_strip_surface_close(StripSurface *surface)
{
    strip_bitmap_close(&surface->mask);
    strip_bitmap_close(&surface->image);
    surface->width = 0;
    surface->height = 0;
}

static bool strip_bitmap_create(StripBitmap *bitmap, int width, int height)
{
    const BITMAPINFO info = {.bmiHeader = {
        .biSize = sizeof(BITMAPINFOHEADER), .biWidth = width, .biHeight = -height,
        .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB
    }};
    void *pixels = NULL;
    bitmap->dc = CreateCompatibleDC(NULL);
    if (!bitmap->dc) return false;
    bitmap->bitmap = CreateDIBSection(bitmap->dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!bitmap->bitmap || !pixels) return false;
    bitmap->original = SelectObject(bitmap->dc, bitmap->bitmap);
    if (!bitmap->original) return false;

    bitmap->pixels = pixels;
    SetBkMode(bitmap->dc, TRANSPARENT);
    return true;
}

static bool strip_surface_prepare(StripSurface *surface, int width, int height)
{
    if (surface->width == width && surface->height == height) return true;

    StripBitmap mask = {0}, image = {0};
    if (!strip_bitmap_create(&mask, width * STRIP_SUPERSAMPLE, height * STRIP_SUPERSAMPLE) ||
        !strip_bitmap_create(&image, width, height)) {
        strip_bitmap_close(&mask);
        strip_bitmap_close(&image);
        return false;
    }

    view_strip_surface_close(surface);
    surface->mask = mask;
    surface->image = image;
    surface->width = width;
    surface->height = height;
    return true;
}

void view_strip_theme(StripSurface *surface)
{
    COLORREF foreground;
    if (view_high_contrast()) {
        foreground = GetSysColor(COLOR_WINDOWTEXT);
    } else {
        DWORD light = 0, size = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
        foreground = light ? RGB(25, 25, 25) : RGB(250, 250, 250);
    }

    // UpdateLayeredWindow expects premultiplied color in BGRA byte order.
    for (DWORD alpha = 0; alpha < 256; ++alpha) {
        const DWORD red = (GetRValue(foreground) * alpha + 127) / 255;
        const DWORD green = (GetGValue(foreground) * alpha + 127) / 255;
        const DWORD blue = (GetBValue(foreground) * alpha + 127) / 255;
        surface->palette[alpha] = (alpha << 24) | (red << 16) | (green << 8) | blue;
    }
}

static void strip_metric(HDC dc, const StripLayout *layout,
                         int x, int y, int width, const wchar_t *label, const wchar_t *value)
{
    wchar_t text[48];
    swprintf(text, 48, L"%ls%ls", label, value);
    RECT row = {x * STRIP_SUPERSAMPLE, y * STRIP_SUPERSAMPLE,
                (x + width) * STRIP_SUPERSAMPLE, (y + layout->row_height) * STRIP_SUPERSAMPLE};
    text_at(dc, row, layout->font, RGB(255, 255, 255), text, DT_LEFT);
}

static void draw_strip_mask(HDC dc, const StripLayout *layout, const Monitor *monitor, bool paused)
{
    const RECT bounds = {0, 0, layout->width * STRIP_SUPERSAMPLE,
                         layout->height * STRIP_SUPERSAMPLE};
    solid(dc, &bounds, RGB(0, 0, 0));

    if (paused) {
        strip_metric(dc, layout, layout->padding,
                     (layout->height - layout->row_height) / 2,
                     layout->width - 2 * layout->padding, L"Stillmeter", L"  |  Sampling paused");
        return;
    }

    wchar_t download[RATE_TEXT_LENGTH], upload[RATE_TEXT_LENGTH], total[RATE_TEXT_LENGTH];
    wchar_t cpu[8], memory[8], gpu[8], frequency[24] = L"\u2014";
    format_rate(monitor->traffic, false, download);
    format_rate(monitor->traffic, true, upload);
    format_rate((TrafficRate){.download = monitor->session_bytes, .available = true}, false, total);
    wchar_t *per_second = wcsstr(total, L"/s");
    if (per_second) *per_second = L'\0';
    percent_text(monitor->cpu, cpu);
    percent_text(monitor->memory, memory);
    percent_text(monitor->gpu, gpu);
    if (monitor->cpu_mhz > 0.0)
        swprintf(frequency, 24, L"%.2f GHz", monitor->cpu_mhz / 1000.0);

    const wchar_t *const labels[] = {
        L"\u2191 ", L"\u2193 ", L"CPU ", L"RAM ", L"GPU ", L"CPU freq ", L"Total traffic", L""
    };
    const wchar_t *const values[] = {upload, download, cpu, memory, gpu, frequency, L"", total};
    const int top = (layout->height - 2 * layout->row_height - layout->row_gap) / 2;
    const int bottom = top + layout->row_height + layout->row_gap;
    int x = layout->padding;
    for (unsigned col = 0; col < 4; ++col) {
        if (col == 3 && layout->column_gap >= 4) {
            const int separator = x - layout->column_gap / 2;
            const RECT rule = {separator * STRIP_SUPERSAMPLE, top * STRIP_SUPERSAMPLE,
                               (separator + 1) * STRIP_SUPERSAMPLE,
                               (bottom + layout->row_height) * STRIP_SUPERSAMPLE};
            solid(dc, &rule, RGB(125, 125, 125));
        }
        const int width = layout->column_width[col];
        strip_metric(dc, layout, x, top, width, labels[2 * col], values[2 * col]);
        strip_metric(dc, layout, x, bottom, width,
                     labels[2 * col + 1], values[2 * col + 1]);
        x += width + layout->column_gap;
    }
}

bool view_strip(HWND window, const POINT *position, const StripLayout *layout,
                StripSurface *surface, const Monitor *monitor, bool paused)
{
    if (!layout || !layout->font || !strip_surface_prepare(surface, layout->width, layout->height))
        return false;

    draw_strip_mask(surface->mask.dc, layout, monitor, paused);
    GdiFlush();
    // Box-filter a 2x grayscale mask to get smooth coverage even for small glyphs.
    const int stride = surface->width * STRIP_SUPERSAMPLE;
    DWORD *pixel = surface->image.pixels;
    for (int y = 0; y < surface->height; ++y) {
        const DWORD *top = surface->mask.pixels + y * STRIP_SUPERSAMPLE * stride;
        const DWORD *bottom = top + stride;
        for (int x = 0; x < stride; x += STRIP_SUPERSAMPLE) {
            const DWORD coverage = (top[x] & 255) + (top[x + 1] & 255) +
                                   (bottom[x] & 255) + (bottom[x + 1] & 255);
            *pixel++ = surface->palette[(coverage + 2) / 4];
        }
    }

    POINT destination;
    if (position) destination = *position;
    SIZE size = {layout->width, layout->height};
    POINT source = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    return UpdateLayeredWindow(window, NULL, position ? &destination : NULL, &size, surface->image.dc,
                               &source, 0, &blend, ULW_ALPHA) != FALSE;
}

void view_tooltip(const Monitor *monitor, bool paused, wchar_t output[TOOLTIP_LENGTH])
{
    if (paused) {
        wcscpy(output, L"Stillmeter | Sampling paused");
        return;
    }
    wchar_t download[RATE_TEXT_LENGTH], upload[RATE_TEXT_LENGTH], cpu[8], memory[8];
    format_rate(monitor->traffic, false, download);
    format_rate(monitor->traffic, true, upload);
    percent_text(monitor->cpu, cpu);
    percent_text(monitor->memory, memory);
    swprintf(output, TOOLTIP_LENGTH, L"Stillmeter\nDownload %ls | Upload %ls\nCPU %ls | RAM %ls",
             download, upload, cpu, memory);
}
