param([string]$ToolchainDirectory)
$ErrorActionPreference = 'Stop'
$pluginDir = Join-Path $PSScriptRoot '..\one-rom-v0.8.0\plugins\user\host-control'
if ($ToolchainDirectory) {
    $toolchainDir = $ToolchainDirectory
} else {
    $compiler = Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue
    if ($compiler) {
        $toolchainDir = Split-Path $compiler.Source
    } else {
        $toolchainDir = Join-Path $env:USERPROFILE '.pico-sdk\toolchain\15_2_Rel1\bin'
    }
}
$gcc = Join-Path $toolchainDir 'arm-none-eabi-gcc.exe'
Push-Location $pluginDir
try {
    New-Item -ItemType Directory -Force build | Out-Null
    $flags = @('-mcpu=cortex-m33','-mthumb','-mfloat-abi=hard','-mfpu=fpv5-sp-d16',
        '-fstack-usage','-fconserve-stack','-O3','-Wall','-Wextra','-Werror',
        '-ffunction-sections','-fdata-sections','-MMD','-MP',
        '-DMAJOR_VERSION=0','-DMINOR_VERSION=1','-DPATCH_VERSION=6','-DBUILD_VERSION=1',
        '-DNV_ERASE_FN_MAX=80u','-I../../../firmware/ora','-I.','-Iinclude')
    & $gcc @flags -c src/host_control_main.c -o build/host_control_main.o
    if ($LASTEXITCODE) { throw 'Plugin compile failed' }
    & $gcc @flags -fPIC -fno-plt -c src/flash_erase.c -o build/flash_erase.o
    if ($LASTEXITCODE) { throw 'Flash routine compile failed' }
    & $gcc @flags -nostdlib '-L../../../firmware/ora' -T host_control_plugin.ld `
        '-Wl,--defsym=NV_ERASE_FN_MAX=80' '-Wl,--defsym=PLUGIN_STATIC_MAX=420' `
        '-Wl,--fatal-warnings' '-Wl,--no-warn-rwx-segments' `
        '-Wl,-Map=build/host_control_plugin.map' `
        build/host_control_main.o build/flash_erase.o -lc -lgcc -o build/host_control_plugin.elf
    if ($LASTEXITCODE) { throw 'Plugin link failed' }
    & (Join-Path $toolchainDir 'arm-none-eabi-objcopy.exe') -O binary build/host_control_plugin.elf build/host_control_plugin.bin
    if ($LASTEXITCODE) { throw 'Binary conversion failed' }
    & (Join-Path $toolchainDir 'arm-none-eabi-size.exe') -A build/host_control_plugin.elf
} finally { Pop-Location }
