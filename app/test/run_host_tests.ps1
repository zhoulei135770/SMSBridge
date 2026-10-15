<#
.SYNOPSIS
    在 PC 上编译并运行纯逻辑模块的单元测试（不需要模组，不需要 ARM 工具链）。

.DESCRIPTION
    被测模块都是"无 SDK 依赖"的纯 C：sms_pdu / app_config / sha256 / webhook / web_ui。
    用 MinGW gcc + AddressSanitizer + UndefinedBehaviorSanitizer 编译运行，
    可以先把短信解码、配置序列化、JSON 转义、HTML 转义与缓冲区边界全部测对，
    再上板，显著降低硬件调试成本。
#>
[CmdletBinding()]
param(
    [string]$AppDir = (Join-Path $PSScriptRoot '..'),
    [switch]$NoSanitizer
)

$ErrorActionPreference = 'Stop'
$AppDir = (Resolve-Path $AppDir).Path
$outDir = Join-Path $AppDir 'test\out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Write-Host "== 1) 生成 PDU 测试向量（独立 Python 编码器）==" -ForegroundColor Cyan
& python (Join-Path $AppDir 'test\gen_pdus.py') --emit-c-header (Join-Path $AppDir 'test\pdu_vectors.h') | Out-Host

Write-Host "== 2) 编译主机测试 ==" -ForegroundColor Cyan
$baseFlags = @('-std=gnu11', '-Wall', '-Wextra', '-g', '-O1',
               "-I$AppDir\inc", "-I$AppDir\test")
$sanFlags  = @('-fsanitize=address,undefined', '-fno-omit-frame-pointer')

$src = @(
    (Join-Path $AppDir 'src\sms_pdu.c'),
    (Join-Path $AppDir 'src\app_config.c'),
    (Join-Path $AppDir 'src\sha256.c'),
    (Join-Path $AppDir 'src\webhook.c'),
    (Join-Path $AppDir 'src\web_ui.c'),
    (Join-Path $AppDir 'src\sms_cmd.c'),      # 模块3：短信指令解析（模块5 依赖它）
    (Join-Path $AppDir 'src\sms_rcfg.c'),     # 模块5：远程配置持久化
    (Join-Path $AppDir 'src\sms_concat.c'),   # 模块2：长短信分包/重组 + SMSC 解析
    (Join-Path $AppDir 'src\sms_admin.c'),    # 集成层：管理员指令路由
    (Join-Path $AppDir 'src\webhook_fmt.c'),  # 模块4：模板渲染/关键词注入/加签 URL
    (Join-Path $AppDir 'src\app_time.c'),     # 时间解析与时钟可信判定
    (Join-Path $AppDir 'src\net_addr.c'),     # IPv4/IPv6 判定与 CGPADDR 双栈解析
    (Join-Path $AppDir 'test\host_tests.c')
)
$exe = Join-Path $outDir 'host_tests.exe'

# MinGW 通常不带 libasan/libubsan 运行时，先探测再决定是否启用消毒器
$useSan = $false
if (-not $NoSanitizer) {
    $probe = Join-Path $outDir 'san_probe.exe'
    $null = & gcc @baseFlags @sanFlags @src -o $probe 2>&1
    if ($LASTEXITCODE -eq 0) {
        $useSan = $true
        Write-Host "  使用 AddressSanitizer + UBSan" -ForegroundColor Green
    } else {
        Write-Host "  本机 gcc 缺少 sanitizer 运行时，改用哨兵字节做越界检测" -ForegroundColor Yellow
    }
}

$flags = $baseFlags
if ($useSan) { $flags += $sanFlags }

& gcc @flags @src -o $exe
if ($LASTEXITCODE -ne 0) { throw "编译失败" }

Write-Host "== 3) 运行 ==" -ForegroundColor Cyan
& $exe
$rc = $LASTEXITCODE

Write-Host ""
if ($rc -eq 0) {
    Write-Host "主机单元测试全部通过。" -ForegroundColor Green
} else {
    Write-Host "存在失败用例（退出码 $rc）" -ForegroundColor Red
}

# 单元测试测不出"栈溢出"（PC 栈有 8MB，固件任务栈只有 3072 字节），
# 因此单独做一次 -fstack-usage 的调用链预算检查。
Write-Host ""
& (Join-Path $PSScriptRoot 'check_stack_budget.ps1') -AppDir $AppDir
$stackRc = $LASTEXITCODE

if ($rc -ne 0 -or $stackRc -ne 0) { exit 1 }
exit 0
