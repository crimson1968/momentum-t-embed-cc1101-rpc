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

$arguments = @($idfPy, '-B', 'build_t_embed', '-p', $Port, 'build', 'flash')
if($Monitor) { $arguments += 'monitor' }

& $python @arguments
if($LASTEXITCODE -ne 0) {
    throw "ESP-IDF build/flash failed with exit code $LASTEXITCODE"
}
