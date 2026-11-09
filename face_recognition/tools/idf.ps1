param([Parameter(ValueFromRemainingArguments=$true)][string[]]$IdfArgs)
$ErrorActionPreference = 'Stop'
$env:IDF_PATH = 'D:/B-Work/ESP/Software/v6.1/esp-idf'
$env:IDF_TOOLS_PATH = 'C:/Espressif/tools'
$env:PYTHONUTF8 = '1'
$env:IDF_PYTHON_ENV_PATH = 'C:/Espressif/tools/python/v6.1/venv'
$pythonExe = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe'
$env:PATH = (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts') + ';' + $env:PATH
. 'C:/Espressif/tools/Microsoft.v6.1.PowerShell_profile.ps1'
# Resolve the project from this script so calls from another terminal directory work.
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    & $pythonExe "$env:IDF_PATH/tools/idf.py" @IdfArgs
    $idfExitCode = $LASTEXITCODE
} finally {
    Pop-Location
}
exit $idfExitCode
