param(
	[Parameter(Mandatory)]
	[string] $DllPath,

	[Parameter(Mandatory)]
	[string] $VerifyTool,

	[string] $CertificateThumbprint = $env:SFSE_MF_SIGNING_CERT,

	[string] $SignTool
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'Signing.psm1') -Force
if (-not $CertificateThumbprint) {
	$CertificateThumbprint = [Environment]::GetEnvironmentVariable('SFSE_MF_SIGNING_CERT', 'User')
}
if (-not $CertificateThumbprint) {
	throw 'Supply -CertificateThumbprint or set SFSE_MF_SIGNING_CERT. Signing never creates a key automatically.'
}

$dll = (Get-Item -LiteralPath $DllPath).FullName
if ([System.IO.Path]::GetExtension($dll) -ne '.dll') {
	throw 'The signing input must be a DLL.'
}
$verifier = (Get-Item -LiteralPath $VerifyTool).FullName
if ([System.IO.Path]::GetExtension($verifier) -ne '.exe') {
	throw 'Supply the standalone signature verifier executable with -VerifyTool.'
}
$signer = Find-SignTool -Path $SignTool
$certificate = Get-SigningCertificate -Thumbprint $CertificateThumbprint -RequirePrivateKey
try {
	$info = Get-SigningPublicInfo -Certificate $certificate
	& $signer sign /q /fd SHA256 /sha1 $certificate.Thumbprint /s My $dll
	if ($LASTEXITCODE -ne 0) {
		throw "SignTool failed with exit code $LASTEXITCODE."
	}
	# The verifier checks the PKCS#7 signer and signed PE digest, without requiring root trust.
	& $verifier $dll $info.PublicKeySha256
	if ($LASTEXITCODE -ne 0) {
		throw "Signature verification failed with exit code $LASTEXITCODE. Do not deploy this DLL."
	}
	# Require the SDK's compiled release pin as well as the configured signer.
	& $verifier $dll
	if ($LASTEXITCODE -ne 0) {
		throw 'The signing key does not match the SDK release pin. Do not deploy this DLL.'
	}
} finally {
	$certificate.Dispose()
}

Write-Host "Signed and verified: $dll"
