# 诊断上报接收端：监听 9090，打印模组 POST 上来的诊断行。
# 用法: pwsh -NoProfile -File tools\diag_listen.ps1
$ErrorActionPreference = 'Stop'

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, 9090)
$listener.Start()
Write-Output '=== diag listener on 0.0.0.0:9090 (Ctrl+C 退出) ==='

$deadline = (Get-Date).AddSeconds(300)   # 最多监听 5 分钟
while ((Get-Date) -lt $deadline) {
    if (-not $listener.Pending()) { Start-Sleep -Milliseconds 200; continue }

    $client = $listener.AcceptTcpClient()
    try {
        $stream = $client.GetStream()
        $buf = New-Object byte[] 8192
        $n = $stream.Read($buf, 0, $buf.Length)
        $text = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)

        $body = $text
        $idx = $text.IndexOf("`r`n`r`n")
        if ($idx -ge 0) { $body = $text.Substring($idx + 4) }

        Write-Output ('[' + (Get-Date -Format 'HH:mm:ss') + '] ' + $body.Trim())

        $resp = "HTTP/1.0 200 OK`r`nContent-Length: 2`r`nConnection: close`r`n`r`nOK"
        $rb = [System.Text.Encoding]::ASCII.GetBytes($resp)
        $stream.Write($rb, 0, $rb.Length)
    } catch {
        Write-Output ('  (read error: ' + $_.Exception.Message + ')')
    } finally {
        $client.Close()
    }
}

$listener.Stop()
Write-Output '=== listener timeout ==='
