# Builds the complete release folder: the app (self-contained, no runtime
# install), the engine and its runtimes, prompts, demo tracks, the README,
# and the model weights. Zip the folder and it is the whole release -
# extract, double-click, done.
#
# Model .cache dirs stay out: they are compiled blobs specific to this
# machine's GPU and driver, and every machine rebuilds its own on first use.
#
#   stage-release.ps1 [-Out C:\dev\ambient\build\ambient]
param(
    [string]$Out = (Join-Path (Split-Path $PSScriptRoot -Parent) "build\ambient")
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

Remove-Item $Out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $Out | Out-Null

dotnet publish "$repo\app\Ambient.App\Ambient.App.csproj" -c Release -r win-x64 `
    --self-contained -p:Platform=x64 -o $Out
if ($LASTEXITCODE -ne 0) { throw "app publish failed" }

# Anything SatelliteResourceLanguages does not catch: the recipient reads
# English, the culture folders are scroll-past noise beside the exe
Get-ChildItem $Out -Directory |
    Where-Object { $_.Name -match '^[a-z]{2,3}(-[A-Za-z0-9]{2,12})*$' -and $_.Name -notmatch '^en' } |
    Remove-Item -Recurse -Force

# The engine is native C++; on a machine without the VC++ redistributable it
# will not start, so the CRT ships beside it
$crt = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio" -Recurse `
        -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\x64\\" -and $_.FullName -notmatch "onecore|debug" } |
    Sort-Object FullName -Descending | Select-Object -First 1
if ($null -eq $crt) { throw "no VC++ CRT redist found beside Visual Studio" }
Copy-Item (Join-Path $crt.FullName "*.dll") $Out

# Beside the exe in the dev tree these are junctions; the package carries real copies
Copy-Item (Join-Path $repo "prompts") (Join-Path $Out "prompts") -Recurse
Copy-Item (Join-Path $repo "demo") (Join-Path $Out "demo") -Recurse
Copy-Item (Join-Path $repo "README.md") $Out

robocopy (Join-Path $repo "models") (Join-Path $Out "models") /E /XD .cache /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "model copy failed" }
$global:LASTEXITCODE = 0

Write-Host "Release folder at $Out - smoke it, then:  tar -a -c -f build\ambient.zip -C build ambient"
