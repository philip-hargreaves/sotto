# Sweeps every note tier through the real note host, twice (cold-ish then warm),
# sampling the host's working set, then the Python bench per model.
$ErrorActionPreference = "Continue"
Set-Location C:\dev\ambient
$out = "C:\dev\ambient\build\perf-loop\tier-sweep.log"
"tier sweep $(Get-Date -Format 'yyyy-MM-dd HH:mm')" | Set-Content $out
foreach ($tier in @("constrained", "default", "accuracy")) {
    foreach ($pass in 1, 2) {
        $env:AMBIENT_SWEEP_TIER = $tier
        $csv = Join-Path $env:TEMP "sweep-$tier-$pass.csv"
        if (Test-Path $csv) { Remove-Item $csv }
        $tp = Start-Process typeperf -ArgumentList @('"\Process(ambient_note_host*)\Working Set"', '-si', '1', '-o', ('"' + $csv + '"'), '-y') -PassThru -WindowStyle Hidden
        $lines = & build\release\engine\tests\models_tests.exe --gtest_filter=WorkerNoteWriter.NoteTierSweep 2>&1 | Select-String "note qwen|first token|tok/s|sweep |FAILED|SKIPPED"
        Stop-Process $tp -Force -ErrorAction SilentlyContinue
        Start-Sleep 1
        $peak = 0
        try { $peak = (Import-Csv $csv | ForEach-Object { $_.PSObject.Properties | Select-Object -Skip 1 | ForEach-Object { try { [double]$_.Value } catch { 0 } } } | Measure-Object -Maximum).Maximum } catch {}
        "=== $tier pass $pass  (host peak working set $([math]::Round($peak/1GB,1)) GB)" | Tee-Object -FilePath $out -Append
        $lines | ForEach-Object { $_.Line } | Tee-Object -FilePath $out -Append
        Start-Sleep 3
    }
}
Remove-Item Env:AMBIENT_SWEEP_TIER -ErrorAction SilentlyContinue
"=== python bench (3,462-token prompt, 200 tokens, 2 reps)" | Tee-Object -FilePath $out -Append
Set-Location C:\dev\intelliscribe\bench\performance
foreach ($m in @(@("qwen3.5-4b-int4", ""), @("qwen3.5-9b-int4", ""), @("qwen3.6-35b-a3b-int4", "--vlm --scale 32"))) {
    $args = @("tier_decode_bench.py", "--model", "C:\dev\ambient\models\$($m[0])", "--tokens", "200", "--reps", "2") + ($m[1] -split ' ' | Where-Object { $_ })
    $line = & python @args 2>&1 | Select-Object -Last 1
    $line | Tee-Object -FilePath $out -Append
}
"done"
