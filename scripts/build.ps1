[CmdletBinding()]
param(
    [string] $Compiler = $env:CXX,
    [switch] $NoPublish,
    [string] $BuildDirectory,
    [string] $DistDirectory
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Compiler) { $Compiler = 'g++.exe' }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $root 'build' }
elseif (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $root $BuildDirectory }
if (-not $DistDirectory) { $DistDirectory = Join-Path $root 'dist' }
elseif (-not [IO.Path]::IsPathRooted($DistDirectory)) { $DistDirectory = Join-Path $root $DistDirectory }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$DistDirectory = [IO.Path]::GetFullPath($DistDirectory)
$command = Get-Command $Compiler -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $command) {
    throw 'A 64-bit MinGW-w64 g++ was not found. Put it on PATH, set CXX, or pass -Compiler <path-to-g++.exe>.'
}
$cxx = $command.Source
$machine = (& $cxx '-dumpmachine' | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $machine -notmatch '^x86_64.*mingw') {
    throw "Expected a 64-bit MinGW-w64 compiler; target reported: $machine"
}
$compilerDir = Split-Path -Parent $cxx
$threadDll = Join-Path $compilerDir 'libwinpthread-1.dll'
if (-not (Test-Path -LiteralPath $threadDll -PathType Leaf)) {
    throw 'The compiler-adjacent libwinpthread-1.dll is missing. Use a complete MinGW-w64 distribution.'
}
$compilerVersion = (& $cxx '-dumpfullversion' | Out-String).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Could not determine the compiler version.' }
$compilerHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $cxx).Hash
$compilerEnvironment = @(
    $env:PATH, $env:GCC_EXEC_PREFIX, $env:COMPILER_PATH, $env:LIBRARY_PATH,
    $env:CPATH, $env:C_INCLUDE_PATH, $env:CPLUS_INCLUDE_PATH, $env:OBJC_INCLUDE_PATH
)

$src = Join-Path $root 'src'
$imgui = Join-Path $root 'third_party/imgui-1.93.0'
$build = $BuildDirectory
$release = Join-Path $build 'release'
$dist = $DistDirectory
$objectCache = Join-Path $build 'objects'
foreach ($dir in @($build, $release, $dist, $objectCache)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}
$common = @('-std=c++17', '-O2', '-static-libgcc', '-static-libstdc++', "-I$src")
# Do not embed workstation paths in assertions, macros or debug information.
$compilerRoot = Split-Path -Parent $compilerDir
foreach ($mapping in @(@($root, '.'), @($compilerRoot, '/toolchain'))) {
    $prefixes = @($mapping[0], $mapping[0].Replace('\', '/')) | Select-Object -Unique
    foreach ($prefix in $prefixes) {
        $common += "-ffile-prefix-map=$prefix=$($mapping[1])"
    }
}
$script:BuildCacheSchemaVersion = 2
$script:FileHashMemo = @{}
$script:ObjectValidationMemo = @{}
$script:ObjectsCompiled = 0
$script:ObjectsReused = 0
# Hashes and validations are memoized for this invocation. Keep sources/headers
# quiescent while the build is running.

function Invoke-Compiler([string[]] $CompilerArgs) {
    & $cxx @CompilerArgs
    if ($LASTEXITCODE -ne 0) { throw "Compiler failed with exit code $LASTEXITCODE." }
}

function Get-ObjectCacheKey([string] $Source, [string[]] $Flags) {
    $identity = [ordered]@{
        CacheSchema = $script:BuildCacheSchemaVersion
        CompilerPath = [IO.Path]::GetFullPath($cxx).ToLowerInvariant()
        CompilerSha256 = $compilerHash
        CompilerVersion = $compilerVersion
        CompilerTarget = $machine
        CompilerEnvironment = $compilerEnvironment
        Source = [IO.Path]::GetFullPath($Source)
        Flags = @($Flags)
    } | ConvertTo-Json -Compress -Depth 6
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity))).Replace('-', '')
    }
    finally { $sha.Dispose() }
}

function Get-CachedFileHash([string] $Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    if ($script:FileHashMemo.ContainsKey($fullPath)) { return $script:FileHashMemo[$fullPath] }
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Cache input is missing: $fullPath"
    }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $fullPath).Hash
    $script:FileHashMemo[$fullPath] = $hash
    return $hash
}

function Get-ObjectDependencies([string] $DependencyPath) {
    if (-not (Test-Path -LiteralPath $DependencyPath -PathType Leaf)) {
        throw "Compiler dependency file is missing: $DependencyPath"
    }
    $dependencyText = [IO.File]::ReadAllText($DependencyPath)
    $dependencyText = [Regex]::Replace($dependencyText, '\\\r?\n', ' ')
    $targetMarker = 'op2_cache_target:'
    $targetIndex = $dependencyText.IndexOf($targetMarker, [StringComparison]::Ordinal)
    if ($targetIndex -lt 0) { throw "Compiler dependency file has no $targetMarker target: $DependencyPath" }

    $dependencies = [Collections.Generic.List[string]]::new()
    $token = [Text.StringBuilder]::new()
    $characters = $dependencyText.Substring($targetIndex + $targetMarker.Length).ToCharArray()
    for ($index = 0; $index -lt $characters.Length; $index++) {
        $character = $characters[$index]
        if ($character -eq '\') {
            if ($index + 1 -lt $characters.Length) {
                $nextCharacter = $characters[$index + 1]
                if ([char]::IsWhiteSpace($nextCharacter) -or $nextCharacter -in @('\', '#', ':')) {
                    [void] $token.Append($nextCharacter)
                    $index++
                    continue
                }
            }
            [void] $token.Append($character)
            continue
        }
        if ([char]::IsWhiteSpace($character)) {
            if ($token.Length -gt 0) {
                $dependencies.Add($token.ToString())
                [void] $token.Clear()
            }
        }
        else { [void] $token.Append($character) }
    }
    if ($token.Length -gt 0) { $dependencies.Add($token.ToString()) }
    if ($dependencies.Count -eq 0) { throw "Compiler dependency file has no inputs: $DependencyPath" }

    $fullDependencies = @($dependencies | ForEach-Object {
        if ([IO.Path]::IsPathRooted($_)) { $path = $_ }
        else { $path = Join-Path (Get-Location).Path $_ }
        [IO.Path]::GetFullPath($path)
    } | Sort-Object -Unique)
    return $fullDependencies
}

function New-ObjectCacheValidationRecord(
    [string] $ObjectPath,
    [string] $DependencyPath,
    [string] $Key,
    [string[]] $Dependencies
) {
    $inputRecords = @(
        foreach ($dependency in $Dependencies) {
            [ordered]@{
                Path = [IO.Path]::GetFullPath($dependency)
                Sha256 = (Get-CachedFileHash $dependency)
            }
        }
    )
    return [ordered]@{
        Schema = $script:BuildCacheSchemaVersion
        Key = $Key
        ObjectFile = [IO.Path]::GetFileName($ObjectPath)
        DependencyFile = [IO.Path]::GetFileName($DependencyPath)
        ObjectSha256 = (Get-CachedFileHash $ObjectPath)
        DependencySha256 = (Get-CachedFileHash $DependencyPath)
        Inputs = $inputRecords
    }
}

function Test-ObjectDependenciesCurrent(
    [string] $ObjectPath,
    [string] $DependencyPath,
    [string] $ValidationPath,
    [string] $ExpectedKey
) {
    $memoKey = [IO.Path]::GetFullPath($ValidationPath)
    if ($script:ObjectValidationMemo.ContainsKey($memoKey)) {
        return [bool] $script:ObjectValidationMemo[$memoKey]
    }

    $isCurrent = $false
    try {
        foreach ($path in @($ObjectPath, $DependencyPath, $ValidationPath)) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'Cache file is missing.' }
        }
        $record = [IO.File]::ReadAllText($ValidationPath) | ConvertFrom-Json -ErrorAction Stop
        if ([int] $record.Schema -ne $script:BuildCacheSchemaVersion -or
            [string] $record.Key -cne $ExpectedKey -or
            [string] $record.ObjectFile -cne [IO.Path]::GetFileName($ObjectPath) -or
            [string] $record.DependencyFile -cne [IO.Path]::GetFileName($DependencyPath)) {
            throw 'Cache validation record identity does not match.'
        }
        if ((Get-CachedFileHash $ObjectPath) -cne [string] $record.ObjectSha256 -or
            (Get-CachedFileHash $DependencyPath) -cne [string] $record.DependencySha256) {
            throw 'Cached object/dependency pair hash does not match its validation record.'
        }

        $dependencies = @(Get-ObjectDependencies $DependencyPath)
        $inputRecords = @($record.Inputs)
        if ($dependencies.Count -ne $inputRecords.Count) { throw 'Dependency list count changed.' }
        for ($index = 0; $index -lt $dependencies.Count; $index++) {
            $recordedPath = [IO.Path]::GetFullPath([string] $inputRecords[$index].Path)
            if (-not [string]::Equals($dependencies[$index], $recordedPath, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Dependency list does not match its validation record.'
            }
            if ((Get-CachedFileHash $dependencies[$index]) -cne [string] $inputRecords[$index].Sha256) {
                throw "Dependency contents changed: $($dependencies[$index])"
            }
        }
        $isCurrent = $true
    }
    catch { $isCurrent = $false }

    $script:ObjectValidationMemo[$memoKey] = $isCurrent
    return $isCurrent
}

function Clear-CachedFileHash([string] $Path) {
    [void] $script:FileHashMemo.Remove([IO.Path]::GetFullPath($Path))
}

function Get-CachedObject([string] $Source, [string[]] $Flags) {
    $sourcePath = [IO.Path]::GetFullPath($Source)
    $key = Get-ObjectCacheKey $sourcePath $Flags
    $objectPath = Join-Path $objectCache "$key.o"
    $dependencyPath = Join-Path $objectCache "$key.d"
    $validationPath = Join-Path $objectCache "$key.record.json"
    if (Test-ObjectDependenciesCurrent $objectPath $dependencyPath $validationPath $key) {
        $script:ObjectsReused++
        return $objectPath
    }

    $temporaryObject = Join-Path $objectCache "$key.$([Guid]::NewGuid().ToString('N')).tmp.o"
    $temporaryDependencies = "$temporaryObject.d"
    $temporaryValidation = "$temporaryObject.record.json"
    try {
        Invoke-Compiler ($Flags + @(
            '-MD', '-MF', $temporaryDependencies, '-MT', 'op2_cache_target',
            '-c', $sourcePath, '-o', $temporaryObject
        ))
        if (-not (Test-Path -LiteralPath $temporaryObject -PathType Leaf) -or
            -not (Test-Path -LiteralPath $temporaryDependencies -PathType Leaf)) {
            throw "Compiler did not produce a complete cached object for $sourcePath."
        }
        $dependencies = @(Get-ObjectDependencies $temporaryDependencies)
        foreach ($dependency in $dependencies) { [void] (Get-CachedFileHash $dependency) }

        # The record is the commit marker: invalidate it before replacing either
        # file, then publish a new record only after both replacements succeed.
        Remove-Item -LiteralPath $validationPath -Force -ErrorAction SilentlyContinue
        Move-Item -LiteralPath $temporaryObject -Destination $objectPath -Force
        Move-Item -LiteralPath $temporaryDependencies -Destination $dependencyPath -Force
        Clear-CachedFileHash $objectPath
        Clear-CachedFileHash $dependencyPath
        $record = New-ObjectCacheValidationRecord $objectPath $dependencyPath $key $dependencies
        $recordJson = $record | ConvertTo-Json -Compress -Depth 8
        [IO.File]::WriteAllText($temporaryValidation, $recordJson, [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporaryValidation, $validationPath)
        $script:ObjectValidationMemo[[IO.Path]::GetFullPath($validationPath)] = $true
        $script:ObjectsCompiled++
        return $objectPath
    }
    finally {
        Remove-Item -LiteralPath $temporaryObject, $temporaryDependencies, $temporaryValidation -Force -ErrorAction SilentlyContinue
    }
}

function Compile-And-Link([string] $Output, [string[]] $Inputs, [string[]] $Flags, [string[]] $LinkFlags = @()) {
    $objects = @($Inputs | ForEach-Object { Get-CachedObject $_ $Flags })
    Invoke-Compiler ($Flags + $objects + $LinkFlags + @('-o', $Output))
}

$production = @(
    'main.cpp', 'game_paths.cpp', 'patcher.cpp', 'wide_fov_patch.cpp',
    'keyboard_layout_patch.cpp', 'startup_debugger.cpp',
    'fov_controller.cpp', 'fov_settings.cpp', 'hotkeys.cpp', 'mini_json.cpp', 'style.cpp'
) | ForEach-Object { Join-Path $src $_ }
$imguiSources = @(
    'imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp',
    'backends/imgui_impl_win32.cpp', 'backends/imgui_impl_dx9.cpp'
) | ForEach-Object { Join-Path $imgui $_ }
$releaseExe = Join-Path $release 'op2_launcher.exe'
$flags = @('-mwindows', '-DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD', "-I$imgui", "-I$(Join-Path $imgui 'backends')")
$libs = @('-ld3d9', '-ldwmapi', '-luser32', '-lgdi32', '-lcomctl32', '-lole32', '-ladvapi32', '-s')
Compile-And-Link $releaseExe ($production + $imguiSources) ($common + $flags) $libs

$definitions = Join-Path $root 'patches.json'
Copy-Item -LiteralPath $threadDll -Destination (Join-Path $release 'libwinpthread-1.dll') -Force
Copy-Item -LiteralPath $definitions -Destination (Join-Path $release 'patches.json') -Force
function Copy-LegalNotices([string] $Directory) {
    $destination = Join-Path $Directory 'licenses'
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $root 'licenses') -File -Filter '*.txt' | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $destination $_.Name) -Force
    }
}
Copy-LegalNotices $release
Write-Host "Compile cache: $($script:ObjectsCompiled) object(s) compiled; $($script:ObjectsReused) reused."

if ($NoPublish) {
    Write-Host "Candidate built without changing dist: $release"
    return
}

# Refuse to replace locally edited patch definitions. The manifest permits
# automatic updates only when the existing file is our previous generated copy.
# Runtime configuration is never overwritten.
$distDefinitions = Join-Path $dist 'patches.json'
$manifestPath = Join-Path $dist 'build-manifest.json'
$sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $definitions).Hash
if (Test-Path -LiteralPath $distDefinitions -PathType Leaf) {
    $currentHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $distDefinitions).Hash
    if ($currentHash -ne $sourceHash) {
        $previousHash = $null
        if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
            try { $previousHash = (Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json).patchesSha256 }
            catch { $previousHash = $null }
        }
        if (-not $previousHash -or $currentHash -ne $previousHash) {
            throw 'dist/patches.json has local changes. Preserve or reconcile them before rebuilding.'
        }
    }
}
Copy-Item -LiteralPath $releaseExe -Destination (Join-Path $dist 'op2_launcher.exe') -Force
Copy-Item -LiteralPath $threadDll -Destination (Join-Path $dist 'libwinpthread-1.dll') -Force
Copy-Item -LiteralPath $definitions -Destination $distDefinitions -Force
Copy-LegalNotices $dist
$distConfig = Join-Path $dist 'config.json'
if (-not (Test-Path -LiteralPath $distConfig)) {
    Copy-Item -LiteralPath (Join-Path $root 'config.example.json') -Destination $distConfig
}
@{
    patchesSha256 = $sourceHash
    launcherSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $releaseExe).Hash
} | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Launcher published: $dist"
