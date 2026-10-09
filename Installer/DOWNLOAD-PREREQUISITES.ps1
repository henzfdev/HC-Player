$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$target = Join-Path $PSScriptRoot 'Prerequisites'
New-Item -ItemType Directory -Path $target -Force | Out-Null

$downloads = @(
    @{
        Name = 'Windows App Runtime 2.4.0 x64'
        Uri = 'https://aka.ms/windowsappsdk/2.4/2.4.0/windowsappruntimeinstall-x64.exe'
        File = 'WindowsAppRuntimeInstall-x64.exe'
    },
    @{
        Name = 'Microsoft Visual C++ v14 Redistributable x64 (latest supported)'
        Uri = 'https://aka.ms/vc14/vc_redist.x64.exe'
        File = 'vc_redist.x64.exe'
    }
)

function Assert-MicrosoftSignature([string]$Path, [string]$Label) {
    $sig = Get-AuthenticodeSignature -FilePath $Path
    if ($sig.Status -ne 'Valid') {
        throw "${Label}: assinatura Authenticode inválida ($($sig.Status))."
    }
    if (-not $sig.SignerCertificate -or $sig.SignerCertificate.Subject -notmatch 'Microsoft') {
        throw "${Label}: o arquivo não parece assinado pela Microsoft."
    }
}

foreach ($item in $downloads) {
    $dest = Join-Path $target $item.File
    Write-Host "Baixando $($item.Name)..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $item.Uri -OutFile $dest -UseBasicParsing
    Assert-MicrosoftSignature $dest $item.Name
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dest).Hash.ToLowerInvariant()
    Write-Host "[OK] $($item.File)" -ForegroundColor Green
    Write-Host "     SHA-256: $hash"
}

Write-Host ''
Write-Host 'PRÉ-REQUISITOS OFICIAIS BAIXADOS E ASSINATURAS MICROSOFT VALIDADAS.' -ForegroundColor Green
Write-Host 'Agora rode .\\CHECK-PAYLOAD.ps1 depois de preparar o Payload.'
