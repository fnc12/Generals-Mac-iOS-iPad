# Builds END Editor for Windows: the C++ core as end_editor_core_c.dll (CMake, MSVC x64, with its
# unit tests and the command line harness) and the WPF client, published self-contained into one
# folder (no .NET install needed to run it).
#   powershell -ExecutionPolicy Bypass -File Editor\windows\build.ps1 [-Out <folder>] [-Build <folder>] [-SkipTests] [-Jobs <n>]
# -Jobs limits the parallel compilers (CMake and dotnet; default: all cores), e.g. while the PC is in use.
# Defaults: build\editor-windows\ (core build tree and app) next to the repository's Editor folder.
param(
    [string]$Out = "",
    [string]$Build = "",
    [switch]$SkipTests,
    [int]$Jobs = 0
)
$ErrorActionPreference = 'Stop'
$editor = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$root = Split-Path $editor
if (-not $Build) { $Build = Join-Path $root 'build\editor-windows' }
if (-not $Out) { $Out = Join-Path $Build 'END Editor' }
$coreBuild = Join-Path $Build 'core'

# MSVC x64 environment (vswhere finds Visual Studio or the Build Tools).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "MSVC (Visual Studio or the Build Tools with the C++ x64 tools) not found" }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
# x64 binaries; on an ARM64 Windows the ARM64-hosted x64 cross compilers (the x64 ones run emulated).
$arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64_amd64' } else { 'x64' }
cmd /c "`"$vcvars`" $arch >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
}
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$generator = if (Test-Path $ninja) { @('-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja") } else { @() }

$tests = if ($SkipTests) { 'OFF' } else { 'ON' }
& cmake -S (Join-Path $editor 'core') -B $coreBuild @generator -DCMAKE_BUILD_TYPE=Release "-DEND_EDITOR_BUILD_TESTS=$tests"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
$parallel = if ($Jobs -gt 0) { @('--parallel', "$Jobs") } else { @('--parallel') }
& cmake --build $coreBuild @parallel
if ($LASTEXITCODE -ne 0) { throw "core build failed" }
if (-not $SkipTests) {
    & (Join-Path $coreBuild 'end_editor_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw "core tests failed ($LASTEXITCODE)" }
}

$dll = Join-Path $coreBuild 'end_editor_core_c.dll'
& dotnet publish (Join-Path $PSScriptRoot 'EndEditor\EndEditor.csproj') -c Release -r win-x64 --self-contained true `
    "-p:EndEditorCoreDll=$dll" -o $Out $(if ($Jobs -gt 0) { "-maxcpucount:$Jobs" })
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed" }
Write-Host "built: $(Join-Path $Out 'END Editor.exe')"
