# Stillmeter 1.1

Stillmeter is a portable Windows monitor for download and upload rates, CPU load, and physical RAM use. It samples once per second and shows the most recent 60 seconds of traffic in the dashboard.

## Run

1. Extract the full ZIP to a local folder.
2. Open `Windows-x64/Stillmeter.exe` on an Intel or AMD laptop. On a Windows ARM laptop, open `Windows-ARM64/Stillmeter.exe`. The build targets Windows 10 version 1607 or later and Windows 11.
3. Close the dashboard to leave Stillmeter running in the notification area. Click its tray icon to reopen the dashboard. Windows may initially place the icon under the hidden-icons arrow.
4. Use the dashboard or the tray menu to hide the strip or pause sampling. Right-click the tray icon and choose **Exit Stillmeter** to close the application. Pause and strip visibility are session settings.

The compact two-row strip sits beside the left edge of a horizontal primary taskbar and passes clicks through to the window underneath. It follows the taskbar edge, adjusts for DPI, and hides when the bar is retracted or a foreground window fills the monitor. Tray access stays available through Windows.

Both executables are unsigned. Check your organization's application policy before running either build on a managed device. The source is included under `Source/`.

## Readings

Download and upload are in bytes per second, using decimal units. Stillmeter sums active physical Wi-Fi and Ethernet adapters. VPN traffic is reflected in the physical adapter totals as it moves across the connection. Virtual adapters, loopback, and cellular adapters are excluded. An em dash means a reading is unavailable or waiting for a baseline. RAM percentage is calculated from total and available physical memory.
 GPU utilization is the busiest GPU engine, using local Windows performance counters. CPU frequency is the Windows reported processor frequency. A dash indicates an unavailable reading. Total traffic counts upload and download during the current Stillmeter session.

## Local operation

- The app makes zero outbound connections. Traffic rates come from the Windows IP Helper interface table and contain byte counts only.
- No telemetry, update requests, packet capture, host lookup, server, or account setup.
- Traffic history lives in memory. Stillmeter writes no settings or monitoring logs, adds no startup entries, and requests no elevated permissions.
- The app runs in its own process with native Windows controls, ASLR, DEP, Control Flow Guard, and system-directory DLL loading. The strip uses its own window and leaves Explorer untouched.
- `SHA256SUMS.txt` contains the executable hashes. PowerShell can verify either file with `Get-FileHash .\Stillmeter.exe -Algorithm SHA256`.

## Build from source

Run `sh build.sh` in `Source/` with LLVM-MinGW 20251104 UCRT on the build machine's PATH. It writes both architectures under `Source/build/`. The script uses the toolchain already present and has no package-fetch step. The finished executables use standard Windows components.
