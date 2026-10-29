param (
    [string]$Target = "engine",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CommandArgs
)

$ErrorActionPreference = "Stop"

# Detect C++ compiler (UCRT64 GCC 15+ recommended for C++20)
$GppPath = "C:\msys64\ucrt64\bin\g++.exe"
if (Test-Path $GppPath) {
    $env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
} else {
    $GppCmd = Get-Command g++ -ErrorAction SilentlyContinue
    if ($GppCmd) {
        $GppPath = $GppCmd.Source
        $env:PATH = (Split-Path $GppPath) + ";" + $env:PATH
    } else {
        Write-Error "Could not find g++.exe in C:\msys64\ucrt64\bin or in system PATH."
        exit 1
    }
}

Write-Host "Using compiler: $GppPath" -ForegroundColor Cyan

$BinDir = Join-Path $PSScriptRoot "bin"
if (-not (Test-Path $BinDir)) {
    New-Item -ItemType Directory -Path $BinDir | Out-Null
}

$IncludeFlags = "-I$PSScriptRoot/include", "-I$PSScriptRoot/benchmarks"
$CommonFlags = @("-std=c++20", "-O3", "-Wall", "-Wextra") + $IncludeFlags

$CoreSources = @(
    (Join-Path $PSScriptRoot "src/OrderBook.cpp"),
    (Join-Path $PSScriptRoot "src/MatchingEngine.cpp"),
    (Join-Path $PSScriptRoot "src/ReferenceEngine.cpp")
)

function Compile-Binary {
    param (
        [string]$Name,
        [string]$MainSource,
        [string[]]$ExtraSources = @()
    )
    $Output = Join-Path $BinDir "$Name.exe"
    Write-Host "Compiling $Name -> $Output..." -ForegroundColor Yellow
    $AllSources = @($MainSource) + $CoreSources + $ExtraSources
    & $GppPath @CommonFlags @AllSources -o $Output
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Compilation of $Name failed!"
        exit 1
    }
    Write-Host "Successfully built $Name.exe" -ForegroundColor Green
}

switch ($Target.ToLower()) {
    "engine" {
        Compile-Binary "engine" (Join-Path $PSScriptRoot "src/main.cpp")
        if ($CommandArgs) {
            Write-Host "`nRunning engine with arguments: $CommandArgs`n" -ForegroundColor Cyan
            & (Join-Path $BinDir "engine.exe") @CommandArgs
        } else {
            Write-Host "`nLaunching Trading Terminal (Interactive Mode):`n" -ForegroundColor Cyan
            & (Join-Path $BinDir "engine.exe")
        }
    }
    "demo" {
        Compile-Binary "engine" (Join-Path $PSScriptRoot "src/main.cpp")
        Write-Host "`nRunning Demo Walkthrough:`n" -ForegroundColor Cyan
        & (Join-Path $BinDir "engine.exe") demo
    }
    "test" {
        Compile-Binary "unit_tests" (Join-Path $PSScriptRoot "tests/UnitTests.cpp")
        Write-Host "`nRunning Unit Tests:`n" -ForegroundColor Cyan
        & (Join-Path $BinDir "unit_tests.exe")
    }
    "diff" {
        Compile-Binary "diff_test" (Join-Path $PSScriptRoot "tests/DifferentialTest.cpp")
        Write-Host "`nRunning Differential Test:`n" -ForegroundColor Cyan
        & (Join-Path $BinDir "diff_test.exe") 25000 42
    }
    "bench" {
        Compile-Binary "benchmark" (Join-Path $PSScriptRoot "benchmarks/BenchmarkEngine.cpp")
        Write-Host "`nRunning Benchmarks:`n" -ForegroundColor Cyan
        & (Join-Path $BinDir "benchmark.exe")
    }
    "sim-build" {
        Compile-Binary "market_making" (Join-Path $PSScriptRoot "simulation/main.cpp") @((Join-Path $PSScriptRoot "src/MarketMaking.cpp"))
    }
    "sim" {
        Compile-Binary "market_making" (Join-Path $PSScriptRoot "simulation/main.cpp") @((Join-Path $PSScriptRoot "src/MarketMaking.cpp"))
        & (Join-Path $BinDir "market_making.exe") @CommandArgs
    }
    "sim-test" {
        Compile-Binary "market_making_tests" (Join-Path $PSScriptRoot "tests/MarketMakingTests.cpp") @((Join-Path $PSScriptRoot "src/MarketMaking.cpp"))
        & (Join-Path $BinDir "market_making_tests.exe")
    }
    "all" {
        Compile-Binary "engine" (Join-Path $PSScriptRoot "src/main.cpp")
        Compile-Binary "unit_tests" (Join-Path $PSScriptRoot "tests/UnitTests.cpp")
        Compile-Binary "diff_test" (Join-Path $PSScriptRoot "tests/DifferentialTest.cpp")
        Compile-Binary "benchmark" (Join-Path $PSScriptRoot "benchmarks/BenchmarkEngine.cpp")
        Compile-Binary "market_making" (Join-Path $PSScriptRoot "simulation/main.cpp") @((Join-Path $PSScriptRoot "src/MarketMaking.cpp"))
        Compile-Binary "market_making_tests" (Join-Path $PSScriptRoot "tests/MarketMakingTests.cpp") @((Join-Path $PSScriptRoot "src/MarketMaking.cpp"))
        Write-Host "`nAll targets compiled successfully into $BinDir!" -ForegroundColor Green
    }
    default {
        # Check if the target is actually a command for engine.exe (like "limit", "market", "cancel", "help")
        Compile-Binary "engine" (Join-Path $PSScriptRoot "src/main.cpp")
        $AllArgs = @($Target) + $CommandArgs
        Write-Host "`nRunning engine with: $AllArgs`n" -ForegroundColor Cyan
        & (Join-Path $BinDir "engine.exe") @AllArgs
    }
}

# Propagate executable failures to callers/CI.
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
