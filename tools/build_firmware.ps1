<#
.SYNOPSIS
    一键编译并打包 ML307A-DSLN 固件。

.DESCRIPTION
    完整复现官方流程，但补掉两个官方脚本在本机会踩的坑：
      1. SDK 自带的 make.exe 需要一个 POSIX shell 才能执行 echo/mkdir 等配方，
         而 SDK 里没有带 sh.exe。本脚本用 SHELL=cmd.exe 让 make 直接用 cmd，
         不依赖 MSYS / Git 的 sh，也不会踩沙箱禁止命名管道的问题。
      2. pack.bat 依赖 build.bat 设置的环境变量（PROJECT_ROOT / MODEL_TARGET /
         FLASH_OPT 等），这里有显式补齐。

    产物：out\images\ML307A\DSLN\ML307A_DSLN_APP.zip（内含全部分区镜像与
    download.json，用于 aboot 下载器烧录）。注意：本 SDK 流程**不产生 .pac**。

.EXAMPLE
    pwsh -File tools/build_firmware.ps1
    pwsh -File tools/build_firmware.ps1 -Clean
#>
[CmdletBinding()]
param(
    [string]$SdkDir  = (Join-Path $PSScriptRoot '..\sdk\ML307A_OpenCPU_Standard_1.4.2'),
    [ValidateSet('DSLN','DCLN','GSLN','GCLN')][string]$Model = 'DSLN',
    [switch]$Clean,
    [switch]$SkipPack
)

$ErrorActionPreference = 'Stop'
$SdkDir = (Resolve-Path $SdkDir).Path

# --- 目标型号参数（与 ML307A_build.bat 一致）---
switch ($Model) {
    'DSLN' { $flash = '8M'; $gnss = 'NO'  }
    'DCLN' { $flash = '4M'; $gnss = 'NO'  }
    'GSLN' { $flash = '8M'; $gnss = 'YES' }
    'GCLN' { $flash = '4M'; $gnss = 'YES' }
}

# --- 先部署应用源码，保证编译的是最新代码 ---
& (Join-Path $PSScriptRoot 'deploy_to_sdk.ps1') -SdkDir $SdkDir | Out-Host

# --- 环境 ---
$env:PATH = @(
    (Join-Path $SdkDir 'tools'),
    (Join-Path $SdkDir 'tools\utils'),
    (Join-Path $SdkDir 'tools\utils\python3'),
    (Join-Path $SdkDir 'tools\toolchain\gcc-arm-none-eabi\bin'),
    (Join-Path $SdkDir 'tools\scripts'),
    (Join-Path $SdkDir 'tools\aboot'),
    $env:PATH
) -join ';'

$env:PROJECT_ROOT  = $SdkDir
$env:MODEL_NAME    = 'ML307A'
$env:MODEL_TARGET  = $Model
$env:HW_REVISION   = 'H0'
$env:GNSS_OPT      = $gnss
$env:FLASH_OPT     = $flash
$env:SW_REVISION   = (Get-Date -Format 'yyyyMMddHHmm')

Push-Location $SdkDir
try {
    if ($Clean) {
        Write-Host "== 清理 ==" -ForegroundColor Cyan
        & make -f tools\scripts\Makefile clean -e MODEL_NAME=ML307A MODEL_TARGET=$Model SHELL=cmd.exe | Out-Host
    }

    Write-Host "== 编译 ML307A-$Model ==" -ForegroundColor Cyan
    & make -f tools\scripts\Makefile all -j4 -e `
        MODEL_NAME=ML307A MODEL_TARGET=$Model `
        HW_REVISION=H0 SW_REVISION=$($env:SW_REVISION) DEMO_SUPPORT=n `
        SHELL=cmd.exe 2>&1 | Out-Host

    if ($LASTEXITCODE -ne 0) {
        throw "编译失败（make 退出码 $LASTEXITCODE）"
    }

    if (-not $SkipPack) {
        Write-Host "== 打包固件 ==" -ForegroundColor Cyan
        & cmd /c "tools\scripts\pack.bat" 2>&1 | Out-Host
    }

    $imgDir = Join-Path $SdkDir "out\images\ML307A\$Model"
    $appBin = Join-Path $imgDir 'ML307A_APP.bin'
    Write-Host ""
    Write-Host "== 产物 ==" -ForegroundColor Green
    Get-ChildItem $imgDir -File | Where-Object { $_.Extension -in '.zip','.bin','.elf','.map' } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 8 |
        ForEach-Object { "{0,12:N0}  {1}" -f $_.Length, $_.Name }

    $pack = Join-Path $imgDir "ML307A_${Model}_APP.zip"
    if (Test-Path $pack) {
        Write-Host ""
        Write-Host "烧录包: $pack" -ForegroundColor Green
    }
}
finally {
    Pop-Location
}
