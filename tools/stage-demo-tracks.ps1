# Stages the bundled demo recordings into demo/ beside the app: five PriMock
# mixed consultations plus the manifest the demo tray reads. Wavs never enter
# git; packaging copies demo/ into the release next to models/.
#
#   stage-demo-tracks.ps1
#   stage-demo-tracks.ps1 -Source D:\wavs -Root C:\out\demo
param(
    [string]$Source = "C:\dev\intelliscribe\bench\transcription\mixed",
    [string]$Root = (Join-Path (Split-Path $PSScriptRoot -Parent) "demo")
)
$ErrorActionPreference = "Stop"

$tracks = @(
    @{ name = "Elbow swelling";     file = "day2_consultation02_mixed.wav" },
    @{ name = "Chest pain";         file = "day2_consultation07_mixed.wav" },
    @{ name = "Antibiotic allergy"; file = "day1_consultation09_mixed.wav" },
    @{ name = "Weight loss";        file = "day3_consultation03_mixed.wav" },
    @{ name = "Stomach upset";      file = "day1_consultation01_mixed.wav" }
)

New-Item -ItemType Directory -Force $Root | Out-Null
foreach ($track in $tracks) {
    $from = Join-Path $Source $track.file
    if (-not (Test-Path $from)) { throw "missing source wav: $from" }
    Copy-Item $from (Join-Path $Root $track.file)
    Write-Host "staged $($track.name) <- $($track.file)"
}

@{ tracks = $tracks } | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $Root "tracks.json")
Write-Host "wrote $(Join-Path $Root 'tracks.json') ($($tracks.Count) tracks)"
