[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'all')]
    [string]$Arch = 'x64',

    [switch]$Zip
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$release = Join-Path $root 'release'
New-Item -ItemType Directory -Force -Path $release | Out-Null

function Get-BuildInfo {
    param([string]$TargetArch)

    if ($TargetArch -eq 'x64') {
        return [pscustomobject]@{
            Compiler = 'C:\msys64\ucrt64\bin\g++.exe'
            Output = (Join-Path $release 'main.exe')
            Name = 'x64'
        }
    }

    return [pscustomobject]@{
        Compiler = 'C:\msys64\mingw32\bin\g++.exe'
        Output = (Join-Path $release 'main32.exe')
        Name = 'x86'
    }
}

function Invoke-Build {
    param([string]$TargetArch)

    $info = Get-BuildInfo $TargetArch
    $compiler = $info.Compiler
    if (-not (Test-Path -LiteralPath $compiler)) {
        throw ("Compiler for {0} was not found: {1}`nInstall the matching MSYS2 toolchain or update tools\build-release.ps1." -f $TargetArch, $compiler)
    }

    $sources = @(
        Get-ChildItem -LiteralPath $root -Filter '*.cpp' -File
        Get-ChildItem -LiteralPath (Join-Path $root 'commands') -Filter '*.cpp' -File -ErrorAction SilentlyContinue
    ) | ForEach-Object { $_.FullName }
    if (-not $sources) {
        throw 'No C++ source files were found.'
    }

    $arguments = @(
        '-fdiagnostics-color=always',
        '-std=c++17',
        '-O2',
        '-s',
        '-static',
        '-I', $root
    ) + $sources + @(
        '-o', $info.Output,
        '-lshell32',
        '-lole32',
        '-ladvapi32',
        '-lbcrypt'
    )

    Write-Host ("Building {0} -> {1}" -f $TargetArch, $info.Output)
    & $compiler @arguments
    if ($LASTEXITCODE -ne 0) {
        throw ("Build failed for {0} with exit code {1}." -f $TargetArch, $LASTEXITCODE)
    }
}

function Copy-NSudoRuntime {
    $destinationRoot = Join-Path $release 'NSudo'
    if (Test-Path -LiteralPath $destinationRoot) {
        Remove-Item -LiteralPath $destinationRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $destinationRoot | Out-Null

    foreach ($runtime in @('x64', 'Win32')) {
        $source = Join-Path $root ("NSudo\{0}" -f $runtime)
        if (-not (Test-Path -LiteralPath $source)) {
            throw ("Missing NSudo runtime directory: {0}" -f $source)
        }

        $destination = Join-Path $destinationRoot $runtime
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        Copy-Item -Path (Join-Path $source '*') -Destination $destination -Recurse -Force
    }
}

function New-ReleaseArchive {
    param([string]$TargetArch)

    $archive = Join-Path $release ("SCT_{0}.zip" -f $TargetArch)
    if (Test-Path -LiteralPath $archive) {
        Remove-Item -LiteralPath $archive -Force
    }

    $executables = switch ($TargetArch) {
        'x64' { @('main.exe') }
        'x86' { @('main32.exe') }
        default { @('main.exe', 'main32.exe') }
    }

    $items = @()
    foreach ($name in $executables) {
        $path = Join-Path $release $name
        if (Test-Path -LiteralPath $path) {
            $items += $path
        }
    }

    $nsudo = Join-Path $release 'NSudo'
    if (Test-Path -LiteralPath $nsudo) {
        $items += $nsudo
    }

    if ($items.Count -eq 0) {
        throw 'Nothing was available to package.'
    }

    Compress-Archive -Path $items -DestinationPath $archive -Force
    Write-Host ("Created {0}" -f $archive)
}

if ($Arch -eq 'all') {
    Invoke-Build 'x64'
    Invoke-Build 'x86'
} else {
    Invoke-Build $Arch
}

Copy-NSudoRuntime

if ($Zip) {
    New-ReleaseArchive $Arch
}

Write-Host ("Release folder ready: {0}" -f $release)
