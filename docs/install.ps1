# Gamma Code installer
#   irm https://zstudio-lab.github.io/Gamma-Code/install.ps1 | iex
#
# Downloads the latest portable build (gamma.exe) from the public GitHub
# Releases, verifies its SHA-256 against the CHECKSUM.txt shipped in the
# archive, installs per-user to %LOCALAPPDATA%\Programs\GammaCode\, and adds
# that directory to your user PATH.
#
# The only network call is the download itself. Nothing else leaves the
# machine - Gamma has no telemetry and talks only to Ollama on 127.0.0.1.
#
# Requires: Windows 10 22H2+ / Windows 11, PowerShell 5.1+.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try {
  [Net.ServicePointManager]::SecurityProtocol =
    [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
} catch { }

$Repo = 'https://github.com/ZStudio-lab/Gamma-Code'
$Url = "$Repo/releases/latest/download/gamma-x64.zip"
$Dest = Join-Path $env:LOCALAPPDATA 'Programs\GammaCode'

Write-Host ''
Write-Host '  Gamma Code - installing' -ForegroundColor Cyan
Write-Host "  download: $Url"

if (Get-Process gamma -ErrorAction SilentlyContinue) {
  Write-Host '  a gamma process is running - close it and run the installer again.' -ForegroundColor Red
  return
}

if ($PSVersionTable.PSVersion.Major -lt 5) {
  Write-Host '  PowerShell 5.1 or newer is required.' -ForegroundColor Red
  return
}

if ($env:PROCESSOR_ARCHITECTURE -notmatch '64') {
  Write-Host '  this build targets x64 Windows only.' -ForegroundColor Red
  return
}

$Tmp = Join-Path ([IO.Path]::GetTempPath()) ('gamma-install-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
try {
  New-Item -ItemType Directory -Path $Tmp | Out-Null

  $Zip = Join-Path $Tmp 'gamma-x64.zip'
  Invoke-WebRequest -Uri $Url -OutFile $Zip -UseBasicParsing
  Expand-Archive -Path $Zip -DestinationPath $Tmp -Force

  $Bin = Join-Path $Tmp 'gamma.exe'
  $Sum = Join-Path $Tmp 'CHECKSUM.txt'
  if (!(Test-Path $Bin)) {
    Write-Host '  the downloaded archive has no gamma.exe - release may be incomplete.' -ForegroundColor Red
    return
  }
  if (!(Test-Path $Sum)) {
    Write-Host '  the downloaded archive has no CHECKSUM.txt - cannot verify.' -ForegroundColor Red
    return
  }

  # Verify the binary hash against the archive-embedded checksum.
  $Hash = (Get-FileHash $Bin -Algorithm SHA256).Hash.ToLowerInvariant()
  $Expected = (Get-Content $Sum -Raw).Trim()
  if ([string]::IsNullOrWhiteSpace($Expected)) {
    Write-Host '  CHECKSUM.txt is empty - cannot verify.' -ForegroundColor Red
    return
  }
  if ($Hash -ne $Expected) {
    Write-Host '  checksum MISMATCH - aborting.' -ForegroundColor Red
    Write-Host "    expected: $Expected" -ForegroundColor Red
    Write-Host "    actual:   $Hash" -ForegroundColor Red
    return
  }
  Write-Host "  sha256  : $Hash" -ForegroundColor Green

  New-Item -ItemType Directory -Force $Dest | Out-Null
  Copy-Item $Bin (Join-Path $Dest 'gamma.exe') -Force
  Copy-Item $Sum (Join-Path $Dest 'CHECKSUM.txt') -Force
  Write-Host "  install : $Dest\gamma.exe"

  # User PATH (idempotent; .NET broadcasts WM_SETTINGCHANGE itself).
  $UserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
  if ($null -eq $UserPath) { $UserPath = '' }
  $Present = $false
  foreach ($Entry in $UserPath.Split(';')) {
    if ($Entry.Trim() -ieq $Dest) { $Present = $true; break }
  }
  if ($Present) {
    Write-Host '  path    : already on the user PATH'
  } else {
    $NewPath = (';' + $UserPath.Trim() + ';' + $Dest).Trim(';')
    [Environment]::SetEnvironmentVariable('Path', $NewPath, 'User')
    Write-Host '  path    : added to the user PATH (new terminals only)' -ForegroundColor Green
  }
} catch {
  Write-Host "  download failed: $($_.Exception.Message)" -ForegroundColor Red
  Write-Host "  check $Repo/releases for a direct copy." -ForegroundColor Red
  return
} finally {
  Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
}

# Ollama check - a purely local probe of 127.0.0.1:11434.
$OllamaUp = $false
try {
  $Tcp = New-Object Net.Sockets.TcpClient
  $Ar = $Tcp.BeginConnect('127.0.0.1', 11434, $null, $null)
  $OllamaUp = $Ar.AsyncWaitHandle.WaitOne(1000) -and $Tcp.Connected
  $Tcp.Close()
} catch { $OllamaUp = $false }

Write-Host ''
if ($OllamaUp) {
  Write-Host '  ollama  : reachable on 127.0.0.1:11434' -ForegroundColor Green
  Write-Host '  open a new terminal and run:  gamma' -ForegroundColor Cyan
} else {
  Write-Host '  ollama  : not running on 127.0.0.1:11434.' -ForegroundColor Yellow
  Write-Host '  No problem - gamma sets it up on first launch:' -ForegroundColor Cyan
  Write-Host '    it offers "Install Ollama? [Yes/No]", installs it, pulls a model' -ForegroundColor Cyan
  Write-Host '    matched to your hardware, and starts ollama serve for you.' -ForegroundColor Cyan
  Write-Host '  open a new terminal and run:  gamma' -ForegroundColor Cyan
}
