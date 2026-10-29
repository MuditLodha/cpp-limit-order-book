param(
    [ValidateSet("single", "study", "test")]
    [string]$Mode = "study",
    [int]$Seeds = 12,
    [int]$Seed = 1,
    [double]$Seconds = 120,
    [double]$Dt = 0.1,
    [ValidateSet("fixed", "inventory", "as")]
    [string]$Strategy = "inventory",
    [string]$Output = ""
)
$ErrorActionPreference = "Stop"
if ($Mode -eq "test") {
    & (Join-Path $PSScriptRoot "build.ps1") -Target sim-test
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $PSScriptRoot "build.ps1") -Target sim-build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
    & (Join-Path $PSScriptRoot "build.ps1") -Target sim-build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if (-not $Output) { $Output = Join-Path $PSScriptRoot "results/$Mode" }
    if ($Mode -eq "single") {
        & (Join-Path $PSScriptRoot "bin/market_making.exe") --strategy $Strategy --seed $Seed --seconds $Seconds --dt $Dt --output $Output
        exit $LASTEXITCODE
    }
}
$PythonCommand = Get-Command python -ErrorAction SilentlyContinue
if (-not $PythonCommand) { throw "Python 3 is required. Install Python, then run: python -m pip install -r simulation/requirements.txt" }
if ($Mode -eq "test") {
    & $PythonCommand.Source (Join-Path $PSScriptRoot "tests/test_simulation.py")
} else {
    & $PythonCommand.Source (Join-Path $PSScriptRoot "simulation/run_study.py") --seeds $Seeds --seed $Seed --seconds $Seconds --dt $Dt --output $Output
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
