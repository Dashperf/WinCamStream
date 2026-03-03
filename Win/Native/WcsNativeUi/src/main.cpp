#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")

namespace {

enum : int {
    IDC_HOST = 101,
    IDC_VIDEO_PORT = 102,
    IDC_CONTROL_PORT = 103,
    IDC_RESOLUTION = 104,
    IDC_FPS = 105,
    IDC_BITRATE = 106,
    IDC_PROFILE = 107,
    IDC_ENTROPY = 108,
    IDC_PROTOCOL = 109,
    IDC_INTRA_ONLY = 110,
    IDC_AUTO_ROTATE = 111,
    IDC_AUTO_BITRATE = 112,
    IDC_MIN_BITRATE = 113,
    IDC_MAX_BITRATE = 114,
    IDC_ORIENTATION = 115,
    IDC_STATUS_TEXT = 116,
    IDC_METRICS_TEXT = 117,
    IDC_LOG_TEXT = 118,

    IDC_BTN_RESTART_IPROXY = 201,
    IDC_BTN_STATUS = 202,
    IDC_BTN_START = 203,
    IDC_BTN_STOP = 204,
    IDC_BTN_RESTART = 205,
    IDC_BTN_KEYFRAME = 206,
    IDC_BTN_APPLY = 207,
    IDC_BTN_PREVIEW = 208,
    IDC_BTN_START_VCAM = 209,
    IDC_BTN_STOP_VCAM = 210,
    IDC_BTN_INSTALL_VCAM = 211,
};

constexpr UINT_PTR STATUS_TIMER_ID = 1;

struct ManagedProcess {
    PROCESS_INFORMATION pi{};
    bool running = false;
};

struct AppState {
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    bool status_in_flight = false;

    ManagedProcess iproxy_video;
    ManagedProcess iproxy_control;
    ManagedProcess vcam;

    std::filesystem::path exe_dir;

    HWND host = nullptr;
    HWND video_port = nullptr;
    HWND control_port = nullptr;
    HWND resolution = nullptr;
    HWND fps = nullptr;
    HWND bitrate = nullptr;
    HWND profile = nullptr;
    HWND entropy = nullptr;
    HWND protocol = nullptr;
    HWND intra_only = nullptr;
    HWND auto_rotate = nullptr;
    HWND auto_bitrate = nullptr;
    HWND min_bitrate = nullptr;
    HWND max_bitrate = nullptr;
    HWND orientation = nullptr;
    HWND status_text = nullptr;
    HWND metrics_text = nullptr;
    HWND log_text = nullptr;
};

AppState* GetState(HWND hwnd) {
    return reinterpret_cast<AppState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
}

std::string Trim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    return s.substr(i, j - i);
}

std::string GetTextA(HWND h) {
    const int len = GetWindowTextLengthA(h);
    std::string s(static_cast<size_t>(len), '\0');
    if (len > 0) {
        GetWindowTextA(h, s.data(), len + 1);
    }
    return s;
}

void SetTextA(HWND h, const std::string& s) {
    SetWindowTextA(h, s.c_str());
}

int ParseIntOr(const std::string& s, int fallback) {
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') return fallback;
    if (v < INT32_MIN || v > INT32_MAX) return fallback;
    return static_cast<int>(v);
}

double ParseDoubleOr(const std::string& s, double fallback) {
    std::string t = s;
    std::replace(t.begin(), t.end(), ',', '.');
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v)) return fallback;
    return v;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::string JsonNumber(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << v;
    std::string s = oss.str();
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s.empty()) s = "0";
    return s;
}

std::string NowStamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32] = {0};
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void AppendLog(AppState& s, const std::string& text) {
    if (!s.log_text) return;
    const std::string line = "[" + NowStamp() + "] " + text + "\r\n";
    SendMessageA(s.log_text, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageA(s.log_text, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

void CloseProcess(ManagedProcess& p) {
    if (p.running) {
        if (p.pi.hProcess) {
            TerminateProcess(p.pi.hProcess, 0);
            WaitForSingleObject(p.pi.hProcess, 1200);
        }
        p.running = false;
    }
    if (p.pi.hThread) {
        CloseHandle(p.pi.hThread);
        p.pi.hThread = nullptr;
    }
    if (p.pi.hProcess) {
        CloseHandle(p.pi.hProcess);
        p.pi.hProcess = nullptr;
    }
}

std::optional<std::filesystem::path> FindInPath(const std::string& exe_name) {
    const char* env = std::getenv("PATH");
    if (!env) return std::nullopt;
    std::string paths = env;
    size_t start = 0;
    while (start < paths.size()) {
        const size_t end = paths.find(';', start);
        std::string part = paths.substr(start, end == std::string::npos ? std::string::npos : (end - start));
        part = Trim(part);
        if (!part.empty()) {
            std::filesystem::path candidate = std::filesystem::path(part) / exe_name;
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) return candidate;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}

std::filesystem::path ResolveExe(AppState& s, const std::string& exe_name) {
    auto local = s.exe_dir / exe_name;
    std::error_code ec;
    if (std::filesystem::exists(local, ec)) return local;
    auto in_path = FindInPath(exe_name);
    if (in_path) return *in_path;
    return std::filesystem::path(exe_name);
}

bool LaunchProcess(const std::filesystem::path& exe, const std::string& args, bool no_window, ManagedProcess& out, const std::string& label, AppState& state) {
    std::string cmd = "\"" + exe.string() + "\"";
    if (!args.empty()) cmd += " " + args;
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    DWORD flags = CREATE_NEW_PROCESS_GROUP;
    if (no_window) flags |= CREATE_NO_WINDOW;
    BOOL ok = CreateProcessA(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        flags,
        nullptr,
        exe.parent_path().string().c_str(),
        &si,
        &pi
    );
    if (!ok) {
        AppendLog(state, "Failed to start " + label + " (" + std::to_string(GetLastError()) + ")");
        return false;
    }

    CloseProcess(out);
    out.pi = pi;
    out.running = true;
    AppendLog(state, label + " started: " + exe.string());
    return true;
}

bool ReadLineFromSocket(SOCKET sock, std::string& line) {
    line.clear();
    char c = 0;
    while (true) {
        const int n = recv(sock, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') return true;
        if (c != '\r') line.push_back(c);
        if (line.size() > 64 * 1024) return false;
    }
}

bool SendControl(AppState& s, const std::string& payload, std::string& response, std::string& err) {
    const std::string host = Trim(GetTextA(s.host));
    const int control_port = std::clamp(ParseIntOr(Trim(GetTextA(s.control_port)), ParseIntOr(Trim(GetTextA(s.video_port)), 5000) + 1), 1, 65535);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addrs = nullptr;
    const std::string port = std::to_string(control_port);
    int gai = getaddrinfo(host.empty() ? "127.0.0.1" : host.c_str(), port.c_str(), &hints, &addrs);
    if (gai != 0) {
        err = "getaddrinfo failed: " + std::to_string(gai);
        return false;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* p = addrs; p; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        BOOL nodelay = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        int timeout = 3000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        if (connect(sock, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(addrs);

    if (sock == INVALID_SOCKET) {
        err = "Unable to connect to control endpoint.";
        return false;
    }

    std::string req = payload + "\n";
    int sent_total = 0;
    while (sent_total < static_cast<int>(req.size())) {
        int n = send(sock, req.data() + sent_total, static_cast<int>(req.size()) - sent_total, 0);
        if (n <= 0) {
            err = "send failed: " + std::to_string(WSAGetLastError());
            closesocket(sock);
            return false;
        }
        sent_total += n;
    }

    response.clear();
    for (int i = 0; i < 6; ++i) {
        std::string line;
        if (!ReadLineFromSocket(sock, line)) break;
        if (line.find("\"type\":\"hello\"") != std::string::npos || line.find("\"type\": \"hello\"") != std::string::npos) {
            continue;
        }
        response = line;
        break;
    }

    closesocket(sock);
    if (response.empty()) {
        err = "No response from control API.";
        return false;
    }
    return true;
}

std::string ExtractJsonString(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    size_t p = json.find(pattern);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pattern.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;
    std::string out;
    while (p < json.size()) {
        char c = json[p++];
        if (c == '"') break;
        if (c == '\\' && p < json.size()) {
            char e = json[p++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(e); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool IsChecked(HWND h) {
    return SendMessageA(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

std::string ComboText(HWND h) {
    int idx = static_cast<int>(SendMessageA(h, CB_GETCURSEL, 0, 0));
    if (idx < 0) return {};
    char buf[128] = {0};
    SendMessageA(h, CB_GETLBTEXT, idx, reinterpret_cast<LPARAM>(buf));
    return buf;
}

void ComboSelectText(HWND h, const std::string& value) {
    const int count = static_cast<int>(SendMessageA(h, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        char buf[128] = {0};
        SendMessageA(h, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(buf));
        if (_stricmp(buf, value.c_str()) == 0) {
            SendMessageA(h, CB_SETCURSEL, i, 0);
            return;
        }
    }
}

void RefreshStatus(AppState& s, bool log_raw) {
    if (s.status_in_flight) return;
    s.status_in_flight = true;

    std::string resp, err;
    if (!SendControl(s, "{\"cmd\":\"get_status\"}", resp, err)) {
        AppendLog(s, "Status error: " + err);
        s.status_in_flight = false;
        return;
    }

    if (log_raw) AppendLog(s, "status: " + resp);
    const std::string status_text = ExtractJsonString(resp, "status");
    const std::string metrics_text = ExtractJsonString(resp, "metrics");
    if (!status_text.empty()) SetTextA(s.status_text, status_text);
    if (!metrics_text.empty()) SetTextA(s.metrics_text, metrics_text);
    s.status_in_flight = false;
}

void SendSimpleCommand(AppState& s, const std::string& cmd, const std::string& log_prefix) {
    std::string resp, err;
    const std::string payload = "{\"cmd\":\"" + cmd + "\"}";
    if (!SendControl(s, payload, resp, err)) {
        AppendLog(s, log_prefix + " error: " + err);
        return;
    }
    AppendLog(s, log_prefix + ": " + resp);
}

void RestartIproxy(AppState& s) {
    CloseProcess(s.iproxy_video);
    CloseProcess(s.iproxy_control);

    const int video_port = std::clamp(ParseIntOr(Trim(GetTextA(s.video_port)), 5000), 1024, 65534);
    const int control_port = std::clamp(ParseIntOr(Trim(GetTextA(s.control_port)), video_port + 1), 1025, 65535);
    SetTextA(s.video_port, std::to_string(video_port));
    SetTextA(s.control_port, std::to_string(control_port));

    const auto iproxy = ResolveExe(s, "iproxy.exe");
    if (!std::filesystem::exists(iproxy)) {
        AppendLog(s, "iproxy.exe not found: " + iproxy.string());
        return;
    }

    const bool ok1 = LaunchProcess(iproxy, "-l " + std::to_string(video_port) + ":" + std::to_string(video_port), true, s.iproxy_video, "iproxy video", s);
    const bool ok2 = LaunchProcess(iproxy, "-l " + std::to_string(control_port) + ":" + std::to_string(control_port), true, s.iproxy_control, "iproxy control", s);
    if (ok1 && ok2) {
        AppendLog(s, "iProxy tunnels ready.");
    }
}

void ApplyConfig(AppState& s) {
    int video_port = std::clamp(ParseIntOr(Trim(GetTextA(s.video_port)), 5000), 1024, 65534);
    SetTextA(s.video_port, std::to_string(video_port));
    int control_port = std::clamp(ParseIntOr(Trim(GetTextA(s.control_port)), video_port + 1), 1025, 65535);
    if (control_port != video_port + 1) {
        control_port = video_port + 1;
    }
    SetTextA(s.control_port, std::to_string(control_port));

    const double fps = std::clamp(ParseDoubleOr(Trim(GetTextA(s.fps)), 120.0), 1.0, 240.0);
    const double bitrate_m = std::max(1.0, ParseDoubleOr(Trim(GetTextA(s.bitrate)), 35.0));
    const double min_m = std::max(1.0, ParseDoubleOr(Trim(GetTextA(s.min_bitrate)), 6.0));
    const double max_m = std::max(min_m, ParseDoubleOr(Trim(GetTextA(s.max_bitrate)), 120.0));

    SetTextA(s.fps, JsonNumber(fps));
    SetTextA(s.bitrate, JsonNumber(bitrate_m));
    SetTextA(s.min_bitrate, JsonNumber(min_m));
    SetTextA(s.max_bitrate, JsonNumber(max_m));

    std::string profile = ComboText(s.profile);
    std::string entropy = ComboText(s.entropy);
    if (_stricmp(profile.c_str(), "baseline") == 0 && _stricmp(entropy.c_str(), "cavlc") != 0) {
        ComboSelectText(s.entropy, "cavlc");
        entropy = "cavlc";
    }

    const int min_bitrate = static_cast<int>(std::llround(min_m * 1000000.0));
    const int max_bitrate = static_cast<int>(std::llround(max_m * 1000000.0));

    std::string payload = "{\"cmd\":\"apply\",\"config\":{";
    payload += "\"port\":" + std::to_string(video_port);
    payload += ",\"resolution\":\"" + JsonEscape(ComboText(s.resolution)) + "\"";
    payload += ",\"fps\":" + JsonNumber(fps);
    payload += ",\"bitrate_mbps\":" + JsonNumber(bitrate_m);
    payload += ",\"intra_only\":" + std::string(IsChecked(s.intra_only) ? "true" : "false");
    payload += ",\"protocol\":\"" + JsonEscape(ComboText(s.protocol)) + "\"";
    payload += ",\"orientation\":\"" + JsonEscape(ComboText(s.orientation)) + "\"";
    payload += ",\"auto_rotate\":" + std::string(IsChecked(s.auto_rotate) ? "true" : "false");
    payload += ",\"profile\":\"" + JsonEscape(profile) + "\"";
    payload += ",\"entropy\":\"" + JsonEscape(entropy) + "\"";
    payload += ",\"auto_bitrate\":" + std::string(IsChecked(s.auto_bitrate) ? "true" : "false");
    payload += ",\"min_bitrate\":" + std::to_string(min_bitrate);
    payload += ",\"max_bitrate\":" + std::to_string(max_bitrate);
    payload += "}}";

    std::string resp, err;
    if (!SendControl(s, payload, resp, err)) {
        AppendLog(s, "Apply error: " + err);
        return;
    }
    AppendLog(s, "Apply: " + resp);
}

void PreviewFfplay(AppState& s) {
    SendSimpleCommand(s, "request_keyframe", "keyframe");

    const auto ffplay = ResolveExe(s, "ffplay.exe");
    if (!std::filesystem::exists(ffplay)) {
        AppendLog(s, "ffplay.exe not found: " + ffplay.string());
        return;
    }

    const std::string host = Trim(GetTextA(s.host)).empty() ? "127.0.0.1" : Trim(GetTextA(s.host));
    const int video_port = std::clamp(ParseIntOr(Trim(GetTextA(s.video_port)), 5000), 1, 65535);
    const std::string uri = "tcp://" + host + ":" + std::to_string(video_port) + "?tcp_nodelay=1";

    const std::string args = "-f h264 -fflags nobuffer -flags low_delay -framedrop -probesize 2048 -analyzeduration 0 -sync ext -i \"" + uri + "\"";
    ManagedProcess temp{};
    if (LaunchProcess(ffplay, args, false, temp, "ffplay preview", s)) {
        if (temp.pi.hThread) CloseHandle(temp.pi.hThread);
        if (temp.pi.hProcess) CloseHandle(temp.pi.hProcess);
    }
}

void InstallVcamDriver(AppState& s) {
    const std::filesystem::path installer = s.exe_dir / "UnityCapture" / "Install.bat";
    if (!std::filesystem::exists(installer)) {
        AppendLog(s, "UnityCapture installer missing: " + installer.string());
        return;
    }

    const HINSTANCE h = ShellExecuteA(
        s.hwnd,
        "runas",
        installer.string().c_str(),
        nullptr,
        installer.parent_path().string().c_str(),
        SW_SHOWNORMAL
    );
    const intptr_t rc = reinterpret_cast<intptr_t>(h);
    if (rc <= 32) {
        AppendLog(s, "Failed to launch UnityCapture installer (error " + std::to_string(rc) + ")");
        return;
    }
    AppendLog(s, "UnityCapture installer launched (UAC may have appeared).");
}

void StartVcam(AppState& s) {
    CloseProcess(s.vcam);

    const auto exe = ResolveExe(s, "wcs_native_vcam.exe");
    if (!std::filesystem::exists(exe)) {
        AppendLog(s, "wcs_native_vcam.exe not found: " + exe.string());
        return;
    }

    const std::string host = Trim(GetTextA(s.host)).empty() ? "127.0.0.1" : Trim(GetTextA(s.host));
    const int video_port = std::clamp(ParseIntOr(Trim(GetTextA(s.video_port)), 5000), 1, 65535);
    const auto res = ComboText(s.resolution);
    int width = 1920, height = 1080;
    if (_stricmp(res.c_str(), "720p") == 0) { width = 1280; height = 720; }
    else if (_stricmp(res.c_str(), "4k") == 0 || _stricmp(res.c_str(), "2160p") == 0) { width = 3840; height = 2160; }
    const int fps = std::clamp(ParseIntOr(Trim(GetTextA(s.fps)), 60), 1, 240);
    const std::string uri = "tcp://" + host + ":" + std::to_string(video_port) + "?tcp_nodelay=1";
    const std::string args =
        "--url \"" + uri + "\" --width " + std::to_string(width) + " --height " + std::to_string(height) +
        " --fps " + std::to_string(fps) + " --cap 0";
    if (LaunchProcess(exe, args, true, s.vcam, "vcam-native", s)) {
        AppendLog(s, "Select 'Unity Video Capture' in target app.");
    }
}

void StopVcam(AppState& s) {
    if (!s.vcam.running) {
        AppendLog(s, "VCam not running.");
        return;
    }
    CloseProcess(s.vcam);
    AppendLog(s, "VCam stopped.");
}

HWND AddLabel(HWND parent, HFONT font, const char* text, int x, int y, int w) {
    HWND h = CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 20, parent, nullptr, GetModuleHandleA(nullptr), nullptr);
    SendMessageA(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

HWND AddEdit(HWND parent, HFONT font, int id, const char* text, int x, int y, int w, DWORD extra_style = 0) {
    HWND h = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extra_style, x, y, w, 24, parent, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), GetModuleHandleA(nullptr), nullptr);
    SendMessageA(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

HWND AddCheck(HWND parent, HFONT font, int id, const char* text, bool checked, int x, int y, int w) {
    HWND h = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, x, y, w, 24, parent, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), GetModuleHandleA(nullptr), nullptr);
    SendMessageA(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageA(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return h;
}

HWND AddButton(HWND parent, HFONT font, int id, const char* text, int x, int y, int w) {
    HWND h = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, y, w, 28, parent, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), GetModuleHandleA(nullptr), nullptr);
    SendMessageA(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

HWND AddCombo(HWND parent, HFONT font, int id, int x, int y, int w) {
    HWND h = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, x, y, w, 300, parent, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), GetModuleHandleA(nullptr), nullptr);
    SendMessageA(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

void FillCombo(HWND h, const std::vector<std::string>& items, int select_index) {
    for (const auto& i : items) {
        SendMessageA(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i.c_str()));
    }
    SendMessageA(h, CB_SETCURSEL, select_index, 0);
}

void BuildUi(AppState& s) {
    const int y0 = 12;
    int y = y0;

    AddLabel(s.hwnd, s.font, "Host", 12, y + 4, 34);
    s.host = AddEdit(s.hwnd, s.font, IDC_HOST, "127.0.0.1", 48, y, 120);
    AddLabel(s.hwnd, s.font, "VideoPort", 186, y + 4, 58);
    s.video_port = AddEdit(s.hwnd, s.font, IDC_VIDEO_PORT, "5000", 248, y, 62);
    AddLabel(s.hwnd, s.font, "CtrlPort", 326, y + 4, 52);
    s.control_port = AddEdit(s.hwnd, s.font, IDC_CONTROL_PORT, "5001", 382, y, 62);

    y += 34;
    AddLabel(s.hwnd, s.font, "Resolution", 12, y + 4, 64);
    s.resolution = AddCombo(s.hwnd, s.font, IDC_RESOLUTION, 80, y, 86);
    FillCombo(s.resolution, {"720p", "1080p", "4k"}, 1);

    AddLabel(s.hwnd, s.font, "FPS", 180, y + 4, 28);
    s.fps = AddEdit(s.hwnd, s.font, IDC_FPS, "120", 212, y, 50);
    AddLabel(s.hwnd, s.font, "Bitrate(M)", 278, y + 4, 65);
    s.bitrate = AddEdit(s.hwnd, s.font, IDC_BITRATE, "35", 347, y, 50);

    AddLabel(s.hwnd, s.font, "Profile", 416, y + 4, 44);
    s.profile = AddCombo(s.hwnd, s.font, IDC_PROFILE, 464, y, 80);
    FillCombo(s.profile, {"baseline", "main", "high"}, 2);

    AddLabel(s.hwnd, s.font, "Entropy", 558, y + 4, 52);
    s.entropy = AddCombo(s.hwnd, s.font, IDC_ENTROPY, 614, y, 80);
    FillCombo(s.entropy, {"cavlc", "cabac"}, 1);

    AddLabel(s.hwnd, s.font, "Protocol", 708, y + 4, 52);
    s.protocol = AddCombo(s.hwnd, s.font, IDC_PROTOCOL, 764, y, 90);
    FillCombo(s.protocol, {"annexb", "avcc"}, 0);

    y += 34;
    s.intra_only = AddCheck(s.hwnd, s.font, IDC_INTRA_ONLY, "Intra-only (All-I)", false, 12, y, 130);
    s.auto_rotate = AddCheck(s.hwnd, s.font, IDC_AUTO_ROTATE, "Auto-rotate", false, 150, y, 96);
    s.auto_bitrate = AddCheck(s.hwnd, s.font, IDC_AUTO_BITRATE, "Auto bitrate", true, 250, y, 96);
    AddLabel(s.hwnd, s.font, "Min(M)", 354, y + 4, 45);
    s.min_bitrate = AddEdit(s.hwnd, s.font, IDC_MIN_BITRATE, "6", 404, y, 52);
    AddLabel(s.hwnd, s.font, "Max(M)", 470, y + 4, 45);
    s.max_bitrate = AddEdit(s.hwnd, s.font, IDC_MAX_BITRATE, "120", 520, y, 52);
    AddLabel(s.hwnd, s.font, "Orientation", 586, y + 4, 58);
    s.orientation = AddCombo(s.hwnd, s.font, IDC_ORIENTATION, 648, y, 150);
    FillCombo(s.orientation, {"portrait", "landscape_right", "landscape_left"}, 0);

    y += 42;
    AddButton(s.hwnd, s.font, IDC_BTN_RESTART_IPROXY, "Restart iProxy", 12, y, 116);
    AddButton(s.hwnd, s.font, IDC_BTN_STATUS, "Status", 134, y, 76);
    AddButton(s.hwnd, s.font, IDC_BTN_START, "Start", 216, y, 76);
    AddButton(s.hwnd, s.font, IDC_BTN_STOP, "Stop", 298, y, 76);
    AddButton(s.hwnd, s.font, IDC_BTN_RESTART, "Restart", 380, y, 80);
    AddButton(s.hwnd, s.font, IDC_BTN_KEYFRAME, "Keyframe", 466, y, 82);
    AddButton(s.hwnd, s.font, IDC_BTN_APPLY, "Apply", 554, y, 78);
    AddButton(s.hwnd, s.font, IDC_BTN_PREVIEW, "Preview ffplay", 638, y, 124);
    AddButton(s.hwnd, s.font, IDC_BTN_START_VCAM, "Start VCam", 768, y, 100);
    AddButton(s.hwnd, s.font, IDC_BTN_STOP_VCAM, "Stop VCam", 874, y, 100);
    AddButton(s.hwnd, s.font, IDC_BTN_INSTALL_VCAM, "Install VCam", 980, y, 100);

    y += 40;
    AddLabel(s.hwnd, s.font, "Status", 12, y + 4, 40);
    s.status_text = AddEdit(s.hwnd, s.font, IDC_STATUS_TEXT, "", 56, y, 520, ES_READONLY);

    y += 30;
    AddLabel(s.hwnd, s.font, "Metrics", 12, y + 4, 46);
    s.metrics_text = AddEdit(s.hwnd, s.font, IDC_METRICS_TEXT, "", 62, y, 700, ES_READONLY);

    y += 34;
    s.log_text = CreateWindowExA(
        WS_EX_CLIENTEDGE,
        "EDIT",
        "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        12, y, 1070, 340,
        s.hwnd,
        reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_LOG_TEXT)),
        GetModuleHandleA(nullptr),
        nullptr);
    SendMessageA(s.log_text, WM_SETFONT, reinterpret_cast<WPARAM>(s.font), TRUE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* state = GetState(hwnd);
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
        auto* s = new AppState();
        s->hwnd = hwnd;
        s->exe_dir = std::filesystem::path(cs->lpCreateParams ? reinterpret_cast<const char*>(cs->lpCreateParams) : "");
        if (s->exe_dir.empty()) {
            char path[MAX_PATH] = {0};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            s->exe_dir = std::filesystem::path(path).parent_path();
        }
        s->font = CreateFontA(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        BuildUi(*s);
        AppendLog(*s, "Native UI client ready.");
        AppendLog(*s, "Tip: iProxy auto-starts at launch. Use 'Restart iProxy' if needed.");
        AppendLog(*s, "Tip: use 'Start VCam' for UnityCapture virtual camera.");
        RestartIproxy(*s);
        SetTimer(hwnd, STATUS_TIMER_ID, 2000, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (state && wParam == STATUS_TIMER_ID) {
            RefreshStatus(*state, false);
        }
        return 0;
    case WM_COMMAND:
        if (!state) return 0;
        switch (LOWORD(wParam)) {
        case IDC_BTN_RESTART_IPROXY: RestartIproxy(*state); break;
        case IDC_BTN_STATUS: RefreshStatus(*state, true); break;
        case IDC_BTN_START: SendSimpleCommand(*state, "start", "start"); break;
        case IDC_BTN_STOP: SendSimpleCommand(*state, "stop", "stop"); break;
        case IDC_BTN_RESTART: SendSimpleCommand(*state, "restart", "restart"); break;
        case IDC_BTN_KEYFRAME: SendSimpleCommand(*state, "request_keyframe", "keyframe"); break;
        case IDC_BTN_APPLY: ApplyConfig(*state); break;
        case IDC_BTN_PREVIEW: PreviewFfplay(*state); break;
        case IDC_BTN_START_VCAM: StartVcam(*state); break;
        case IDC_BTN_STOP_VCAM: StopVcam(*state); break;
        case IDC_BTN_INSTALL_VCAM: InstallVcamDriver(*state); break;
        case IDC_AUTO_ROTATE:
            if (HIWORD(wParam) == BN_CLICKED) {
                EnableWindow(state->orientation, IsChecked(state->auto_rotate) ? FALSE : TRUE);
            }
            break;
        default:
            break;
        }
        return 0;
    case WM_DESTROY:
        if (state) {
            KillTimer(hwnd, STATUS_TIMER_ID);
            CloseProcess(state->iproxy_video);
            CloseProcess(state->iproxy_control);
            CloseProcess(state->vcam);
            if (state->font) DeleteObject(state->font);
            delete state;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxA(nullptr, "WSAStartup failed.", "WinCamStream Native UI", MB_ICONERROR | MB_OK);
        return 1;
    }

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = "WcsNativeUiWindow";

    if (!RegisterClassExA(&wc)) {
        WSACleanup();
        MessageBoxA(nullptr, "RegisterClassEx failed.", "WinCamStream Native UI", MB_ICONERROR | MB_OK);
        return 1;
    }

    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::filesystem::path exe_dir = std::filesystem::path(exe_path).parent_path();

    HWND hwnd = CreateWindowExA(
        0,
        wc.lpszClassName,
        "WinCamStream Native Client (C++)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1120, 770,
        nullptr,
        nullptr,
        hInstance,
        reinterpret_cast<LPVOID>(const_cast<char*>(exe_dir.string().c_str()))
    );
    if (!hwnd) {
        WSACleanup();
        MessageBoxA(nullptr, "CreateWindowEx failed.", "WinCamStream Native UI", MB_ICONERROR | MB_OK);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    WSACleanup();
    return static_cast<int>(msg.wParam);
}
