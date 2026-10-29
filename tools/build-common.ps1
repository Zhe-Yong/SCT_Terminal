function Resolve-BuildCompiler {
    param(
        [string]$Explicit,
        [string[]]$CommandNames,
        [string]$Architecture,
        [string]$EnvironmentVariable
    )

    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit)) {
            throw ("Compiler for {0} was not found: {1}`nSet {2} to a valid compiler path." -f $Architecture, $Explicit, $EnvironmentVariable)
        }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }

    foreach ($name in $CommandNames) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) {
            return $command.Source
        }
    }

    throw ("Compiler for {0} was not found.`nSet {1} or add one of these to PATH: {2}" -f $Architecture, $EnvironmentVariable, ($CommandNames -join ', '))
}

function Invoke-BuildCompiler {
    param(
        [string]$Compiler,
        [string[]]$Arguments
    )

    $compilerDirectory = Split-Path -Parent $Compiler
    $originalPath = $env:PATH
    try {
        $env:PATH = $compilerDirectory + ';' + $originalPath
        & $Compiler @Arguments
        return $LASTEXITCODE
    } finally {
        $env:PATH = $originalPath
    }
}

function Get-ProjectSourceFiles {
    param([string]$Root)

    Push-Location $Root
    try {
        $files = @()
        $files += Get-ChildItem -LiteralPath '.' -Filter '*.cpp' -File |
            ForEach-Object { $_.Name }
        $files += Get-ChildItem -LiteralPath '.\commands' -Filter '*.cpp' -File -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path 'commands' $_.Name }
        return $files
    } finally {
        Pop-Location
    }
}

function Get-ProjectPrefixMapArguments {
    param([string]$Root)

    $arguments = @('-ffile-prefix-map=' + $Root + '=.')
    if ($Root -match '^([A-Za-z]):\\(.*)$') {
        $drive = $matches[1].ToUpperInvariant() + ':'
        $relative = $matches[2]
        $disk = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$drive'" -ErrorAction SilentlyContinue
        if ($disk -and $disk.ProviderName) {
            $uncRoot = Join-Path $disk.ProviderName $relative
            if ($uncRoot -ne $Root) {
                $arguments += '-ffile-prefix-map=' + $uncRoot + '=.'
            }
        }
    }
    $arguments += '-fcanon-prefix-map'
    return $arguments
}
