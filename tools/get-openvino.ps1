# Fetches the pinned OpenVINO GenAI toolchain the engine build requires

$ErrorActionPreference = "Stop"

$version = "2026.3.0.0"
$archive = "openvino_genai_windows_${version}_x86_64.zip"
$url = "https://storage.openvinotoolkit.org/repositories/openvino_genai/packages/2026.3/windows/$archive"
$sha256 = "3D01DAEE17953A1B787841CFF6A4D19B5DE1D06D996AF9433969139B2B5EA657"

$root = Split-Path $PSScriptRoot -Parent
$dest = Join-Path $root "external\openvino"
$pin = Join-Path $dest ".pin"

if ((Test-Path $pin) -and ((Get-Content $pin) -eq $sha256)) {
    Write-Host "OpenVINO GenAI $version already installed"
    exit 0
}

$zip = Join-Path ([IO.Path]::GetTempPath()) $archive
Write-Host "Downloading $archive"
curl.exe -4 --retry 3 --fail --location -o $zip $url
if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }

$actual = (Get-FileHash $zip -Algorithm SHA256).Hash
if ($actual -ne $sha256) { throw "SHA256 mismatch: expected $sha256, got $actual" }

$staging = Join-Path ([IO.Path]::GetTempPath()) "openvino-staging"
Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive $zip $staging

# The zip wraps everything in one versioned directory; install at a stable path
$inner = @(Get-ChildItem $staging)
if ($inner.Count -ne 1) { throw "unexpected archive layout: $($inner.Name -join ', ')" }
Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
Move-Item $inner[0].FullName $dest

Set-Content $pin $sha256
Remove-Item $zip, $staging -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "OpenVINO GenAI $version installed at $dest"
