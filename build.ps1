$ErrorActionPreference = 'Stop'
cmake -S . -B build -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Built: $PWD\build\Release\LoopCatch.exe"
