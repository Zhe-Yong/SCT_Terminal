[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'all')]
    [string]$Arch = 'x64',

    [switch]$Zip,

    [string]$X64Compiler = $env:SCT_X64_COMPILER,

    [string]$X86Compiler = $env:SCT_X86_COMPILER
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'build-common.ps1')

$root = Split-Path -Parent $PSScriptRoot
$release = Join-Path $root 'release'
$buildId = Get-ProjectBuildId -Root $root
New-Item -ItemType Directory -Force -Path $release | Out-Null

function Get-BuildInfo {
    param([string]$TargetArch)

    if ($TargetArch -eq 'x64') {
        return [pscustomobject]@{
            Compiler = Resolve-BuildCompiler -Explicit $X64Compiler `
                -CommandNames @('x86_64-w64-mingw32-g++.exe', 'g++.exe') `
                -Architecture 'x64' -EnvironmentVariable 'SCT_X64_COMPILER'
            Output = (Join-Path $release 'SCT64.exe')
            Name = 'x64'
        }
    }

    return [pscustomobject]@{
        Compiler = Resolve-BuildCompiler -Explicit $X86Compiler `
            -CommandNames @('i686-w64-mingw32-g++.exe') `
            -Architecture 'x86' -EnvironmentVariable 'SCT_X86_COMPILER'
        Output = (Join-Path $release 'SCT32.exe')
        Name = 'x86'
    }
}

function Invoke-Build {
    param([string]$TargetArch)

    $info = Get-BuildInfo $TargetArch
    $compiler = $info.Compiler

    $sources = @(Get-ProjectSourceFiles -Root $root)
    if (-not $sources) {
        throw 'No C++ source files were found.'
    }

    $iconResource = Build-IconResource -Root $root -Compiler $compiler `
        -TargetArch $TargetArch `
        -OutputDirectory (Join-Path $root ("build\obj\{0}" -f $TargetArch))

    $arguments = @(
        '-fdiagnostics-color=always',
        '-std=c++17',
        '-O2',
        '-s',
        '-static',
        '-mwindows',
        ('-DSCT_BUILD_NUMBER={0}' -f $buildId),
        '-I', '.'
    ) + @(Get-ProjectPrefixMapArguments -Root $root) + $sources + @(
        $iconResource,
        '-o', $info.Output,
        '-lshell32',
        '-lole32',
        '-lgdi32',
        '-ladvapi32',
        '-lcrypt32',
        '-lwintrust',
        '-lbcrypt',
        '-lversion'
    )

    Write-Host ("Building {0} -> {1}" -f $TargetArch, $info.Output)
    Push-Location $root
    try {
        $exitCode = Invoke-BuildCompiler -Compiler $compiler -Arguments $arguments
        if ($exitCode -ne 0) {
            throw ("Build failed for {0} with exit code {1}." -f $TargetArch, $exitCode)
        }
    } finally {
        Pop-Location
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

function Copy-PplRuntime {
    foreach ($name in @('ppl64.dll', 'ppl32.dll')) {
        $source = Join-Path $root ("ppl\{0}" -f $name)
        if (-not (Test-Path -LiteralPath $source)) {
            throw ("Missing PPL DLL: {0}" -f $source)
        }
        Copy-Item -LiteralPath $source -Destination (Join-Path $release $name) -Force
    }
}

function New-ReleaseArchive {
    param([string]$TargetArch)

    $archive = Join-Path $release ("SCT_{0}.zip" -f $TargetArch)
    if (Test-Path -LiteralPath $archive) {
        Remove-Item -LiteralPath $archive -Force
    }

    $executables = switch ($TargetArch) {
        'x64' { @('SCT64.exe') }
        'x86' { @('SCT32.exe') }
        default { @('SCT64.exe', 'SCT32.exe') }
    }

    $items = @()
    foreach ($name in $executables) {
        $path = Join-Path $release $name
        if (Test-Path -LiteralPath $path) {
            $items += $path
        }
    }

    foreach ($dll in @('ppl64.dll', 'ppl32.dll')) {
        $path = Join-Path $release $dll
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
Copy-PplRuntime

if ($Zip) {
    if ($Arch -eq 'all') {
        New-ReleaseArchive 'x64'
        New-ReleaseArchive 'x86'
        New-ReleaseArchive 'all'
    } else {
        New-ReleaseArchive $Arch
    }
}

Write-Host ("Release folder ready: {0}" -f $release)
