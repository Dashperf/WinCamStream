#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

struct Args {
    std::string host = "127.0.0.1";
    int video_port = 5000;
    int control_port = 0;
    std::string cmd;
    std::string config_json;
    bool start_iproxy = false;
    bool preview = false;
    bool start_vcam = false;
    bool wait_after_spawn = false;
    int width = 0;
    int height = 0;
    int fps = 0;
    std::string iproxy_path;
    std::string ffplay_path;
    std::string vcam_path;
    bool help = false;
};

struct ChildProcess {
    PROCESS_INFORMATION pi{};
    std::string name;
};

class WsaContext {
public:
    WsaContext() {
        WSADATA data{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &data);
        ok_ = (rc == 0);
        if (!ok_) {
            std::cerr << "WSAStartup failed: " << rc << "\n";
        }
    }
    ~WsaContext() {
        if (ok_) {
            WSACleanup();
        }
    }
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

void PrintUsage(const char* exe) {
    std::cout
        << "Usage: " << exe << " [options]\n"
        << "  --host <ip>                   default: 127.0.0.1\n"
        << "  --video-port <N>              default: 5000\n"
        << "  --control-port <N>            default: video+1\n"
        << "  --cmd <ping|status|start|stop|restart|keyframe|apply>\n"
        << "  --config-json \"{...}\"         required with --cmd apply\n"
        << "  --start-iproxy                start iproxy tunnels (video+control)\n"
        << "  --preview                     start ffplay preview\n"
        << "  --start-vcam                  start native vcam bridge\n"
        << "  --width <W> --height <H>      vcam output size (default: source size)\n"
        << "  --fps <N>                     vcam pacing fps (default 0 = no cap)\n"
        << "  --iproxy-path <path>\n"
        << "  --ffplay-path <path>\n"
        << "  --vcam-path <path>\n"
        << "  --wait                        wait for ENTER before exit when spawning processes\n"
        << "  --help\n";
}

bool ParseInt(const char* s, int& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (!end || *end != '\0') return false;
    if (v < INT32_MIN || v > INT32_MAX) return false;
    out = static_cast<int>(v);
    return true;
}

bool ParseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        auto require_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (key == "--help" || key == "-h") {
            a.help = true;
            return true;
        } else if (key == "--host") {
            const char* v = require_value("--host");
            if (!v) return false;
            a.host = v;
        } else if (key == "--video-port") {
            const char* v = require_value("--video-port");
            if (!v || !ParseInt(v, a.video_port)) return false;
        } else if (key == "--control-port") {
            const char* v = require_value("--control-port");
            if (!v || !ParseInt(v, a.control_port)) return false;
        } else if (key == "--cmd") {
            const char* v = require_value("--cmd");
            if (!v) return false;
            a.cmd = v;
        } else if (key == "--config-json") {
            const char* v = require_value("--config-json");
            if (!v) return false;
            a.config_json = v;
        } else if (key == "--start-iproxy") {
            a.start_iproxy = true;
        } else if (key == "--preview") {
            a.preview = true;
        } else if (key == "--start-vcam") {
            a.start_vcam = true;
        } else if (key == "--width") {
            const char* v = require_value("--width");
            if (!v || !ParseInt(v, a.width)) return false;
        } else if (key == "--height") {
            const char* v = require_value("--height");
            if (!v || !ParseInt(v, a.height)) return false;
        } else if (key == "--fps") {
            const char* v = require_value("--fps");
            if (!v || !ParseInt(v, a.fps)) return false;
        } else if (key == "--iproxy-path") {
            const char* v = require_value("--iproxy-path");
            if (!v) return false;
            a.iproxy_path = v;
        } else if (key == "--ffplay-path") {
            const char* v = require_value("--ffplay-path");
            if (!v) return false;
            a.ffplay_path = v;
        } else if (key == "--vcam-path") {
            const char* v = require_value("--vcam-path");
            if (!v) return false;
            a.vcam_path = v;
        } else if (key == "--wait") {
            a.wait_after_spawn = true;
        } else {
            std::cerr << "Unknown argument: " << key << "\n";
            return false;
        }
    }

    if (a.control_port == 0) {
        a.control_port = a.video_port + 1;
    }
    if (a.video_port < 1 || a.video_port > 65535 || a.control_port < 1 || a.control_port > 65535) {
        std::cerr << "Invalid ports.\n";
        return false;
    }
    if ((a.width > 0 && a.height == 0) || (a.height > 0 && a.width == 0)) {
        std::cerr << "--width and --height must be set together.\n";
        return false;
    }
    if (a.width < 0 || a.height < 0 || a.width > 3840 || a.height > 2160 || a.fps < 0 || a.fps > 240) {
        std::cerr << "Invalid width/height/fps.\n";
        return false;
    }
    if (a.cmd == "apply" && a.config_json.empty()) {
        std::cerr << "--config-json is required for --cmd apply\n";
        return false;
    }
    return true;
}

std::filesystem::path ExeDir() {
    char path[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
    if (n == 0 || n >= sizeof(path)) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(path).parent_path();
}

bool FileExists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec);
}

std::optional<std::filesystem::path> FindInPath(const std::string& exe_name) {
    const char* env = std::getenv("PATH");
    if (!env) return std::nullopt;

    std::string paths = env;
    size_t start = 0;
    while (start < paths.size()) {
        const size_t end = paths.find(';', start);
        const std::string part = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) {
            std::filesystem::path p = std::filesystem::path(part) / exe_name;
            if (FileExists(p)) {
                return p;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}

std::filesystem::path ResolveExe(const std::string& custom, const std::string& exe_name) {
    if (!custom.empty()) {
        return std::filesystem::path(custom);
    }
    auto local = ExeDir() / exe_name;
    if (FileExists(local)) {
        return local;
    }
    auto from_path = FindInPath(exe_name);
    if (from_path) return *from_path;
    return std::filesystem::path(exe_name);
}

bool LaunchBackgroundProcess(const std::filesystem::path& exe, const std::string& args, ChildProcess& out, const std::string& label) {
    std::string cmd = "\"" + exe.string() + "\"";
    if (!args.empty()) {
        cmd += " " + args;
    }
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessA(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
        nullptr,
        exe.parent_path().string().c_str(),
        &si,
        &pi);
    if (!ok) {
        std::cerr << "Failed to start " << label << ": " << GetLastError() << "\n";
        return false;
    }

    out.pi = pi;
    out.name = label;
    std::cout << label << " started (pid=" << pi.dwProcessId << ")\n";
    return true;
}

std::string BuildControlPayload(const Args& a) {
    if (a.cmd.empty()) return {};
    std::string cmd = a.cmd;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (cmd == "status") cmd = "get_status";
    if (cmd == "keyframe") cmd = "request_keyframe";

    if (cmd == "apply") {
        return std::string("{\"cmd\":\"apply\",\"config\":") + a.config_json + "}";
    }
    return std::string("{\"cmd\":\"") + cmd + "\"}";
}

bool SendControlRequest(const Args& a, const std::string& payload) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string port = std::to_string(a.control_port);
    addrinfo* info = nullptr;
    int rc = getaddrinfo(a.host.c_str(), port.c_str(), &hints, &info);
    if (rc != 0) {
        std::cerr << "getaddrinfo failed: " << rc << "\n";
        return false;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* p = info; p; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        BOOL no_delay = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
        if (connect(sock, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) {
            break;
        }
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(info);

    if (sock == INVALID_SOCKET) {
        std::cerr << "Unable to connect to control " << a.host << ":" << a.control_port << "\n";
        return false;
    }

    std::string req = payload + "\n";
    int sent = send(sock, req.data(), static_cast<int>(req.size()), 0);
    if (sent != static_cast<int>(req.size())) {
        std::cerr << "send failed: " << WSAGetLastError() << "\n";
        closesocket(sock);
        return false;
    }

    std::string buf;
    char chunk[1024];
    std::string response;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        int n = recv(sock, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            break;
        }
        buf.append(chunk, chunk + n);

        size_t pos = 0;
        while (true) {
            size_t nl = buf.find('\n', pos);
            if (nl == std::string::npos) {
                if (pos > 0) {
                    buf.erase(0, pos);
                }
                break;
            }
            std::string line = buf.substr(pos, nl - pos);
            pos = nl + 1;
            if (line.find("\"type\":\"hello\"") != std::string::npos || line.find("\"type\": \"hello\"") != std::string::npos) {
                continue;
            }
            response = line;
            break;
        }
        if (!response.empty()) break;
    }

    closesocket(sock);
    if (response.empty()) {
        std::cerr << "No response from control API.\n";
        return false;
    }
    std::cout << "Control response: " << response << "\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    if (!ParseArgs(argc, argv, a)) {
        PrintUsage(argv[0]);
        return 2;
    }
    if (a.help) {
        PrintUsage(argv[0]);
        return 0;
    }

    WsaContext wsa;
    if (!wsa.ok()) {
        return 1;
    }

    std::vector<ChildProcess> children;

    if (a.start_iproxy) {
        auto iproxy = ResolveExe(a.iproxy_path, "iproxy.exe");
        if (!FileExists(iproxy)) {
            std::cerr << "iproxy.exe not found: " << iproxy.string() << "\n";
            return 1;
        }
        ChildProcess p1{}, p2{};
        const bool ok1 = LaunchBackgroundProcess(iproxy, "-l " + std::to_string(a.video_port) + ":" + std::to_string(a.video_port), p1, "iproxy-video");
        const bool ok2 = LaunchBackgroundProcess(iproxy, "-l " + std::to_string(a.control_port) + ":" + std::to_string(a.control_port), p2, "iproxy-control");
        if (!ok1 || !ok2) {
            return 1;
        }
        children.push_back(p1);
        children.push_back(p2);
    }

    if (!a.cmd.empty()) {
        const std::string payload = BuildControlPayload(a);
        if (payload.empty() || !SendControlRequest(a, payload)) {
            return 1;
        }
    }

    if (a.preview) {
        auto ffplay = ResolveExe(a.ffplay_path, "ffplay.exe");
        if (!FileExists(ffplay)) {
            std::cerr << "ffplay.exe not found: " << ffplay.string() << "\n";
            return 1;
        }
        const std::string uri = "tcp://" + a.host + ":" + std::to_string(a.video_port) + "?tcp_nodelay=1";
        ChildProcess p{};
        const std::string ffplay_args =
            "-f h264 -fflags nobuffer -flags low_delay -framedrop -probesize 2048 -analyzeduration 0 -sync ext -i \"" + uri + "\"";
        if (!LaunchBackgroundProcess(ffplay, ffplay_args, p, "ffplay")) {
            return 1;
        }
        children.push_back(p);
    }

    if (a.start_vcam) {
        auto vcam = ResolveExe(a.vcam_path, "wcs_native_vcam.exe");
        if (!FileExists(vcam)) {
            std::cerr << "wcs_native_vcam.exe not found: " << vcam.string() << "\n";
            return 1;
        }
        const std::string uri = "tcp://" + a.host + ":" + std::to_string(a.video_port) + "?tcp_nodelay=1";
        ChildProcess p{};
        std::string vcam_args = "--url \"" + uri + "\" --cap 0 --resize-mode linear --timeout-ms 0";
        if (a.width > 0 && a.height > 0) {
            vcam_args += " --width " + std::to_string(a.width) + " --height " + std::to_string(a.height);
        }
        if (a.fps > 0) {
            vcam_args += " --fps " + std::to_string(a.fps);
        }
        if (!LaunchBackgroundProcess(vcam, vcam_args, p, "vcam-native")) {
            return 1;
        }
        children.push_back(p);
    }

    if (a.wait_after_spawn && !children.empty()) {
        std::cout << "Press ENTER to exit this launcher (child processes keep running).\n";
        std::string line;
        std::getline(std::cin, line);
    }

    for (auto& c : children) {
        if (c.pi.hThread) CloseHandle(c.pi.hThread);
        if (c.pi.hProcess) CloseHandle(c.pi.hProcess);
    }

    return 0;
}
