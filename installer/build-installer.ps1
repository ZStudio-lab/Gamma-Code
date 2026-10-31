# Builds the Gamma Code install packages.
#   staging/: gamma.exe + gamma.ico + CHECKSUM.txt (built hash)
#   out/:     Gamma-<ver>-setup.exe   (Inno Setup, per-user)
#             gamma-x64.zip           (portable: gamma.exe + CHECKSUM.txt)
#             SHA256SUMS.txt
#   winget/:  manifest regenerated under the version path, real hashes filled in.
param(
  [string]$Version = "0.1.2",
  [string]$Iscc = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repo = Split-Path $PSScriptRoot -Parent
$gammaExe = Join-Path $repo "gammacode\build\gamma.exe"
if (!(Test-Path $gammaExe)) { throw "gamma.exe not found - build gammacode first" }
$staging = Join-Path $PSScriptRoot "staging"
$out = Join-Path $PSScriptRoot "out"
New-Item -ItemType Directory -Force $staging, $out | Out-Null

# 1 - stage ------------------------------------------------------------------
Copy-Item $gammaExe (Join-Path $staging "gamma.exe") -Force
Copy-Item (Join-Path $repo "gammacode\assets\Gamma.ico") (Join-Path $staging "gamma.ico") -Force
$binHash = (Get-FileHash (Join-Path $staging "gamma.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path (Join-Path $staging "CHECKSUM.txt") -Value $binHash -NoNewline

# 2 - Inno Setup ---------------------------------------------------------------
$iss = Join-Path $PSScriptRoot "inno\gamma.iss"
& $Iscc /Qp "/DAppVersion=$Version" $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }

# 3 - portable zip ---------------------------------------------------------------
$zip = Join-Path $out "gamma-x64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
tar -a -c -f $zip -C $staging gamma.exe CHECKSUM.txt

# 4 - SHA256SUMS -----------------------------------------------------------------
$setupPath = Join-Path $out "Gamma-$Version-setup.exe"
if (!(Test-Path $setupPath)) { throw "installer missing: $setupPath" }
$sumsPath = Join-Path $out "SHA256SUMS.txt"
$lines = @()
foreach ($f in (Get-ChildItem $out -File |
    Where-Object { $_.Name -ne "SHA256SUMS.txt" })) {
  $lines += "{0} *{1}" -f (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $f.Name
}
Set-Content -Path $sumsPath -Value $lines

# 5 - winget manifest ------------------------------------------------------------
$manifestDir = Join-Path $PSScriptRoot "winget\manifest\G\GammaCode\GammaCode.Gamma\$Version"
New-Item -ItemType Directory -Force $manifestDir | Out-Null
$setupHash = (Get-FileHash $setupPath -Algorithm SHA256).Hash.ToLowerInvariant()
$zipHash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$manPath = Join-Path $manifestDir "GammaCode.Gamma.yaml"
Copy-Item (Join-Path $PSScriptRoot "winget\template.yaml") $manPath -Force
$filled = (Get-Content $manPath -Raw).Replace("%VERSION%", $Version).Replace("%SETUPSHA%", $setupHash)
Set-Content -Path $manPath -Value $filled -NoNewline

Write-Host ""
Write-Host "  gamma.exe sha256:  $binHash"
Write-Host "  setup sha256:      $setupHash"
Write-Host "  zip sha256:        $zipHash"
Write-Host "  manifest:          $manPath"
Write-Host ("  setup:  " + $setupPath)
Write-Host ("  zip:    " + $zip)
