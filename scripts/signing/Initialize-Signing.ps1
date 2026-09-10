[CmdletBinding(DefaultParameterSetName = 'Create')]
param(
    [Parameter(ParameterSetName = 'Create')][string] $Subject = 'CN=Quantumyilmaz',
    [Parameter(ParameterSetName = 'Create')][ValidateRange(1, 10)][int] $ValidityYears = 10,
    [Parameter(Mandatory, ParameterSetName = 'Reuse')][string] $CertificateThumbprint
)
$ErrorActionPreference = 'Stop'
$tools = Join-Path $PSScriptRoot '../../../SFSE-MCP/lib/clib-utils-qtr/tools/signing'
if ($PSCmdlet.ParameterSetName -eq 'Reuse') {
    & "$tools/Initialize-Signing.ps1" -CertificateThumbprint $CertificateThumbprint
} else {
    & "$tools/Initialize-Signing.ps1" -Subject $Subject -ValidityYears $ValidityYears
}
