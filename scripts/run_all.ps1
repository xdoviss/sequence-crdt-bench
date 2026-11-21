# Builds the project, runs the test suite, all experiments and the analysis (Windows PowerShell)
param(
    [ValidateSet("quick", "full")] [string]$Mode = "quick",
    [string]$BuildDir = "build",
    [string]$Python = ""   # optional path to the python.exe used for analysis
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

# picks the first Python 3 that can import the analysis packages (skips the MS Store placeholder
# and installs without matplotlib/pandas); -Python <path> overrides the search
function Find-Python {
    if ($Python) { return @{ Exe = $Python; Prefix = @() } }

    $Candidates = @("py -3", "python", "python3")
    # every version the py launcher knows about, e.g. "py -V:3.12"
    if (Get-Command py -ErrorAction SilentlyContinue) {
        $Candidates += @(py -0 2>$null | ForEach-Object { if ($_ -match "-V:(\S+)") { "py -V:$($Matches[1])" } })
    }

    $FirstPython3 = $null
    foreach ($Candidate in $Candidates) {
        $Parts = $Candidate -split " "
        $Command = Get-Command $Parts[0] -ErrorAction SilentlyContinue
        if (-not $Command) { continue }
        $Prefix = @($Parts | Select-Object -Skip 1)

        try {
            $Version = & $Command.Source @Prefix --version 2>&1
            if ($LASTEXITCODE -ne 0 -or "$Version" -notmatch "Python 3") { continue }
            if (-not $FirstPython3) { $FirstPython3 = "$Candidate" }

            & $Command.Source @Prefix -c "import matplotlib, pandas" 2>&1 | Out-Null
            if ($LASTEXITCODE -eq 0) {
                Write-Host "   using Python: $Candidate ($Version)"
                return @{ Exe = $Command.Source; Prefix = $Prefix }
            }
        } catch {
            continue
        }
    }

    if ($FirstPython3) {
        throw "No Python with matplotlib + pandas found. Install them with: $FirstPython3 -m pip install -r scripts\requirements.txt"
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
$Py = Find-Python
Invoke-Checked $Py.Exe ($Py.Prefix + @("scripts\analyze.py", $Out))

Write-Host "== done: results in $Out, figures in $Out\figures, tables in $Out\tables.md"
