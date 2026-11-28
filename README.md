# Stillmeter

Stillmeter is a portable Windows monitor for download and upload rates, CPU load, and physical RAM use. It samples once per second and shows the most recent 60 seconds of traffic in the dashboard.

## Download

[Download the latest Stillmeter release for Windows](https://github.com/aakashad01/Stillmeter/releases/latest)

The release ZIP includes both Windows executables, source code, and SHA-256 checksums.

## Run

1. Extract the downloaded ZIP to a local folder.
2. Open `Windows-x64/Stillmeter.exe` on an Intel or AMD laptop. On a Windows ARM laptop, open `Windows-ARM64/Stillmeter.exe`. The build targets Windows 10 version 1607 or later and Windows 11.
3. Close the dashboard to leave Stillmeter running in the notification area. Click its tray icon to reopen the dashboard. Windows may initially place the icon under the hidden-icons arrow.
4. Use the dashboard or the tray menu to hide the strip or pause sampling. Right-click the tray icon and choose **Exit Stillmeter** to close the application. Pause and strip visibility are session settings.

The two-row strip is centered vertically inside a clear section of the horizontal primary taskbar. Its click-through window uses a transparent background. Stillmeter reads local Windows accessibility bounds to avoid Start, Search, pinned apps, widgets, and the notification area. It uses a smaller layout when needed and hides when there is no safe fit or Windows hides the taskbar. When Windows allows attachment, the strip follows the taskbar’s own stacking order during Start and Search. The dashboard identifies when Stillmeter is using its standalone overlay fallback. Tray access stays available through Windows.

Both executables are unsigned. Check your organization's application policy before running either build on a managed device. The source is included under `Source/`.

## Taskbar appearance

Open **Strip appearance** from the dashboard or tray menu. Start with Balanced, Compact, or Large text, then adjust the settings in the preview. Starting with 1.5.3, you can show or hide Upload, Download, CPU, Memory, GPU, CPU frequency, and Total traffic individually. You can also choose one or two rows, prefer the left or right side of the taskbar, and set the font weight, font size (9–20 px), column spacing (0–24 px), and row spacing (0–12 px). Apply saves the changes and keeps the dialog open. Cancel discards edits made since the last Apply. Defaults restores the Balanced preset.

Sizes scale with monitor DPI. The preview uses sample readings. Actual placement depends on the taskbar controls and the space available. The strip may use smaller text or tighter row spacing to fit.

The strip draws semibold, anti-aliased text with transparent edges and scales for the monitor's DPI. Stillmeter reuses its drawing buffers and measured columns. Taskbar accessibility checks run every two seconds during normal use and resume after Start or Search closes. Metric sampling remains once per second.

## Readings

Download and upload are in bytes per second, using decimal units. An em dash means a reading is unavailable or waiting for a baseline. RAM percentage is calculated from total and available physical memory.

GPU utilization is the busiest GPU engine, using local Windows performance counters. CPU frequency is the Windows reported processor frequency. A dash indicates an unavailable reading. Total traffic counts upload and download since Stillmeter started or since the adapter selection last changed.

## Network adapter

Open **Network adapter** from the dashboard or tray menu.

- **All adapters** adds up traffic across all active physical Wi-Fi and Ethernet adapters. This is the default.
- **Auto select** follows one active physical adapter using Windows' default-route costs. If there is no eligible default route, it keeps the current active adapter when possible or picks another active adapter.
- **A named adapter** pins traffic readings to that Wi-Fi or Ethernet adapter. Readings resume when it reconnects.

The selection applies to the dashboard, tray, and taskbar strip. It is saved for the next run. Changing it clears the traffic graph and total. VPN traffic is reflected in physical adapter readings as it moves across the connection.

## Local operation

- The app makes zero outbound connections. Traffic rates come from the Windows IP Helper interface table and contain byte counts only.
- No telemetry, update requests, packet capture, host lookup, server, or account setup.
- Traffic history lives in memory. Strip appearance and adapter selection are saved under the current user’s Windows registry. Stillmeter writes no monitoring logs, adds no startup entries, and requests no elevated permissions.
- The app runs in its own process with native Windows controls, ASLR, DEP, Control Flow Guard, and system-directory DLL loading. The strip uses public Win32 window APIs to attach from Stillmeter’s separate process.
- `SHA256SUMS.txt` contains the executable hashes. PowerShell can verify either file with `Get-FileHash .\Stillmeter.exe -Algorithm SHA256`.

## Build from source

Run `sh build.sh` in `Source/` with LLVM-MinGW 20251104 UCRT on the build machine's PATH. It writes both architectures under `Source/build/`. The script uses the toolchain already present and has no package-fetch step. The finished executables use standard Windows components.

## License

Stillmeter is available under the [MIT License](LICENSE).
