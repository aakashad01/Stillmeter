# Security Policy

## Reporting a vulnerability

Use **Report a vulnerability** if the option is available on the [Security tab](https://github.com/aakashad01/Stillmeter/security).

If the option is unavailable, open an issue titled **Request for private security contact**. Keep the issue limited to asking for a private reporting channel. Share vulnerability details, sensitive logs, and proof-of-concept files privately. [Open a contact request](https://github.com/aakashad01/Stillmeter/issues/new)

In your private report, include:

- The Stillmeter version, Windows version, and architecture (x64 or ARM64)
- A brief description of the issue and its potential impact
- Steps to reproduce the issue, with personal or workplace information removed

If possible, check whether the issue affects the [latest release](https://github.com/aakashad01/Stillmeter/releases/latest).

## Downloads and verification

Download Stillmeter from [GitHub Releases](https://github.com/aakashad01/Stillmeter/releases). After extracting the ZIP, compare the executable’s SHA-256 hash with the matching entry in `SHA256SUMS.txt`. Stillmeter executables are unsigned.

## Local operation

Stillmeter reads Windows system and network adapter counters locally. Auto select also reads the local Windows routing table. Stillmeter sends no telemetry and performs no automatic update checks. Traffic history is kept in memory. Appearance preferences and the adapter selection are saved in the current user’s Windows registry.
