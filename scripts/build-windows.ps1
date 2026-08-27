param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$compiler = 'C:\Develop\MinGW64\bin\g++.exe'
$resourceCompiler = 'C:\Develop\MinGW64\bin\windres.exe'
$outputDirectory = Join-Path $projectRoot 'build\windows-release'
$output = Join-Path $outputDirectory 'dsh-launcher.exe'

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "C++ compiler not found: $compiler"
}

New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

& "$PSScriptRoot\generate-icon.ps1"
if (-not $?) { throw 'Icon generation failed' }
$resourceObject = Join-Path $outputDirectory 'app-icon.o'
Push-Location $projectRoot
try {
    & $resourceCompiler 'resources\windows\app.rc' '-O' 'coff' '-o' $resourceObject
    if ($LASTEXITCODE -ne 0) { throw 'Windows resource compilation failed' }
} finally {
    Pop-Location
}

$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-mwindows', "-I$projectRoot\src")
if ($Configuration -eq 'Release') {
    $flags += @('-O2', '-DNDEBUG', '-s', '-static-libgcc', '-static-libstdc++')
} else {
    $flags += @('-O0', '-g')
}

$sources = @(
    "$projectRoot\src\app\main_windows.cpp",
    "$projectRoot\src\core\log.cpp",
    "$projectRoot\src\core\semver.cpp",
    "$projectRoot\src\core\service.cpp",
    "$projectRoot\src\platform\platform_windows.cpp"
    $resourceObject
)

& $compiler @flags @sources '-o' $output '-lws2_32' '-lshell32' '-lwinhttp' '-lole32' '-luuid' '-lcomctl32' '-ldwmapi'
if ($LASTEXITCODE -ne 0) {
    throw "Windows build failed with exit code $LASTEXITCODE"
}

$item = Get-Item -LiteralPath $output
Write-Output "Built: $($item.FullName)"
Write-Output "Size:  $($item.Length) bytes"
