$ErrorActionPreference = 'Stop'

$zigVersion = '0.14.1'
$zigRoot = Join-Path $PSScriptRoot 'zig'
$zigExe = Join-Path $zigRoot 'zig.exe'

if (Test-Path -LiteralPath $zigExe) {
    $installedVersion = (& $zigExe version 2>$null)
    $versionExitCode = $LASTEXITCODE
    if ($versionExitCode -eq 0 -and $installedVersion.Trim() -eq $zigVersion) {
        Write-Host "Using Zig $zigVersion at $zigExe"
        return
    }
}

$archiveUrl = "https://ziglang.org/download/$zigVersion/zig-x86_64-windows-$zigVersion.zip"
$expectedSha256 = '554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c'
$operationId = [guid]::NewGuid().ToString('N')
$tempRoot = Join-Path $PSScriptRoot (".zig-bootstrap-$operationId")
$backupRoot = Join-Path $PSScriptRoot (".zig-backup-$operationId")
$archivePath = Join-Path $tempRoot 'zig.zip'
$extractPath = Join-Path $tempRoot 'extract'

New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
try {
    $ProgressPreference = 'SilentlyContinue'
    Write-Host "Zig $zigVersion is missing; downloading the official Windows compiler..."
    Invoke-WebRequest -Uri $archiveUrl -OutFile $archivePath -UseBasicParsing

    $actualSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualSha256 -ne $expectedSha256) {
        throw "Zig archive checksum mismatch: expected $expectedSha256, received $actualSha256"
    }

    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractPath -Force
    $extractedRoot = Join-Path $extractPath "zig-x86_64-windows-$zigVersion"
    $extractedExe = Join-Path $extractedRoot 'zig.exe'
    if (!(Test-Path -LiteralPath $extractedExe)) {
        throw "The Zig archive did not contain the expected compiler: $extractedExe"
    }

    if (Test-Path -LiteralPath $zigRoot) {
        Move-Item -LiteralPath $zigRoot -Destination $backupRoot
    }
    try {
        Move-Item -LiteralPath $extractedRoot -Destination $zigRoot
    } catch {
        if (Test-Path -LiteralPath $backupRoot) {
            Move-Item -LiteralPath $backupRoot -Destination $zigRoot
        }
        throw
    }

    $installedVersion = (& $zigExe version 2>$null)
    $versionExitCode = $LASTEXITCODE
    if ($versionExitCode -ne 0 -or $installedVersion.Trim() -ne $zigVersion) {
        throw "Zig installation verification failed at $zigExe"
    }
    if (Test-Path -LiteralPath $backupRoot) {
        $resolvedTools = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\') + '\'
        $resolvedBackup = [IO.Path]::GetFullPath($backupRoot)
        if ($resolvedBackup.StartsWith($resolvedTools, [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedBackup).StartsWith('.zig-backup-', [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedBackup -Recurse -Force
        }
    }
    Write-Host "Installed Zig $zigVersion at $zigExe"
}
finally {
    $resolvedTools = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\') + '\'
    $resolvedTemp = [IO.Path]::GetFullPath($tempRoot)
    if ($resolvedTemp.StartsWith($resolvedTools, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolvedTemp).StartsWith('.zig-bootstrap-', [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
