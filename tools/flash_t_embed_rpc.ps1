[CmdletBinding()]
param(
    [string]$Port = 'COM3',
    [string]$IdfPath = 'C:\Espressif\frameworks\esp-idf-v5.4.1',
    [string]$IdfToolsPath = 'C:\Espressif',
    [switch]$Monitor
)

$ErrorActionPreference = 'Stop'
$python = Join-Path $IdfToolsPath 'python_env\idf5.4_py3.11_env\Scripts\python.exe'
$idfPy = Join-Path $IdfPath 'tools\idf.py'
$cmake = Join-Path $IdfToolsPath 'tools\cmake\3.30.2\bin'
$ninja = Join-Path $IdfToolsPath 'tools\ninja\1.12.1'

foreach($required in @($python, $idfPy, $cmake, $ninja)) {
    if(-not (Test-Path -LiteralPath $required)) {
        throw "Required ESP-IDF path not found: $required"
    }
}

$env:IDF_PATH = $IdfPath
$env:IDF_TOOLS_PATH = $IdfToolsPath
$env:IDF_PYTHON_ENV_PATH = Split-Path -Parent (Split-Path -Parent $python)
$env:Path = "$cmake;$ninja;$env:Path"

$projectRoot = Get-Location
$cmakeLists = Join-Path $projectRoot 'CMakeLists.txt'
if(Test-Path -LiteralPath $cmakeLists) {
    $arguments = @($idfPy, '-B', 'build_t_embed', '-p', $Port, 'build', 'flash')
    if($Monitor) { $arguments += 'monitor' }
    & $python @arguments
} else {
    $esptool = Join-Path $IdfPath 'components\esptool_py\esptool\esptool.py'
    $images = @(
        @{ Offset = '0x0'; File = 'bootloader.bin' },
        @{ Offset = '0x8000'; File = 'partition-table.bin' },
        @{ Offset = '0x10000'; File = 'ota_data_initial.bin' },
        @{ Offset = '0x20000'; File = 'furi_esp32.bin' }
    )
    foreach($image in $images) {
        if(-not (Test-Path -LiteralPath (Join-Path $projectRoot $image.File))) {
            throw "Firmware image not found: $($image.File)"
        }
    }
    $arguments = @(
        $esptool, '--chip', 'esp32s3', '-p', $Port, '-b', '460800',
        '--before', 'default_reset', '--after', 'hard_reset', 'write_flash',
        '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '16MB'
    )
    foreach($image in $images) {
        $arguments += @($image.Offset, (Join-Path $projectRoot $image.File))
    }
    & $python @arguments
    if($LASTEXITCODE -eq 0 -and $Monitor) {
        Write-Host 'Starting serial monitor. Press Ctrl+] to exit.'
        & $python -m serial.tools.miniterm $Port 115200
    }
}
if($LASTEXITCODE -ne 0) {
    throw "ESP-IDF build/flash failed with exit code $LASTEXITCODE"
}
