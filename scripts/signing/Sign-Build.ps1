param(
    [Parameter(Mandatory)][string] $DllPath,
    [Parameter(Mandatory)][string] $VerifyTool,
    [string] $CertificateThumbprint = $env:SFSE_MF_SIGNING_CERT,
    [string] $SignTool
)
$ErrorActionPreference = 'Stop'
$tools = Join-Path $PSScriptRoot '../../../SFSE-MCP/lib/clib-utils-qtr/tools/signing'
if (-not $CertificateThumbprint) {
    $CertificateThumbprint = [Environment]::GetEnvironmentVariable('SFSE_MF_SIGNING_CERT', 'User')
}
if (-not $CertificateThumbprint) {
    throw 'Supply -CertificateThumbprint or set SFSE_MF_SIGNING_CERT. Signing never creates a key automatically.'
}
$info = & "$tools/Export-PublicKey.ps1" -CertificateThumbprint $CertificateThumbprint
& "$tools/Sign-Build.ps1" -DllPath $DllPath -VerifyTool $VerifyTool -CertificateThumbprint $CertificateThumbprint `
    -ExpectedPublicKeyHash $info.PublicKeySha256 -SignTool $SignTool

# Also require the SFSE SDK's compiled release pin, not merely the selected signer.
& $VerifyTool $DllPath
if ($LASTEXITCODE -ne 0) {
    throw 'The signing key does not match the SDK release pin. Do not deploy this DLL.'
}
