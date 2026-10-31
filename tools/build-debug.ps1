[CmdletBinding()]
param(
    [string]$Compiler = $env:SCT_X64_COMPILER
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'build-common.ps1')

$root = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $root 'build\debug'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

$compiler = Resolve-BuildCompiler -Explicit $Compiler `
    -CommandNames @('x86_64-w64-mingw32-g++.exe', 'g++.exe') `
    -Architecture 'x64' -EnvironmentVariable 'SCT_X64_COMPILER'

$sources = @(Get-ProjectSourceFiles -Root $root)
if (-not $sources) {
    throw 'No C++ source files were found.'
}

$buildId = Get-ProjectBuildId -Root $root
$output = Join-Path $outputDirectory 'SCT64.exe'
$arguments = @(
    '-fdiagnostics-color=always',
    '-std=c++17',
    '-g3',
    '-O0',
    '-static',
    '-mwindows',
    '-Wall',
    '-Wextra',
    ('-DSCT_BUILD_NUMBER={0}' -f $buildId),
    '-I', '.'
) + @(Get-ProjectPrefixMapArguments -Root $root) + $sources + @(
    '-o', $output,
    '-lshell32',
    '-lole32',
    '-lgdi32',
    '-ladvapi32',
    '-lcrypt32',
    '-lwintrust',
    '-lbcrypt',
    '-lversion'
)

Write-Host ("Building x64 debug -> {0}" -f $output)
Push-Location $root
try {
    $exitCode = Invoke-BuildCompiler -Compiler $compiler -Arguments $arguments
    if ($exitCode -ne 0) {
        throw ("Debug build failed with exit code {0}." -f $exitCode)
    }
} finally {
    Pop-Location
}

$nsudoDestination = Join-Path $outputDirectory 'NSudo'
if (Test-Path -LiteralPath $nsudoDestination) {
    Remove-Item -LiteralPath $nsudoDestination -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $nsudoDestination | Out-Null

foreach ($runtime in @('x64', 'Win32')) {
    $source = Join-Path $root ("NSudo\{0}" -f $runtime)
    if (-not (Test-Path -LiteralPath $source)) {
        throw ("Missing NSudo runtime directory: {0}" -f $source)
    }

    $destination = Join-Path $nsudoDestination $runtime
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Copy-Item -Path (Join-Path $source '*') -Destination $destination -Recurse -Force
}

foreach ($name in @('ppl64.dll', 'ppl32.dll')) {
    $source = Join-Path $root ("ppl\{0}" -f $name)
    if (-not (Test-Path -LiteralPath $source)) {
        throw ("Missing PPL DLL: {0}" -f $source)
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $outputDirectory $name) -Force
}

Write-Host ("Debug build ready: {0}" -f $output)
