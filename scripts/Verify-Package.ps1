param(
	[Parameter(Mandatory)]
	[string] $Archive,

	[Parameter(Mandatory)]
	[string] $BuiltDll
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$expectedFiles = @(
	'Data/COPYING'
	'Data/EXCEPTIONS'
	'Data/SFSE/Plugins/Fonts/fa-brands-400.ttf'
	'Data/SFSE/Plugins/Fonts/fa-regular-400.ttf'
	'Data/SFSE/Plugins/Fonts/fa-solid-900.ttf'
	'Data/SFSE/Plugins/Fonts/Font-Awesome-LICENSE.txt'
	'Data/SFSE/Plugins/Fonts/FreeType-FTL.txt'
	'Data/SFSE/Plugins/Fonts/Jost-400-Book.ttf'
	'Data/SFSE/Plugins/Fonts/Jost-500-Medium.ttf'
	'Data/SFSE/Plugins/Fonts/Jost-OFL.txt'
	'Data/SFSE/Plugins/Fonts/SpaceGrotesk-Medium.ttf'
	'Data/SFSE/Plugins/Fonts/SpaceGrotesk-OFL.txt'
	'Data/SFSE/Plugins/Fonts/SpaceGrotesk[wght].ttf'
	'Data/SFSE/Plugins/SFSEMenuFramework.dll'
	'Data/SFSE/Plugins/SFSEMenuFramework.ini.example'
	'Data/SFSE/Plugins/SFSEMenuFrameworkStrings.json'
	'Data/SFSE/Plugins/SFSEMenuFrameworkCursors/ring.json'
	'Data/SFSE/Plugins/SFSEMenuFrameworkCursors/ring.png'
	'Data/SFSE/Plugins/SFSEMenuFrameworkThemes/blackest sea.json'
	'Data/SFSE/Plugins/SFSEMenuFrameworkThemes/constellation.json'
	'Data/SFSE/Plugins/SFSEMenuFrameworkThemes/classic.json'
	'Data/SFSE/Plugins/SFSEMenuFrameworkThemes/modern.json'
	'Data/SFSE/Plugins/SFSEMenuFrameworkThemes/starfield.json'
	'Data/SFSE/Plugins/SFSEMenuFrameworkThemes/the void.json'
	'Data/SFSE/Plugins/SFSEMenuFrameworkThemes/unity.json'
	'Data/SFSE/Plugins/SFSEMenuFrameworkThemes/wallpapers/unity.png'
	'Data/THIRD_PARTY_NOTICES.md'
) | ForEach-Object { $_.Substring(5) } | Sort-Object

$archivePath = (Resolve-Path -LiteralPath $Archive).Path
$builtDllPath = (Resolve-Path -LiteralPath $BuiltDll).Path
$builtPdbPath = [IO.Path]::ChangeExtension($builtDllPath, '.pdb')
if (Test-Path -LiteralPath $builtPdbPath) {
	$expectedFiles = @($expectedFiles) + 'SFSE/Plugins/SFSEMenuFramework.pdb' | Sort-Object
}
$zip = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
try {
	$actualFiles = @(
		$zip.Entries |
			Where-Object { -not $_.FullName.EndsWith('/') } |
			ForEach-Object FullName |
			Sort-Object
	)
	$differences = @(Compare-Object $expectedFiles $actualFiles)
	if ($actualFiles.Count -ne $expectedFiles.Count -or
		$differences.Count -ne 0) {
		$details = $differences | Out-String
		throw "Package manifest mismatch:`n$details"
	}

	$dllEntry = $zip.GetEntry('SFSE/Plugins/SFSEMenuFramework.dll')
	$entryStream = $dllEntry.Open()
	try {
		$sha256 = [System.Security.Cryptography.SHA256]::Create()
		try {
			$archiveDllHash = -join (
				$sha256.ComputeHash($entryStream) |
					ForEach-Object { $_.ToString('X2') })
		} finally {
			$sha256.Dispose()
		}
	} finally {
		$entryStream.Dispose()
	}
	$builtStream = [IO.File]::OpenRead($builtDllPath)
	$builtSha = [Security.Cryptography.SHA256]::Create()
	try {
		$builtDllHash = [BitConverter]::ToString($builtSha.ComputeHash($builtStream)).Replace('-', '')
	} finally {
		$builtStream.Dispose()
		$builtSha.Dispose()
	}
	if ($archiveDllHash -ne $builtDllHash) {
		throw 'Packaged DLL does not match the reviewed build output.'
	}
	$repo = Split-Path $PSScriptRoot
	$payload = @{
		'SFSE/Plugins/SFSEMenuFramework.ini.example' = Join-Path $repo 'public/SFSE/Plugins/SFSEMenuFramework.ini.example'
		'SFSE/Plugins/SFSEMenuFrameworkStrings.json' = Join-Path $repo 'public/SFSE/Plugins/SFSEMenuFrameworkStrings.json'
		'COPYING' = Join-Path $repo 'COPYING'
		'EXCEPTIONS' = Join-Path $repo 'EXCEPTIONS'
		'THIRD_PARTY_NOTICES.md' = Join-Path $repo 'THIRD_PARTY_NOTICES.md'
	}
	if (Test-Path -LiteralPath $builtPdbPath) {
		$payload['SFSE/Plugins/SFSEMenuFramework.pdb'] = $builtPdbPath
	}
	foreach ($item in $payload.GetEnumerator()) {
		$stream = $zip.GetEntry($item.Key).Open()
		$sha = [Security.Cryptography.SHA256]::Create()
		try {
			$hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')
		} finally { $sha.Dispose(); $stream.Dispose() }
		if ($hash -ne (Get-FileHash -LiteralPath $item.Value -Algorithm SHA256).Hash) {
			throw "Packaged file differs from its source: $($item.Key)"
		}
	}
} finally {
	$zip.Dispose()
}

Write-Host "Verified package manifest and DLL hash: $archivePath"
