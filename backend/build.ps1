param([ValidateSet('all','cli','test','scenario','mock','e2e','adapters','bench','runtime-bench','review')][string]$Target='all')
$ErrorActionPreference='Stop'
Push-Location $PSScriptRoot
try {
    $zigCompiler = Join-Path $PSScriptRoot 'tools/zig/zig.exe'
    $useBootstrap = !(Test-Path -LiteralPath $zigCompiler)
    if (!$useBootstrap) {
        $localVersion = (& $zigCompiler version 2>$null)
        $versionExitCode = $LASTEXITCODE
        $useBootstrap = $versionExitCode -ne 0 -or $localVersion.Trim() -ne '0.14.1'
    }
    if ($useBootstrap) {
        $systemZig = Get-Command zig -ErrorAction SilentlyContinue
        $systemVersion = $null
        if ($systemZig) {
            $systemVersion = (& $systemZig.Source version 2>$null)
            $systemVersionExitCode = $LASTEXITCODE
        }
        if ($systemZig -and $systemVersionExitCode -eq 0 -and $systemVersion.Trim() -eq '0.14.1' -and
            !(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'tools/zig/zig.exe'))) {
            $zigCompiler = $systemZig.Source
        } else {
            & (Join-Path $PSScriptRoot 'tools/ensure-zig.ps1')
            $zigCompiler = Join-Path $PSScriptRoot 'tools/zig/zig.exe'
        }
    }
    python tools/gen_attachment_extractor.py
    if ($LASTEXITCODE) { throw 'Attachment processor generation failed' }
    python tools/gen_web_ui.py
    if ($LASTEXITCODE) { throw 'UI generation failed' }
    New-Item -ItemType Directory -Force build | Out-Null
    $sourceFiles = @(Get-ChildItem src -Recurse -Filter *.c | Where-Object {
        $_.FullName -notmatch '[\\/]os[\\/](linux|macos|posix)[\\/]'
    } | ForEach-Object { $_.FullName })
    $sourceFiles += @('third_party/cJSON/cJSON.c','third_party/wasm3/wasm3_all.c')
    $targets = [ordered]@{
        cli=@('cognitive-os-agent','cli/main.c'); test=@('cognitive-os-agent-test','tests/test_all.c')
        scenario=@('cognitive-os-agent-scenario','tests/test_scenario.c'); mock=@('mock-llm-server','tools/mock_llm_server.c')
        adapters=@('test-adapters','tests/test_adapters.c'); e2e=@('cognitive-os-agent-e2e','tests/test_e2e.c')
        review=@('test-review','tests/test_review.c')
        bench=@('cognitive-os-agent-bench','tests/bench_agent.c')
        'runtime-bench'=@('bench-runtime','tests/bench_runtime.c')
    }
    foreach ($name in $targets.Keys) {
        if ($Target -ne 'all' -and $Target -ne $name) { continue }
        $spec=$targets[$name]
        Write-Host "[build] $($spec[0])"
        & $zigCompiler cc -std=c11 -Wall -Wextra -O1 -g -Iinclude -Ithird_party/cJSON -Ithird_party/wasm3 -o "build/$($spec[0]).exe" @sourceFiles $spec[1] -lws2_32 -lwinhttp -lbcrypt -lcrypt32 -lshell32 -lm
        if ($LASTEXITCODE) { throw "Build failed: $name" }
    }
} finally { Pop-Location }
