# Stop-to-last-token comparison across note tiers at real time: every track through
# every arm, interleaved by track so a warming machine lands on each arm alike. Each
# invocation is a fresh engine, so the per-run record also carries that arm's load.
# Results: runs-<tag>.jsonl per arm, logs/<tag>-<track>.log per run; summarise with
# tier_report.py.
param(
    [string[]]$Tracks = @("c02m", "c05m", "c10m"),
    [string]$Out = "C:\dev\ambient\build\perf-loop"
)
$ErrorActionPreference = "Continue"
Set-Location C:\dev\ambient
$arms = @(
    @{ tag = "tier-4b";        tier = "constrained"; vlmPrefill = "" },
    @{ tag = "tier-9b";        tier = "default";     vlmPrefill = "" },
    @{ tag = "tier-35b";       tier = "accuracy";    vlmPrefill = "" },
    @{ tag = "tier-35b-pf";    tier = "accuracy";    vlmPrefill = "1" }
)
"campaign $(Get-Date -Format 'yyyy-MM-dd HH:mm')  power: $((Get-CimInstance -Namespace root/cimv2/power -ClassName Win32_PowerPlan -Filter 'IsActive=true' -ErrorAction SilentlyContinue).ElementName)" | Tee-Object -FilePath (Join-Path $Out "tier-campaign.log") -Append
foreach ($track in $Tracks) {
    foreach ($arm in $arms) {
        $env:PERF_NOTE_TIER = $arm.tier
        $env:PERF_TAG = "-" + $arm.tag
        $env:PERF_CYCLES = "1"
        if ($arm.vlmPrefill) { $env:AMBIENT_VLM_PREFILL = $arm.vlmPrefill } else { Remove-Item Env:AMBIENT_VLM_PREFILL -ErrorAction SilentlyContinue }
        "=== $track / $($arm.tag)  $(Get-Date -Format 'HH:mm:ss')" | Tee-Object -FilePath (Join-Path $Out "tier-campaign.log") -Append
        python tools/perf-loop/perf_loop.py 1 $track 2>&1 | Select-Object -Last 3 | Tee-Object -FilePath (Join-Path $Out "tier-campaign.log") -Append
        $log = Join-Path $Out "logs\engine-001.log"
        if (Test-Path $log) { Copy-Item $log (Join-Path $Out "logs\$($arm.tag)-$track.log") -Force }
        Start-Sleep 5
    }
}
Remove-Item Env:AMBIENT_VLM_PREFILL, Env:PERF_NOTE_TIER, Env:PERF_TAG -ErrorAction SilentlyContinue
"campaign done $(Get-Date -Format 'HH:mm:ss')" | Tee-Object -FilePath (Join-Path $Out "tier-campaign.log") -Append
