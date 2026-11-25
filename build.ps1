# Builds NanoBook with g++ directly (no make/cmake needed), then runs the selected targets.
# Usage: .\build.ps1 [all|test|bench|demo|build]   (default: all = tests + demo snapshot + benchmark)
param([ValidateSet('all', 'test', 'bench', 'demo', 'build')][string]$Mode = 'all')

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$cxx = if ($env:CXX) { $env:CXX } else { 'g++' }
$flags = @('-std=c++20', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Wpedantic', '-Wshadow',
           '-Wconversion', '-Wsign-conversion', '-Wold-style-cast', '-Werror', '-Iinclude')

function Invoke-Compiler([string[]]$Arguments) {
    & $cxx @flags @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($Arguments -join ' ')" }
}

New-Item -ItemType Directory -Force build | Out-Null

Write-Host '[build] library'
Invoke-Compiler @('-c', 'src/order_book.cpp', '-o', 'build/order_book.o')
Write-Host '[build] tests'
$testSources = Get-ChildItem tests -Filter *.cpp | ForEach-Object { "tests/$($_.Name)" }
Invoke-Compiler (@($testSources) + @('build/order_book.o', '-pthread', '-o', 'build/nanobook_tests.exe'))
Write-Host '[build] benchmark'
Invoke-Compiler @('-march=native', 'benchmarks/bench_latency.cpp', 'build/order_book.o', '-pthread', '-o', 'build/nanobook_bench.exe')
Write-Host '[build] demo'
Invoke-Compiler @('demo/nanobook_demo.cpp', 'build/order_book.o', '-o', 'build/nanobook_demo.exe')

if ($Mode -in 'all', 'test') {
    Write-Host ''
    & .\build\nanobook_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Unit tests failed' }
}
if ($Mode -eq 'all') {
    Write-Host ''
    & .\build\nanobook_demo.exe --frames 1
    if ($LASTEXITCODE -ne 0) { throw 'Demo snapshot failed' }
}
if ($Mode -eq 'demo') {
    & .\build\nanobook_demo.exe
}
if ($Mode -in 'all', 'bench') {
    Write-Host ''
    & .\build\nanobook_bench.exe
}
