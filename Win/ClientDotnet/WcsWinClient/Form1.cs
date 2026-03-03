using System.Diagnostics;
using System.Globalization;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace WcsWinClient;

public partial class Form1 : Form
{
    private readonly WcsControlClient _client = new();

    private TextBox _hostBox = null!;
    private TextBox _videoPortBox = null!;
    private TextBox _controlPortBox = null!;

    private ComboBox _resolutionBox = null!;
    private TextBox _fpsBox = null!;
    private TextBox _bitrateBox = null!;
    private ComboBox _profileBox = null!;
    private ComboBox _entropyBox = null!;
    private ComboBox _protocolBox = null!;
    private ComboBox _orientationBox = null!;

    private CheckBox _intraOnlyCheck = null!;
    private CheckBox _autoRotateCheck = null!;
    private CheckBox _autoBitrateCheck = null!;
    private TextBox _minBitrateBox = null!;
    private TextBox _maxBitrateBox = null!;

    private TextBox _calStartBox = null!;
    private TextBox _calMaxBox = null!;
    private TextBox _calStepBox = null!;
    private TextBox _calTxLimitBox = null!;
    private TextBox _calDropLimitBox = null!;
    private TextBox _calSettleBox = null!;

    private TextBox _statusBox = null!;
    private TextBox _metricsBox = null!;
    private TextBox _logBox = null!;

    private Button _statusButton = null!;
    private Button _startButton = null!;
    private Button _stopButton = null!;
    private Button _restartButton = null!;
    private Button _keyframeButton = null!;
    private Button _applyButton = null!;
    private Button _syncRemoteButton = null!;
    private Button _calibrateButton = null!;
    private Button _iproxyStartButton = null!;
    private Button _iproxyStopButton = null!;
    private Button _previewButton = null!;

    private readonly System.Windows.Forms.Timer _statusTimer;
    private bool _statusRefreshInFlight;
    private bool _updatingUiFromRemote;
    private bool _configDirty;
    private bool _remoteConfigPending;
    private long _lastConfigRevision;
    private CancellationTokenSource? _calibrationCts;
    private Process? _videoIproxyProcess;
    private Process? _controlIproxyProcess;

    public Form1()
    {
        InitializeComponent();

        Text = "WinCamStream Windows Client V1";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(980, 700);
        Size = new Size(1080, 780);

        BuildUi();

        _statusTimer = new System.Windows.Forms.Timer { Interval = 1000 };
        _statusTimer.Tick += async (_, _) => await RefreshStatusAsync(logRawJson: false);
        _statusTimer.Start();

        FormClosed += (_, _) =>
        {
            _statusTimer.Stop();
            _calibrationCts?.Cancel();
            _calibrationCts?.Dispose();
            StopIproxyTunnels(logWhenAlreadyStopped: false);
        };

        Log("Client ready.");
        Log("Tip: use 'Start iProxy' to open USB forwarding from this app.");
        Log("Tip: use 'Preview ffplay' for local low-latency preview.");
    }

    private void BuildUi()
    {
        var container = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 2,
            Padding = new Padding(10),
        };
        container.RowStyles.Add(new RowStyle(SizeType.Absolute, 320));
        container.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        Controls.Add(container);

        var controlsPanel = new Panel
        {
            Dock = DockStyle.Fill,
            BorderStyle = BorderStyle.FixedSingle,
            AutoScroll = true,
        };
        container.Controls.Add(controlsPanel, 0, 0);

        _logBox = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            Font = new Font("Consolas", 9f),
        };
        container.Controls.Add(_logBox, 0, 1);

        int y = 12;

        AddLabel(controlsPanel, "Host", 12, y, 35);
        _hostBox = AddTextBox(controlsPanel, "127.0.0.1", 52, y, 110);
        AddLabel(controlsPanel, "VideoPort", 180, y, 58);
        _videoPortBox = AddTextBox(controlsPanel, "5000", 242, y, 62);
        AddLabel(controlsPanel, "CtrlPort", 318, y, 52);
        _controlPortBox = AddTextBox(controlsPanel, "5001", 374, y, 62);

        y += 34;

        AddLabel(controlsPanel, "Resolution", 12, y, 64);
        _resolutionBox = AddCombo(controlsPanel, new[] { "720p", "1080p", "4k" }, "1080p", 80, y, 86);
        AddLabel(controlsPanel, "FPS", 180, y, 28);
        _fpsBox = AddTextBox(controlsPanel, "120", 212, y, 50);
        AddLabel(controlsPanel, "Bitrate(M)", 278, y, 65);
        _bitrateBox = AddTextBox(controlsPanel, "35", 347, y, 50);
        AddLabel(controlsPanel, "Profile", 416, y, 44);
        _profileBox = AddCombo(controlsPanel, new[] { "baseline", "main", "high" }, "high", 464, y, 80);
        AddLabel(controlsPanel, "Entropy", 558, y, 52);
        _entropyBox = AddCombo(controlsPanel, new[] { "cavlc", "cabac" }, "cabac", 614, y, 80);
        AddLabel(controlsPanel, "Protocol", 708, y, 52);
        _protocolBox = AddCombo(controlsPanel, new[] { "annexb", "avcc" }, "annexb", 764, y, 90);

        y += 34;

        _intraOnlyCheck = AddCheck(controlsPanel, "Intra-only (All-I)", false, 12, y, 130);
        _autoRotateCheck = AddCheck(controlsPanel, "Auto-rotate", false, 150, y, 92);
        _autoBitrateCheck = AddCheck(controlsPanel, "Auto bitrate", true, 248, y, 92);
        AddLabel(controlsPanel, "Min(M)", 354, y, 45);
        _minBitrateBox = AddTextBox(controlsPanel, "6", 404, y, 52);
        AddLabel(controlsPanel, "Max(M)", 470, y, 45);
        _maxBitrateBox = AddTextBox(controlsPanel, "120", 520, y, 52);
        AddLabel(controlsPanel, "Orientation", 586, y, 58);
        _orientationBox = AddCombo(controlsPanel, new[] { "portrait", "landscape_right", "landscape_left" }, "portrait", 648, y, 150);

        y += 34;

        AddLabel(controlsPanel, "Cal start(M)", 12, y, 72);
        _calStartBox = AddTextBox(controlsPanel, "12", 88, y, 52);
        AddLabel(controlsPanel, "max(M)", 150, y, 50);
        _calMaxBox = AddTextBox(controlsPanel, "120", 202, y, 52);
        AddLabel(controlsPanel, "step(M)", 264, y, 50);
        _calStepBox = AddTextBox(controlsPanel, "4", 316, y, 45);
        AddLabel(controlsPanel, "tx<=ms", 374, y, 45);
        _calTxLimitBox = AddTextBox(controlsPanel, "4.0", 422, y, 52);
        AddLabel(controlsPanel, "drop<=", 486, y, 45);
        _calDropLimitBox = AddTextBox(controlsPanel, "1", 534, y, 42);
        AddLabel(controlsPanel, "settle(s)", 586, y, 50);
        _calSettleBox = AddTextBox(controlsPanel, "2", 640, y, 45);

        y += 40;

        _iproxyStartButton = AddButton(controlsPanel, "Start iProxy", 12, y, 104);
        _iproxyStopButton = AddButton(controlsPanel, "Stop iProxy", 122, y, 104);
        _statusButton = AddButton(controlsPanel, "Status", 232, y, 76);
        _startButton = AddButton(controlsPanel, "Start", 314, y, 76);
        _stopButton = AddButton(controlsPanel, "Stop", 396, y, 76);
        _restartButton = AddButton(controlsPanel, "Restart", 478, y, 80);
        _keyframeButton = AddButton(controlsPanel, "Keyframe", 564, y, 82);
        _applyButton = AddButton(controlsPanel, "Apply", 652, y, 78);
        _syncRemoteButton = AddButton(controlsPanel, "Sync remote", 736, y, 96);
        _calibrateButton = AddButton(controlsPanel, "Calibrate", 838, y, 92);
        _previewButton = AddButton(controlsPanel, "Preview ffplay", 936, y, 124);

        y += 38;

        AddLabel(controlsPanel, "Status", 12, y, 40);
        _statusBox = AddTextBox(controlsPanel, "", 56, y, 430);
        _statusBox.ReadOnly = true;

        y += 30;

        AddLabel(controlsPanel, "Metrics", 12, y, 46);
        _metricsBox = AddTextBox(controlsPanel, "", 62, y, 700);
        _metricsBox.ReadOnly = true;

        BindConfigDirtyTracking();
        BindEvents();
        _orientationBox.Enabled = !_autoRotateCheck.Checked;
        UpdateIproxyButtonsState();
        ClearConfigDirty();
    }

    private void BindEvents()
    {
        _iproxyStartButton.Click += (_, _) => StartIproxyTunnels();
        _iproxyStopButton.Click += (_, _) => StopIproxyTunnels(logWhenAlreadyStopped: true);

        _statusButton.Click += async (_, _) => await RefreshStatusAsync(logRawJson: true);

        _startButton.Click += async (_, _) =>
        {
            await SendSimpleCommandAsync("start");
            await RefreshStatusAsync(logRawJson: false);
        };

        _stopButton.Click += async (_, _) =>
        {
            await SendSimpleCommandAsync("stop");
            await RefreshStatusAsync(logRawJson: false);
        };

        _restartButton.Click += async (_, _) =>
        {
            await SendSimpleCommandAsync("restart");
            await RefreshStatusAsync(logRawJson: false);
        };

        _keyframeButton.Click += async (_, _) =>
        {
            await SendSimpleCommandAsync("request_keyframe");
            await RefreshStatusAsync(logRawJson: false);
        };

        _applyButton.Click += async (_, _) =>
        {
            try
            {
                var cfg = BuildConfigFromUi();
                using var doc = await SendControlCommandAsync(new { cmd = "apply", config = cfg }, CancellationToken.None);
                Log("Apply: " + doc.RootElement.GetRawText());
                _lastConfigRevision = Math.Max(_lastConfigRevision, GetLong(doc.RootElement, "config_revision", _lastConfigRevision));
                ClearConfigDirty();
                await RefreshStatusAsync(logRawJson: false);
            }
            catch (Exception ex)
            {
                Log("Apply error: " + ex.Message);
            }
        };

        _syncRemoteButton.Click += async (_, _) =>
        {
            ClearConfigDirty();
            await RefreshStatusAsync(logRawJson: true);
        };

        _autoRotateCheck.CheckedChanged += (_, _) =>
        {
            _orientationBox.Enabled = !_autoRotateCheck.Checked;
        };

        _calibrateButton.Click += async (_, _) => await RunCalibrationAsync();

        _previewButton.Click += async (_, _) =>
        {
            try
            {
                try
                {
                    using var keyframeDoc = await SendControlCommandAsync(new { cmd = "request_keyframe" }, CancellationToken.None);
                    _ = keyframeDoc.RootElement;
                }
                catch (Exception ex)
                {
                    Log("Preview preflight keyframe warning: " + ex.Message);
                }

                var uri = $"tcp://{GetHost()}:{GetVideoPort()}?tcp_nodelay=1";
                var ffplay = ResolveFfplayPath();
                StartFfplayPreview(ffplay, uri);
            }
            catch (Exception ex)
            {
                Log("Preview error: " + ex.Message);
            }
        };
    }

    private async Task SendSimpleCommandAsync(string cmd)
    {
        try
        {
            using var doc = await SendControlCommandAsync(new { cmd }, CancellationToken.None);
            Log($"{cmd}: " + doc.RootElement.GetRawText());
        }
        catch (Exception ex)
        {
            Log($"{cmd} error: {ex.Message}");
        }
    }

    private void BindConfigDirtyTracking()
    {
        void MarkIfUserEdit()
        {
            if (!_updatingUiFromRemote)
            {
                MarkConfigDirty();
            }
        }

        _videoPortBox.TextChanged += (_, _) => MarkIfUserEdit();
        _controlPortBox.TextChanged += (_, _) => MarkIfUserEdit();
        _resolutionBox.SelectedIndexChanged += (_, _) => MarkIfUserEdit();
        _fpsBox.TextChanged += (_, _) => MarkIfUserEdit();
        _bitrateBox.TextChanged += (_, _) => MarkIfUserEdit();
        _profileBox.SelectedIndexChanged += (_, _) => MarkIfUserEdit();
        _entropyBox.SelectedIndexChanged += (_, _) => MarkIfUserEdit();
        _protocolBox.SelectedIndexChanged += (_, _) => MarkIfUserEdit();
        _orientationBox.SelectedIndexChanged += (_, _) => MarkIfUserEdit();
        _intraOnlyCheck.CheckedChanged += (_, _) => MarkIfUserEdit();
        _autoRotateCheck.CheckedChanged += (_, _) => MarkIfUserEdit();
        _autoBitrateCheck.CheckedChanged += (_, _) => MarkIfUserEdit();
        _minBitrateBox.TextChanged += (_, _) => MarkIfUserEdit();
        _maxBitrateBox.TextChanged += (_, _) => MarkIfUserEdit();
    }

    private void MarkConfigDirty()
    {
        _configDirty = true;
        _syncRemoteButton.Enabled = true;
    }

    private void ClearConfigDirty()
    {
        _configDirty = false;
        _remoteConfigPending = false;
        _syncRemoteButton.Enabled = false;
    }

    private void StartIproxyTunnels()
    {
        try
        {
            StopIproxyTunnels(logWhenAlreadyStopped: false);

            int videoPort = GetVideoPort();
            int controlPort = GetControlPort();
            _videoPortBox.Text = videoPort.ToString(CultureInfo.InvariantCulture);
            _controlPortBox.Text = controlPort.ToString(CultureInfo.InvariantCulture);

            var iproxyExe = ResolveIproxyPath();
            _videoIproxyProcess = StartIproxyProcess(iproxyExe, videoPort, videoPort, "video");
            _controlIproxyProcess = StartIproxyProcess(iproxyExe, controlPort, controlPort, "control");

            UpdateIproxyButtonsState();
            Log($"iProxy running from {iproxyExe}.");
        }
        catch (Exception ex)
        {
            StopIproxyTunnels(logWhenAlreadyStopped: false);
            Log("iProxy start error: " + ex.Message);
        }
    }

    private void StopIproxyTunnels(bool logWhenAlreadyStopped)
    {
        bool hadRunningTunnel = IsProcessRunning(_videoIproxyProcess) || IsProcessRunning(_controlIproxyProcess);

        StopIproxyProcess(ref _videoIproxyProcess, "video");
        StopIproxyProcess(ref _controlIproxyProcess, "control");

        UpdateIproxyButtonsState();

        if (!hadRunningTunnel && logWhenAlreadyStopped)
        {
            Log("iProxy not running.");
        }
    }

    private static Process StartIproxyProcess(string iproxyExe, int localPort, int devicePort, string label)
    {
        var info = new ProcessStartInfo
        {
            FileName = iproxyExe,
            Arguments = $"-l {localPort}:{devicePort}",
            WorkingDirectory = Path.GetDirectoryName(iproxyExe) ?? AppContext.BaseDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
        };

        var process = Process.Start(info) ?? throw new InvalidOperationException($"Failed to start iProxy for {label} tunnel.");
        if (process.WaitForExit(300))
        {
            process.Dispose();
            throw new InvalidOperationException($"iProxy exited immediately for {label} tunnel (port {localPort}).");
        }

        return process;
    }

    private void StopIproxyProcess(ref Process? process, string label)
    {
        if (process is null)
        {
            return;
        }

        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
                process.WaitForExit(1500);
            }

            Log($"iProxy {label} tunnel stopped.");
        }
        catch (Exception ex)
        {
            Log($"iProxy {label} stop warning: {ex.Message}");
        }
        finally
        {
            process.Dispose();
            process = null;
        }
    }

    private void UpdateIproxyButtonsState()
    {
        bool running = IsProcessRunning(_videoIproxyProcess) || IsProcessRunning(_controlIproxyProcess);
        _iproxyStartButton.Enabled = !running;
        _iproxyStopButton.Enabled = running;
    }

    private static bool IsProcessRunning(Process? process)
    {
        if (process is null)
        {
            return false;
        }

        try
        {
            return !process.HasExited;
        }
        catch
        {
            return false;
        }
    }

    private async Task RefreshStatusAsync(bool logRawJson)
    {
        if (_statusRefreshInFlight)
        {
            return;
        }

        _statusRefreshInFlight = true;
        try
        {
            using var doc = await SendControlCommandAsync(new { cmd = "get_status" }, CancellationToken.None);
            var root = doc.RootElement;

            if (logRawJson)
            {
                Log("status: " + root.GetRawText());
            }

            _statusBox.Text = GetString(root, "status");
            _metricsBox.Text = GetString(root, "metrics");
            var revision = GetLong(root, "config_revision", _lastConfigRevision);
            if (revision < _lastConfigRevision)
            {
                return;
            }
            _lastConfigRevision = revision;

            if (root.TryGetProperty("config", out var cfg))
            {
                if (!_configDirty)
                {
                    ApplyRemoteConfigToUi(root, cfg);
                    _remoteConfigPending = false;
                    _syncRemoteButton.Enabled = false;
                }
                else
                {
                    if (!_remoteConfigPending)
                    {
                        Log("Remote config changed while local edits are pending. Click 'Sync remote' or 'Apply'.");
                    }
                    _remoteConfigPending = true;
                    _syncRemoteButton.Enabled = true;
                }
            }
        }
        catch (Exception ex)
        {
            Log("Status error: " + ex.Message);
        }
        finally
        {
            _statusRefreshInFlight = false;
        }
    }

    private async Task RunCalibrationAsync()
    {
        if (_calibrationCts != null)
        {
            Log("Calibration already running.");
            return;
        }

        _calibrationCts = new CancellationTokenSource();
        var ct = _calibrationCts.Token;

        SetActionButtonsEnabled(false);
        _calibrateButton.Enabled = false;

        var statusTimerWasEnabled = _statusTimer.Enabled;
        _statusTimer.Stop();

        try
        {
            int startMbps = Math.Max(2, ParseInt(_calStartBox.Text, 12));
            int maxMbps = Math.Max(startMbps, ParseInt(_calMaxBox.Text, 120));
            int stepMbps = Math.Max(1, ParseInt(_calStepBox.Text, 4));
            double txLimitMs = Math.Max(0.1, ParseDouble(_calTxLimitBox.Text, 4.0));
            int dropLimit = Math.Max(0, ParseInt(_calDropLimitBox.Text, 1));
            int settleSeconds = Math.Max(1, ParseInt(_calSettleBox.Text, 2));

            Log($"Calibration start: {startMbps}M -> {maxMbps}M, step {stepMbps}M, tx<={txLimitMs}ms, drop<={dropLimit}");

            int best = startMbps;
            bool saturated = false;

            var preCfg = new Dictionary<string, object>
            {
                ["auto_bitrate"] = false,
                ["min_bitrate"] = startMbps * 1_000_000,
                ["max_bitrate"] = maxMbps * 1_000_000,
            };
            using (var applyDoc = await SendControlCommandAsync(new { cmd = "apply", config = preCfg }, ct))
            {
                Log("Calibration baseline apply: " + applyDoc.RootElement.GetRawText());
            }

            for (int mbps = startMbps; mbps <= maxMbps; mbps += stepMbps)
            {
                ct.ThrowIfCancellationRequested();

                var cfg = new Dictionary<string, object>
                {
                    ["bitrate_mbps"] = mbps,
                    ["auto_bitrate"] = false,
                };

                using (var applyDoc = await SendControlCommandAsync(new { cmd = "apply", config = cfg }, ct))
                {
                    _ = applyDoc.RootElement;
                }

                await Task.Delay(TimeSpan.FromSeconds(settleSeconds), ct);

                using var statusDoc = await SendControlCommandAsync(new { cmd = "get_status" }, ct);
                var statusRoot = statusDoc.RootElement;

                int drop = 0;
                double txMs = 0;
                int fps = 0;
                double realMbps = 0;

                if (statusRoot.TryGetProperty("stats", out var stats))
                {
                    drop = GetInt(stats, "dropped", 0);
                    txMs = GetDouble(stats, "tx_ms", 0);
                    fps = GetInt(stats, "fps", 0);
                    realMbps = GetDouble(stats, "mbps", 0);
                }

                Log($"Probe {mbps}M => fps:{fps} drop:{drop} tx:{txMs:0.00}ms link:{realMbps:0.0}Mb/s");

                if (drop > dropLimit || txMs > txLimitMs)
                {
                    saturated = true;
                    break;
                }

                best = mbps;
            }

            int autoMin = Math.Max(2, (int)Math.Floor(best * 0.6));
            int autoMax = Math.Max(best, (int)Math.Floor(best * 1.2));
            var finalCfg = new Dictionary<string, object>
            {
                ["bitrate_mbps"] = best,
                ["auto_bitrate"] = true,
                ["min_bitrate"] = autoMin * 1_000_000,
                ["max_bitrate"] = autoMax * 1_000_000,
            };

            using (var finalDoc = await SendControlCommandAsync(new { cmd = "apply", config = finalCfg }, ct))
            {
                Log("Calibration final apply: " + finalDoc.RootElement.GetRawText());
            }

            if (saturated)
            {
                Log($"Saturation detected. Selected {best}M (auto {autoMin}-{autoMax}M)");
            }
            else
            {
                Log($"No saturation before max. Selected {best}M (auto {autoMin}-{autoMax}M)");
            }

            await RefreshStatusAsync(logRawJson: false);
        }
        catch (OperationCanceledException)
        {
            Log("Calibration canceled.");
        }
        catch (Exception ex)
        {
            Log("Calibration error: " + ex.Message);
        }
        finally
        {
            _calibrationCts.Dispose();
            _calibrationCts = null;

            if (statusTimerWasEnabled)
            {
                _statusTimer.Start();
            }

            SetActionButtonsEnabled(true);
            _calibrateButton.Enabled = true;
        }
    }

    private async Task<JsonDocument> SendControlCommandAsync(object payload, CancellationToken ct)
    {
        return await _client.SendAsync(GetHost(), GetControlPort(), payload, timeoutMs: 3000, ct);
    }

    private string GetHost() => string.IsNullOrWhiteSpace(_hostBox.Text) ? "127.0.0.1" : _hostBox.Text.Trim();

    private int GetVideoPort() => Math.Clamp(ParseInt(_videoPortBox.Text, 5000), 1024, 65534);

    private int GetControlPort() => Math.Clamp(ParseInt(_controlPortBox.Text, 5001), 1025, 65535);

    private void ApplyRemoteConfigToUi(JsonElement root, JsonElement cfg)
    {
        _updatingUiFromRemote = true;
        try
        {
            int videoPort = GetInt(root, "video_port", GetVideoPort());
            int controlPort = GetInt(root, "control_port", GetControlPort());
            _videoPortBox.Text = Math.Clamp(videoPort, 1024, 65534).ToString(CultureInfo.InvariantCulture);
            _controlPortBox.Text = Math.Clamp(controlPort, 1025, 65535).ToString(CultureInfo.InvariantCulture);

            TrySetComboFromJson(_resolutionBox, cfg, "resolution");
            TrySetComboFromJson(_profileBox, cfg, "profile");
            TrySetComboFromJson(_entropyBox, cfg, "entropy");
            TrySetComboFromJson(_protocolBox, cfg, "protocol");
            TrySetComboFromJson(_orientationBox, cfg, "orientation");

            _fpsBox.Text = GetDouble(cfg, "fps", 120).ToString("0.##", CultureInfo.InvariantCulture);
            _bitrateBox.Text = (GetInt(cfg, "bitrate", 35_000_000) / 1_000_000.0).ToString("0.##", CultureInfo.InvariantCulture);
            _minBitrateBox.Text = (GetInt(cfg, "min_bitrate", 6_000_000) / 1_000_000.0).ToString("0.##", CultureInfo.InvariantCulture);
            _maxBitrateBox.Text = (GetInt(cfg, "max_bitrate", 120_000_000) / 1_000_000.0).ToString("0.##", CultureInfo.InvariantCulture);
            _intraOnlyCheck.Checked = GetBool(cfg, "intra_only", false);
            _autoRotateCheck.Checked = GetBool(cfg, "auto_rotate", false);
            _autoBitrateCheck.Checked = GetBool(cfg, "auto_bitrate", true);
        }
        finally
        {
            _updatingUiFromRemote = false;
        }
    }

    private Dictionary<string, object> BuildConfigFromUi()
    {
        int videoPort = GetVideoPort();
        _videoPortBox.Text = videoPort.ToString(CultureInfo.InvariantCulture);

        int controlPort = GetControlPort();
        if (controlPort != videoPort + 1)
        {
            _controlPortBox.Text = (videoPort + 1).ToString(CultureInfo.InvariantCulture);
        }

        double fps = Math.Clamp(ParseDouble(_fpsBox.Text, 120), 1, 240);
        double bitrateMbps = Math.Max(1, ParseDouble(_bitrateBox.Text, 35));
        double minMbps = Math.Max(1, ParseDouble(_minBitrateBox.Text, 6));
        double maxMbps = Math.Max(minMbps, ParseDouble(_maxBitrateBox.Text, 120));

        _fpsBox.Text = fps.ToString("0.##", CultureInfo.InvariantCulture);
        _bitrateBox.Text = bitrateMbps.ToString("0.##", CultureInfo.InvariantCulture);
        _minBitrateBox.Text = minMbps.ToString("0.##", CultureInfo.InvariantCulture);
        _maxBitrateBox.Text = maxMbps.ToString("0.##", CultureInfo.InvariantCulture);

        if (_profileBox.SelectedItem?.ToString() == "baseline")
        {
            _entropyBox.SelectedItem = "cavlc";
        }

        return new Dictionary<string, object>
        {
            ["port"] = videoPort,
            ["resolution"] = _resolutionBox.SelectedItem?.ToString() ?? "1080p",
            ["fps"] = fps,
            ["bitrate_mbps"] = bitrateMbps,
            ["intra_only"] = _intraOnlyCheck.Checked,
            ["protocol"] = _protocolBox.SelectedItem?.ToString() ?? "annexb",
            ["orientation"] = _orientationBox.SelectedItem?.ToString() ?? "portrait",
            ["auto_rotate"] = _autoRotateCheck.Checked,
            ["profile"] = _profileBox.SelectedItem?.ToString() ?? "high",
            ["entropy"] = _entropyBox.SelectedItem?.ToString() ?? "cabac",
            ["auto_bitrate"] = _autoBitrateCheck.Checked,
            ["min_bitrate"] = (int)Math.Round(minMbps * 1_000_000),
            ["max_bitrate"] = (int)Math.Round(maxMbps * 1_000_000),
        };
    }

    private static Label AddLabel(Control parent, string text, int x, int y, int width)
    {
        var lbl = new Label
        {
            Text = text,
            Location = new Point(x, y + 4),
            Size = new Size(width, 22),
        };
        parent.Controls.Add(lbl);
        return lbl;
    }

    private static TextBox AddTextBox(Control parent, string text, int x, int y, int width)
    {
        var tb = new TextBox
        {
            Text = text,
            Location = new Point(x, y),
            Size = new Size(width, 24),
        };
        parent.Controls.Add(tb);
        return tb;
    }

    private static ComboBox AddCombo(Control parent, string[] items, string defaultValue, int x, int y, int width)
    {
        var cb = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
            Location = new Point(x, y),
            Size = new Size(width, 24),
        };
        cb.Items.AddRange(items);
        cb.SelectedItem = items.Contains(defaultValue) ? defaultValue : items[0];
        parent.Controls.Add(cb);
        return cb;
    }

    private static CheckBox AddCheck(Control parent, string text, bool @checked, int x, int y, int width)
    {
        var cb = new CheckBox
        {
            Text = text,
            Checked = @checked,
            Location = new Point(x, y + 2),
            Size = new Size(width, 24),
        };
        parent.Controls.Add(cb);
        return cb;
    }

    private static Button AddButton(Control parent, string text, int x, int y, int width)
    {
        var btn = new Button
        {
            Text = text,
            Location = new Point(x, y),
            Size = new Size(width, 28),
        };
        parent.Controls.Add(btn);
        return btn;
    }

    private void Log(string text)
    {
        var stamp = DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture);
        _logBox.AppendText($"[{stamp}] {text}{Environment.NewLine}");
    }

    private void SetActionButtonsEnabled(bool enabled)
    {
        _statusButton.Enabled = enabled;
        _startButton.Enabled = enabled;
        _stopButton.Enabled = enabled;
        _restartButton.Enabled = enabled;
        _keyframeButton.Enabled = enabled;
        _applyButton.Enabled = enabled;
        _previewButton.Enabled = enabled;
    }

    private void StartFfplayPreview(string ffplay, string uri)
    {
        var primaryArgs = $"-fflags nobuffer -flags low_delay -framedrop -probesize 32768 -analyzeduration 0 -i \"{uri}\"";
        if (TryStartFfplay(ffplay, primaryArgs, out var primaryError))
        {
            Log($"Preview started with {ffplay} on {uri}");
            return;
        }

        Log("Preview primary args failed: " + primaryError);

        var fallbackArgs = $"-fflags nobuffer -flags low_delay -i \"{uri}\"";
        if (TryStartFfplay(ffplay, fallbackArgs, out var fallbackError))
        {
            Log($"Preview fallback started with {ffplay} on {uri}");
            return;
        }

        throw new InvalidOperationException("ffplay launch failed. " + fallbackError);
    }

    private static bool TryStartFfplay(string ffplay, string args, out string error)
    {
        var info = new ProcessStartInfo
        {
            FileName = ffplay,
            Arguments = args,
            UseShellExecute = false,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            CreateNoWindow = true,
        };

        using var process = Process.Start(info);
        if (process is null)
        {
            error = "Process.Start returned null.";
            return false;
        }

        if (process.WaitForExit(1400))
        {
            var stderr = process.StandardError.ReadToEnd().Trim();
            var stdout = process.StandardOutput.ReadToEnd().Trim();
            var details = string.IsNullOrWhiteSpace(stderr) ? stdout : stderr;
            error = string.IsNullOrWhiteSpace(details)
                ? $"ffplay exited immediately with code {process.ExitCode}."
                : $"ffplay exited immediately with code {process.ExitCode}: {details}";
            return false;
        }

        error = string.Empty;
        return true;
    }

    private static string ResolveFfplayPath() =>
        ResolveBinaryPath("ffplay.exe", Path.Combine("Win", "ffmpeg-master-latest-win64-gpl-shared", "bin"));

    private static string ResolveIproxyPath() =>
        ResolveBinaryPath("iproxy.exe", Path.Combine("Win", "Iproxy"));

    private static string ResolveBinaryPath(string exeName, string repoRelativeFolder)
    {
        var bundled = Path.Combine(AppContext.BaseDirectory, exeName);
        if (File.Exists(bundled))
        {
            return bundled;
        }

        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 12 && dir is not null; i++)
        {
            var candidate = Path.Combine(dir.FullName, repoRelativeFolder, exeName);
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = dir.Parent;
        }

        return TryFindExeInPath(exeName, out var pathFromPath) ? pathFromPath : exeName;
    }

    private static bool TryFindExeInPath(string exeName, out string fullPath)
    {
        var path = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
        foreach (var rawDir in path.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            try
            {
                var candidate = Path.Combine(rawDir, exeName);
                if (File.Exists(candidate))
                {
                    fullPath = candidate;
                    return true;
                }
            }
            catch
            {
                // ignore invalid path entries
            }
        }

        fullPath = string.Empty;
        return false;
    }

    private static void TrySetComboFromJson(ComboBox box, JsonElement obj, string name)
    {
        if (!obj.TryGetProperty(name, out var value) || value.ValueKind != JsonValueKind.String)
        {
            return;
        }

        var text = value.GetString();
        if (string.IsNullOrWhiteSpace(text))
        {
            return;
        }

        foreach (var item in box.Items)
        {
            if (string.Equals(item?.ToString(), text, StringComparison.OrdinalIgnoreCase))
            {
                box.SelectedItem = item;
                return;
            }
        }
    }

    private static string GetString(JsonElement obj, string name)
    {
        if (obj.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String)
        {
            return v.GetString() ?? string.Empty;
        }
        return string.Empty;
    }

    private static int GetInt(JsonElement obj, string name, int fallback)
    {
        if (!obj.TryGetProperty(name, out var v))
        {
            return fallback;
        }

        if (v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var i))
        {
            return i;
        }

        if (v.ValueKind == JsonValueKind.String && int.TryParse(v.GetString(), out var si))
        {
            return si;
        }

        return fallback;
    }

    private static long GetLong(JsonElement obj, string name, long fallback)
    {
        if (!obj.TryGetProperty(name, out var v))
        {
            return fallback;
        }

        if (v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out var i))
        {
            return i;
        }

        if (v.ValueKind == JsonValueKind.String && long.TryParse(v.GetString(), out var si))
        {
            return si;
        }

        return fallback;
    }

    private static double GetDouble(JsonElement obj, string name, double fallback)
    {
        if (!obj.TryGetProperty(name, out var v))
        {
            return fallback;
        }

        if (v.ValueKind == JsonValueKind.Number && v.TryGetDouble(out var d))
        {
            return d;
        }

        if (v.ValueKind == JsonValueKind.String &&
            double.TryParse(v.GetString(), NumberStyles.Float, CultureInfo.InvariantCulture, out var sd))
        {
            return sd;
        }

        return fallback;
    }

    private static bool GetBool(JsonElement obj, string name, bool fallback)
    {
        if (!obj.TryGetProperty(name, out var v))
        {
            return fallback;
        }

        if (v.ValueKind == JsonValueKind.True) return true;
        if (v.ValueKind == JsonValueKind.False) return false;
        if (v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var i)) return i != 0;

        if (v.ValueKind == JsonValueKind.String)
        {
            var s = v.GetString()?.Trim().ToLowerInvariant();
            return s switch
            {
                "1" or "true" or "yes" or "on" => true,
                "0" or "false" or "no" or "off" => false,
                _ => fallback,
            };
        }

        return fallback;
    }

    private static int ParseInt(string text, int fallback)
    {
        return int.TryParse(text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var value)
            ? value
            : fallback;
    }

    private static double ParseDouble(string text, double fallback)
    {
        var trimmed = text.Trim();
        if (double.TryParse(trimmed, NumberStyles.Float, CultureInfo.CurrentCulture, out var localValue))
        {
            return localValue;
        }

        var normalized = trimmed.Replace(',', '.');
        return double.TryParse(normalized, NumberStyles.Float, CultureInfo.InvariantCulture, out var invariantValue)
            ? invariantValue
            : fallback;
    }
}

internal sealed class WcsControlClient
{
    public async Task<JsonDocument> SendAsync(string host, int port, object payload, int timeoutMs, CancellationToken ct)
    {
        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeoutCts.CancelAfter(timeoutMs);

        using var tcp = new TcpClient();
        tcp.NoDelay = true;
        await tcp.ConnectAsync(host, port, timeoutCts.Token);

        await using var stream = tcp.GetStream();
        var json = JsonSerializer.Serialize(payload);
        var request = Encoding.UTF8.GetBytes(json + "\n");

        await stream.WriteAsync(request.AsMemory(0, request.Length), timeoutCts.Token);
        await stream.FlushAsync(timeoutCts.Token);

        JsonDocument? hello = null;
        for (int i = 0; i < 4; i++)
        {
            var line = await ReadLineAsync(stream, timeoutCts.Token);
            if (string.IsNullOrWhiteSpace(line))
            {
                continue;
            }

            JsonDocument doc;
            try
            {
                doc = JsonDocument.Parse(line);
            }
            catch (Exception ex)
            {
                hello?.Dispose();
                throw new InvalidOperationException($"Invalid JSON from control API: {ex.Message}");
            }

            if (IsHelloPayload(doc.RootElement))
            {
                hello?.Dispose();
                hello = doc;
                continue;
            }

            hello?.Dispose();
            return doc;
        }

        hello?.Dispose();
        throw new InvalidOperationException("No command response from control API.");
    }

    private static async Task<string> ReadLineAsync(NetworkStream stream, CancellationToken ct)
    {
        var bytes = new List<byte>(2048);
        var one = new byte[1];

        while (true)
        {
            int read = await stream.ReadAsync(one.AsMemory(0, 1), ct);
            if (read == 0)
            {
                break;
            }

            if (one[0] == (byte)'\n')
            {
                break;
            }

            if (one[0] != (byte)'\r')
            {
                bytes.Add(one[0]);
            }

            if (bytes.Count > 64 * 1024)
            {
                throw new InvalidOperationException("Control response line too large.");
            }
        }

        return Encoding.UTF8.GetString(bytes.ToArray());
    }

    private static bool IsHelloPayload(JsonElement root)
    {
        if (!root.TryGetProperty("type", out var t) || t.ValueKind != JsonValueKind.String)
        {
            return false;
        }

        return string.Equals(t.GetString(), "hello", StringComparison.OrdinalIgnoreCase);
    }
}

