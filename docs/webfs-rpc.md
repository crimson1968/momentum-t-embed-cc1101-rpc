# WebFS RPC API

The LilyGO T-Embed CC1101 Plus exposes this API while **Web Filesystem** is
running in station mode. Examples below use the device address
`http://192.168.178.35`.

The radio API is receive-only. There is no arbitrary-command or RF-transmit
endpoint. Storage access is confined to `/ext`; `..` path traversal is rejected.

## Device information

```text
GET /api/status
GET /api/capabilities
GET /api/settings
GET /api/diagnostics
```

`settings` reports locale, clock/date format and timezone state without allowing
remote changes. `diagnostics` reports battery, current WiFi link, internal heap,
SD-card capacity and timestamps.

## Receive jobs

Start a receive job:

```powershell
$body = @{ frequency_hz = 433920000; duration_ms = 5000 } | ConvertTo-Json
$job = Invoke-RestMethod -Method Post -ContentType 'application/json' `
  -Uri 'http://192.168.178.35/api/subghz/rx' -Body $body
```

List the retained job or fetch it by ID:

```powershell
Invoke-RestMethod 'http://192.168.178.35/api/jobs'
Invoke-RestMethod "http://192.168.178.35/api/jobs?id=$($job.id)"
```

Cancel a running job:

```powershell
Invoke-RestMethod -Method Post `
  "http://192.168.178.35/api/jobs/cancel?id=$($job.id)"
```

Only one job runs at a time and only the latest job is retained. Cancellation
normally changes the result to `cancelled` within one 20 ms sampling interval.

## SD-card storage

The RPC storage names reuse the WebFS implementation and its path validation:

```text
GET  /api/storage/list?path=/ext
GET  /api/storage/download?path=/ext/example.txt
POST /api/storage/upload?path=/ext/example.txt
```

PowerShell examples:

```powershell
Invoke-RestMethod 'http://192.168.178.35/api/storage/list?path=/ext'
Invoke-WebRequest 'http://192.168.178.35/api/storage/download?path=/ext/example.txt' `
  -OutFile .\example.txt
Invoke-WebRequest -Method Post `
  'http://192.168.178.35/api/storage/upload?path=/ext/example.txt' `
  -InFile .\example.txt -ContentType 'application/octet-stream'
```

The original `/api/list`, `/api/download`, and `/api/upload` WebFS routes remain
available for compatibility.

## Smoke test

```powershell
python .\tools\wlan_rpc_smoke.py --base http://192.168.178.35
```

Add `--exercise-cancel` to start and cancel a 30-second receive job. The default
test performs no radio operation and does not write to the SD card.
