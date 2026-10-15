# 诊断上报接收端（改进版）：完整读取请求体，支持多连接/分段
# 用法: pwsh -NoProfile -File tools/diag_listen2.ps1 [-Seconds 300] [-Port 9090]
param([int]$Seconds = 300, [int]$Port = 9090)

$ErrorActionPreference = 'Stop'
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, $Port)
$listener.Start()
Write-Output "=== diag listener on 0.0.0.0:$Port （最多 $Seconds 秒）==="
$deadline = (Get-Date).AddSeconds($Seconds)
$count = 0
while ((Get-Date) -lt $deadline) {
    if (-not $listener.Pending()) { Start-Sleep -Milliseconds 150; continue }
    $client = $listener.AcceptTcpClient()
    $count++
    try {
        $client.ReceiveTimeout = 3000
        $stream = $client.GetStream()
        $ms = New-Object System.IO.MemoryStream
        $buf = New-Object byte[] 4096
        $idle = 0
        while ($idle -lt 4) {
            try { $n = $stream.Read($buf, 0, $buf.Length) } catch { break }
            if ($n -le 0) { break }
            $ms.Write($buf, 0, $n)
            $idle = 0
            # 已读到完整的头 + 声明的 Content-Length 就收工
            $txt = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
            $hdrEnd = $txt.IndexOf("`r`n`r`n")
            if ($hdrEnd -ge 0) {
                if ($txt -match 'Content-Length:\s*(\d+)') {
                    $need = [int]$Matches[1]
                    if (($txt.Length - $hdrEnd - 4) -ge $need) { break }
                }
                $idle++
            }
        }
        $all = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
        $sep = $all.IndexOf("`r`n`r`n")
        $head = if ($sep -ge 0) { $all.Substring(0, $sep) } else { '' }
        $body = if ($sep -ge 0) { $all.Substring($sep + 4) } else { $all }
        $reqLine = ($head -split "`r`n")[0]
        Write-Output ("[{0}] #{1} {2}" -f (Get-Date).ToString('HH:mm:ss'), $count, $reqLine)
        if ($body.Trim().Length -gt 0) { Write-Output ("        body: " + $body.Trim()) }
        else { Write-Output "        body: <空>" }
        $rb = [System.Text.Encoding]::ASCII.GetBytes("HTTP/1.0 200 OK`r`nContent-Length: 2`r`nConnection: close`r`n`r`nOK")
        $stream.Write($rb, 0, $rb.Length)
    } catch {
        Write-Output ("  (错误: " + $_.Exception.Message + ")")
    } finally { $client.Close() }
}
$listener.Stop()
Write-Output "=== 结束：共 $count 次连接 ==="
