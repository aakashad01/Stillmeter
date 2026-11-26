#ifndef STILLMETER_MONITOR_H
#define STILLMETER_MONITOR_H

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>

#include "meter.h"

typedef NETIO_STATUS (WINAPI *ReadInterfaceTable)(PMIB_IF_TABLE2 *);
typedef VOID (WINAPI *ReleaseInterfaceTable)(PVOID);

typedef struct {
    TrafficRate traffic;
    TrafficHistory history;
    int cpu;
    int memory;
    uint64_t memory_used;
    uint64_t memory_total;
    bool network_readable;
    size_t active_adapters;

    HMODULE ip_helper;
    ReadInterfaceTable read_interfaces;
    ReleaseInterfaceTable release_interfaces;
    AdapterCounters *previous;
    size_t previous_count;
    size_t previous_capacity;
    AdapterCounters *current;
    size_t current_capacity;
    CpuCounters previous_cpu;
    bool previous_cpu_valid;
    uint64_t last_tick;
} Monitor;

void monitor_init(Monitor *monitor);
void monitor_sample(Monitor *monitor);
void monitor_reset(Monitor *monitor);
void monitor_close(Monitor *monitor);

#endif
