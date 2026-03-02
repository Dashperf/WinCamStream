Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

function Send-WcsCommand {
    param(
        [string]$HostName,
        [int]$Port,
        [hashtable]$Payload,
        [int]$TimeoutMs = 2500
    )

    $client = New-Object System.Net.Sockets.TcpClient
    $client.SendTimeout = $TimeoutMs
    $client.ReceiveTimeout = $TimeoutMs
    $client.Connect($HostName, $Port)

    try {
        $stream = $client.GetStream()
        $writer = New-Object System.IO.StreamWriter($stream, [Text.Encoding]::UTF8, 4096, $true)
        $writer.NewLine = "`n"
        $writer.AutoFlush = $true

        $reader = New-Object System.IO.StreamReader($stream, [Text.Encoding]::UTF8, $false, 4096, $true)
        $json = ($Payload | ConvertTo-Json -Compress -Depth 12)
        $writer.WriteLine($json)
        $line = $reader.ReadLine()
        if ([string]::IsNullOrWhiteSpace($line)) {
            return @{ type = "error"; error = "empty_response" }
        }
        return $line | ConvertFrom-Json
    } finally {
        $client.Dispose()
    }
}

function Invoke-WcsApply {
    param(
        [string]$HostName,
        [int]$ControlPort,
        [hashtable]$Config
    )

    return Send-WcsCommand -HostName $HostName -Port $ControlPort -Payload @{
        cmd = "apply"
        config = $Config
    }
}

function Get-WcsStatus {
    param([string]$HostName, [int]$ControlPort)
    return Send-WcsCommand -HostName $HostName -Port $ControlPort -Payload @{ cmd = "get_status" }
}

function Get-StatValue {
    param($Status, [string]$Name, $Default)
    if ($null -ne $Status -and $null -ne $Status.stats -and $null -ne $Status.stats.$Name) {
        return $Status.stats.$Name
    }
    return $Default
}

function Run-Calibration {
    param(
        [string]$HostName,
        [int]$ControlPort,
        [int]$StartMbps,
        [int]$MaxMbps,
        [int]$StepMbps,
        [double]$TxLimitMs,
        [int]$DropLimit,
        [int]$SettleSeconds,
        [scriptblock]$Log
    )

    if ($StartMbps -lt 2) { $StartMbps = 2 }
    if ($MaxMbps -lt $StartMbps) { $MaxMbps = $StartMbps }
    if ($StepMbps -lt 1) { $StepMbps = 1 }
    if ($SettleSeconds -lt 1) { $SettleSeconds = 1 }

    & $Log "Calibration start: ${StartMbps}M -> ${MaxMbps}M, step ${StepMbps}M, tx<=${TxLimitMs}ms, drop<=${DropLimit}"

    $best = $StartMbps
    $saturated = $false

    $baselineCfg = @{
        auto_bitrate = $false
        min_bitrate  = [int]($StartMbps * 1000000)
        max_bitrate  = [int]($MaxMbps * 1000000)
    }
    [void](Invoke-WcsApply -HostName $HostName -ControlPort $ControlPort -Config $baselineCfg)

    for ($mbps = $StartMbps; $mbps -le $MaxMbps; $mbps += $StepMbps) {
        $cfg = @{
            bitrate      = [int]($mbps * 1000000)
            auto_bitrate = $false
        }
        [void](Invoke-WcsApply -HostName $HostName -ControlPort $ControlPort -Config $cfg)
        Start-Sleep -Seconds $SettleSeconds

        $status = Get-WcsStatus -HostName $HostName -ControlPort $ControlPort
        $drop = [int](Get-StatValue -Status $status -Name "dropped" -Default 0)
        $txMs = [double](Get-StatValue -Status $status -Name "tx_ms" -Default 0.0)
        $fps = [int](Get-StatValue -Status $status -Name "fps" -Default 0)
        $realMbps = [double](Get-StatValue -Status $status -Name "mbps" -Default 0.0)

        & $Log ("Probe {0}M => fps:{1} drop:{2} tx:{3:N2}ms link:{4:N1}Mb/s" -f $mbps, $fps, $drop, $txMs, $realMbps)

        if ($drop -gt $DropLimit -or $txMs -gt $TxLimitMs) {
            $saturated = $true
            break
        }
        $best = $mbps
    }

    $autoMin = [Math]::Max(2, [Math]::Floor($best * 0.6))
    $autoMax = [Math]::Max($best, [Math]::Floor($best * 1.2))
    $finalCfg = @{
        bitrate      = [int]($best * 1000000)
        auto_bitrate = $true
        min_bitrate  = [int]($autoMin * 1000000)
        max_bitrate  = [int]($autoMax * 1000000)
    }
    [void](Invoke-WcsApply -HostName $HostName -ControlPort $ControlPort -Config $finalCfg)

    if ($saturated) {
        & $Log "Saturation detected. Selected stable bitrate: ${best}M (auto range ${autoMin}-${autoMax}M)"
    } else {
        & $Log "No saturation before max. Selected bitrate: ${best}M (auto range ${autoMin}-${autoMax}M)"
    }
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "WinCamStream Windows Client V1"
$form.Size = New-Object System.Drawing.Size(1020, 760)
$form.StartPosition = "CenterScreen"

$font = New-Object System.Drawing.Font("Segoe UI", 9)
$form.Font = $font

$panel = New-Object System.Windows.Forms.Panel
$panel.Dock = "Top"
$panel.Height = 280
$panel.AutoScroll = $true
$form.Controls.Add($panel)

$logBox = New-Object System.Windows.Forms.TextBox
$logBox.Multiline = $true
$logBox.ScrollBars = "Vertical"
$logBox.ReadOnly = $true
$logBox.Dock = "Fill"
$form.Controls.Add($logBox)

function Add-Log {
    param([string]$Text)
    $stamp = (Get-Date).ToString("HH:mm:ss")
    $line = "[$stamp] $Text"
    $logBox.AppendText($line + [Environment]::NewLine)
}

$y = 14
function Add-Label {
    param([string]$Text, [int]$X, [int]$Y, [int]$W = 110)
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $Text
    $l.Location = New-Object System.Drawing.Point($X, $Y)
    $l.Size = New-Object System.Drawing.Size($W, 22)
    $panel.Controls.Add($l)
    return $l
}
function Add-TextBox {
    param([string]$Default, [int]$X, [int]$Y, [int]$W = 80)
    $t = New-Object System.Windows.Forms.TextBox
    $t.Text = $Default
    $t.Location = New-Object System.Drawing.Point($X, $Y)
    $t.Size = New-Object System.Drawing.Size($W, 24)
    $panel.Controls.Add($t)
    return $t
}
function Add-Check {
    param([string]$Text, [bool]$Checked, [int]$X, [int]$Y, [int]$W = 140)
    $c = New-Object System.Windows.Forms.CheckBox
    $c.Text = $Text
    $c.Checked = $Checked
    $c.Location = New-Object System.Drawing.Point($X, $Y)
    $c.Size = New-Object System.Drawing.Size($W, 24)
    $panel.Controls.Add($c)
    return $c
}
function Add-Combo {
    param([string[]]$Items, [string]$Default, [int]$X, [int]$Y, [int]$W = 120)
    $cb = New-Object System.Windows.Forms.ComboBox
    $cb.DropDownStyle = "DropDownList"
    $cb.Location = New-Object System.Drawing.Point($X, $Y)
    $cb.Size = New-Object System.Drawing.Size($W, 24)
    [void]$cb.Items.AddRange($Items)
    $idx = [Array]::IndexOf($Items, $Default)
    if ($idx -ge 0) { $cb.SelectedIndex = $idx } else { $cb.SelectedIndex = 0 }
    $panel.Controls.Add($cb)
    return $cb
}
function Add-Button {
    param([string]$Text, [int]$X, [int]$Y, [int]$W = 95)
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $Text
    $b.Location = New-Object System.Drawing.Point($X, $Y)
    $b.Size = New-Object System.Drawing.Size($W, 28)
    $panel.Controls.Add($b)
    return $b
}

Add-Label "Host" 14 $y 40 | Out-Null
$tbHost = Add-TextBox "127.0.0.1" 58 $y 110
Add-Label "VideoPort" 180 $y 70 | Out-Null
$tbVideoPort = Add-TextBox "5000" 254 $y 65
Add-Label "CtrlPort" 330 $y 60 | Out-Null
$tbCtrlPort = Add-TextBox "5001" 394 $y 65

$y += 36
Add-Label "Resolution" 14 $y 70 | Out-Null
$cbResolution = Add-Combo @("720p","1080p","4k") "1080p" 90 $y 90
Add-Label "FPS" 190 $y 30 | Out-Null
$tbFps = Add-TextBox "120" 224 $y 50
Add-Label "Bitrate(M)" 286 $y 70 | Out-Null
$tbBitrate = Add-TextBox "35" 360 $y 55
Add-Label "Profile" 430 $y 45 | Out-Null
$cbProfile = Add-Combo @("baseline","main","high") "high" 480 $y 80
Add-Label "Entropy" 570 $y 50 | Out-Null
$cbEntropy = Add-Combo @("cavlc","cabac") "cabac" 625 $y 80
Add-Label "Protocol" 715 $y 50 | Out-Null
$cbProtocol = Add-Combo @("annexb","avcc") "annexb" 770 $y 90

$y += 36
$ckIntraOnly = Add-Check "Intra-only (All-I)" $false 14 $y 140
$ckAutoRotate = Add-Check "Auto-rotate" $false 164 $y 100
$ckAutoBitrate = Add-Check "Auto bitrate" $true 274 $y 100
Add-Label "Min(M)" 390 $y 44 | Out-Null
$tbMin = Add-TextBox "6" 438 $y 50
Add-Label "Max(M)" 500 $y 44 | Out-Null
$tbMax = Add-TextBox "120" 548 $y 55
Add-Label "Orientation" 614 $y 64 | Out-Null
$cbOrientation = Add-Combo @("portrait","landscape_right","landscape_left") "portrait" 684 $y 150

$y += 40
Add-Label "Calib start(M)" 14 $y 84 | Out-Null
$tbCalStart = Add-TextBox "12" 102 $y 46
Add-Label "max(M)" 154 $y 45 | Out-Null
$tbCalMax = Add-TextBox "120" 200 $y 50
Add-Label "step(M)" 256 $y 48 | Out-Null
$tbCalStep = Add-TextBox "4" 306 $y 45
Add-Label "tx<=ms" 358 $y 44 | Out-Null
$tbCalTx = Add-TextBox "4.0" 404 $y 50
Add-Label "drop<=" 460 $y 44 | Out-Null
$tbCalDrop = Add-TextBox "1" 504 $y 45
Add-Label "settle(s)" 556 $y 54 | Out-Null
$tbCalSettle = Add-TextBox "2" 612 $y 42

$y += 44
$btnStatus = Add-Button "Status" 14 $y
$btnStart = Add-Button "Start" 114 $y
$btnStop = Add-Button "Stop" 214 $y
$btnRestart = Add-Button "Restart" 314 $y
$btnKeyframe = Add-Button "Keyframe" 414 $y
$btnApply = Add-Button "Apply" 514 $y
$btnCalibrate = Add-Button "Calibrate" 614 $y 110
$btnPreview = Add-Button "Preview ffplay" 734 $y 120

function Get-Endpoint {
    return @{
        host = $tbHost.Text.Trim()
        videoPort = [int]$tbVideoPort.Text
        ctrlPort = [int]$tbCtrlPort.Text
    }
}

function Build-ConfigFromUi {
    return @{
        port         = [int]$tbVideoPort.Text
        resolution   = $cbResolution.SelectedItem.ToString()
        fps          = [double]$tbFps.Text
        bitrate_mbps = [double]$tbBitrate.Text
        intra_only   = $ckIntraOnly.Checked
        protocol     = $cbProtocol.SelectedItem.ToString()
        orientation  = $cbOrientation.SelectedItem.ToString()
        auto_rotate  = $ckAutoRotate.Checked
        profile      = $cbProfile.SelectedItem.ToString()
        entropy      = $cbEntropy.SelectedItem.ToString()
        auto_bitrate = $ckAutoBitrate.Checked
        min_bitrate  = [double]$tbMin.Text * 1000000
        max_bitrate  = [double]$tbMax.Text * 1000000
    }
}

$btnStatus.Add_Click({
    try {
        $ep = Get-Endpoint
        $status = Get-WcsStatus -HostName $ep.host -ControlPort $ep.ctrlPort
        Add-Log ($status | ConvertTo-Json -Depth 8 -Compress)
    } catch {
        Add-Log ("Status error: " + $_.Exception.Message)
    }
})

$btnStart.Add_Click({
    try {
        $ep = Get-Endpoint
        $r = Send-WcsCommand -HostName $ep.host -Port $ep.ctrlPort -Payload @{ cmd = "start" }
        Add-Log ("Start: " + ($r | ConvertTo-Json -Compress))
    } catch {
        Add-Log ("Start error: " + $_.Exception.Message)
    }
})

$btnStop.Add_Click({
    try {
        $ep = Get-Endpoint
        $r = Send-WcsCommand -HostName $ep.host -Port $ep.ctrlPort -Payload @{ cmd = "stop" }
        Add-Log ("Stop: " + ($r | ConvertTo-Json -Compress))
    } catch {
        Add-Log ("Stop error: " + $_.Exception.Message)
    }
})

$btnRestart.Add_Click({
    try {
        $ep = Get-Endpoint
        $r = Send-WcsCommand -HostName $ep.host -Port $ep.ctrlPort -Payload @{ cmd = "restart" }
        Add-Log ("Restart: " + ($r | ConvertTo-Json -Compress))
    } catch {
        Add-Log ("Restart error: " + $_.Exception.Message)
    }
})

$btnKeyframe.Add_Click({
    try {
        $ep = Get-Endpoint
        $r = Send-WcsCommand -HostName $ep.host -Port $ep.ctrlPort -Payload @{ cmd = "request_keyframe" }
        Add-Log ("Keyframe: " + ($r | ConvertTo-Json -Compress))
    } catch {
        Add-Log ("Keyframe error: " + $_.Exception.Message)
    }
})

$btnApply.Add_Click({
    try {
        $ep = Get-Endpoint
        $cfg = Build-ConfigFromUi
        $r = Invoke-WcsApply -HostName $ep.host -ControlPort $ep.ctrlPort -Config $cfg
        Add-Log ("Apply: " + ($r | ConvertTo-Json -Compress))
    } catch {
        Add-Log ("Apply error: " + $_.Exception.Message)
    }
})

$btnCalibrate.Add_Click({
    try {
        $ep = Get-Endpoint
        Run-Calibration -HostName $ep.host `
            -ControlPort $ep.ctrlPort `
            -StartMbps ([int]$tbCalStart.Text) `
            -MaxMbps ([int]$tbCalMax.Text) `
            -StepMbps ([int]$tbCalStep.Text) `
            -TxLimitMs ([double]$tbCalTx.Text) `
            -DropLimit ([int]$tbCalDrop.Text) `
            -SettleSeconds ([int]$tbCalSettle.Text) `
            -Log ${function:Add-Log}
    } catch {
        Add-Log ("Calibration error: " + $_.Exception.Message)
    }
})

$btnPreview.Add_Click({
    try {
        $ep = Get-Endpoint
        $uri = "tcp://$($ep.host):$($ep.videoPort)?tcp_nodelay=1"
        $args = "-fflags nobuffer -flags low_delay -probesize 2048 -analyzeduration 0 -vsync drop -use_wallclock_as_timestamps 1 -i `"$uri`""
        Start-Process -FilePath "ffplay" -ArgumentList $args
        Add-Log "Preview launched with ffplay on $uri"
    } catch {
        Add-Log ("Preview error (ffplay missing?): " + $_.Exception.Message)
    }
})

Add-Log "WinCamStream client ready."
Add-Log "Tip: first run iProxy forwarding, e.g. iproxy 5000 5000 and iproxy 5001 5001."

[void]$form.ShowDialog()
