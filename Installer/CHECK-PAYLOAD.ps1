$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$failed = $false
function Pass([string]$Message) { Write-Host "[OK]   $Message" -ForegroundColor Green }
function Warn([string]$Message) { Write-Host "[WARN] $Message" -ForegroundColor Yellow }
function Fail([string]$Message) { Write-Host "[FAIL] $Message" -ForegroundColor Red; $script:failed = $true }

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
    finally { $reader.Close(); $stream.Close() }
}

function Check-Hash([string]$Path, [string]$Expected, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Fail "$Label ausente"; return }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
    if ($actual -eq $Expected.ToLowerInvariant()) { Pass "$Label SHA-256 aprovado" }
    else { Fail "$Label SHA-256 inesperado: $actual" }
}

function Check-MicrosoftSignature([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Fail "$Label ausente"; return }
    $sig = Get-AuthenticodeSignature -FilePath $Path
    if ($sig.Status -ne 'Valid') { Fail "$Label sem assinatura Authenticode válida: $($sig.Status)"; return }
    $subject = ''
    if ($sig.SignerCertificate) { $subject = $sig.SignerCertificate.Subject }
    if ($subject -notmatch 'Microsoft') { Fail "$Label não parece assinado pela Microsoft: $subject"; return }
    Pass "$Label assinado pela Microsoft"
}

$payload = Join-Path $PSScriptRoot 'Payload'
$prereq = Join-Path $PSScriptRoot 'Prerequisites'
$setupIcon = Join-Path $PSScriptRoot 'InstallerAssets\HCPlayer_Setup.ico'
$iss = Join-Path $PSScriptRoot 'HC_Player_1.5.0_x64.iss'

Write-Host 'HC PLAYER 1.5.0 - VERIFICAÇÃO FINAL DO PAYLOAD DO INNO' -ForegroundColor Cyan
Write-Host '====================================================='

if (-not (Test-Path -LiteralPath $payload -PathType Container)) { Fail 'Pasta Payload ausente' }
if (-not (Test-Path -LiteralPath $iss -PathType Leaf)) { Fail 'HC_Player_1.5.0_x64.iss ausente' }

$required = @(
    'HC Player.exe', 'HC Player.ico', 'small.ico', 'HC Player.pri', 'HCPlayer.winmd',
    'libmpv-2.dll', 'MediaInfo.dll', 'LICENSE', 'SOURCE-MANIFEST.md',
    'THIRD-PARTY-NOTICES.md', 'default-input.conf', 'scripts\stats.lua', 'App.xbf', 'MainPage.xbf',
    'SettingsPage.xbf', 'PlaylistPage.xbf', 'MediaInfoPage.xbf', 'ContextMenuPage.xbf', 'YouTubeCommentsPage.xbf'
)
foreach ($name in $required) {
    if (Test-Path -LiteralPath (Join-Path $payload $name) -PathType Leaf) { Pass "$name presente" }
    else { Fail "$name ausente" }
}
if (Test-Path -LiteralPath (Join-Path $payload 'Assets') -PathType Container) { Pass 'Assets presente' } else { Fail 'Assets ausente' }
if (Test-Path -LiteralPath (Join-Path $payload 'Licenses') -PathType Container) { Pass 'Licenses presente' } else { Fail 'Licenses ausente' }

if (Test-Path -LiteralPath (Join-Path $payload 'portable.flag') -PathType Leaf) { Fail 'portable.flag NÃO pode estar no instalador' }
else { Pass 'portable.flag ausente' }

$portableOnlyNames = @('Microsoft.WindowsAppRuntime.dll','onnxruntime.dll','DirectML.dll','Microsoft.ui.xaml.dll')
foreach ($name in $portableOnlyNames) {
    if (Test-Path -LiteralPath (Join-Path $payload $name) -PathType Leaf) { Fail "$name indica saída Portable/self-contained" }
    else { Pass "$name ausente do payload framework-dependent" }
}

$dev = @(Get-ChildItem -LiteralPath $payload -Recurse -File -ErrorAction SilentlyContinue | Where-Object {
    $_.Extension.ToLowerInvariant() -in @('.pdb','.lib','.exp','.obj','.ilk','.iobj','.ipdb')
})
if ($dev.Count -eq 0) { Pass 'Nenhum PDB/LIB/EXP/OBJ/ILK no Payload' }
else { Fail ("Artefatos de build encontrados: " + (($dev | ForEach-Object Name) -join ', ')) }

$exe = Join-Path $payload 'HC Player.exe'
if (Test-Path -LiteralPath $exe -PathType Leaf) {
    $machine = Get-PeMachine $exe
    if ($machine -eq 0x8664) { Pass 'HC Player.exe é x64' } else { Fail ('HC Player.exe Machine=0x{0:X4}, esperado x64 0x8664' -f $machine) }
    $fv = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
    if ($fv.FileVersion -eq '1.5.0.0' -and $fv.ProductVersion -eq '1.5.0.0' -and $fv.ProductName -eq 'HC Player') {
        Pass 'Identidade/versionamento do HC Player.exe = 1.5.0.0 / HC Player'
    } else {
        Fail "Metadados do EXE inesperados: File=$($fv.FileVersion) Product=$($fv.ProductVersion) Name=$($fv.ProductName)"
    }
}

Check-Hash (Join-Path $payload 'libmpv-2.dll') '965efde4c8199f942bf9ed9d3e6fbcb7dd9dc961524d5780a9ca67da53f14d0c' 'libmpv-2.dll'
Check-Hash (Join-Path $payload 'scripts\stats.lua') '7cead8a7b39a9fbd0ccb54b367dce48f65339695dbfb085b873c86ed9b93b247' 'scripts\stats.lua'
Check-Hash (Join-Path $payload 'MediaInfo.dll') 'a2612fa8bf639349aee9747d8a555d361f5db95b049b3af9b0c3851a21a4308d' 'MediaInfo.dll'
Check-Hash (Join-Path $payload 'HC Player.ico') 'ec75352a2abc90fe265079e0f7bc567ddb154eb4aa28f59a73707f8bb25a613f' 'HC Player.ico'
Check-Hash $setupIcon 'f540bb0f98165dc1a90684bd4d1cbed29adaacff708b5364435b5307dcace773' 'HCPlayer_Setup.ico'
Check-Hash $iss 'f36067a3f2922857f95dfb53a3780fee55aa8e6d6cf28f2c431f436310c3bac3' 'HC_Player_1.5.0_x64.iss SAFE'

$vc = Join-Path $prereq 'vc_redist.x64.exe'
$appRuntime = Join-Path $prereq 'WindowsAppRuntimeInstall-x64.exe'
Check-MicrosoftSignature $vc 'vc_redist.x64.exe'
Check-MicrosoftSignature $appRuntime 'WindowsAppRuntimeInstall-x64.exe'

if (Test-Path -LiteralPath $iss -PathType Leaf) {
    $issText = Get-Content -LiteralPath $iss -Raw
    $mustContain = @(
        '#define MyAppId "{{805B14D6-30AC-45A2-BE42-CE0EB9F68408}"',
        'PrivilegesRequired=admin',
        'ChangesAssociations=no',
        'SetupIconFile=InstallerAssets\HCPlayer_Setup.ico',
        'UninstallDisplayName=HC Player 1.5.0',
        '--hc-initial-language {code:InitialLanguageTag}',
        'runasoriginaluser',
        'CleanupHCPlayerLoadedUserProfiles'
    )
    foreach ($needle in $mustContain) {
        if ($issText.Contains($needle)) { Pass "ISS contém: $needle" } else { Fail "ISS perdeu configuração crítica: $needle" }
    }
    if ($issText -match '(?im)^\s*\[Registry\]\s*$') { Fail 'O template final NÃO deve conter seção [Registry]' }
    else { Pass 'Sem seção [Registry] no Inno; associações pertencem ao app' }
    if ($issText -match 'UserChoice') { Pass 'ISS contém somente referências defensivas/comentários a UserChoice; nenhuma exclusão genérica é criada' }
}

Write-Host ''
if ($failed) {
    Write-Host 'PAYLOAD REPROVADO. NÃO COMPILE/PUBLIQUE O SETUP.' -ForegroundColor Red
    exit 1
}

Write-Host 'PAYLOAD APROVADO PARA COMPILAR NO INNO SETUP.' -ForegroundColor Green
Write-Host 'Compile HC_Player_1.5.0_x64.iss no Inno Setup 7 x64.'
exit 0
