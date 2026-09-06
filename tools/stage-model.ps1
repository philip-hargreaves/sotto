# Stages one model into the store: copies from a local export directory or
# downloads pinned files from Hugging Face, hashes everything locally, writes
# the manifest, and installs atomically (temp dir, then rename).
#
#   stage-model.ps1 -Id whisper-turbo-int8 -Task asr -Tier default -Licence MIT `
#       -Source C:\dev\intelliscribe\ml-models\whisper-turbo-int8
#   stage-model.ps1 -Id silero-vad -Task vad -Tier default -Licence MIT -Device CPU `
#       -Repo onnx-community/silero-vad -Revision <40-hex-commit> -Files model.onnx
#
# -InPlace hashes the source where it is, writes the manifest beside it, and
# junctions it into the store: for an export too large to copy on a
# development machine. -Pipeline and -Property say how the model loads:
# absent means the LLM pipeline with no properties.
#
#   stage-model.ps1 -Id qwen3.6-35b-a3b-int4 -Task note -Tier accuracy -Licence Apache-2.0 `
#       -Name "Qwen3.6 35B" -Pipeline vlm -Property ACTIVATIONS_SCALE_FACTOR=32 `
#       -Source C:\dev\intelliscribe\ml-models\summariser\qwen3.6-35b-a3b-int4-ov -InPlace
param(
    [Parameter(Mandatory)] [string]$Id,
    [Parameter(Mandatory)] [string]$Task,
    [Parameter(Mandatory)] [string]$Tier,
    [Parameter(Mandatory)] [string]$Licence,
    [string]$Name = "",   # display name for the shell, e.g. "Whisper Large v3 Turbo"
    [string]$Device = "GPU",
    [ValidateSet("llm", "vlm")] [string]$Pipeline = "",
    [string[]]$Property = @(),   # OpenVINO properties, KEY=value; numbers and bools typed
    [string]$Source,
    [switch]$InPlace,
    [string]$Repo,
    [string]$Revision,
    [string[]]$Files,
    [string]$Root = (Join-Path (Split-Path $PSScriptRoot -Parent) "models")
)
$ErrorActionPreference = "Stop"

$sourceRecord = $null
if ($InPlace) {
    if (-not $Source) { throw "-InPlace needs -Source <dir>" }
    $staging = (Resolve-Path $Source).Path
    $sourceRecord = @{ path = $staging }
} else {
    $staging = Join-Path ([IO.Path]::GetTempPath()) "ambient-stage-$Id"
    Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $staging | Out-Null
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
}

# The compile cache and a previous manifest are never part of the model.
# Hashes are the provenance record and the delivery tools' check; the engine
# checks presence and size at load
$hashes = [ordered]@{}
$bytes = [ordered]@{}
foreach ($file in (Get-ChildItem $staging -File -Recurse | Where-Object {
        $_.FullName -notlike (Join-Path $staging ".cache*") -and $_.Name -ne "manifest.json" })) {
    # Not $name: PowerShell variables are case-insensitive and that is the -Name parameter
    $relative = $file.FullName.Substring($staging.Length + 1) -replace '\\', '/'
    Write-Host "Hashing $relative"
    $hashes[$relative] = (Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLower()
    $bytes[$relative] = $file.Length
}
if ($hashes.Count -eq 0) { throw "nothing staged from the source" }

$runtime = [ordered]@{ device = $Device }
if ($Pipeline) { $runtime.pipeline = $Pipeline }
if ($Property.Count -gt 0) {
    $properties = [ordered]@{}
    foreach ($entry in $Property) {
        $key, $value = $entry -split '=', 2
        if (-not $value) { throw "-Property entries are KEY=value: $entry" }
        $typed = $value
        if ($value -match '^-?\d+$') { $typed = [int64]$value }
        elseif ($value -match '^-?\d*\.\d+$') { $typed = [double]$value }
        elseif ($value -in @("true", "false")) { $typed = [bool]::Parse($value) }
        $properties[$key] = $typed
    }
    $runtime.properties = $properties
}

$manifest = [ordered]@{
    manifestVersion = 1
    id              = $Id
    name            = $(if ($Name) { $Name } else { $Id })
    task            = $Task
    tier            = $Tier
    licence         = $Licence
    runtime         = $runtime
    source          = $sourceRecord
    files           = $hashes
    bytes           = $bytes
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $staging "manifest.json")

New-Item -ItemType Directory -Force $Root | Out-Null
$dest = Join-Path $Root $Id
if ($InPlace) {
    if (Test-Path $dest) { (Get-Item $dest).Delete() }   # a junction, never its target
    New-Item -ItemType Junction -Path $dest -Target $staging | Out-Null
    Write-Host "Staged $Id ($Task/$Tier, $($hashes.Count) files) in place at $staging, junction $dest"
} else {
    Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item $staging $dest
    Write-Host "Staged $Id ($Task/$Tier, $($hashes.Count) files) at $dest"
}
