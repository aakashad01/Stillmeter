#include "meter.h"

TrafficRate traffic_rate(const AdapterCounters *current, size_t current_count,
                         const AdapterCounters *previous, size_t previous_count,
                         uint64_t elapsed_ms)
{
    TrafficRate rate = {0};
    if (elapsed_ms == 0 || elapsed_ms > MAX_SAMPLE_GAP_MS) return rate;

    for (size_t i = 0; i < current_count; ++i) {
        const AdapterCounters *now = &current[i];
        for (size_t j = 0; j < previous_count; ++j) {
            const AdapterCounters *old = &previous[j];
            if (now->id != old->id) continue;
            if (now->received >= old->received && now->sent >= old->sent) {
                rate.download += (double)(now->received - old->received);
                rate.upload += (double)(now->sent - old->sent);
                rate.available = true;
            }
            break;
        }
    }
    rate.download *= 1000.0 / (double)elapsed_ms;
    rate.upload *= 1000.0 / (double)elapsed_ms;
    return rate;
}

int cpu_percent(CpuCounters current, CpuCounters previous, uint64_t elapsed_ms)
{
    if (elapsed_ms == 0 || elapsed_ms > MAX_SAMPLE_GAP_MS ||
        current.idle < previous.idle || current.kernel < previous.kernel ||
        current.user < previous.user) return -1;

    const double idle = (double)(current.idle - previous.idle);
    // Windows includes idle time in the kernel counter.
    const double total = (double)(current.kernel - previous.kernel) +
                         (double)(current.user - previous.user);
    if (total <= 0.0 || idle > total) return -1;
    return (int)((total - idle) * 100.0 / total + 0.5);
}

void format_rate(TrafficRate rate, bool upload, wchar_t output[RATE_TEXT_LENGTH])
{
    if (!rate.available) {
        wcscpy(output, L"\u2014");
        return;
    }

    static const wchar_t *const units[] = { L"B/s", L"kB/s", L"MB/s", L"GB/s", L"TB/s" };
    double value = upload ? rate.upload : rate.download;
    size_t unit = 0;
    while (value >= 999.5 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1000.0;
        ++unit;
    }
    // Format from fixed-point integers so readings are stable across C locales.
    if (unit != 0 && value < 99.95) {
        const unsigned tenths = (unsigned)(value * 10.0 + 0.5);
        swprintf(output, RATE_TEXT_LENGTH, L"%u.%u %ls", tenths / 10, tenths % 10, units[unit]);
    } else {
        const unsigned long long whole = (unsigned long long)(value + 0.5);
        swprintf(output, RATE_TEXT_LENGTH, L"%llu %ls", whole, units[unit]);
    }
}

void history_append(TrafficHistory *history, TrafficRate rate)
{
    history->entries[history->next] = rate;
    history->next = (history->next + 1) % HISTORY_LENGTH;
    if (history->count < HISTORY_LENGTH) ++history->count;
}

TrafficRate history_at(const TrafficHistory *history, unsigned index)
{
    const unsigned first = (history->next + HISTORY_LENGTH - history->count) % HISTORY_LENGTH;
    return history->entries[(first + index) % HISTORY_LENGTH];
}
