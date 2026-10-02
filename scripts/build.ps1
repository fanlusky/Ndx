<#
.SYNOPSIS
    Builds Ndx with CMake + Ninja using the MSVC x64 toolchain.

.EXAMPLE
    .\scripts\build.ps1                    # Release build -> build\release\Ndx.exe
    .\scripts\build.ps1 -Config Debug      # Debug build   -> build\debug\Ndx.exe
    .\scripts\build.ps1 -Clean -Run        # Rebuild from scratch, then launch
#>
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    # Delete the build directory before configuring
    [switch]$Clean,
    # Launch Ndx.exe after a successful build
    [switch]$Run
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build\$($Config.ToLower())"

# Enter the Visual Studio developer environment unless cl.exe is already available
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found, please install Visual Studio with the C++ workload."
    }

    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) {
        throw "No Visual Studio installation with the C++ x64 toolset was found."
    }

    Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

foreach ($tool in 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool not found in PATH."
    }
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir" -ForegroundColor Cyan
    Remove-Item -Recurse -Force $buildDir
}

Write-Host "Configuring ($Config)" -ForegroundColor Cyan
cmake -S $root -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

Write-Host "Building ($Config)" -ForegroundColor Cyan
cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

# Let clangd pick up the compile flags (compile_commands.json is git-ignored)
$compileCommands = Join-Path $buildDir 'compile_commands.json'
if (Test-Path $compileCommands) {
    Copy-Item $compileCommands (Join-Path $root 'compile_commands.json') -Force
}

$exe = Join-Path $buildDir 'Ndx.exe'
Write-Host "Done: $exe" -ForegroundColor Green

if ($Run) {
    Start-Process $exe
}
