param(
    [Parameter(Mandatory = $true)]
    [string]$ReleaseOutput
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Fail([string]$Message) {
    Write-Host "ERRO: $Message" -ForegroundColor Red
    exit 1
}

function Get-PeMachine([string]$Path) {
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    $reader = New-Object System.IO.BinaryReader($stream)
    try {
        $stream.Seek(0x3C, [System.IO.SeekOrigin]::Begin) | Out-Null
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 64 -or $peOffset -gt ($stream.Length - 6)) { return 0 }
        $stream.Seek($peOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
        if ($reader.ReadUInt32() -ne 0x00004550) { return 0 }
        return $reader.ReadUInt16()
    }
    finally {
        $reader.Close()
        $stream.Close()
    }
}

function Assert-Hash([string]$Path, [string]$Expected, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail "$Label não encontrado: $Path"
    }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
    if ($actual -ne $Expected.ToLowerInvariant()) {
        Fail "$Label com SHA-256 inesperado. Esperado: $Expected | Atual: $actual"
    }
}

function Test-FileContainsBytes([string]$Path, [byte[]]$Needle) {
    $haystack = [System.IO.File]::ReadAllBytes($Path)
    if ($Needle.Length -eq 0 -or $haystack.Length -lt $Needle.Length) { return $false }
    for ($i = 0; $i -le $haystack.Length - $Needle.Length; $i++) {
        $match = $true
        for ($j = 0; $j -lt $Needle.Length; $j++) {
            if ($haystack[$i + $j] -ne $Needle[$j]) { $match = $false; break }
        }
        if ($match) { return $true }
    }
    return $false
}

$source = (Resolve-Path -LiteralPath $ReleaseOutput).Path.TrimEnd('\')
$payload = Join-Path $PSScriptRoot 'Payload'
$payloadFull = [System.IO.Path]::GetFullPath($payload).TrimEnd('\')
$rootFull = [System.IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')

if ($payloadFull -eq $rootFull -or -not $payloadFull.StartsWith($rootFull + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    Fail 'Proteção interna: a pasta Payload não está contida no template.'
}
if ($source -eq $payloadFull -or $source.StartsWith($payloadFull + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    Fail 'A saída Release não pode ser a própria pasta Payload do template.'
}

$exe = Join-Path $source 'HC Player.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    Fail 'HC Player.exe não encontrado na raiz indicada. Aponte para a pasta de saída Release | x64.'
}

$portableMarkers = @(Get-ChildItem -LiteralPath $source -Recurse -File -Filter 'portable.flag' -ErrorAction SilentlyContinue)
if ($portableMarkers.Count -gt 0) {
    Fail 'portable.flag encontrado. NÃO use a saída Portable | x64 no instalador.'
}

$portableOnlyNames = @(
    'Microsoft.WindowsAppRuntime.dll',
    'onnxruntime.dll',
    'DirectML.dll',
    'Microsoft.ui.xaml.dll'
)
foreach ($name in $portableOnlyNames) {
    if (Test-Path -LiteralPath (Join-Path $source $name) -PathType Leaf) {
        Fail "Arquivo típico da saída self-contained/Portable encontrado: $name. Compile Release | x64."
    }
}

$machine = Get-PeMachine $exe
if ($machine -ne 0x8664) {
    Fail ('HC Player.exe não é PE x64 (Machine=0x{0:X4}).' -f $machine)
}

$version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
if ($version.FileVersion -ne '1.5.0.0' -or $version.ProductVersion -ne '1.5.0.0') {
    Fail "Versão do HC Player.exe inesperada. FileVersion=$($version.FileVersion) ProductVersion=$($version.ProductVersion)"
}
if ($version.ProductName -ne 'HC Player') {
    Fail "ProductName inesperado no HC Player.exe: $($version.ProductName)"
}

$required = @(
    'HC Player.exe', 'HC Player.ico', 'small.ico', 'HC Player.pri', 'HCPlayer.winmd',
    'libmpv-2.dll', 'MediaInfo.dll', 'LICENSE', 'SOURCE-MANIFEST.md',
    'THIRD-PARTY-NOTICES.md', 'default-input.conf', 'scripts\stats.lua', 'App.xbf', 'MainPage.xbf',
    'SettingsPage.xbf', 'PlaylistPage.xbf', 'MediaInfoPage.xbf', 'ContextMenuPage.xbf', 'YouTubeCommentsPage.xbf'
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $name) -PathType Leaf)) {
        Fail "Arquivo obrigatório ausente na saída Release: $name"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $source 'Licenses') -PathType Container)) {
    Fail 'Pasta Licenses ausente na saída Release.'
}
if (-not (Test-Path -LiteralPath (Join-Path $source 'Assets') -PathType Container)) {
    Fail 'Pasta Assets ausente na saída Release.'
}

Assert-Hash (Join-Path $source 'libmpv-2.dll') '965efde4c8199f942bf9ed9d3e6fbcb7dd9dc961524d5780a9ca67da53f14d0c' 'libmpv-2.dll'
Assert-Hash (Join-Path $source 'scripts\stats.lua') '7cead8a7b39a9fbd0ccb54b367dce48f65339695dbfb085b873c86ed9b93b247' 'scripts\stats.lua'
Assert-Hash (Join-Path $source 'MediaInfo.dll') 'a2612fa8bf639349aee9747d8a555d361f5db95b049b3af9b0c3851a21a4308d' 'MediaInfo.dll'
Assert-Hash (Join-Path $source 'HC Player.ico') 'ec75352a2abc90fe265079e0f7bc567ddb154eb4aa28f59a73707f8bb25a613f' 'HC Player.ico'

$githubBytes = [System.Text.Encoding]::Unicode.GetBytes('https://github.com/henzfdev/HC-Player')
if (-not (Test-FileContainsBytes $exe $githubBytes)) {
    Fail 'O HC Player.exe não contém o link final do repositório GitHub. Pode ser um EXE antigo.'
}
$emailBytes = [System.Text.Encoding]::Unicode.GetBytes('mailto:henzfdev@gmail.com')
$settingsXbf = Join-Path $source 'SettingsPage.xbf'
if (-not (Test-FileContainsBytes $settingsXbf $emailBytes)) {
    Fail 'SettingsPage.xbf não contém o e-mail final henzfdev@gmail.com. Pode ser uma saída antiga.'
}
$oldEmailBytes = [System.Text.Encoding]::Unicode.GetBytes('mailto:henzf97@gmail.com')
if (Test-FileContainsBytes $settingsXbf $oldEmailBytes) {
    Fail 'SettingsPage.xbf ainda contém o e-mail antigo henzf97@gmail.com.'
}

Write-Host 'Limpando Payload anterior...' -ForegroundColor Cyan
if (Test-Path -LiteralPath $payload -PathType Container) {
    Get-ChildItem -LiteralPath $payload -Force | Remove-Item -Recurse -Force
}
else {
    New-Item -ItemType Directory -Path $payload | Out-Null
}

$excludedExtensions = @('.pdb', '.lib', '.exp', '.obj', '.ilk', '.iobj', '.ipdb')
$excludedNames = @('portable.flag')
$copied = 0
$skipped = 0

Get-ChildItem -LiteralPath $source -Recurse -File | ForEach-Object {
    $extension = $_.Extension.ToLowerInvariant()
    if (($excludedExtensions -contains $extension) -or ($excludedNames -contains $_.Name.ToLowerInvariant())) {
        $skipped++
        return
    }

    $relative = $_.FullName.Substring($source.Length).TrimStart('\')
    $destination = Join-Path $payload $relative
    $parent = Split-Path -Parent $destination
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
    $copied++
}

# Replace portable-specific release documents with Installed-edition documents.
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'InstallerDocs\SOURCE-MANIFEST.md') -Destination (Join-Path $payload 'SOURCE-MANIFEST.md') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'InstallerDocs\THIRD-PARTY-NOTICES.md') -Destination (Join-Path $payload 'THIRD-PARTY-NOTICES.md') -Force

# Final hard guards.
if (Get-ChildItem -LiteralPath $payload -Recurse -File | Where-Object { $_.Extension.ToLowerInvariant() -in @('.pdb','.lib','.exp','.obj','.ilk','.iobj','.ipdb') }) {
    Fail 'Artefato de build permaneceu no Payload após staging.'
}
if (Test-Path -LiteralPath (Join-Path $payload 'portable.flag') -PathType Leaf) {
    Fail 'portable.flag apareceu no Payload após staging.'
}

Write-Host ''
Write-Host 'STAGING CONCLUÍDO COM SEGURANÇA.' -ForegroundColor Green
Write-Host "Arquivos copiados: $copied"
Write-Host "Artefatos de build ignorados: $skipped"
Write-Host "Payload: $payload"
Write-Host 'Agora rode .\DOWNLOAD-PREREQUISITES.ps1 (se necessário) e depois .\CHECK-PAYLOAD.ps1.'
