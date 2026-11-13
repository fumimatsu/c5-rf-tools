param([switch]$Flash, [string]$Port, [string]$IdfPath = $env:IDF_PATH)
$ErrorActionPreference = 'Stop'
if ($Flash -and [string]::IsNullOrWhiteSpace($Port)) { throw 'For flashing, explicitly pass -Port after verifying the Tab5 identity.' }
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
  if ([string]::IsNullOrWhiteSpace($IdfPath)) { throw 'Open the ESP-IDF 5.4.2 PowerShell environment, or pass -IdfPath.' }
  $exportScript = Join-Path $IdfPath 'export.ps1'
  if (-not (Test-Path -LiteralPath $exportScript)) { throw 'ESP-IDF export.ps1 not found.' }
  . $exportScript
}
Push-Location $PSScriptRoot
try {
  idf.py build
  if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
  if ($Flash) {
    idf.py -p $Port -b 921600 flash
    if ($LASTEXITCODE -ne 0) { throw 'Flash failed' }
  }
} finally { Pop-Location }
