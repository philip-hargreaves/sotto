# Stages one model into the store: copies from a local export directory or
# downloads pinned files from Hugging Face, hashes everything locally, writes
# the manifest, and installs atomically (temp dir, then rename).
#
#   stage-model.ps1 -Id whisper-turbo-int8 -Task asr -Tier default -Licence MIT `
#       -Source C:\dev\intelliscribe\ml-models\whisper-turbo-int8
#   stage-model.ps1 -Id silero-vad -Task vad -Tier default -Licence MIT -Device CPU `
#       -Repo onnx-community/silero-vad -Revision <40-hex-commit> -Files model.onnx
param(
    [Parameter(Mandatory)] [string]$Id,
    [Parameter(Mandatory)] [string]$Task,
    [Parameter(Mandatory)] [string]$Tier,
    [Parameter(Mandatory)] [string]$Licence,
    [string]$Device = "GPU",
    [string]$Source,
    [string]$Repo,
    [string]$Revision,
    [string[]]$Files,
    [string]$Root = (Join-Path (Split-Path $PSScriptRoot -Parent) "models")
)
$ErrorActionPreference = "Stop"

$staging = Join-Path ([IO.Path]::GetTempPath()) "ambient-stage-$Id"
Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $staging | Out-Null

$sourceRecord = $null
if ($Source) {
    Copy-Item (Join-Path $Source "*") $staging -Recurse
    $sourceRecord = @{ path = (Resolve-Path $Source).Path }
} elseif ($Repo) {
    # Commit-pinned URLs only; a branch name is not a provenance
    if ($Revision -notmatch '^[0-9a-f]{40}$') { throw "-Revision must be a 40-hex commit SHA" }
    if (-not $Files) { throw "-Files is required with -Repo" }
    foreach ($file in $Files) {
        $url = "https://huggingface.co/$Repo/resolve/$Revision/$file"
        Write-Host "Downloading $file"
        curl.exe -4 --retry 3 --fail --location -o (Join-Path $staging $file) $url
        if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }
    }
    $sourceRecord = @{ repo = $Repo; revision = $Revision }
} else {
    throw "give either -Source <dir> or -Repo/-Revision/-Files"
}

$hashes = [ordered]@{}
foreach ($file in (Get-ChildItem $staging -File -Recurse)) {
    $name = $file.FullName.Substring($staging.Length + 1) -replace '\\', '/'
    $hashes[$name] = (Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLower()
}
if ($hashes.Count -eq 0) { throw "nothing staged from the source" }

$manifest = [ordered]@{
    manifestVersion = 1
    id              = $Id
    task            = $Task
    tier            = $Tier
    licence         = $Licence
    runtime         = @{ device = $Device }
    source          = $sourceRecord
    files           = $hashes
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $staging "manifest.json")

New-Item -ItemType Directory -Force $Root | Out-Null
$dest = Join-Path $Root $Id
Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
Move-Item $staging $dest
Write-Host "Staged $Id ($Task/$Tier, $($hashes.Count) files) at $dest"
