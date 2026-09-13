param(
    [Parameter(Mandatory)][string] $BuiltDll,
    [Parameter(Mandatory)][string] $Version
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$build = Join-Path $repo 'build'
$stage = Join-Path $build 'staging'
$packages = Join-Path $build 'packages'
$dll = (Resolve-Path -LiteralPath $BuiltDll).Path
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Expected a three-part release version.' }
$name = [IO.Path]::GetFileNameWithoutExtension($dll)
$archive = Join-Path $packages "$name-$Version.zip"
# Never follow a junction out of the repository's generated output folders.
foreach ($folder in @($build, $stage, $packages)) {
    if (Test-Path -LiteralPath $folder) {
        if ((Get-Item -LiteralPath $folder).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing a redirected output folder: $folder"
        }
    }
}
New-Item -ItemType Directory -Path $build, $packages -Force | Out-Null
$temporary = Join-Path $build ('release-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    Copy-Item -LiteralPath (Join-Path $repo 'public\SFSE') -Destination $temporary -Recurse
    foreach ($license in @('COPYING', 'EXCEPTIONS', 'THIRD_PARTY_NOTICES.md')) {
        Copy-Item -LiteralPath (Join-Path $repo $license) -Destination $temporary
    }
    Copy-Item -LiteralPath $dll -Destination (Join-Path $temporary "SFSE\Plugins\$name.dll")
    $pdb = [IO.Path]::ChangeExtension($dll, '.pdb')
    if (Test-Path -LiteralPath $pdb) {
        Copy-Item -LiteralPath $pdb -Destination (Join-Path $temporary "SFSE\Plugins\$name.pdb")
    }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $temporaryZip = "$temporary.zip"
    $zip = [IO.Compression.ZipFile]::Open($temporaryZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $temporary -Recurse -File) {
            $entryName = $file.FullName.Substring($temporary.Length + 1).Replace('\', '/')
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $file.FullName, $entryName, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $zip.Dispose() }
    & (Join-Path $PSScriptRoot 'Verify-Package.ps1') -Archive $temporaryZip -BuiltDll $dll
    if (Test-Path -LiteralPath $stage) {
        $links = Get-ChildItem -LiteralPath $stage -Recurse -Force |
            Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }
        if ($links) { throw 'Refusing a staging folder containing redirected paths.' }
        if ([IO.Path]::GetFullPath($stage) -ne [IO.Path]::GetFullPath((Join-Path $repo 'build\staging'))) {
            throw 'Unexpected staging path.'
        }
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
    Move-Item -LiteralPath $temporary -Destination $stage
    Move-Item -LiteralPath $temporaryZip -Destination $archive -Force
    Write-Host "Release folder: $stage"
    Write-Host "Release archive: $archive"
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
    if ($temporaryZip -and (Test-Path -LiteralPath $temporaryZip)) { Remove-Item -LiteralPath $temporaryZip -Force }
}
