# T-Embed CC1101 RPC Firmware User Guide

This guide covers everyday use of the RPC-enabled Momentum firmware on the
LilyGO T-Embed CC1101 Plus. The companion Docker gateway is documented in the
[gateway user guide](https://github.com/crimson1968/RPC-Gateway_for_momentum-t-embed-cc1101-rpc/blob/main/USER_GUIDE.md).

## What the firmware provides

The firmware exposes two independent remote-control connections:

- **Wi-Fi WebFS RPC** provides device information, diagnostics, SD-card file
  access, and receive-only Sub-GHz jobs.
- **qFlipper USB** provides the live 128 x 64 logical screen and remote button
  presses through the gateway.

For full gateway operation, keep both connections active. USB control can
continue without WebFS, but status, files, and Sub-GHz RPC need WebFS.

## Normal startup

1. Power on the T-Embed and wait for the main menu.
2. Connect the device to the configured Wi-Fi network.
3. Start **Web Filesystem** and leave it running.
4. Enable the **qFlipper** switch when USB screen and button control is needed.
5. Connect the USB cable to the Docker host or NAS.
6. Open the gateway at
   `https://tembed-gateway.myhomelabs.work/` on the trusted LAN.

The verified device address is `http://192.168.178.35`. If DHCP assigns a new
address, update `TEMBED_URL` in the gateway configuration.

## Web Filesystem and Wi-Fi RPC

Web Filesystem serves the browser file interface and the RPC API on port 80.
It must remain open for Wi-Fi operations. Leaving the app, disabling Wi-Fi, or
starting a Wi-Fi mode that takes over the radio can make the gateway report
`device_unreachable_or_timeout`.

Useful checks from Windows PowerShell:

```powershell
Invoke-RestMethod 'http://192.168.178.35/api/status' | ConvertTo-Json
Invoke-RestMethod 'http://192.168.178.35/api/capabilities' | ConvertTo-Json
```

A healthy status reports `network_up: true`, `webfs_running: true`, and API
version `1.1`.

## qFlipper USB mode

qFlipper mode makes the device appear on Linux as a serial device. The verified
NAS names are:

```text
/dev/ttyACM0
/dev/serial/by-id/usb-Flipper_Devices_Inc._Warp_FZESP32-if01
```

The USB identity changes when qFlipper is disabled or when the device enters a
different USB mode. Enable qFlipper before starting or recreating the gateway
container. If the cable is unplugged, qFlipper is toggled, or USB Storage or
BadUSB takes over the port, recreate the gateway container after `/dev/ttyACM0`
returns.

Confirm the current device on the NAS with:

```sh
ls -l /dev/ttyACM* /dev/serial/by-id/ 2>&1
```

Do not open the same serial device simultaneously in another program.

## Receive-only Sub-GHz RPC

The RPC layer can listen and return pulse/RSSI statistics. It cannot transmit.
Only one receive job can run at a time, and only the newest job is retained.

```powershell
$body = @{ frequency_hz = 433920000; duration_ms = 5000 } | ConvertTo-Json
$job = Invoke-RestMethod -Method Post -ContentType 'application/json' `
  -Uri 'http://192.168.178.35/api/subghz/rx' -Body $body

Invoke-RestMethod "http://192.168.178.35/api/jobs?id=$($job.id)"
```

Use only frequencies and reception activities permitted in your location. The
RPC implementation deliberately has no RF-transmit or arbitrary-command API.

## SD-card files

RPC storage access is restricted to `/ext`:

```powershell
Invoke-RestMethod 'http://192.168.178.35/api/storage/list?path=/ext'
Invoke-WebRequest `
  'http://192.168.178.35/api/storage/download?path=/ext/example.txt' `
  -OutFile .\example.txt
Invoke-WebRequest -Method Post `
  'http://192.168.178.35/api/storage/upload?path=/ext/example.txt' `
  -InFile .\example.txt -ContentType 'application/octet-stream'
```

The gateway web interface adds file browsing, upload, download, rename, folder
creation, and deletion. Review the path before modifying or deleting files.

## Clock and timezone

This patched build initializes the device time for the configured Europe/Berlin
timezone. The RPC settings endpoint is read-only and reports the current clock,
date-format, locale, and timezone state; it does not change them remotely.

## Troubleshooting

### The gateway says the device is unreachable

Check that Wi-Fi is connected, the address is still correct, and Web Filesystem
is open. Test `http://192.168.178.35/api/status` directly.

### USB is configured but not connected

Enable qFlipper, reconnect the cable, confirm `/dev/ttyACM0` exists, then
recreate the gateway container. A Docker device mapping is resolved when the
container starts and does not automatically follow later USB re-enumeration.

### WebFS works until qFlipper is enabled

Use the current boot-safe/qFlipper-WebFS firmware from this repository. It
allocates the RPC receive worker only when needed so qFlipper and WebFS can run
together. Start Web Filesystem again after changing USB mode if necessary.

### The display stays black after flashing

Use `tools/flash_t_embed_rpc.ps1`. The Momentum image uses the generated
partition layout with the application at `0x20000`; older Sor3nt flashing
offsets can write successfully but produce a non-booting device.

### A receive request reports busy

Check `/api/status` and `/api/jobs` before trying again. Do not automatically
repeat an uncertain receive request, because the first request may already have
started on the device.

## Related documentation

- [WebFS RPC API](webfs-rpc.md)
- [Gateway repository](https://github.com/crimson1968/RPC-Gateway_for_momentum-t-embed-cc1101-rpc)
- [Gateway user guide](https://github.com/crimson1968/RPC-Gateway_for_momentum-t-embed-cc1101-rpc/blob/main/USER_GUIDE.md)
