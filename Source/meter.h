#ifndef STILLMETER_METER_H
#define STILLMETER_METER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define HISTORY_LENGTH 60
#define MAX_SAMPLE_GAP_MS 5000
#define RATE_TEXT_LENGTH 24

typedef struct {
    uint64_t id;
    uint64_t received;
    uint64_t sent;
} AdapterCounters;

typedef struct {
    double download;
    double upload;
    bool available;
} TrafficRate;

typedef struct {
    uint64_t idle;
    uint64_t kernel;
    uint64_t user;
} CpuCounters;

typedef struct {
    TrafficRate entries[HISTORY_LENGTH];
    unsigned next;
    unsigned count;
} TrafficHistory;

TrafficRate traffic_rate(const AdapterCounters *current, size_t current_count,
                         const AdapterCounters *previous, size_t previous_count,
                         uint64_t elapsed_ms);
int cpu_percent(CpuCounters current, CpuCounters previous, uint64_t elapsed_ms);
void format_rate(TrafficRate rate, bool upload, wchar_t output[RATE_TEXT_LENGTH]);
void history_append(TrafficHistory *history, TrafficRate rate);
TrafficRate history_at(const TrafficHistory *history, unsigned index);

#endif
