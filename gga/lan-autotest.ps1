# Scripted two-instance LAN (v2) test for GGA.
#
# Drives the loopback control surface (GGA_CTL_PORT) so a full host/join
# session can be reproduced without touching the GUI:
#   .\gga\lan-autotest.ps1 -Exe <path to GGA.exe> -Rom <rom> -Frames 200
# The executable is never assumed: pass -Exe or set GGA_EXE.
# Per-process stderr logs land in %APPDATA%\GGA\lanplay-<pid>.log, which is
# what gga\lan-diff.ps1 compares.
param(
	[string]$Exe = $env:GGA_EXE,
	[Parameter(Mandatory)][string]$Rom,
	[int]$HostPort = 27183,
	[int]$Frames = 200,
	[int]$TimeoutSec = 120,
	[int]$HostCtlPort = 27800,
	[int]$GuestCtlPort = 27810,
	[string]$DumpDir = (Join-Path $env:TEMP 'gga-lan-autotest')
)

$ErrorActionPreference = 'Stop'
if (-not $Exe -or -not (Test-Path -LiteralPath $Exe)) {
	throw "pass -Exe <path to GGA.exe> or set GGA_EXE"
}
$appData = Join-Path $env:APPDATA 'GGA'
$testRom = Join-Path $DumpDir 'lan-test.gba'

function Send-GgaCommand {
	param([int]$Port, [string]$Cmd, [hashtable]$Params = @{}, [int]$WaitMs = 15000)
	$client = New-Object System.Net.Sockets.TcpClient
	$client.Connect('127.0.0.1', $Port)
	$stream = $client.GetStream()
	$stream.ReadTimeout = $WaitMs
	$writer = New-Object System.IO.StreamWriter($stream)
	$writer.NewLine = "`n"
	$payload = @{ id = 1; cmd = $Cmd; params = $Params } | ConvertTo-Json -Compress -Depth 6
	$writer.WriteLine($payload)
	$writer.Flush()
	$reader = New-Object System.IO.StreamReader($stream)
	$line = $reader.ReadLine()
	$client.Close()
	if (-not $line) { throw "no reply to $Cmd on port $Port" }
	return $line | ConvertFrom-Json
}

function Wait-PortFile {
	param([int]$Pid_, [int]$WaitMs = 30000)
	$path = Join-Path $appData "ctl-$Pid_.port"
	$deadline = (Get-Date).AddMilliseconds($WaitMs)
	while ((Get-Date) -lt $deadline) {
		if (Test-Path -LiteralPath $path) {
			return [int]((Get-Content -LiteralPath $path -Raw).Trim())
		}
		Start-Sleep -Milliseconds 100
	}
	throw "no control port file for pid $Pid_"
}

function Start-Gga {
	param([int]$CtlPort, [int]$SlotPort)
	$env:GGA_CTL_PORT = "$CtlPort"
	$proc = Start-Process -FilePath $Exe -ArgumentList @("`"$testRom`"") -PassThru
	return $proc
}

foreach ($old in @($HostCtlPort, $GuestCtlPort)) {
	Remove-Item -LiteralPath (Join-Path $appData "ctl-$old.port") -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null
Copy-Item -LiteralPath $Rom -Destination $testRom -Force

Write-Host "launching host (ctl $HostCtlPort) and guest (ctl $GuestCtlPort)"
$hostProc = Start-Gga -CtlPort $HostCtlPort -SlotPort $HostPort
$guestProc = Start-Gga -CtlPort $GuestCtlPort -SlotPort $HostPort

$hostCtl = Wait-PortFile -Pid_ $hostProc.Id
$guestCtl = Wait-PortFile -Pid_ $guestProc.Id
Write-Host "host pid=$($hostProc.Id) ctl=$hostCtl ; guest pid=$($guestProc.Id) ctl=$guestCtl"

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$hostReady = $false
$guestReady = $false
while ((Get-Date) -lt $deadline -and -not ($hostReady -and $guestReady)) {
	if (-not $hostReady) {
		$s = Send-GgaCommand -Port $hostCtl -Cmd 'lan.status'
		if ($s.ok -and $s.result.rom) { $hostReady = $true }
	}
	if (-not $guestReady) {
		$s = Send-GgaCommand -Port $guestCtl -Cmd 'lan.status'
		if ($s.ok -and $s.result.rom) { $guestReady = $true }
	}
	if (-not ($hostReady -and $guestReady)) { Start-Sleep -Milliseconds 200 }
}
if (-not ($hostReady -and $guestReady)) { throw "rom never loaded (host=$hostReady guest=$guestReady)" }

$r = Send-GgaCommand -Port $hostCtl -Cmd 'lan.host' -Params @{ port = $HostPort }
if (-not $r.ok) { throw "lan.host failed: $($r.error)" }
Start-Sleep -Milliseconds 500
$r = Send-GgaCommand -Port $guestCtl -Cmd 'lan.join' -Params @{ addr = '127.0.0.1'; port = $HostPort }
if (-not $r.ok) { throw "lan.join failed: $($r.error)" }

Write-Host "waiting for $Frames frames"
$last = 0
while ((Get-Date) -lt $deadline) {
	$h = Send-GgaCommand -Port $hostCtl -Cmd 'lan.status'
	$g = Send-GgaCommand -Port $guestCtl -Cmd 'lan.status'
	$hf = $h.result.lan_v2_frame
	$gf = $g.result.lan_v2_frame
	if ($hf -ne $last) {
		Write-Host ("  host f={0} run={1} | guest f={2} run={3}" -f $hf, $h.result.lan_v2_running, $gf, $g.result.lan_v2_running)
		$last = $hf
	}
	if ($hf -ge $Frames -and $gf -ge $Frames) { break }
	if (-not $h.result.lan_v2_running -and -not $g.result.lan_v2_running -and $hf -gt 0) {
		Write-Host "  both stopped early (host f=$hf guest f=$gf)"
		break
	}
	Start-Sleep -Milliseconds 500
}

if ($hf -lt $Frames -or $gf -lt $Frames) {
	throw "LAN session did not reach $Frames frames (host=$hf guest=$gf)"
}
if (-not $h.result.lan_v2_running -or -not $g.result.lan_v2_running) {
	throw "LAN session stopped before verification (host=$hf guest=$gf)"
}

foreach ($pair in @(@($hostCtl, 'host'), @($guestCtl, 'guest'))) {
	try { Send-GgaCommand -Port $pair[0] -Cmd 'emu.quit' | Out-Null } catch { }
}
Start-Sleep -Milliseconds 800
foreach ($p in @($hostProc, $guestProc)) {
	if (-not $p.HasExited) { $p.Kill() }
}

foreach ($pair in @(@($hostProc.Id, 'host'), @($guestProc.Id, 'guest'))) {
	$src = Join-Path $appData "lanplay-$($pair[0]).log"
	if (Test-Path -LiteralPath $src) {
		Copy-Item -LiteralPath $src -Destination (Join-Path $DumpDir "lanplay-$($pair[1])-$($pair[0]).log") -Force
	}
}
"host pid=$($hostProc.Id) guest pid=$($guestProc.Id)"
Get-ChildItem -LiteralPath $DumpDir -Filter 'lanplay-*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 4 | ForEach-Object { "  $($_.Name) $($_.Length) bytes" }
"LAN-AUTOTEST PASS host=$hf guest=$gf"
