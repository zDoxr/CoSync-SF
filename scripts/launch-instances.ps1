<#
.SYNOPSIS
    Launch several windowed Starfield instances on one PC for CoSync-SF testing.

.DESCRIPTION
    - Forces windowed (not fullscreen, not borderless) mode via
      Documents\My Games\Starfield\StarfieldPrefs.ini and StarfieldCustom.ini.
      Originals are backed up once as *.cosync-backup.
    - Writes one CoSync-SF config per instance under <repo>\local\instances\<n>\:
      instance 1 hosts a session, the others join it on 127.0.0.1.
      The plugin picks it up via the COSYNC_INI / COSYNC_INSTANCE environment
      variables, which sfse_loader passes on to Starfield.exe.
    - Launches each instance through sfse_loader.exe (or Starfield.exe directly
      with -NoSfse), waits for its window, and tiles the windows side by side.

    All instances share one Steam account, one Documents folder and one save
    folder. Do not save in both instances at the same time.

.EXAMPLE
    ./scripts/launch-instances.ps1                 # 2 instances via SFSE, CoSync host + client
    ./scripts/launch-instances.ps1 -NoSfse         # vanilla game, no plugin
    ./scripts/launch-instances.ps1 -WindowedOnly   # only apply the windowed settings
    ./scripts/launch-instances.ps1 -RestoreDisplay # put the original display settings back
#>
param(
    [ValidateRange(1, 4)]
    [int]$Count = 2,
    [string]$GameDir,
    [int]$Width = 0,                 # 0 = fit $Count windows side by side on the primary monitor
    [int]$Height = 0,
    [switch]$NoSfse,
    [switch]$NoCoSyncConfig,
    [int]$Port = 37015,
    [int]$StartupTimeoutSeconds = 180,
    [int]$DelaySeconds = 10,
    [switch]$WindowedOnly,
    [switch]$Avatars,                # experimental: spawn stand-ins for remote players
    [double]$AvatarScale = 1.0,      # debug: scale stand-ins so they are easy to spot
    [ValidateSet(0, 1, 2)]
    [int]$AvatarFreezeAI = 0,        # proxy AI: 0 on, 1 off at spawn (semi-transparent), 2 off after 3D load + delay
    [double]$AvatarFreezeDelay = 3,
    [ValidateSet('player', 'nearby')]
    [string]$AvatarBase = 'player',  # proxy base NPC
    [switch]$AvatarAnimDiag,         # log locomotion graph variables of player and proxies
    [switch]$AvatarAnimate,          # drive proxy locomotion variables from network velocity
    [switch]$AvatarNoMove,           # experiment: do not SetPosition/SetAngle stand-ins
    [double]$RenderScale = 0.5,      # test instances share one GPU: render at this scale (DLSS/FSR upscales)
    [switch]$NoVsync,                # vsync stops the focused copy from starving the other of GPU time
    [switch]$RestoreDisplay
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$prefsDir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Starfield'
$iniFiles = @('StarfieldPrefs.ini', 'StarfieldCustom.ini')
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Find-GameDir {
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { return $null }
    $libraries = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"')) {
            $libraries += $m.Matches[0].Groups[1].Value -replace '\\\\', '\'
        }
    }
    foreach ($lib in $libraries | Select-Object -Unique) {
        $candidate = Join-Path $lib 'steamapps\common\Starfield'
        if (Test-Path (Join-Path $candidate 'Starfield.exe')) { return $candidate }
    }
    return $null
}

# Sets key=value in [section], preserving every other line.
function Set-IniValue([string]$Path, [string]$Section, [string]$Key, [string]$Value) {
    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path $Path) { $lines.AddRange([string[]](Get-Content -LiteralPath $Path)) }

    $sectionStart = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim() -ieq "[$Section]") { $sectionStart = $i; break }
    }
    if ($sectionStart -lt 0) {
        if ($lines.Count -gt 0 -and $lines[$lines.Count - 1].Trim() -ne '') { $lines.Add('') }
        $lines.Add("[$Section]")
        $lines.Add("$Key=$Value")
    } else {
        $sectionEnd = $lines.Count
        for ($i = $sectionStart + 1; $i -lt $lines.Count; $i++) {
            if ($lines[$i].Trim().StartsWith('[')) { $sectionEnd = $i; break }
        }
        $found = $false
        $pattern = '^\s*' + [regex]::Escape($Key) + '\s*='
        for ($i = $sectionStart + 1; $i -lt $sectionEnd; $i++) {
            if ($lines[$i] -match $pattern) { $lines[$i] = "$Key=$Value"; $found = $true; break }
        }
        if (-not $found) { $lines.Insert($sectionStart + 1, "$Key=$Value") }
    }
    [System.IO.File]::WriteAllLines($Path, $lines, $utf8NoBom)
}

function Backup-Once([string]$Path) {
    $backup = "$Path.cosync-backup"
    if (Test-Path $backup) { return }
    if (Test-Path $Path) { Copy-Item -LiteralPath $Path -Destination $backup }
    else { New-Item -ItemType File -Path $backup | Out-Null }  # empty marker: "file did not exist"
}

function Restore-Display {
    foreach ($name in $iniFiles) {
        $path = Join-Path $prefsDir $name
        $backup = "$path.cosync-backup"
        if (-not (Test-Path $backup)) { continue }
        if ((Get-Item $backup).Length -eq 0) {
            Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
            Write-Host "removed $name (it did not exist before)"
        } else {
            Copy-Item -LiteralPath $backup -Destination $path -Force
            Write-Host "restored $name"
        }
        Remove-Item -LiteralPath $backup
    }
}

if ($RestoreDisplay) { Restore-Display; return }

# ---- window geometry -------------------------------------------------------
Add-Type -AssemblyName System.Windows.Forms
$work = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$frameW = 16; $frameH = 39   # approximate window border + title bar
if ($Width -le 0) {
    $Width = [math]::Min(1920, [math]::Floor($work.Width / $Count) - $frameW)
    $Height = [math]::Floor($Width * 9 / 16)
    if ($Height + $frameH -gt $work.Height) {
        $Height = $work.Height - $frameH
        $Width = [math]::Floor($Height * 16 / 9)
    }
} elseif ($Height -le 0) {
    $Height = [math]::Floor($Width * 9 / 16)
}

# ---- windowed display settings --------------------------------------------
# Verified 2026-10-27 on 1.16.244: the game's first launch runs hardware
# autodetection and goes fullscreen regardless of StarfieldCustom.ini. Display
# settings are honoured from StarfieldPrefs.ini, which that first launch creates.
$prefsPath = Join-Path $prefsDir 'StarfieldPrefs.ini'
if (-not (Test-Path $prefsPath)) {
    throw ("StarfieldPrefs.ini does not exist yet. Start Starfield once normally, let it reach the main menu, " +
           "then quit, so it finishes first-run detection. Then run this script again.")
}
New-Item -ItemType Directory -Force -Path $prefsDir | Out-Null
foreach ($name in $iniFiles) {
    $path = Join-Path $prefsDir $name
    Backup-Once $path
    Set-IniValue $path 'Display' 'bFull Screen' '0'
    Set-IniValue $path 'Display' 'bBorderless' '0'
    Set-IniValue $path 'Display' 'iSize W' "$Width"
    Set-IniValue $path 'Display' 'iSize H' "$Height"
    # Bethesda games pause their main loop when the window loses focus; with two
    # copies only the focused one would tick (observed 2026-10-27). Keep both running.
    Set-IniValue $path 'General' 'bAlwaysActive' '1'
    # Two copies share one GPU, and Windows favours the foreground window, so the
    # unfocused copy lagged at 11-16 FPS while the GPU sat at 99% (2026-11-02).
    # Cap the frame rate with vsync and lower the render scale for test runs.
    Set-IniValue $path 'Display' 'bEnableVsync' $(if ($NoVsync) { '0' } else { '1' })
    Set-IniValue $path 'Display' 'fRenderResolutionScaleFactor' $RenderScale.ToString('0.0000', [Globalization.CultureInfo]::InvariantCulture)
    if ($Avatars) {
        # Papyrus log (Documents\My Games\Starfield\Logs\Script) records VM call errors.
        Set-IniValue $path 'Papyrus' 'bEnableLogging' '1'
        Set-IniValue $path 'Papyrus' 'bEnableTrace' '1'
    }
    Write-Host "windowed ${Width}x${Height}, always active -> $path"
}
if ($WindowedOnly) { return }

# ---- game location ----------------------------------------------------------
if (-not $GameDir) { $GameDir = Find-GameDir }
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir 'Starfield.exe'))) {
    throw 'Starfield.exe not found; pass -GameDir <folder>.'
}
$loader = Join-Path $GameDir 'sfse_loader.exe'
$useSfse = -not $NoSfse
if ($useSfse -and -not (Test-Path $loader)) {
    throw "sfse_loader.exe not found in $GameDir. Install SFSE 0.2.21, or use -NoSfse to launch the vanilla game."
}
$gameVersion = (Get-Item (Join-Path $GameDir 'Starfield.exe')).VersionInfo.FileVersion
Write-Host "Starfield $gameVersion at $GameDir ($(if ($useSfse) { 'via SFSE' } else { 'vanilla, no plugin' }))"

# ---- per-instance CoSync configs -------------------------------------------
$instanceDir = Join-Path $repo 'local\instances'
function Write-InstanceConfig([int]$n) {
    $dir = Join-Path $instanceDir "$n"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $ini = Join-Path $dir 'CoSyncSF.ini'
    $mode = if ($n -eq 1) { 'host' } else { 'client' }
    @(
        '; generated by scripts/launch-instances.ps1'
        '[Network]'
        "Mode=$mode"
        "Address=127.0.0.1:$Port"
        "Port=$Port"
        "DiscoveryPort=$(if ($n -eq 1) { $Port + 1 } else { 0 })"
        'ServerName=CoSync-SF Local Test'
        'MaxPlayers=8'
        '[Player]'
        "Name=Player$n"
        '[Security]'
        'Password='
        '[Experimental]'
        "RemoteAvatars=$(if ($Avatars) { 1 } else { 0 })"
        "AvatarScale=$($AvatarScale.ToString([Globalization.CultureInfo]::InvariantCulture))"
        "AvatarFreezeAI=$AvatarFreezeAI"
        "AvatarFreezeDelay=$($AvatarFreezeDelay.ToString([Globalization.CultureInfo]::InvariantCulture))"
        "AvatarBase=$AvatarBase"
        "AvatarAnimDiag=$(if ($AvatarAnimDiag) { 1 } else { 0 })"
        "AvatarAnimate=$(if ($AvatarAnimate) { 1 } else { 0 })"
        "AvatarMove=$(if ($AvatarNoMove) { 0 } else { 1 })"
        '[Debug]'
        'LogLevel=debug'
    ) | Set-Content -LiteralPath $ini -Encoding ASCII
    return $ini
}

# ---- launch ------------------------------------------------------------------
Add-Type -Namespace CoSync -Name Win32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
[DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr hWnd, int index);
'@

$launched = @()
for ($n = 1; $n -le $Count; $n++) {
    $before = @(Get-Process Starfield -ErrorAction SilentlyContinue | ForEach-Object Id)

    $saved = @{}
    foreach ($k in 'COSYNC_INI', 'COSYNC_INSTANCE', 'COSYNC_MULTI_INSTANCE', 'SteamAppId', 'SteamGameId') {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process')
    }
    try {
        if (-not $NoCoSyncConfig) {
            $env:COSYNC_INI = Write-InstanceConfig $n
            $env:COSYNC_INSTANCE = "p$n"
        }
        if ($useSfse) {
            # Starfield exits if a "Starfield" window already exists; the CoSync
            # plugin bypasses that check only when this is set (MultiInstance.cpp).
            $env:COSYNC_MULTI_INSTANCE = '1'
            Start-Process -FilePath $loader -WorkingDirectory $GameDir | Out-Null
        } else {
            # Same environment sfse_loader sets, so Steam does not relaunch the game.
            $env:SteamAppId = '1716740'
            $env:SteamGameId = '1716740'
            Start-Process -FilePath (Join-Path $GameDir 'Starfield.exe') -WorkingDirectory $GameDir | Out-Null
        }
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
    }

    Write-Host "instance $n starting..."
    $deadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
    $proc = $null
    while ((Get-Date) -lt $deadline) {
        $proc = Get-Process Starfield -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | Select-Object -First 1
        if ($proc) {
            $proc.Refresh()
            if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $proc) { throw "instance ${n}: Starfield.exe did not start within $StartupTimeoutSeconds s" }
    if ($proc.MainWindowHandle -eq [IntPtr]::Zero) { throw "instance ${n} (pid $($proc.Id)): no window within $StartupTimeoutSeconds s" }

    # A game that only allows one copy usually exits right after starting.
    Start-Sleep -Seconds 5
    if ($proc.HasExited) {
        $hint = if ($n -gt 1) {
            if ($useSfse) { '; check CoSyncSF-p' + $n + '.log for the multi-instance line' }
            else { '; Starfield refuses a second copy (FindWindowA single-instance check); use SFSE mode' }
        } else { '' }
        throw "instance ${n} (pid $($proc.Id)) exited right after starting (code $($proc.ExitCode))$hint"
    }

    # A normal window has a caption (title bar); fullscreen/borderless do not.
    $style = [CoSync.Win32]::GetWindowLong($proc.MainWindowHandle, -16)  # GWL_STYLE
    if (($style -band 0x00C00000) -ne 0x00C00000) {                      # WS_CAPTION
        Write-Warning "instance ${n}: window has no title bar - it is fullscreen or borderless. Check [Display] in $prefsPath."
    }

    # Tile: move only (resizing a D3D swap chain window from outside is unreliable).
    $x = $work.X + ($n - 1) * ($Width + $frameW)
    $null = [CoSync.Win32]::ShowWindow($proc.MainWindowHandle, 1)  # SW_SHOWNORMAL
    $null = [CoSync.Win32]::SetWindowPos($proc.MainWindowHandle, [IntPtr]::Zero, $x, $work.Y, 0, 0, 0x0001 -bor 0x0004)  # NOSIZE|NOZORDER
    Write-Host "instance $n running: pid $($proc.Id), window at x=$x"
    $launched += $proc

    if ($n -lt $Count) { Start-Sleep -Seconds $DelaySeconds }
}

Write-Host ''
Write-Host "Started $($launched.Count) instance(s): $(($launched | ForEach-Object Id) -join ', ')"
if ($useSfse -and -not $NoCoSyncConfig) {
    $logs = Join-Path $prefsDir 'SFSE\Logs'
    Write-Host "CoSync logs: $logs\CoSyncSF-p<n>.log (instance 1 hosts on UDP $Port)"
}
