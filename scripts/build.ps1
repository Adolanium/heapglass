param([ValidateSet('Release','Debug')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    python scripts/verify-dependencies.py
    if ($LASTEXITCODE) { throw 'Dependency validation failed' }
    cmake -S . -B build -A x64
    if ($LASTEXITCODE) { throw 'Configure failed' }
    cmake --build build --config $Configuration --parallel
    if ($LASTEXITCODE) { throw 'Build failed' }
    ctest --test-dir build -C $Configuration --output-on-failure
    if ($LASTEXITCODE) { throw 'Validation failed' }
} finally { Pop-Location }
