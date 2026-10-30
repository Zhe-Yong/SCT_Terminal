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

function Get-Crc32Hex {
    param([byte[]]$Bytes)

    [uint32]$crc = [uint32]::MaxValue
    [uint32]$polynomial = [Convert]::ToUInt32('EDB88320', 16)
    foreach ($value in $Bytes) {
        $crc = [uint32]($crc -bxor [uint32]$value)
        for ($bit = 0; $bit -lt 8; $bit++) {
            if (($crc -band [uint32]1) -ne 0) {
                $crc = [uint32](($crc -shr 1) -bxor $polynomial)
            } else {
                $crc = [uint32]($crc -shr 1)
            }
        }
    }
    $crc = [uint32]($crc -bxor [uint32]::MaxValue)
    return ('{0:X8}' -f $crc)
}

function Get-ProjectBuildId {
    param([string]$Root)

    $extensions = @(
        '.c', '.cc', '.cpp', '.cxx',
        '.h', '.hh', '.hpp', '.hxx',
        '.rc', '.def', '.asm'
    )
    $excluded = @('.git', 'build', 'release', 'NSudo')

    $paths = @(
        Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object {
            $relative = $_.FullName.Substring($Root.Length).TrimStart('\', '/')
            $skip = $false
            foreach ($segment in ($relative -split '[\\/]')) {
                if ($excluded -contains $segment) {
                    $skip = $true
                    break
                }
            }
            (-not $skip) -and ($extensions -contains $_.Extension.ToLowerInvariant())
        } | ForEach-Object { $_.FullName }
    )

    if ($paths.Count -eq 0) {
        return '00000000'
    }

    [Array]::Sort($paths, [StringComparer]::Ordinal)
    $stream = New-Object System.IO.MemoryStream
    try {
        foreach ($path in $paths) {
            $relative = $path.Substring($Root.Length).TrimStart('\', '/').Replace('\', '/')
            $nameBytes = [Text.Encoding]::UTF8.GetBytes($relative)
            $stream.Write($nameBytes, 0, $nameBytes.Length)
            $stream.WriteByte(0)
            $bytes = [IO.File]::ReadAllBytes($path)
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.WriteByte(0)
        }

        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            $digest = $sha256.ComputeHash($stream.ToArray())
        } finally {
            $sha256.Dispose()
        }
    } finally {
        $stream.Dispose()
    }

    return Get-Crc32Hex -Bytes $digest
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
