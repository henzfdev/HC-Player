$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Fail([string]$Message) {
    Write-Host ''
    Write-Host "ERRO: $Message" -ForegroundColor Red
    exit 1
}
function Invoke-HCScript([string]$ScriptPath, [string[]]$Arguments = @()) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ScriptPath @Arguments
    if ($LASTEXITCODE -ne 0) { Fail "Falhou: $ScriptPath (codigo $LASTEXITCODE)" }
}

$Installer = $PSScriptRoot

# Find the actual solution root instead of assuming a fixed Installer layout.
# This supports both:
#   <solution>\Installer
#   <solution>\HC Player\Installer
$SolutionRoot = $null
$cursor = $Installer
for ($i = 0; $i -lt 5; $i++) {
    $cursor = Split-Path -Parent $cursor
    if (-not $cursor) { break }
    if (Test-Path -LiteralPath (Join-Path $cursor 'HC Player.slnx') -PathType Leaf) {
        $SolutionRoot = $cursor
        break
    }
}

if (-not $SolutionRoot) {
    # Safe fallback for uncommon layouts: use the Installer parent and let the
    # recursive Release search below try to locate the executable.
    $SolutionRoot = Split-Path -Parent $Installer
}

Write-Host "Raiz da solucao detectada: $SolutionRoot" -ForegroundColor DarkGray

$candidateDirs = @(
    (Join-Path $SolutionRoot 'x64\Release\HC Player'),
    (Join-Path $SolutionRoot 'x64\Release'),
    (Join-Path $SolutionRoot 'HC Player\x64\Release\HC Player'),
    (Join-Path $SolutionRoot 'HC Player\x64\Release')
)

$release = $null
foreach ($dir in $candidateDirs) {
    if (Test-Path -LiteralPath (Join-Path $dir 'HC Player.exe') -PathType Leaf) {
        $release = (Resolve-Path -LiteralPath $dir).Path
        break
    }
}

if (-not $release) {
    Write-Host 'Procurando HC Player.exe Release na arvore da solucao...' -ForegroundColor Yellow
    $found = @(
        Get-ChildItem -LiteralPath $SolutionRoot -Filter 'HC Player.exe' -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object {
            $_.FullName -notlike '*\Installer\*' -and
            $_.FullName -notlike '*\Debug\*' -and
            $_.FullName -match '\\Release\\'
        } |
        Sort-Object LastWriteTime -Descending
    )
    if ($found.Count -gt 0) { $release = $found[0].Directory.FullName }
}

if (-not $release) {
    Fail "HC Player.exe Release nao encontrado abaixo de: $SolutionRoot`n`nConfirme no Visual Studio: Release | x64 > Limpar Solucao > Recompilar Solucao."
}

Write-Host "Release detectado: $release" -ForegroundColor Green

$download = Join-Path $Installer 'DOWNLOAD-PREREQUISITES.ps1'
$stage = Join-Path $Installer 'STAGE-PAYLOAD.ps1'
$check = Join-Path $Installer 'CHECK-PAYLOAD.ps1'
$build = Join-Path $Installer 'BUILD-INSTALLER.ps1'
foreach ($p in @($download,$stage,$check,$build)) {
    if (-not (Test-Path -LiteralPath $p)) { Fail "Arquivo ausente: $p" }
}

$vc = Join-Path $Installer 'Prerequisites\vc_redist.x64.exe'
$appRuntime = Join-Path $Installer 'Prerequisites\WindowsAppRuntimeInstall-x64.exe'
if (-not (Test-Path -LiteralPath $vc) -or -not (Test-Path -LiteralPath $appRuntime)) {
    Write-Host '[1/4] Baixando/validando pre-requisitos oficiais Microsoft...' -ForegroundColor Cyan
    Invoke-HCScript $download
} else {
    Write-Host '[1/4] Pre-requisitos ja presentes; o checker validara as assinaturas.' -ForegroundColor Green
}
Write-Host '[2/4] Montando Payload a partir da Release | x64...' -ForegroundColor Cyan
Invoke-HCScript $stage @('-ReleaseOutput', $release)
Write-Host '[3/4] Auditando Payload + template SAFE...' -ForegroundColor Cyan
Invoke-HCScript $check
Write-Host '[4/4] Compilando no Inno Setup 7...' -ForegroundColor Cyan
Invoke-HCScript $build

$setup = Join-Path $Installer 'Output\HC_Player_1.5.1_x64_Setup.exe'
$hashFile = Join-Path $Installer 'Output\HC_Player_1.5.1_x64_Setup.exe.sha256.txt'
if (-not (Test-Path -LiteralPath $setup)) { Fail 'O Inno terminou, mas o Setup final nao foi encontrado.' }
Write-Host ''
Write-Host '============================================================' -ForegroundColor Green
Write-Host ' HC PLAYER 1.5.1 - SETUP FINAL SAFE GERADO' -ForegroundColor Green
Write-Host '============================================================' -ForegroundColor Green
Write-Host "Setup: $setup"
Write-Host "Hash:  $hashFile"
