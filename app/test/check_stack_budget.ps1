<#
.SYNOPSIS
   任务栈预算检查：用 gcc 的 -fstack-usage 量出每个函数占多少栈，再按"任务内的调用链"求和，
   与固件真实可用的任务栈（3072 字节，见 sdk_task_create 的钳制）比对。

.DESCRIPTION
   为什么需要这个检查（2026-10-15 的真实事故苗头）：
     sms_svc 任务的有效栈只有 3072 字节，而集成"管理员指令 + 回执分包"时，
     单是 handle_pdu 一帧就 2688 字节、sms_admin_handle 3024 字节、
     sms_concat_build_tx 2608 字节 —— 链式调用合计 5~6KB，**远超栈上限**。
     主机单元测试完全测不出来（PC 栈有 8MB），只有上板才会表现为崩溃。
     这些大对象已改为静态区，本脚本负责防止回归。

   方法：
     - 用 -O1 -fstack-usage 编译 app/src 下**能在主机编译**的模块（纯逻辑 + 业务层）；
     - 单函数上限 SingleMax（默认 1280 字节）；
     - 已知调用链求和上限 ChainMax（默认 2048 字节，给 3072 留 1KB 余量）；
     - 主机是 x86-64，指针 8 字节、对齐 16，同一份代码的帧通常**大于** ARM32，
       因此在主机上通过即对固件偏保守（安全方向）。

   注意：sdk_port.c 等直接 include SDK 头文件的文件无法在主机编译，其函数不参与统计
   （它们是既有 SDK 适配层，不在本次改动范围内）。

.EXAMPLE
    pwsh -File app/test/check_stack_budget.ps1
#>
[CmdletBinding()]
param(
    [string]$AppDir   = (Join-Path $PSScriptRoot '..'),
    [int]   $SingleMax = 1280,
    [int]   $ChainMax  = 2048
)

$ErrorActionPreference = 'Stop'
$AppDir = (Resolve-Path $AppDir).Path
$srcDir = Join-Path $AppDir 'src'
$outDir = Join-Path $AppDir 'test\out\su'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Get-ChildItem $outDir -File -ErrorAction SilentlyContinue | Remove-Item -Force

$inc = @("-I$AppDir\inc")

Write-Host "== 栈预算检查（-fstack-usage）==" -ForegroundColor Cyan

# ---- 1) 逐个编译出 .su（编译失败的模块直接跳过，不算错误）----
$usage = @{}
foreach ($f in (Get-ChildItem $srcDir -Filter *.c)) {
    $obj = Join-Path $outDir ($f.BaseName + '.o')
    $log = & gcc -c -std=gnu11 -O1 -fstack-usage @inc $f.FullName -o $obj 2>&1
    # .su 的命名随 gcc 版本而异：可能跟输出文件（xxx.o.su），也可能跟源文件名（xxx.su）
    $su = @("$obj.su", (Join-Path $outDir ($f.BaseName + '.su'))) |
          Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $su) { continue }
    foreach ($line in (Get-Content $su)) {
        $p = $line -split "`t"
        if ($p.Count -ge 2 -and $p[1] -match '^\d+$') {
            $fn = ($p[0] -split ':')[-1]
            $usage[$fn] = [int]$p[1]
        }
    }
}

if ($usage.Count -eq 0) { throw "没有采集到任何 -fstack-usage 结果（gcc 是否支持该选项？）" }

# ---- 2) 打印占用最大的若干函数 ----
Write-Host "  单函数栈占用 Top 10："
$usage.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 10 | ForEach-Object {
    Write-Host ("    {0,6} 字节  {1}" -f $_.Value, $_.Key)
}

# ---- 3) 任务内已知调用链（函数缺失即视为被内联/未编译，跳过）----
$chains = [ordered]@{
    '短信指令链 (sms task)' = @('handle_pdu','sms_admin_handle','sms_cmd_parse','sms_rcfg_apply','append_reply')
    '回执发送链 (sms task)' = @('handle_pdu','send_reply','sms_concat_build_tx','sms_concat_parse_csca')
    '转发链 (sms task)'     = @('handle_pdu','webhook_build_forward','webhook_render_template','build_payload','do_send_raw')
    '配置落盘链 (sms task)' = @('handle_pdu','sms_rcfg_save','sms_rcfg_serialize')
    '时间同步链 (sms task)' = @('time_sync_try','sdk_time_sync_from_modem','app_time_parse_cclk')
    '短信导出链 (eloop)'    = @('web_on_client','sms_svc_export')
}

$bad = 0
Write-Host ""
Write-Host "  调用链合计（任务有效栈上限 = 3072 字节，链预算 = $ChainMax）："
foreach ($name in $chains.Keys) {
    $sum = 0; $parts = @()
    foreach ($fn in $chains[$name]) {
        if ($usage.ContainsKey($fn)) { $sum += $usage[$fn]; $parts += "$fn=$($usage[$fn])" }
    }
    $flag = if ($sum -gt $ChainMax) { $bad++; '✗ 超预算' } else { '✓' }
    $color = if ($sum -gt $ChainMax) { 'Red' } else { 'Green' }
    Write-Host ("    {0,6} 字节  {1}  {2}" -f $sum, $flag, $name) -ForegroundColor $color
    Write-Host ("             {0}" -f ($parts -join ' + ')) -ForegroundColor DarkGray
}

# ---- 4) 单函数上限 ----
Write-Host ""
$over = $usage.GetEnumerator() | Where-Object { $_.Value -gt $SingleMax } | Sort-Object Value -Descending
if ($over) {
    foreach ($o in $over) {
        Write-Host ("  [FAIL] 单函数 {$($o.Key)} 占用 $($o.Value) 字节 > 上限 $SingleMax" ) -ForegroundColor Red
        $bad++
    }
}

Write-Host ""
if ($bad -eq 0) {
    Write-Host "栈预算检查通过（单函数 <= $SingleMax，各调用链 <= $ChainMax）。" -ForegroundColor Green
    exit 0
} else {
    Write-Host "栈预算检查失败：$bad 项超限。大对象请改放静态区（见 docs/10 §四 坑表）。" -ForegroundColor Red
    exit 1
}
