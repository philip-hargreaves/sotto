# Builds the ready-to-zip release folder: the app, the fetch tool, the
# weights registry, and get-models.cmd. Zip the output and attach it to the
# release alongside the packed weight assets.
#
#   stage-release.ps1 [-Out C:\dev\ambient\build\release-package]
param(
    [string]$Out = (Join-Path (Split-Path $PSScriptRoot -Parent) "build\release-package")
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

Remove-Item $Out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $Out | Out-Null

dotnet publish "$repo\app\Ambient.App\Ambient.App.csproj" -c Release -r win-x64 -o $Out
if ($LASTEXITCODE -ne 0) { throw "app publish failed" }
dotnet publish "$repo\app\Ambient.Fetch\Ambient.Fetch.csproj" -c Release -r win-x64 `
    --self-contained -p:PublishSingleFile=true -o (Join-Path $Out "fetch")
if ($LASTEXITCODE -ne 0) { throw "fetch publish failed" }

Copy-Item (Join-Path $repo "weights") (Join-Path $Out "weights") -Recurse
Copy-Item (Join-Path $repo "tools\get-models.cmd") $Out
Write-Host "Release package at $Out - run get-models.cmd there to smoke it, then zip"
