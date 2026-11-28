#ifndef STILLMETER_MONITOR_H
#define STILLMETER_MONITOR_H

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <pdh.h>

#include "meter.h"

typedef NETIO_STATUS (WINAPI *ReadInterfaceTable)(PMIB_IF_TABLE2 *);
typedef NETIO_STATUS (WINAPI *ReadRouteTable)(ADDRESS_FAMILY, PMIB_IPFORWARD_TABLE2 *);
typedef NETIO_STATUS (WINAPI *ReadIpInterface)(PMIB_IPINTERFACE_ROW);
typedef VOID (WINAPI *ReleaseMibTable)(PVOID);

enum { ADAPTER_ALL, ADAPTER_AUTO, ADAPTER_SINGLE };

typedef struct {
    DWORD mode;
    GUID guid;
} AdapterSelection;

_Static_assert(sizeof(AdapterSelection) == 20, "Adapter settings format must be stable");

typedef struct {
    TrafficRate traffic;
    TrafficHistory history;
    int cpu;
    int memory;
    int gpu;
    double cpu_mhz;
    double session_bytes;
    PDH_HQUERY local_query;
    PDH_HCOUNTER gpu_counter;
    PDH_HCOUNTER frequency_counter;
    void *gpu_samples;
    DWORD gpu_capacity;
    uint64_t memory_used;
    uint64_t memory_total;
    bool network_readable;
    size_t active_adapters;
    AdapterSelection adapter;
    wchar_t adapter_name[IF_MAX_STRING_SIZE + 1];

    HMODULE ip_helper;
    ReadInterfaceTable read_interfaces;
    ReadRouteTable read_routes;
    ReadIpInterface read_ip_interface;
    ReleaseMibTable release_table;
    NET_LUID auto_luid;
    uint64_t route_checked_at;
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
bool monitor_set_adapter(Monitor *monitor, const AdapterSelection *selection);
PMIB_IF_TABLE2 monitor_adapter_table(const Monitor *monitor);
bool monitor_adapter_supported(const MIB_IF_ROW2 *row);
bool monitor_adapter_active(const MIB_IF_ROW2 *row);
const wchar_t *monitor_adapter_name(const MIB_IF_ROW2 *row);
void monitor_sample(Monitor *monitor);
void monitor_reset(Monitor *monitor);
void monitor_close(Monitor *monitor);

#endif
