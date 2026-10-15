<#
.SYNOPSIS
    ML307A-DSLN 应用镜像（app-only）一键刷写脚本 —— 可复用、无任何写死的本地路径。

.DESCRIPTION
    本脚本把本工程的"app-only 刷写流程"固化下来，供任何克隆本仓库的人直接使用。

    背景（为什么需要它）：
      中移 ML307A OpenCPU SDK 的打包脚本 `tools\scripts\pack.bat` 调用 ASR 的
      `arelease.exe`，产出一个**整包** `ML307A_DSLN_APP.zip`（约 10 MB，内含
      cp.bin / system.img / bootloader.ubi / rf.bin 等全部分区镜像，并含
      `erase nvm` / `erase factory` 这类破坏性动作）。
      只升级本应用时，不需要（也不应该）重刷整包：
        * 若设备内核与 SDK 基线不同源（应用里的 `cm_stub_DSLN.o` 是硬编码内核
          绝对地址的跳转表），整包会顺带把内核换掉；换内核属于高风险动作。
        * 整包会擦除 `nvm` / `factory` 分区，可能丢失 IMEI / 产测数据。
        * 整包体积大、耗时长。

      因此本脚本从 SDK 的整包里**只抽出刷写器必需的 4 个文件**
      （`preboot.img` / `flasher.img` / `flashinfo.bin` / `partition.bin`），
      换上我们要烧的 `user_app.bin`，生成一个约 130 KB 的 app-only 下载包
      （`download.json` 里**没有任何 erase 指令**），再去呼叫官方下载器
      `adownload.exe`。

    流程（共 6 步，与上板实测流程一致）：
      1) 从 --ReleaseZip 指定的 SDK 整包 zip 中抽出 preboot.img / flasher.img /
         flashinfo.bin / partition.bin；
      2) 用 --AppBin 指定的应用镜像作为 `user_app.bin` 放进同一个目录；
      3) 生成 app-only 的 `download.json`（等价于 `arelease` 的 group 6 只保留
         `flash user_app` 那一条）；
      4) 通过**正常模式 AT 口**（默认 COM3 @115200）发 `AT$MYDOWNLOAD=1`，
         让模组自己重启进 aboot 下载模式（免短接 BOOT_MODE）；
      5) 调 `adownload.exe -u -a -r -q -s 115200 <包>` 刷写（**绝对不要加 -m**）；
      6) 轮询 AT 口，等设备回到正常模式（能回 `OK`）。

    重要前提与风险（请先读完再执行）：
      * `AT$MYDOWNLOAD=1` 这条软入口**只在 MBR 基线内核上有效**；出厂 MTS 内核上
        它返回 ERROR，此时只能靠 BOOT_MODE 触点短接 + 上电（硬件操作，本脚本不涉及）。
      * app-only 包**必须与设备当前内核同源**。若不确定设备内核版本，请先用
        `ATI` 读取内核版本，与 SDK 基线对照；不同源请烧整包（不在本脚本范围）。
      * 脚本**永不**给 `adownload.exe` 传 `-m` / `--production`。production 模式会
        执行 `download.json` 里 `productionOnly: true` 的动作（`erase factory`、
        `fuse`），可能抹掉 IMEI 与产测数据。
      * 刷写有风险，请自行确认设备可恢复（例如事先备份 IMEI/SN、准备好整包与
        短接触点方案）。风险自担，详见仓库 README 的免责声明。
      * 刷写前请先关闭任何占用 AT 口 / 下载口的程序（串口助手、其它脚本、
        `tools/diag_listen2.ps1` 不占串口但请注意别同时开）。

.PARAMETER ReleaseZip
    SDK 产出的整包路径，默认按"SDK 放在本仓库根目录的 sdk/ 下"来推断：
    `<仓库根>\sdk\ML307A_OpenCPU_Standard_1.4.2\out\images\ML307A\DSLN\ML307A_DSLN_APP.zip`
    （相对本脚本所在目录解析；也可传任意绝对路径）。
    这个 zip 里必须能找到 preboot.img / flasher.img / flashinfo.bin / partition.bin。

.PARAMETER AdownloadExe
    `adownload.exe` 的完整路径。它来自中移/ASR 的 aboot 工具包
    （`aboot-tools-*-win-x64`），**不随本仓库分发**，请自行获取。
    例：`C:\aboot-tools-2022.03.01-win-x64\adownload.exe`

.PARAMETER AppBin
    要刷入的应用镜像，默认 `..\firmware\user_app.bin`（即本仓库自带的编译产物）。
    也可以用自己编译出来的 `ML307A_DSLN_APP.bin`。

.PARAMETER AtPort
    正常模式下的 AT 串口，默认 `COM3`。用来发 `AT$MYDOWNLOAD=1`，
    以及刷完后轮询设备是否回到正常模式。

.PARAMETER BaudRate
    AT 口与下载口速率，默认 `115200`（`adownload.exe -s` 也用这个值）。

.PARAMETER WorkDir
    解包/组包的临时目录，默认 `$env:TEMP\ml307a_apponly_flash`。

.PARAMETER NormalPortTimeoutSec
    发送 `AT$MYDOWNLOAD=1` 后，等待 AT 口消失（模组重启进下载模式）的秒数，默认 30。

.PARAMETER FlashTimeoutSec
    `adownload.exe` 的最长执行秒数，默认 600。

.PARAMETER ReturnTimeoutSec
    刷完后等待设备回到正常模式（能回 OK）的秒数，默认 120。

.PARAMETER SkipEnterDownload
    跳过第 4 步。适用于你已经用别的方式（例如 BOOT_MODE 短接）让模组进入下载模式。

.PARAMETER DryRun
    只做第 1~3 步（抽包 + 组包），打印将要执行的命令行，不碰串口、不刷写。

.EXAMPLE
    # 先干跑一次，确认包能正确组装（不碰设备）
    pwsh -File firmware\flash_apponly.ps1 -AdownloadExe C:\aboot-tools\adownload.exe -DryRun

.EXAMPLE
    # 正式刷写（默认 COM3 @115200）
    pwsh -File firmware\flash_apponly.ps1 -AdownloadExe C:\aboot-tools\adownload.exe

.EXAMPLE
    # 换个串口，并刷自己刚编译出来的镜像
    pwsh -File firmware\flash_apponly.ps1 -AdownloadExe C:\aboot-tools\adownload.exe `
        -AtPort COM7 -AppBin sdk\ML307A_OpenCPU_Standard_1.4.2\out\images\ML307A\DSLN\ML307A_DSLN_APP.bin

.NOTES
    已验证过的软硬件组合：ML307A-DSLN（ASR1606）+ DEMO-ML307X-V1.04 底板，
    MBR 基线内核，AT 口 COM3 @115200，下载口由 adownload 自动枚举（实测为 COM5）。
#>
[CmdletBinding()]
param(
    [string]$ReleaseZip = (Join-Path $PSScriptRoot '..\sdk\ML307A_OpenCPU_Standard_1.4.2\out\images\ML307A\DSLN\ML307A_DSLN_APP.zip'),
    [Parameter(Mandatory = $true)]
    [string]$AdownloadExe,
    [string]$AppBin = (Join-Path $PSScriptRoot 'user_app.bin'),
    [string]$AtPort = 'COM3',
    [int]$BaudRate = 115200,
    [string]$WorkDir = (Join-Path $env:TEMP 'ml307a_apponly_flash'),
    [int]$NormalPortTimeoutSec = 30,
    [int]$FlashTimeoutSec = 600,
    [int]$ReturnTimeoutSec = 120,
    [switch]$SkipEnterDownload,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

# 抽包 + 组包阶段需要的镜像（这 4 个来自 SDK 整包，属于 vendor 产物，不随仓库分发）
$VendorImages = @('preboot.img', 'flasher.img', 'flashinfo.bin', 'partition.bin')
$AppImageName = 'user_app.bin'

function Write-Step([string]$text) { Write-Host "`n== $text ==" -ForegroundColor Cyan }
function Write-Ok([string]$text)   { Write-Host "   $text" -ForegroundColor Green }
function Write-Warn2([string]$text){ Write-Host "   $text" -ForegroundColor Yellow }

# ---------------------------------------------------------------------------
# 串口小工具：全部走 .NET SerialPort，不依赖任何外部模块
# ---------------------------------------------------------------------------
function Open-AtPort {
    param([string]$Port, [int]$Baud, [int]$TimeoutMs = 1500)
    $sp = New-Object System.IO.Ports.SerialPort($Port, $Baud,
              [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $sp.ReadTimeout = $TimeoutMs
    $sp.WriteTimeout = 1500
    $sp.Open()
    Start-Sleep -Milliseconds 150
    try { $sp.DiscardInBuffer() } catch { }
    return $sp
}

function Send-AtCmd {
    param($sp, [string]$Cmd, [int]$ReadMs = 1500)
    $sb = New-Object System.Text.StringBuilder
    try {
        $sp.Write("$Cmd`r`n")
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ($sw.ElapsedMilliseconds -lt $ReadMs) {
            try {
                $line = $sp.ReadLine().Trim()
                if ($line) { [void]$sb.AppendLine($line) }
            } catch { }
            $t = $sb.ToString()
            if ($t -match '(?m)^(OK|ERROR)$' -or $t -match '\+CME ERROR|\+CMS ERROR') { break }
        }
    } catch { return $null }   # 掉线（模组重启）
    return $sb.ToString().Trim()
}

function Test-AtAlive {
    param([string]$Port, [int]$Baud)
    $sp = $null
    try {
        $sp = Open-AtPort -Port $Port -Baud $Baud -TimeoutMs 400
        $r = Send-AtCmd -sp $sp -Cmd 'AT' -ReadMs 800
        return ($r -match 'OK')
    } catch {
        return $false
    } finally {
        if ($sp) { try { if ($sp.IsOpen) { $sp.Close() }; $sp.Dispose() } catch { } }
    }
}

# ===========================================================================
# 第 1~3 步：解包 → 换 user_app → 重新组包
# ===========================================================================
Write-Step "1/6 检查输入"

if (-not (Test-Path $AdownloadExe)) { throw "找不到 adownload.exe: $AdownloadExe（请用 -AdownloadExe 指定正确路径）" }
$AdownloadExe = (Resolve-Path $AdownloadExe).Path

if (-not (Test-Path $AppBin)) { throw "找不到应用镜像: $AppBin（请用 -AppBin 指定，或先编译生成）" }
$AppBin = (Resolve-Path $AppBin).Path

if (-not (Test-Path $ReleaseZip)) {
    throw @"
找不到 SDK 整包: $ReleaseZip
  这个 zip 由 SDK 的打包脚本产出（tools\scripts\pack.bat → arelease.exe），
  **不随本仓库分发**。请先按 README「编译步骤」跑通：
      pwsh -File tools/build_firmware.ps1
  或用 -ReleaseZip 指定你手上的 ML307A_DSLN_APP.zip。
"@
}
$ReleaseZip = (Resolve-Path $ReleaseZip).Path

Write-Ok "SDK 整包   : $ReleaseZip"
Write-Ok "应用镜像   : $AppBin  ($((Get-Item $AppBin).Length) 字节)"
Write-Ok "下载器     : $AdownloadExe"
Write-Ok "AT 口      : $AtPort @ $BaudRate"

Write-Step "2/6 从 SDK 整包中抽出刷写器镜像"

if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$zip = [System.IO.Compression.ZipFile]::OpenRead($ReleaseZip)
try {
    $names = $zip.Entries | ForEach-Object { $_.FullName }
    foreach ($img in $VendorImages) {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq $img } | Select-Object -First 1
        if (-not $entry) {
            throw "整包 $ReleaseZip 里没有 $img —— 这不像是一个 arelease 产出的 ML307A 整包。包内条目：`n  $($names -join "`n  ")"
        }
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $WorkDir $img), $true)
        Write-Ok ("抽出 {0,-16} {1,10:N0} 字节" -f $img, $entry.Length)
    }
}
finally { $zip.Dispose() }

Copy-Item $AppBin (Join-Path $WorkDir $AppImageName) -Force
Write-Ok ("放入 {0,-16} {1,10:N0} 字节  <- {2}" -f $AppImageName, (Get-Item $AppBin).Length, (Split-Path $AppBin -Leaf))

Write-Step "3/6 生成 app-only 的 download.json"

# 只保留"刷写器自身 + 写 user_app"，不含任何 erase / erase factory / fuse 动作。
# 结构等价于 arelease 产出包中 group 6 的那一条 flash user_app。
$downloadJson = @'
[
   { "command":"require","group":"0","name":"product","value":"arom-tiny" },
   { "command":"require","group":"0","name":"version","value":"0.5" },
   { "command":"require","group":"0","name":"version-bootrom","value":"2021.10.16" },
   { "command":"progress","group":"0","name":"weight","value":1179648 },
   { "command":"call","group":"0","image":"preboot.img" },
   { "command":"call","group":"0","image":"flasher.img" },
   { "command":"partition","group":"0","image":"flashinfo.bin" },
   { "command":"partition","group":"0","image":"partition.bin" },
   { "command":"flash","group":"6","image":"user_app.bin","partition":"user_app","weight":1179648 }
]
'@
$dlPath = Join-Path $WorkDir 'download.json'
[System.IO.File]::WriteAllText($dlPath, $downloadJson, (New-Object System.Text.UTF8Encoding($false)))
Write-Ok "已写出 download.json（无 erase / 无 fuse / 无 productionOnly）"

$outZip = Join-Path $WorkDir 'ML307A_DSLN_APP_apponly.zip'
if (Test-Path $outZip) { Remove-Item $outZip -Force }
$pack = [System.IO.Compression.ZipFile]::Open($outZip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($n in (@($VendorImages) + @($AppImageName) + @('download.json'))) {
        $src = Join-Path $WorkDir $n
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($pack, $src, $n)
    }
}
finally { $pack.Dispose() }

$outZipHash = (Get-FileHash $outZip -Algorithm SHA256).Hash.ToLower()
Write-Ok ("app-only 包 : {0}" -f $outZip)
Write-Ok ("             {0:N0} 字节  SHA256 {1}" -f (Get-Item $outZip).Length, $outZipHash)

# 自检：确认包内不含任何破坏性指令
$check = [System.IO.Compression.ZipFile]::OpenRead($outZip)
try {
    $entry = $check.Entries | Where-Object { $_.FullName -eq 'download.json' }
    $sr = New-Object System.IO.StreamReader($entry.Open())
    $body = $sr.ReadToEnd(); $sr.Close()
    if ($body -match '"erase"|"fuse"|productionOnly') {
        throw "自检失败：生成的 download.json 里出现了破坏性指令（erase/fuse/productionOnly），已中止。"
    }
}
finally { $check.Dispose() }
Write-Ok "自检通过：包内无 erase / fuse / productionOnly"

$adownloadArgs = @('-u', '-a', '-r', '-q', '-s', "$BaudRate", $outZip)
$adownloadCmdLine = '"{0}" {1}' -f $AdownloadExe, ($adownloadArgs -join ' ')
Write-Host ""
Write-Host "  将要执行的命令：" -ForegroundColor DarkGray
Write-Host "    $adownloadCmdLine" -ForegroundColor DarkGray
Write-Host "  （-u 只用 Arom USB 下载口 / -a 自动枚举 / -r 完成后重启 / -q 完成即退出；" -ForegroundColor DarkGray
Write-Host "    刻意不加 -m：production 模式会 erase factory 与 fuse，可能丢 IMEI）" -ForegroundColor DarkGray

if ($DryRun) {
    Write-Host ""
    Write-Host "DryRun：已完成抽包与组包，未接触设备、未刷写。包在 $outZip" -ForegroundColor Yellow
    exit 0
}

# ===========================================================================
# 第 4 步：进下载模式（软入口 AT$MYDOWNLOAD=1）
# ===========================================================================
Write-Step "4/6 让模组进入下载模式"

if ($SkipEnterDownload) {
    Write-Warn2 "已指定 -SkipEnterDownload，跳过软入口步骤（请确认设备已在下载模式）"
}
else {
    if (Test-AtAlive -Port $AtPort -Baud $BaudRate) {
        Write-Ok "$AtPort 上的 AT 口可用"
    }
    else {
        Write-Warn2 "$AtPort 上没有应答 OK；仍会尝试发送软入口命令（若失败请检查串口号/波特率/占用）"
    }

    $sp = $null
    try {
        $sp = Open-AtPort -Port $AtPort -Baud $BaudRate
        Write-Ok "发送 AT`$MYDOWNLOAD=1"
        [void](Send-AtCmd -sp $sp -Cmd 'AT$MYDOWNLOAD=1' -ReadMs 1500)
    }
    catch {
        Write-Warn2 "打开 $AtPort 失败：$($_.Exception.Message)"
    }
    finally {
        if ($sp) { try { if ($sp.IsOpen) { $sp.Close() }; $sp.Dispose() } catch { } }
    }

    Write-Host "   等待 AT 口消失（模组重启进下载模式，最多 $NormalPortTimeoutSec 秒）..." -ForegroundColor DarkGray
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $gone = $false
    while ($sw.Elapsed.TotalSeconds -lt $NormalPortTimeoutSec) {
        if (-not (Test-AtAlive -Port $AtPort -Baud $BaudRate)) { $gone = $true; break }
        Start-Sleep -Milliseconds 500
    }
    if ($gone) {
        Write-Ok "AT 口已断开，模组应已进入下载模式（不要再用其它程序打开 $AtPort）"
    }
    else {
        Write-Warn2 "AT 口在 $NormalPortTimeoutSec 秒内一直有应答。可能原因："
        Write-Warn2 "  * 该内核不支持 AT`$MYDOWNLOAD=1（MTS 出厂内核会返回 ERROR），需用 BOOT_MODE 短接方案"
        Write-Warn2 "  * 串口号/波特率不对，或命令没真正发到模组的 AT 口"
        Write-Warn2 "  3 秒后仍会继续尝试刷写（adownload -a 会自行枚举下载口）"
        Start-Sleep -Seconds 3
    }
}

# ===========================================================================
# 第 5 步：呼叫 adownload.exe
# ===========================================================================
Write-Step "5/6 刷写（adownload.exe）"
Write-Host "  提示：AT 口必须已关闭，本脚本已确保没有句柄占用 $AtPort" -ForegroundColor DarkGray

$logPath = Join-Path $WorkDir ('adownload_{0:yyyyMMdd_HHmmss}.log' -f (Get-Date))
Write-Host "  日志：$logPath" -ForegroundColor DarkGray

$proc = Start-Process -FilePath $AdownloadExe -ArgumentList $adownloadArgs `
        -NoNewWindow -PassThru -RedirectStandardOutput $logPath -RedirectStandardError "$logPath.err"
$done = $proc.WaitForExit($FlashTimeoutSec * 1000)
if (-not $done) {
    try { $proc.Kill() } catch { }
    throw "adownload.exe 超过 $FlashTimeoutSec 秒未结束，已终止。请查看日志：$logPath"
}
$exit = $proc.ExitCode

$logText = ''
foreach ($p in @($logPath, "$logPath.err")) { if (Test-Path $p) { $logText += (Get-Content $p -Raw) } }
$logText | Write-Host

if ($exit -ne 0) {
    throw "adownload.exe 退出码 $exit（非 0）。完整日志：$logPath"
}
if ($logText -notmatch 'SUCCEEDED|all finished|program "user_app" done|program ''user_app'' done') {
    Write-Warn2 "日志里没看到明确的成功标志（SUCCEEDED / all finished / program user_app done）。"
    Write-Warn2 "请人工核对上面的完整输出；若确实失败，常见原因：内核与 app 镜像不同源、包与设备型号不匹配、USB 线/口问题。"
}
else {
    Write-Ok "刷写日志显示成功"
}

# ===========================================================================
# 第 6 步：等设备回到正常模式
# ===========================================================================
Write-Step "6/6 等待设备回到正常模式"

Write-Host "   轮询 $AtPort（最多 $ReturnTimeoutSec 秒）。若模组回到正常模式，应能回 OK。" -ForegroundColor DarkGray
$sw = [Diagnostics.Stopwatch]::StartNew()
$back = $false
while ($sw.Elapsed.TotalSeconds -lt $ReturnTimeoutSec) {
    if (Test-AtAlive -Port $AtPort -Baud $BaudRate) { $back = $true; break }
    Start-Sleep -Milliseconds 800
}

if (-not $back) {
    Write-Warn2 "$ReturnTimeoutSec 秒内 $AtPort 没有回应 OK。"
    Write-Warn2 "  排查建议："
    Write-Warn2 "    1) 换一个串口试试（正常模式 AT 口可能被重新枚举成别的 COM 号）：-AtPort COMn"
    Write-Warn2 "    2) 用串口助手看是否有开机横幅 / 复位原因打印"
    Write-Warn2 "    3) 若一直不出 AT 口，可能是应用启动即崩溃 → 用整包回退，或用探针/日志定位"
    Write-Warn2 "    4) 检查是否为 USB 供电不足（5V/2A 直供，别用劣质 HUB）"
    exit 2
}

Write-Ok "$AtPort 已回到正常模式"
Write-Host ""
try {
    $sp = Open-AtPort -Port $AtPort -Baud $BaudRate
    Write-Host "  ATI   -> $(Send-AtCmd -sp $sp -Cmd 'ATI' -ReadMs 2000)"
    Write-Host "  AT+CPIN? -> $(Send-AtCmd -sp $sp -Cmd 'AT+CPIN?' -ReadMs 2000)"
    Write-Host "  AT+CMGF? -> $(Send-AtCmd -sp $sp -Cmd 'AT+CMGF?' -ReadMs 2000)  (0 = PDU 模式，说明本固件已运行)"
    Write-Host "  AT+CNMI? -> $(Send-AtCmd -sp $sp -Cmd 'AT+CNMI?' -ReadMs 2000)  (2,2,0,0,0 = +CMT 直投已开启)"
}
catch { Write-Warn2 "读取设备状态失败（不影响刷写结论）：$($_.Exception.Message)" }
finally { if ($sp) { try { if ($sp.IsOpen) { $sp.Close() }; $sp.Dispose() } catch { } } }

Write-Host ""
Write-Host "刷写完成。app-only 包与日志留在：$WorkDir" -ForegroundColor Green
exit 0
