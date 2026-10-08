param([switch]$Test, [switch]$Benchmark)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $flags = @('-std=c11', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-D__USE_MINGW_ANSI_STDIO=1')
    foreach ($program in @(@('main.c', 'allocator_demo.exe'), @('tests.c', 'allocator_tests.exe'), @('benchmark.c', 'allocator_bench.exe'))) {
        & gcc @flags allocator.c $program[0] -o $program[1]
        if ($LASTEXITCODE -ne 0) { throw "Build failed for $($program[0])" }
    }
    Write-Host 'Built demo, tests, and benchmark.'
    if ($Test) {
        & .\allocator_tests.exe
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    }
    if ($Benchmark) {
        & .\allocator_bench.exe | Tee-Object -FilePath benchmark-results.csv
        if ($LASTEXITCODE -ne 0) { throw 'Benchmark failed.' }
    }
} finally {
    Pop-Location
}
