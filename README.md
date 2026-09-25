# DuoTCP

Windows fault-isolation bridge for **SDRplay RSPduo dual-tuner mode -> two rtl_tcp-compatible endpoints** for SDRTrunk.

The goal is recovery, not remote SDR. SDRTrunk talks only to localhost TCP sockets, while DuoTCP owns the physical RSPduo and the SDRplay API. If the Duo is unplugged, fails, or the API reports device loss, DuoTCP drops both client sockets, retries hardware acquisition, and lets SDRTrunk's network-tuner auto-reconnect recover cleanly after replug.

## Layout

- Tuner A: `127.0.0.1:1240`
- Tuner B: `127.0.0.1:1241`
- Output: rtl_tcp unsigned 8-bit interleaved I/Q
- Output rate: **2.048 MS/s per tuner**
- RSPduo ADC: **8.192 MHz**, dual-tuner mode, 2.048 MHz IF, 1.536 MHz IF bandwidth. The Duo's internal /4 path yields 2.048 MS/s per tuner.
- Bind address is intentionally loopback-only.

## Requirements

- Windows 10/11 x64
- SDRplay Hardware API **3.15** installed at the normal location (`C:\Program Files\SDRplay\API`)
- Visual Studio 2022 C++ Build Tools
- CMake 3.20+

DuoTCP dynamically loads the installed `sdrplay_api.dll`; it does **not** ship or copy SDRplay's DLL. This avoids getting the DLL out of sync with the SDRplay service.

## Build

Open PowerShell in the repository:

```powershell
.\scripts\build-windows.ps1
```

Result:

```text
dist\duotcp.exe
```

Protocol-only tests build on Linux and in GitHub Actions; the actual bridge requires the SDRplay Windows development headers.

## Run

```powershell
.\dist\duotcp.exe
```

Optional:

```powershell
.\dist\duotcp.exe --port-a 1240 --port-b 1241 --serial YOUR_RSPDUO_SERIAL
```

For unattended operation, `scripts\run-watchdog.ps1` restarts the process if the process itself exits. Normal RSPduo unplug/replug is handled inside DuoTCP without requiring process restart.

## SDRTrunk setup

Use an SDRTrunk build containing the pending upstream rtl_tcp network-tuner support (PR #2471 or equivalent).

Add two network tuners:

| Tuner | Host | Port | Sample rate | Auto reconnect |
|---|---|---:|---:|---|
| RSPduo A | `127.0.0.1` | 1240 | 2,048,000 | On |
| RSPduo B | `127.0.0.1` | 1241 | 2,048,000 | On |

AGC is enabled by default. Frequency commands are independent for A and B. Bias-T is applied only to tuner B. The rtl_tcp manual-gain command is mapped conservatively to SDRplay gain reduction; exact RSP gain-table emulation is not yet implemented.

## Recovery behavior

1. SDRTrunk connects to the two localhost endpoints.
2. DuoTCP owns the RSPduo and sends each API callback stream to its endpoint.
3. On `DeviceRemoved` or `DeviceFailure`, DuoTCP immediately closes both TCP clients.
4. SDRTrunk sees ordinary network tuner disconnects instead of a half-dead native SDRplay object.
5. DuoTCP releases/closes the API and retries RSPduo discovery.
6. After the Duo returns, the listeners become available and SDRTrunk reconnects.

## Current status

Initial implementation. Protocol helpers are test-covered. Hardware validation still needs to be performed on a real Windows machine with an RSPduo before calling the bridge production-ready.

## License

GPL-3.0-or-later.
