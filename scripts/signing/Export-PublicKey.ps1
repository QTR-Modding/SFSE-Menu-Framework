param(
    [string] $CertificateThumbprint = $env:SFSE_MF_SIGNING_CERT,
    [string] $OutputPath
)
$ErrorActionPreference = 'Stop'
$tools = Join-Path $PSScriptRoot '../../../SFSE-MCP/lib/clib-utils-qtr/tools/signing'
if (-not $CertificateThumbprint) { throw 'Supply -CertificateThumbprint or set SFSE_MF_SIGNING_CERT.' }
& "$tools/Export-PublicKey.ps1" -CertificateThumbprint $CertificateThumbprint -OutputPath $OutputPath
