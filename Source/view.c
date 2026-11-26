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

static HFONT make_font(int pixels, int weight, UINT dpi)
{
    HFONT font = CreateFontW(-MulDiv(pixels, (int)dpi, 96), 0, 0, 0, weight,
                            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    return font ? font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

void view_fonts_close(ViewFonts *fonts)
{
    const HFONT stock = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    const HFONT handles[] = { fonts->heading, fonts->regular, fonts->label,
                              fonts->value, fonts->strip };
    for (unsigned i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i)
        if (handles[i] && handles[i] != stock) DeleteObject(handles[i]);
    *fonts = (ViewFonts){0};
}

void view_fonts_init(ViewFonts *fonts, UINT dpi)
{
    if (fonts->dpi == dpi) return;
    view_fonts_close(fonts);
    fonts->heading = make_font(25, FW_SEMIBOLD, dpi);
    fonts->regular = make_font(13, FW_NORMAL, dpi);
    fonts->label = make_font(12, FW_SEMIBOLD, dpi);
    fonts->value = make_font(26, FW_SEMIBOLD, dpi);
    fonts->strip = make_font(13, FW_SEMIBOLD, dpi);
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

void view_dashboard(HWND window, const ViewFonts *fonts, const Monitor *monitor, bool paused)
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
    text_at(dc, box(fonts, 23, 407, 429, 20), fonts->regular, colors.muted, status, DT_LEFT);
    paint_end(window, &buffer);
}

void view_strip(HWND window, const ViewFonts *fonts, const Monitor *monitor, bool paused)
{
    PaintBuffer buffer;
    paint_begin(window, &buffer);
    HDC dc = buffer.canvas;
    const Palette colors = palette(true);
    solid(dc, &buffer.bounds, colors.background);
    card(dc, fonts, buffer.bounds, colors);

    if (paused) {
        text_at(dc, box(fonts, 0, 0, 202, 43), fonts->strip, colors.muted,
                L"Stillmeter  \u00b7  Paused", DT_CENTER);
        paint_end(window, &buffer);
        return;
    }

    wchar_t download[RATE_TEXT_LENGTH], upload[RATE_TEXT_LENGTH], cpu[8], memory[8];
    format_rate(monitor->traffic, false, download);
    format_rate(monitor->traffic, true, upload);
    percent_text(monitor->cpu, cpu);
    percent_text(monitor->memory, memory);

    text_at(dc, box(fonts, 11, 4, 18, 17), fonts->strip, colors.download, L"\u2193", DT_LEFT);
    text_at(dc, box(fonts, 11, 22, 18, 17), fonts->strip, colors.upload, L"\u2191", DT_LEFT);
    text_at(dc, box(fonts, 30, 4, 85, 17), fonts->strip, colors.text, download, DT_RIGHT);
    text_at(dc, box(fonts, 30, 22, 85, 17), fonts->strip, colors.text, upload, DT_RIGHT);
    line(dc, px(fonts, 125), px(fonts, 7), px(fonts, 125), px(fonts, 36), colors.border);
    text_at(dc, box(fonts, 134, 4, 31, 17), fonts->label, colors.muted, L"CPU", DT_LEFT);
    text_at(dc, box(fonts, 134, 22, 31, 17), fonts->label, colors.muted, L"RAM", DT_LEFT);
    text_at(dc, box(fonts, 162, 4, 31, 17), fonts->strip, colors.text, cpu, DT_RIGHT);
    text_at(dc, box(fonts, 162, 22, 31, 17), fonts->strip, colors.text, memory, DT_RIGHT);
    paint_end(window, &buffer);
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
