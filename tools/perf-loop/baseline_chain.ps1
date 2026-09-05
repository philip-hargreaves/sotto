# Unattended chain, run detached (Start-Process) so no parent job watchdog ends it during
# the 35B load: note-tier quality baseline generation, its metrics, then the load sweep.
# Logs to build/perf-loop/baseline-chain.log.
$log = "C:\dev\ambient\build\perf-loop\baseline-chain.log"
function Say($s) { $line = "$(Get-Date -Format 'HH:mm:ss') $s"; $line | Out-File -FilePath $log -Append -Encoding utf8 }
$env:PYTHONIOENCODING = "utf-8"
try {
    Set-Location C:\dev\intelliscribe\bench\summarisation
    Say "=== generation"
    python generate/tier_baseline.py 2>&1 | ForEach-Object { Say $_ }
    Say "=== metrics"
    python score/tier_baseline_metrics.py 2>&1 | Out-File -FilePath results/tier-baseline-metrics.md -Encoding utf8
    Say "metrics written"
    Start-Sleep 20
    Say "=== sweep"
    & (Join-Path $PSScriptRoot "tier_sweep.ps1") 2>&1 | ForEach-Object { Say $_ }
    Say "=== chain done"
} catch {
    Say "CHAIN FAILED: $($_.Exception.Message)"
}
