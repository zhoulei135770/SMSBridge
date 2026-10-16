# 用保持连接的方式与模组交互，抢在 5 秒重置窗口内完成 IMEI/SN 写回
# 用法: pwsh -NoProfile -File tools\restore_mrd.ps1

function New-Port {
    param([string]$Port = 'COM3', [int]$Tries = 40)
    for ($k = 1; $k -le $Tries; $k++) {
        try {
            $sp = New-Object System.IO.Ports.SerialPort($Port, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
            $sp.ReadTimeout = 300
            $sp.WriteTimeout = 400
            $sp.Open()
            Start-Sleep -Milliseconds 200
            try { $sp.DiscardInBuffer() } catch {}
            return $sp
        } catch {
            if ($sp) { try { $sp.Dispose() } catch {} }
            Start-Sleep -Milliseconds 400
        }
    }
    return $null
}

function Send-Cmd {
    param($sp, [string]$Cmd, [int]$ReadMs = 1200)
    $sb = New-Object System.Text.StringBuilder
    try {
        $sp.Write("$Cmd`r`n")
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ($sw.ElapsedMilliseconds -lt $ReadMs) {
            try {
                $l = $sp.ReadLine().Trim()
                if ($l) { [void]$sb.AppendLine($l) }
            } catch {}
            if ($sb.ToString() -match '(?m)^(OK|ERROR)$') { break }
        }
    } catch { return $null }   # 掉线
    return ($sb.ToString().Trim() -replace "`r?`n", ' | ')
}

$IMEI = '000000000000000'
$SN   = '20209M0029639S034670'

Write-Output '=== 写回 IMEI / SN（整包烧录擦除了 nvm 导致其丢失）==='
$done = $false
for ($round = 1; $round -le 25 -and -not $done; $round++) {
    $sp = New-Port
    if (-not $sp) { Write-Output "  轮次 $round: 端口打不开，继续重试"; continue }

    $r1 = Send-Cmd -sp $sp -Cmd 'AT'
    if ($null -eq $r1) { Write-Output "  轮次 $round: 打开后立即掉线（模组在重启）"; try { $sp.Dispose() } catch {}; continue }

    Write-Output "  轮次 $round: 端口稳定 ✓"
    Write-Output ("    AT*MRD_IMEI? -> " + (Send-Cmd -sp $sp -Cmd 'AT*MRD_IMEI?'))
    Write-Output ("    AT*MRD_SN?   -> " + (Send-Cmd -sp $sp -Cmd 'AT*MRD_SN?'))

    # 依次尝试几种写入语法，遇到 OK 即停
    $ok = $false
    foreach ($cmd in @("AT*MRD_IMEI=W,$IMEI", "AT*MRD_IMEI=1,$IMEI", "AT*MRD_IMEI=$IMEI")) {
        $res = Send-Cmd -sp $sp -Cmd $cmd -ReadMs 1500
        if ($null -eq $res) { Write-Output "    $cmd -> 掉线"; break }
        Write-Output ("    {0} -> {1}" -f $cmd, $res)
        if ($res -match 'OK' -and $res -notmatch 'ERROR') { $ok = $true; break }
    }
    if ($ok) {
        Write-Output ("    AT*MRD_IMEI? 复核 -> " + (Send-Cmd -sp $sp -Cmd 'AT*MRD_IMEI?'))
        foreach ($cmd in @("AT*MRD_SN=W,$SN", "AT*MRD_SN=1,$SN")) {
            $res = Send-Cmd -sp $sp -Cmd $cmd -ReadMs 1500
            Write-Output ("    {0} -> {1}" -f $cmd, $res)
            if ($res -match 'OK' -and $res -notmatch 'ERROR') { break }
        }
        $done = $true
    }
    try { if ($sp.IsOpen) { $sp.Close() }; $sp.Dispose() } catch {}
    Start-Sleep -Milliseconds 500
}

Write-Output ''
Write-Output '=== 写入后观察 40 秒：重启循环是否停止 ==='
$states = @()
for ($i = 1; $i -le 40; $i++) {
    $a = Get-NetAdapter -EA SilentlyContinue | Where-Object { $_.InterfaceDescription -match 'NDIS' } | Select-Object -First 1
    $states += $(if ($a) { 'U' } else { '.' })
    Start-Sleep -Seconds 1
}
Write-Output ('  时间线: ' + ($states -join ''))
Write-Output ('  掉线秒数: ' + ([regex]::Matches(($states -join ''), '\.')).Count + ' / 40')

