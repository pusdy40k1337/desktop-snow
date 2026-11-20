<#
  One-command Windows build for Desktop Snow.

    build.cmd                 Release build + unit tests -> dist\DesktopSnow.exe
    build.cmd -Config Debug   Debug build -> dist-debug\
    build.cmd -Clean          wipe the build folder first
    build.cmd -NoTests        skip unit tests

  Needs Visual Studio 2019/2022 (or Build Tools) with "Desktop development with C++".
  CMake is taken from PATH or from the copy bundled with Visual Studio.
#>
param(
  [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
  [switch]$Clean,
  [switch]$NoTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Fail($msg) {
  Write-Host ""
  Write-Host "BUILD FAILED: $msg" -ForegroundColor Red
  exit 1
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
  Fail ("Visual Studio with C++ tools was not found. Install the free Build Tools:`n" +
        "  winget install Microsoft.VisualStudio.2022.BuildTools --override `"--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive`"")
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vsVer = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
if (-not $vsPath) { Fail "Visual Studio is installed but the C++ workload (VC tools x64) is missing." }
$major = [int]($vsVer.Split('.')[0])
switch ($major) {
  16 { $generator = 'Visual Studio 16 2019' }
  17 { $generator = 'Visual Studio 17 2022' }
  18 { $generator = 'Visual Studio 18 2026' }
  default { Fail "Unsupported Visual Studio version $vsVer" }
}

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
  $cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
  if (-not (Test-Path $cmake)) { Fail "CMake not found (install it, or add the 'C++ CMake tools' VS component)." }
}

$buildDir = Join-Path $root 'build\msvc'
if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

Write-Host "==> Configure ($generator, x64)" -ForegroundColor Cyan
& $cmake -S $root -B $buildDir -G $generator -A x64
if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed" }

Write-Host "==> Build $Config" -ForegroundColor Cyan
& $cmake --build $buildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { Fail "compilation failed" }

$bin = Join-Path $buildDir 'bin'
if (-not $NoTests) {
  Write-Host "==> Unit tests" -ForegroundColor Cyan
  & (Join-Path $bin 'snow_tests.exe')
  if ($LASTEXITCODE -ne 0) { Fail "unit tests failed" }
}

if ($Config -eq 'Release') { $dist = Join-Path $root 'dist' } else { $dist = Join-Path $root 'dist-debug' }
New-Item -ItemType Directory -Force -Path $dist | Out-Null
foreach ($exe in @('DesktopSnow.exe', 'SnowHarness.exe')) {
  $src = Join-Path $bin $exe
  if (Test-Path $src) { Copy-Item -Force $src $dist }
}
$assets = Join-Path $root 'assets'
if (Test-Path $assets) {
  Copy-Item -Recurse -Force $assets (Join-Path $dist 'assets')
}
Write-Host ""
Write-Host "OK: $(Join-Path $dist 'DesktopSnow.exe')" -ForegroundColor Green
Write-Host "    (single self-contained exe; copy it anywhere and run it)"
