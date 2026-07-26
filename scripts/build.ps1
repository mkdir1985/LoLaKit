param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Debug",

  [switch]$Clean,

  [switch]$SkipTests,

  [switch]$RunBenchmarks,

  [switch]$RunIoBenchmarks,

  [UInt64]$BenchmarkIterations = 1000000,

  [UInt64]$BenchmarkWarmupIterations = 200000,

  [UInt64]$BenchmarkBatchSize = 50000,

  [int]$BenchmarkAffinityCpu = -1,

  [string]$BenchmarkCsvPath = "",

  [UInt64]$IoBenchmarkIterations = 200000,

  [UInt64]$IoBenchmarkWarmupIterations = 50000,

  [UInt64]$IoBenchmarkBatchSize = 10000,

  [UInt64]$IoBenchmarkBlockSize = 256,

  [int]$IoBenchmarkAffinityCpu = -1,

  [string]$IoBenchmarkCsvPath = "",

  [string]$IoBenchmarkTempDirectory = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# Some hosted shells inject invalid wildcard keys into PSDefaultParameterValues,
# which breaks unrelated cmdlets during parameter binding. The build script runs
# in its own process, so clearing them here is safe and keeps the environment
# bootstrap deterministic.
if ($PSDefaultParameterValues.Count -gt 0) {
  $PSDefaultParameterValues.Clear()
}

function Find-VsDevCmd {
  $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $installPath = & $vswhere `
      -latest `
      -products * `
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
      -property installationPath

    if ($LASTEXITCODE -eq 0 -and $installPath) {
      $candidate = Join-Path $installPath "Common7\Tools\VsDevCmd.bat"
      if (Test-Path $candidate) {
        return $candidate
      }
    }
  }

  $searchRoots = @(
    "C:\Program Files\Microsoft Visual Studio",
    "C:\Program Files (x86)\Microsoft Visual Studio"
  )

  foreach ($root in $searchRoots) {
    if (-not (Test-Path $root)) {
      continue
    }

    $candidate = Get-ChildItem $root -Recurse -Filter "VsDevCmd.bat" -ErrorAction SilentlyContinue |
      Select-Object -First 1 -ExpandProperty FullName

    if ($candidate) {
      return $candidate
    }
  }

  throw "Unable to locate VsDevCmd.bat. Please install Visual Studio C++ Build Tools."
}

function Find-CMake {
  $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
  if ($cmakeCommand) {
    return $cmakeCommand.Source
  }

  $candidates = @(
    "C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\CMake\bin\cmake.exe"
  )

  foreach ($candidate in $candidates) {
    if (Test-Path $candidate) {
      return $candidate
    }
  }

  throw "Unable to locate cmake.exe."
}

function Invoke-InVsDevCmd {
  param($VsDevCmdPath, $InnerCommand)

  $command = "call `"$VsDevCmdPath`" -arch=x64 -host_arch=x64 >nul && $InnerCommand"
  & cmd.exe /d /s /c $command

  if ($LASTEXITCODE -ne 0) {
    throw "Command failed inside Visual Studio developer environment: $InnerCommand"
  }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$presetSuffix = $Configuration.ToLowerInvariant()
$configurePreset = $presetSuffix
$buildPreset = "build-$presetSuffix"
$testPreset = "test-$presetSuffix"
$buildDir = Join-Path $repoRoot "build\$presetSuffix"

if ($Clean -and (Test-Path $buildDir)) {
  Remove-Item -Recurse -Force $buildDir
}

$vsDevCmd = Find-VsDevCmd
$cmake = Find-CMake
$ctest = Join-Path (Split-Path $cmake -Parent) "ctest.exe"
if (-not (Test-Path $ctest)) {
  $ctest = "ctest.exe"
}

Push-Location $repoRoot
try {
  Invoke-InVsDevCmd -VsDevCmdPath $vsDevCmd -InnerCommand "`"$cmake`" --preset $configurePreset"
  Invoke-InVsDevCmd -VsDevCmdPath $vsDevCmd -InnerCommand "`"$cmake`" --build --preset $buildPreset"

  if (-not $SkipTests) {
    Invoke-InVsDevCmd -VsDevCmdPath $vsDevCmd -InnerCommand "`"$ctest`" --preset $testPreset"
  }

  if ($RunBenchmarks) {
    $benchmarkExe = Join-Path $buildDir "lolakit_core_benchmark.exe"
    if (-not (Test-Path $benchmarkExe)) {
      throw "Benchmark executable not found: $benchmarkExe"
    }

    $benchmarkArgs = @(
      "--iterations", $BenchmarkIterations.ToString(),
      "--warmup-iterations", $BenchmarkWarmupIterations.ToString(),
      "--batch-size", $BenchmarkBatchSize.ToString()
    )

    if ($BenchmarkAffinityCpu -ge 0) {
      $benchmarkArgs += @("--affinity-cpu", $BenchmarkAffinityCpu.ToString())
    }

    if ($BenchmarkCsvPath) {
      $benchmarkArgs += @("--csv", $BenchmarkCsvPath)
    }

    & $benchmarkExe @benchmarkArgs
    if ($LASTEXITCODE -ne 0) {
      throw "Benchmark execution failed."
    }
  }

  if ($RunIoBenchmarks) {
    $ioBenchmarkExe = Join-Path $buildDir "lolakit_io_benchmark.exe"
    if (-not (Test-Path $ioBenchmarkExe)) {
      throw "IO benchmark executable not found: $ioBenchmarkExe"
    }

    $ioBenchmarkArgs = @(
      "--iterations", $IoBenchmarkIterations.ToString(),
      "--warmup-iterations", $IoBenchmarkWarmupIterations.ToString(),
      "--batch-size", $IoBenchmarkBatchSize.ToString(),
      "--block-size", $IoBenchmarkBlockSize.ToString()
    )

    if ($IoBenchmarkAffinityCpu -ge 0) {
      $ioBenchmarkArgs += @("--affinity-cpu", $IoBenchmarkAffinityCpu.ToString())
    }

    if ($IoBenchmarkCsvPath) {
      $ioBenchmarkArgs += @("--csv", $IoBenchmarkCsvPath)
    }

    if ($IoBenchmarkTempDirectory) {
      $ioBenchmarkArgs += @("--temp-directory", $IoBenchmarkTempDirectory)
    }

    & $ioBenchmarkExe @ioBenchmarkArgs
    if ($LASTEXITCODE -ne 0) {
      throw "IO benchmark execution failed."
    }
  }
}
finally {
  Pop-Location
}
