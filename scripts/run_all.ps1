# Builds the project, runs the test suite, all experiments and the analysis (Windows PowerShell)
param(
    [ValidateSet("quick", "full")] [string]$Mode = "quick",
    [string]$BuildDir = "build"
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

function Invoke-Checked {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "command failed ($LASTEXITCODE): $Exe $($Arguments -join ' ')" }
}

# cmake on PATH, otherwise the copy inside the newest Visual Studio installation (found with vswhere)
function Find-CMake {
    $OnPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($OnPath) { return $OnPath.Source }

    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VsWhere) {
        $Bundled = & $VsWhere -latest -prerelease -products * `
            -find "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" | Select-Object -First 1
        if ($Bundled -and (Test-Path $Bundled)) { return $Bundled }
    }

    throw ("CMake not found. Open the Visual Studio Installer, choose Modify, and under " +
           "'Desktop development with C++' tick 'C++ CMake tools for Windows'; " +
           "or install CMake with: winget install Kitware.CMake")
}

# skips MS placeholder
function Find-Python {
    foreach ($Candidate in @("py -3", "python", "python3")) {
        $Parts = $Candidate -split " "
        $Command = Get-Command $Parts[0] -ErrorAction SilentlyContinue
        if (-not $Command) { continue }

        $Prefix = @($Parts | Select-Object -Skip 1)
        try {
            $Version = & $Command.Source @Prefix --version 2>&1
            if ($LASTEXITCODE -eq 0 -and "$Version" -match "Python 3") {
                return @{ Exe = $Command.Source; Prefix = $Prefix }
            }
        } catch {
            continue
        }
    }

    throw "Python 3 not found (needed only for graphs): winget install Python.Python.3.12"
}

$CMake = Find-CMake
Write-Host "== configure + build (Release) using $CMake"
Invoke-Checked $CMake @("-S", ".", "-B", $BuildDir)
Invoke-Checked $CMake @("--build", $BuildDir, "--config", "Release", "--parallel")

# Visual Studio generators put binaries in <build>\Release, Ninja/Makefiles in <build>
$Bin = Join-Path $BuildDir "Release"
if (-not (Test-Path (Join-Path $Bin "crdt_bench.exe"))) { $Bin = $BuildDir }
$Bench = Join-Path $Bin "crdt_bench.exe"
$Tests = Join-Path $Bin "crdt_tests.exe"

$Out = Join-Path "results" $Mode
New-Item -ItemType Directory -Force -Path $Out | Out-Null

Write-Host "== environment"
& $Bench env | Tee-Object -FilePath (Join-Path $Out "environment.txt")

Write-Host "== correctness tests"
& $Tests --json (Join-Path $Out "tests_summary.json") | Tee-Object -FilePath (Join-Path $Out "tests.log")
if ($LASTEXITCODE -ne 0) { throw "tests failed" }

Write-Host "== experiments ($Mode)"
foreach ($cfg in @("micro", "merge", "memory", "workloads", "network", "sync_intervals", "scale_replicas", "scale_ops", "intensity")) {
    Invoke-Checked $Bench @("run", "configs\$Mode\$cfg.ini", "--out", $Out)
}

Write-Host "== analysis (graphs + tables)"
$Python = Find-Python
Invoke-Checked $Python.Exe ($Python.Prefix + @("scripts\analyze.py", $Out))

Write-Host "== done: results in $Out, figures in $Out\figures, tables in $Out\tables.md"
