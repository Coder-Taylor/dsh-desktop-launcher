param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$systemToolchain = 'C:\Develop\MinGW64'
$fallbackToolchain = Join-Path $projectRoot 'build\toolchain\mingw64'
$toolchain = if ($env:DSH_LAUNCHER_TOOLCHAIN) { $env:DSH_LAUNCHER_TOOLCHAIN } else { $systemToolchain }
if (-not $env:DSH_LAUNCHER_TOOLCHAIN -and
    (-not (Test-Path -LiteralPath (Join-Path $systemToolchain 'bin\g++.exe')) -or
     -not (Test-Path -LiteralPath (Join-Path $systemToolchain 'x86_64-w64-mingw32\include\windows.h')))) {
    $toolchain = $fallbackToolchain
}
$compiler = Join-Path $toolchain 'bin\g++.exe'
if (-not (Test-Path -LiteralPath $compiler)) {
    throw "Windows compiler toolchain is incomplete: $toolchain"
}

$outputDirectory = Join-Path $projectRoot 'build\windows-tests'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$commonFlags = @('-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-static', "-I$projectRoot\src")

function Invoke-NativeTest {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string[]]$Sources,
        [string[]]$Libraries = @()
    )

    $output = Join-Path $outputDirectory "$Name.exe"
    & $compiler @commonFlags @Sources '-o' $output @Libraries
    if ($LASTEXITCODE -ne 0) { throw "$Name compilation failed with exit code $LASTEXITCODE" }
    & $output
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE" }
    Write-Output "PASS: $Name"
}

Push-Location $projectRoot
try {
    Invoke-NativeTest -Name 'semver-test' -Sources @(
        'tests\semver_test.cpp',
        'src\core\semver.cpp'
    )
    Invoke-NativeTest -Name 'log-test' -Sources @(
        'tests\log_test.cpp',
        'src\core\log.cpp'
    )
    Invoke-NativeTest -Name 'integrity-test' -Sources @(
        'tests\integrity_test.cpp',
        'src\platform\platform_windows.cpp'
    ) -Libraries @('-lws2_32', '-lshell32', '-lwinhttp', '-lole32', '-luuid')
    Invoke-NativeTest -Name 'uninstall-test' -Sources @(
        'tests\uninstall_test.cpp',
        'src\platform\platform_windows.cpp'
    ) -Libraries @('-lws2_32', '-lshell32', '-lwinhttp', '-lole32', '-luuid')
} finally {
    Pop-Location
}
