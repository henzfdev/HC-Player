$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

& (Join-Path $PSScriptRoot 'CHECK-PAYLOAD.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
$pf = [Environment]::GetEnvironmentVariable('ProgramFiles')
$candidates = @()
if ($pf86) { $candidates += (Join-Path $pf86 'Inno Setup 7\ISCC.exe') }
if ($pf) { $candidates += (Join-Path $pf 'Inno Setup 7\ISCC.exe') }
$iscc = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
if (-not $iscc) {
    Write-Host 'ISCC.exe não encontrado. Instale o Inno Setup 7 ou abra HC_Player_1.5.0_x64.iss manualmente.' -ForegroundColor Yellow
    exit 2
}

$iss = Join-Path $PSScriptRoot 'HC_Player_1.5.0_x64.iss'
Write-Host "Compilando com: $iscc" -ForegroundColor Cyan
& $iscc $iss
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$setup = Join-Path $PSScriptRoot 'Output\HC_Player_1.5.0_x64_Setup.exe'
if (-not (Test-Path -LiteralPath $setup -PathType Leaf)) {
    Write-Host 'Compilação terminou, mas o Setup esperado não foi encontrado.' -ForegroundColor Red
    exit 3
}
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $setup).Hash.ToLowerInvariant()
$hashFile = Join-Path $PSScriptRoot 'Output\HC_Player_1.5.0_x64_Setup.exe.sha256.txt'
"$hash  HC_Player_1.5.0_x64_Setup.exe" | Set-Content -LiteralPath $hashFile -Encoding ascii
Write-Host ''
Write-Host 'SETUP HC PLAYER 1.5.0 GERADO:' -ForegroundColor Green
Write-Host $setup
Write-Host "SHA-256: $hash"
Write-Host "Hash file: $hashFile"
