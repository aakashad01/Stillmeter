#include "monitor.h"

#include <stdlib.h>
#include <string.h>
#include <pdhmsg.h>

#define AUTO_ROUTE_INTERVAL_MS 3000

static uint64_t file_time_value(FILETIME value)
{
    return ((uint64_t)value.dwHighDateTime << 32) | value.dwLowDateTime;
}

static void sample_cpu(Monitor *monitor, uint64_t elapsed_ms)
{
    FILETIME idle, kernel, user;
    monitor->cpu = -1;
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        monitor->previous_cpu_valid = false;
        return;
    }

    const CpuCounters now = {
        file_time_value(idle), file_time_value(kernel), file_time_value(user)
    };
    if (monitor->previous_cpu_valid)
        monitor->cpu = cpu_percent(now, monitor->previous_cpu, elapsed_ms);
    monitor->previous_cpu = now;
    monitor->previous_cpu_valid = true;
}

static void sample_memory(Monitor *monitor)
{
    MEMORYSTATUSEX state = {.dwLength = sizeof(state)};
    monitor->memory = -1;
    monitor->memory_used = 0;
    monitor->memory_total = 0;
    if (!GlobalMemoryStatusEx(&state) || state.ullTotalPhys == 0 ||
        state.ullAvailPhys > state.ullTotalPhys) return;
    monitor->memory_used = state.ullTotalPhys - state.ullAvailPhys;
    monitor->memory_total = state.ullTotalPhys;
    monitor->memory = (int)(100.0 * (double)monitor->memory_used /
                            (double)monitor->memory_total + 0.5);
}

bool monitor_adapter_supported(const MIB_IF_ROW2 *row)
{
    return (row->Type == IF_TYPE_ETHERNET_CSMACD || row->Type == IF_TYPE_IEEE80211) &&
           row->InterfaceAndOperStatusFlags.HardwareInterface &&
           !row->InterfaceAndOperStatusFlags.FilterInterface;
}

bool monitor_adapter_active(const MIB_IF_ROW2 *row)
{
    return monitor_adapter_supported(row) && row->OperStatus == IfOperStatusUp &&
           !row->InterfaceAndOperStatusFlags.NotMediaConnected;
}

const wchar_t *monitor_adapter_name(const MIB_IF_ROW2 *row)
{
    if (row->Alias[0]) return row->Alias;
    if (row->Description[0]) return row->Description;
    return row->Type == IF_TYPE_IEEE80211 ? L"Wi-Fi" : L"Ethernet";
}

PMIB_IF_TABLE2 monitor_adapter_table(const Monitor *monitor)
{
    PMIB_IF_TABLE2 table = NULL;
    return monitor->read_interfaces && monitor->read_interfaces(&table) == NO_ERROR
           ? table : NULL;
}

static const MIB_IF_ROW2 *active_interface(const MIB_IF_TABLE2 *table, NET_LUID luid)
{
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2 *row = &table->Table[i];
        if (row->InterfaceLuid.Value == luid.Value && monitor_adapter_active(row)) return row;
    }
    return NULL;
}

static const MIB_IF_ROW2 *default_route_adapter(const Monitor *monitor, const MIB_IF_TABLE2 *table)
{
    PMIB_IPFORWARD_TABLE2 routes = NULL;
    if (!monitor->read_routes || !monitor->read_ip_interface ||
        monitor->read_routes(AF_UNSPEC, &routes) != NO_ERROR || !routes)
        return NULL;

    const MIB_IF_ROW2 *best = NULL;
    uint64_t best_cost = 0;
    for (ULONG i = 0; i < routes->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2 *route = &routes->Table[i];
        if (route->DestinationPrefix.PrefixLength != 0 || route->Loopback) continue;
        const MIB_IF_ROW2 *row = active_interface(table, route->InterfaceLuid);
        if (!row) continue;

        MIB_IPINTERFACE_ROW ip = {.Family = route->DestinationPrefix.Prefix.si_family,
                                 .InterfaceLuid = route->InterfaceLuid};
        if (monitor->read_ip_interface(&ip) != NO_ERROR || ip.DisableDefaultRoutes) continue;
        const uint64_t cost = (uint64_t)route->Metric + ip.Metric;
        if (!best || cost < best_cost ||
            (cost == best_cost && row->InterfaceLuid.Value < best->InterfaceLuid.Value)) {
            best = row;
            best_cost = cost;
        }
    }
    monitor->release_table(routes);
    return best;
}

static const MIB_IF_ROW2 *auto_adapter(Monitor *monitor, const MIB_IF_TABLE2 *table,
                                      uint64_t now)
{
    const MIB_IF_ROW2 *first = NULL;
    const MIB_IF_ROW2 *previous = NULL;
    size_t count = 0;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2 *row = &table->Table[i];
        if (!monitor_adapter_active(row)) continue;
        ++count;
        if (!first || row->InterfaceLuid.Value < first->InterfaceLuid.Value) first = row;
        if (row->InterfaceLuid.Value == monitor->auto_luid.Value) previous = row;
    }
    if (count <= 1) {
        monitor->route_checked_at = 0;
        return first;
    }
    if (previous && monitor->route_checked_at && now - monitor->route_checked_at < AUTO_ROUTE_INTERVAL_MS)
        return previous;

    monitor->route_checked_at = now;
    const MIB_IF_ROW2 *best = default_route_adapter(monitor, table);
    if (best) return best;
    return previous ? previous : first;
}

static const MIB_IF_ROW2 *saved_adapter(const Monitor *monitor, const MIB_IF_TABLE2 *table)
{
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2 *row = &table->Table[i];
        if (monitor_adapter_supported(row) && IsEqualGUID(&row->InterfaceGuid, &monitor->adapter.guid))
            return row;
    }
    return NULL;
}

static void clear_network_reading(Monitor *monitor)
{
    monitor->traffic = (TrafficRate){0};
    monitor->active_adapters = 0;
    monitor->network_readable = false;
    monitor->adapter_name[0] = L'\0';
}

static void sample_network(Monitor *monitor, uint64_t elapsed_ms, uint64_t now)
{
    clear_network_reading(monitor);
    PMIB_IF_TABLE2 table = monitor_adapter_table(monitor);
    if (!table) {
        monitor->previous_count = 0;
        monitor->route_checked_at = 0;
        return;
    }

    const MIB_IF_ROW2 *selected = NULL;
    size_t count = 0;
    if (monitor->adapter.mode == ADAPTER_ALL) {
        for (ULONG i = 0; i < table->NumEntries; ++i)
            if (monitor_adapter_active(&table->Table[i])) ++count;
    } else {
        if (monitor->adapter.mode == ADAPTER_AUTO) {
            selected = auto_adapter(monitor, table, now);
            monitor->auto_luid = selected ? selected->InterfaceLuid : (NET_LUID){0};
        } else {
            selected = saved_adapter(monitor, table);
        }
        if (selected) {
            wcscpy(monitor->adapter_name, monitor_adapter_name(selected));
            if (monitor_adapter_active(selected)) count = 1;
        }
    }
    if (count > monitor->current_capacity) {
        AdapterCounters *entries = realloc(monitor->current, count * sizeof(*entries));
        if (!entries) {
            monitor->previous_count = 0;
            monitor->release_table(table);
            return;
        }
        monitor->current = entries;
        monitor->current_capacity = count;
    }

    size_t index = 0;
    for (ULONG i = 0; i < table->NumEntries && index < count; ++i) {
        const MIB_IF_ROW2 *row = &table->Table[i];
        if ((!selected || row == selected) && monitor_adapter_active(row)) {
            monitor->current[index++] = (AdapterCounters){
                row->InterfaceLuid.Value, row->InOctets, row->OutOctets
            };
        }
    }
    monitor->release_table(table);

    monitor->network_readable = true;
    monitor->active_adapters = count;
    monitor->traffic = traffic_rate(monitor->current, count, monitor->previous,
                                    monitor->previous_count, elapsed_ms);

    // Reuse both buffers for the next sample.
    AdapterCounters *entries = monitor->previous;
    const size_t capacity = monitor->previous_capacity;
    monitor->previous = monitor->current;
    monitor->previous_capacity = monitor->current_capacity;
    monitor->previous_count = count;
    monitor->current = entries;
    monitor->current_capacity = capacity;
}

static const wchar_t *gpu_engine_key(const PDH_FMT_COUNTERVALUE_ITEM_W *item)
{
    const wchar_t *key = wcsstr(item->szName, L"_luid_");
    return key ? key : item->szName;
}

static int compare_gpu_engines(const void *first, const void *second)
{
    return wcscmp(gpu_engine_key(first), gpu_engine_key(second));
}

static bool valid_counter(DWORD status)
{
    return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA;
}

static void sample_gpu(Monitor *monitor)
{
    if (!monitor->gpu_counter) return;
    DWORD bytes = monitor->gpu_capacity, count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(monitor->gpu_counter, PDH_FMT_DOUBLE,
                                                      &bytes, &count, monitor->gpu_samples);
    if (status == (PDH_STATUS)PDH_MORE_DATA && bytes > monitor->gpu_capacity) {
        void *samples = realloc(monitor->gpu_samples, bytes);
        if (!samples) return;
        monitor->gpu_samples = samples;
        monitor->gpu_capacity = bytes;
        status = PdhGetFormattedCounterArrayW(monitor->gpu_counter, PDH_FMT_DOUBLE,
                                              &bytes, &count, samples);
    }
    if (status != ERROR_SUCCESS || !count) return;

    PDH_FMT_COUNTERVALUE_ITEM_W *samples = monitor->gpu_samples;
    qsort(samples, count, sizeof(*samples), compare_gpu_engines);
    double busiest = -1.0;
    for (DWORD i = 0; i < count;) {
        const wchar_t *key = gpu_engine_key(&samples[i]);
        double usage = 0;
        bool valid = false;
        do {
            const PDH_FMT_COUNTERVALUE value = samples[i].FmtValue;
            if (valid_counter(value.CStatus) && value.doubleValue >= 0.0) {
                usage += value.doubleValue;
                valid = true;
            }
            ++i;
        } while (i < count && wcscmp(key, gpu_engine_key(&samples[i])) == 0);
        if (valid && usage > busiest) busiest = usage;
    }
    if (busiest >= 0.0) monitor->gpu = busiest >= 100.0 ? 100 : (int)(busiest + 0.5);
}

static void sample_performance(Monitor *monitor, uint64_t elapsed_ms)
{
    monitor->gpu = -1;
    monitor->cpu_mhz = 0.0;
    if (!monitor->local_query || PdhCollectQueryData(monitor->local_query) != ERROR_SUCCESS)
        return;

    PDH_FMT_COUNTERVALUE frequency;
    if (monitor->frequency_counter &&
        PdhGetFormattedCounterValue(monitor->frequency_counter, PDH_FMT_DOUBLE, NULL,
                                    &frequency) == ERROR_SUCCESS &&
        valid_counter(frequency.CStatus) && frequency.doubleValue > 0.0)
        monitor->cpu_mhz = frequency.doubleValue;
    if (elapsed_ms && elapsed_ms <= MAX_SAMPLE_GAP_MS) sample_gpu(monitor);
}

void monitor_init(Monitor *monitor)
{
    *monitor = (Monitor){0};
    monitor_reset(monitor);
    if (PdhOpenQueryW(NULL, 0, &monitor->local_query) == ERROR_SUCCESS) {
        PdhAddEnglishCounterW(monitor->local_query, L"\\GPU Engine(*)\\Utilization Percentage",
                              0, &monitor->gpu_counter);
        PdhAddEnglishCounterW(monitor->local_query, L"\\Processor Information(_Total)\\Processor Frequency",
                              0, &monitor->frequency_counter);
    }
    monitor->ip_helper = LoadLibraryExW(L"iphlpapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!monitor->ip_helper) return;

    monitor->read_interfaces =
        (ReadInterfaceTable)(void *)GetProcAddress(monitor->ip_helper, "GetIfTable2");
    monitor->release_table =
        (ReleaseMibTable)(void *)GetProcAddress(monitor->ip_helper, "FreeMibTable");
    if (!monitor->release_table) {
        monitor->read_interfaces = NULL;
        return;
    }
    monitor->read_routes =
        (ReadRouteTable)(void *)GetProcAddress(monitor->ip_helper, "GetIpForwardTable2");
    monitor->read_ip_interface =
        (ReadIpInterface)(void *)GetProcAddress(monitor->ip_helper, "GetIpInterfaceEntry");
}

bool monitor_set_adapter(Monitor *monitor, const AdapterSelection *selection)
{
    if (monitor->adapter.mode == selection->mode &&
        (selection->mode != ADAPTER_SINGLE || IsEqualGUID(&monitor->adapter.guid, &selection->guid)))
        return false;

    monitor->adapter = *selection;
    clear_network_reading(monitor);
    monitor->previous_count = 0;
    monitor->route_checked_at = 0;
    monitor->auto_luid = (NET_LUID){0};
    monitor->history = (TrafficHistory){0};
    monitor->session_bytes = 0.0;
    return true;
}

void monitor_sample(Monitor *monitor)
{
    const uint64_t now = GetTickCount64();
    const uint64_t elapsed_ms = monitor->last_tick ? now - monitor->last_tick : 0;
    monitor->last_tick = now;
    if (elapsed_ms > MAX_SAMPLE_GAP_MS) monitor->history = (TrafficHistory){0};

    sample_cpu(monitor, elapsed_ms);
    sample_memory(monitor);
    sample_network(monitor, elapsed_ms, now);
    sample_performance(monitor, elapsed_ms);
    if (monitor->traffic.available)
        monitor->session_bytes += (monitor->traffic.download + monitor->traffic.upload) *
                                  (double)elapsed_ms / 1000.0;
    history_append(&monitor->history, monitor->traffic);
}

void monitor_reset(Monitor *monitor)
{
    clear_network_reading(monitor);
    monitor->history = (TrafficHistory){0};
    monitor->gpu = -1;
    monitor->cpu_mhz = 0.0;
    monitor->cpu = -1;
    monitor->memory = -1;
    monitor->memory_used = 0;
    monitor->memory_total = 0;
    monitor->previous_count = 0;
    monitor->route_checked_at = 0;
    monitor->previous_cpu_valid = false;
    monitor->last_tick = 0;
}

void monitor_close(Monitor *monitor)
{
    if (monitor->local_query) PdhCloseQuery(monitor->local_query);
    free(monitor->gpu_samples);
    free(monitor->current);
    free(monitor->previous);
    if (monitor->ip_helper) FreeLibrary(monitor->ip_helper);
}
