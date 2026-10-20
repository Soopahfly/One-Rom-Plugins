# Builds an image on disk only. No program, reboot, control or OTP operations.
param([switch]$UpdateMenu)

$ErrorActionPreference = 'Stop'
$workspaceDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$candidateDir = Join-Path $PSScriptRoot 'build-v080'
$cli = Join-Path $workspaceDir 'onerom-new.exe'
$backup = Join-Path $workspaceDir 'onerom-working-0.6.13-full-backup.bin'
$plugin = Join-Path $PSScriptRoot '..\one-rom-v0.8.0\plugins\user\host-control\build\host_control_plugin.bin'

# Check the exact board backup already verified during preparation.
$expectedBackupHash = '48B89CD73CD025EEEEF947A47A5A3071EFC5D041E3448E73DDC57D9B174D8189'
if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne $expectedBackupHash) {
    throw 'Working backup changed. Stop and verify it before proceeding.'
}
$pluginBytes = [IO.File]::ReadAllBytes($plugin)
if ($pluginBytes.Length -ne 65536) { throw 'Expected a 64KB user plugin' }
$backupBytes = [IO.File]::ReadAllBytes($backup)
# Keep the entire 4KB NV page, including the saved selection (currently 5).
[Array]::Copy($backupBytes, 0x2F000, $pluginBytes, 0xF000, 4096)
New-Item -ItemType Directory -Force -Path $candidateDir | Out-Null
$settingsPlugin = Join-Path $candidateDir 'host_control_rgb_0.1.6.1-with-settings.bin'
[IO.File]::WriteAllBytes($settingsPlugin, $pluginBytes)

$romNames = @('c64_bootloader.bin','901227-03.rom','JiffyDOS_C64_6.01.bin',
    'jaffydos.bin','EXOS_V3.rom','jiffy_dolphin.bin','destest-kernal.rom')
$romLabels = @{
    'c64_bootloader.bin' = 'One ROM Boot Menu'
    '901227-03.rom' = 'Commodore Stock Rev 3'
    'JiffyDOS_C64_6.01.bin' = 'JiffyDOS 6.01'
    'jaffydos.bin' = 'JaffyDOS 1.3'
    'EXOS_V3.rom' = 'EXOS V3'
    'jiffy_dolphin.bin' = 'JiffyDOS + DolphinDOS'
    'destest-kernal.rom' = 'DesTest Diagnostics'
}
$expectedRomHashes = Get-Content (Join-Path $PSScriptRoot 'v080\rom-sha256.json') -Raw | ConvertFrom-Json
$newMenu = $null
if ($UpdateMenu) {
    # Same bootloader family as the installed image. Never overwrite the original.
    $newMenu = Join-Path $candidateDir 'c64_bootloader-v0.1.2.bin'
    $menuHash = '925B548EF393641FE138439CA35B5D902BF327A41CBBFD1F0EC42B074AB50200'
    $menuValid = (Test-Path -LiteralPath $newMenu) -and
        ((Get-FileHash -LiteralPath $newMenu).Hash -eq $menuHash)
    if (-not $menuValid) {
        Invoke-WebRequest -Uri 'https://images.onerom.org/roms/host-control/c64-bootloader/v0.1.2/c64_bootloader.bin' -OutFile $newMenu
    }
    if ((Get-Item -LiteralPath $newMenu).Length -ne 8192 -or
        (Get-FileHash -LiteralPath $newMenu).Hash -ne $menuHash) {
        throw 'Downloaded menu failed the published size/SHA256 check'
    }
}
$buildArgs = @('firmware','build','--board','fire-24-e','--size','M',
    '--version','0.8.0','--plugin','usb,version=0.3.2',
    '--plugin',"file=$settingsPlugin",'--config-name','C64 RGB boot menu v0.8.0 candidate')
foreach ($romName in $romNames) {
    $romPath = Join-Path $workspaceDir $romName
    if ((Get-FileHash -LiteralPath $romPath).Hash -ne $expectedRomHashes.$romName) {
        throw "ROM changed since verification: $romName"
    }
    # label replaces the filename in firmware metadata and the C64 menu.
    $label = $romLabels[$romName]
    $sourceFile = $romName
    if ($UpdateMenu -and $romName -eq 'c64_bootloader.bin') {
        $sourceFile = $newMenu
        $label = 'One ROM Boot Menu 0.1.2'
    }
    $buildArgs += @('--slot',"file=$sourceFile,label=$label,type=2364,cs1=active-low,cpu-freq=150")
}
$candidate = Join-Path $candidateDir 'onerom-fire-24-e-0.8.0-rgb-candidate.bin'
if ($UpdateMenu) {
    $candidate = Join-Path $candidateDir 'onerom-fire-24-e-0.8.0-rgb-menu0.1.2-candidate.bin'
}
$buildArgs += @('--output',$candidate,'--save-config',(Join-Path $candidateDir 'candidate-config.json'))
Push-Location $workspaceDir
try {
    & $cli @buildArgs
    if ($LASTEXITCODE) { throw 'Candidate image build failed. No device was modified.' }
} finally { Pop-Location }
& $cli firmware inspect --firmware $candidate
if ($LASTEXITCODE) { throw 'Candidate inspection failed' }
$candidateBytes = [IO.File]::ReadAllBytes($candidate)
for ($i = 0; $i -lt 65536; $i++) {
    if ($candidateBytes[0x20000 + $i] -ne $pluginBytes[$i]) {
        throw "Embedded plugin differs at offset $i"
    }
}
Get-FileHash -LiteralPath $candidate -Algorithm SHA256 | Format-List
Write-Host 'Candidate built and embedded plugin/settings verified. Nothing has been flashed.'
