# Quick test script for DT bulk build (PowerShell)
# Usage: .\run-dt-bulk-test.ps1

Write-Host "╔══════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║   DT Bulk Build Quick Test                  ║" -ForegroundColor Cyan
Write-Host "╚══════════════════════════════════════════════╝" -ForegroundColor Cyan
Write-Host ""

# Check if we're in the right directory
if (-not (Test-Path "tests\test-dt-bulk.c")) {
    Write-Host "✗ Error: test-dt-bulk.c not found" -ForegroundColor Red
    Write-Host "Please run this script from the OVS root directory" -ForegroundColor Red
    exit 1
}

# Step 1: Check if dt-classifier files exist
Write-Host "Step 1: Checking DT classifier files..." -ForegroundColor Yellow
if (-not (Test-Path "lib\dt-classifier.c")) {
    Write-Host "✗ lib\dt-classifier.c not found" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path "lib\dt-classifier.h")) {
    Write-Host "✗ lib\dt-classifier.h not found" -ForegroundColor Red
    exit 1
}
Write-Host "✓ DT classifier files found" -ForegroundColor Green
Write-Host ""

# Step 2: Check if test file is in build system
Write-Host "Step 2: Checking build system..." -ForegroundColor Yellow
$automake = Get-Content "tests\automake.mk" -Raw -ErrorAction SilentlyContinue
if ($automake -and -not ($automake -match "test-dt-bulk\.c")) {
    Write-Host "⚠ Warning: test-dt-bulk.c not in tests\automake.mk" -ForegroundColor Yellow
    Write-Host "   You may need to add it manually:" -ForegroundColor Yellow
    Write-Host "   tests_ovstest_SOURCES += tests/test-dt-bulk.c" -ForegroundColor Yellow
    Write-Host ""
}

# Step 3: Compile
Write-Host "Step 3: Compiling..." -ForegroundColor Yellow
Write-Host "Running: make tests/ovstest" -ForegroundColor Gray

try {
    $output = & make tests/ovstest 2>&1 | Out-String
    
    if ($output -match "error:") {
        Write-Host "✗ Compilation failed" -ForegroundColor Red
        Write-Host $output -ForegroundColor Gray
        exit 1
    }
    
    if (-not (Test-Path "tests\ovstest.exe") -and -not (Test-Path "tests\ovstest")) {
        Write-Host "✗ tests/ovstest not found after compilation" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "✓ Compilation successful" -ForegroundColor Green
} catch {
    Write-Host "✗ Compilation error: $_" -ForegroundColor Red
    exit 1
}

Write-Host ""

# Step 4: Run test
Write-Host "Step 4: Running tests..." -ForegroundColor Yellow

# Determine the correct executable name
$testExe = if (Test-Path "tests\ovstest.exe") { "tests\ovstest.exe" } else { "tests\ovstest" }

Write-Host "Running: $testExe test-dt-bulk" -ForegroundColor Gray
Write-Host ""
Write-Host "════════════════════════════════════════════════" -ForegroundColor Cyan

try {
    & $testExe test-dt-bulk
    $exitCode = $LASTEXITCODE
    
    Write-Host "════════════════════════════════════════════════" -ForegroundColor Cyan
    Write-Host ""
    
    if ($exitCode -eq 0) {
        Write-Host "✓ Test execution completed successfully" -ForegroundColor Green
    } else {
        Write-Host "⚠ Test execution completed with exit code: $exitCode" -ForegroundColor Yellow
    }
} catch {
    Write-Host "════════════════════════════════════════════════" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "✗ Test execution error: $_" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "╔══════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║   Quick test finished!                       ║" -ForegroundColor Cyan
Write-Host "╚══════════════════════════════════════════════╝" -ForegroundColor Cyan
