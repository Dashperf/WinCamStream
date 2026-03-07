#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop.store(true);
        return TRUE;
    default:
        return FALSE;
    }
}

std::string AvErrToString(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

struct Args {
    std::string url = "tcp://127.0.0.1:5000?tcp_nodelay=1";
    int cap_num = 0;
    int width = 0;
    int height = 0;
    int fps = 0;
    int reconnect_ms = 300;
    int timeout_ms = 0;
    bool resize_linear = true;
};

constexpr int kUnityReceivePollMs = 200;
constexpr int kTimeoutHoldLastFrameMs = 60'000;

int EffectiveUnityTimeoutMs(int requested_timeout_ms) {
    // UnityCapture treats timeout as "stale frame threshold".
    // timeout=0 would trigger its "sending stopped" pattern almost immediately.
    if (requested_timeout_ms <= 0) {
        return kTimeoutHoldLastFrameMs;
    }
    return (requested_timeout_ms > kUnityReceivePollMs) ? requested_timeout_ms : kUnityReceivePollMs;
}

void PrintUsage(const char* exe) {
    std::cout
        << "Usage: " << exe << " [options]\n"
        << "  --url <tcp_url>           default: tcp://127.0.0.1:5000?tcp_nodelay=1\n"
        << "  --cap <N>                 UnityCapture device index (default: 0)\n"
        << "  --width <W>               output width (default: source width)\n"
        << "  --height <H>              output height (default: source height)\n"
        << "  --fps <N>                 output frame pacing (0 = no pacing)\n"
        << "  --resize-mode <linear|disabled>  default: linear\n"
        << "  --timeout-ms <N>          stale-frame threshold (0 = hold last frame)\n"
        << "  --reconnect-ms <N>        reconnect delay in ms (default: 300)\n"
        << "  --help                    show this help\n";
}

bool ParseInt(const char* s, int& out) {
    if (s == nullptr || *s == '\0') return false;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == nullptr || *end != '\0') return false;
    if (v < INT32_MIN || v > INT32_MAX) return false;
    out = static_cast<int>(v);
    return true;
}

bool ParseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto require_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--help" || a == "-h") {
            PrintUsage(argv[0]);
            return false;
        }
        if (a == "--url") {
            const char* v = require_value("--url");
            if (!v) return false;
            args.url = v;
            continue;
        }
        if (a == "--cap") {
            const char* v = require_value("--cap");
            if (!v || !ParseInt(v, args.cap_num)) {
                std::cerr << "Invalid --cap value\n";
                return false;
            }
            continue;
        }
        if (a == "--width") {
            const char* v = require_value("--width");
            if (!v || !ParseInt(v, args.width) || args.width < 0) {
                std::cerr << "Invalid --width value\n";
                return false;
            }
            continue;
        }
        if (a == "--height") {
            const char* v = require_value("--height");
            if (!v || !ParseInt(v, args.height) || args.height < 0) {
                std::cerr << "Invalid --height value\n";
                return false;
            }
            continue;
        }
        if (a == "--fps") {
            const char* v = require_value("--fps");
            if (!v || !ParseInt(v, args.fps) || args.fps < 0) {
                std::cerr << "Invalid --fps value\n";
                return false;
            }
            continue;
        }
        if (a == "--reconnect-ms") {
            const char* v = require_value("--reconnect-ms");
            if (!v || !ParseInt(v, args.reconnect_ms) || args.reconnect_ms < 10) {
                std::cerr << "Invalid --reconnect-ms value\n";
                return false;
            }
            continue;
        }
        if (a == "--timeout-ms") {
            const char* v = require_value("--timeout-ms");
            if (!v || !ParseInt(v, args.timeout_ms) || args.timeout_ms < 0 || args.timeout_ms > 60000) {
                std::cerr << "Invalid --timeout-ms value\n";
                return false;
            }
            continue;
        }
        if (a == "--resize-mode") {
            const char* v = require_value("--resize-mode");
            if (!v) return false;
            std::string mode = v;
            std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (mode == "linear") {
                args.resize_linear = true;
            } else if (mode == "disabled") {
                args.resize_linear = false;
            } else {
                std::cerr << "Invalid --resize-mode value (use linear|disabled)\n";
                return false;
            }
            continue;
        }

        std::cerr << "Unknown argument: " << a << "\n";
        return false;
    }

    if ((args.width > 0 && args.height == 0) || (args.height > 0 && args.width == 0)) {
        std::cerr << "--width and --height must be set together.\n";
        return false;
    }
    return true;
}

// Based on UnityCapture shared memory protocol (zlib license project).
struct SharedImageMemory {
    enum { MAX_CAPNUM = ('z' - '0') };
    enum { MAX_SHARED_IMAGE_SIZE = (3840 * 2160 * 4 * static_cast<int>(sizeof(short))) };
    enum EFormat { FORMAT_UINT8, FORMAT_FP16_GAMMA, FORMAT_FP16_LINEAR };
    enum EResizeMode { RESIZEMODE_DISABLED = 0, RESIZEMODE_LINEAR = 1 };
    enum EMirrorMode { MIRRORMODE_DISABLED = 0, MIRRORMODE_HORIZONTALLY = 1 };
    enum ESendResult { SENDRES_TOOLARGE, SENDRES_WARN_FRAMESKIP, SENDRES_OK };

    explicit SharedImageMemory(int32_t cap_num) : cap_num_(cap_num) {}
    ~SharedImageMemory() {
        if (mapped_) UnmapViewOfFile(mapped_);
        if (h_mutex_) CloseHandle(h_mutex_);
        if (h_want_event_) CloseHandle(h_want_event_);
        if (h_sent_event_) CloseHandle(h_sent_event_);
        if (h_shared_file_) CloseHandle(h_shared_file_);
    }

    bool SendIsReady() { return OpenSend(); }

    ESendResult Send(int width, int height, int stride, DWORD data_size, EFormat format, EResizeMode resize_mode, EMirrorMode mirror_mode, int timeout, const uint8_t* buffer) {
        if (!buffer || !OpenSend()) {
            return SENDRES_WARN_FRAMESKIP;
        }
        if (mapped_->max_size < data_size) {
            return SENDRES_TOOLARGE;
        }

        WaitForSingleObject(h_mutex_, INFINITE);
        mapped_->width = width;
        mapped_->height = height;
        mapped_->stride = stride;
        mapped_->format = format;
        mapped_->resize_mode = resize_mode;
        mapped_->mirror_mode = mirror_mode;
        mapped_->timeout = timeout;
        std::memcpy(mapped_->data, buffer, data_size);
        ReleaseMutex(h_mutex_);

        SetEvent(h_sent_event_);
        const bool skipped = (WaitForSingleObject(h_want_event_, 0) != WAIT_OBJECT_0);
        return skipped ? SENDRES_WARN_FRAMESKIP : SENDRES_OK;
    }

private:
    struct SharedMemHeader {
        DWORD max_size;
        int width;
        int height;
        int stride;
        int format;
        int resize_mode;
        int mirror_mode;
        int timeout;
        uint8_t data[1];
    };

    bool OpenSend() {
        if (mapped_) return true;

        if (cap_num_ < 0) cap_num_ = 0;
        if (cap_num_ > MAX_CAPNUM) cap_num_ = MAX_CAPNUM;
        const char cap_char = (cap_num_ ? static_cast<char>('0' + cap_num_) : '\0');

        char name_mutex[] = "UnityCapture_Mutx0";
        char name_event_want[] = "UnityCapture_Want0";
        char name_event_sent[] = "UnityCapture_Sent0";
        char name_shared[] = "UnityCapture_Data0";
        name_mutex[sizeof(name_mutex) - 2] = cap_char;
        name_event_want[sizeof(name_event_want) - 2] = cap_char;
        name_event_sent[sizeof(name_event_sent) - 2] = cap_char;
        name_shared[sizeof(name_shared) - 2] = cap_char;

        if (!h_mutex_) {
            h_mutex_ = OpenMutexA(SYNCHRONIZE, FALSE, name_mutex);
            if (!h_mutex_) return false;
        }

        WaitForSingleObject(h_mutex_, INFINITE);
        struct UnlockGuard {
            explicit UnlockGuard(HANDLE h) : h_(h) {}
            ~UnlockGuard() { ReleaseMutex(h_); }
            HANDLE h_;
        } guard(h_mutex_);

        if (!h_want_event_) {
            h_want_event_ = CreateEventA(nullptr, FALSE, FALSE, name_event_want);
            if (!h_want_event_) return false;
        }
        if (!h_sent_event_) {
            h_sent_event_ = OpenEventA(EVENT_MODIFY_STATE, FALSE, name_event_sent);
            if (!h_sent_event_) return false;
        }
        if (!h_shared_file_) {
            h_shared_file_ = OpenFileMappingA(FILE_MAP_WRITE, FALSE, name_shared);
            if (!h_shared_file_) return false;
        }

        mapped_ = reinterpret_cast<SharedMemHeader*>(MapViewOfFile(h_shared_file_, FILE_MAP_WRITE, 0, 0, 0));
        return mapped_ != nullptr;
    }

    int32_t cap_num_ = 0;
    HANDLE h_mutex_ = nullptr;
    HANDLE h_want_event_ = nullptr;
    HANDLE h_sent_event_ = nullptr;
    HANDLE h_shared_file_ = nullptr;
    SharedMemHeader* mapped_ = nullptr;
};

struct Decoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* sws = nullptr;
    int video_stream = -1;
    int src_w = 0;
    int src_h = 0;
    AVPixelFormat src_fmt = AV_PIX_FMT_NONE;
    int dst_w = 0;
    int dst_h = 0;
    std::vector<uint8_t> rgba;
    uint8_t* dst_data[4] = {nullptr, nullptr, nullptr, nullptr};
    int dst_linesize[4] = {0, 0, 0, 0};
};

void CloseDecoder(Decoder& d) {
    if (d.sws) sws_freeContext(d.sws);
    if (d.frame) av_frame_free(&d.frame);
    if (d.pkt) av_packet_free(&d.pkt);
    if (d.codec) avcodec_free_context(&d.codec);
    if (d.fmt) avformat_close_input(&d.fmt);
    d = {};
}

bool RebuildScalerIfNeeded(Decoder& d, const AVFrame* frame, const Args& args, std::string& err) {
    const int src_w = frame->width;
    const int src_h = frame->height;
    const AVPixelFormat src_fmt = static_cast<AVPixelFormat>(frame->format);
    const int dst_w = args.width > 0 ? args.width : src_w;
    const int dst_h = args.height > 0 ? args.height : src_h;

    if (d.sws && d.src_w == src_w && d.src_h == src_h && d.src_fmt == src_fmt && d.dst_w == dst_w && d.dst_h == dst_h) {
        return true;
    }

    if (dst_w <= 0 || dst_h <= 0 || dst_w > 3840 || dst_h > 2160) {
        err = "Invalid output size (supported up to 3840x2160).";
        return false;
    }

    if (d.sws) sws_freeContext(d.sws);
    d.sws = sws_getContext(src_w, src_h, src_fmt, dst_w, dst_h, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!d.sws) {
        err = "sws_getContext failed.";
        return false;
    }

    const int buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, dst_w, dst_h, 1);
    if (buffer_size <= 0) {
        err = "av_image_get_buffer_size failed.";
        return false;
    }
    d.rgba.assign(static_cast<size_t>(buffer_size), 0);

    const int fill = av_image_fill_arrays(d.dst_data, d.dst_linesize, d.rgba.data(), AV_PIX_FMT_RGBA, dst_w, dst_h, 1);
    if (fill < 0) {
        err = "av_image_fill_arrays failed: " + AvErrToString(fill);
        return false;
    }

    d.src_w = src_w;
    d.src_h = src_h;
    d.src_fmt = src_fmt;
    d.dst_w = dst_w;
    d.dst_h = dst_h;
    return true;
}

bool OpenDecoder(Decoder& d, const Args& args, std::string& err) {
    const AVInputFormat* input_format = av_find_input_format("h264");
    if (!input_format) {
        err = "av_find_input_format(h264) failed.";
        return false;
    }

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "flags", "low_delay", 0);
    av_dict_set(&opts, "probesize", "2048", 0);
    av_dict_set(&opts, "analyzeduration", "0", 0);
    av_dict_set(&opts, "rw_timeout", "2000000", 0);

    int ret = avformat_open_input(&d.fmt, args.url.c_str(), input_format, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        err = "avformat_open_input failed: " + AvErrToString(ret);
        return false;
    }

    ret = avformat_find_stream_info(d.fmt, nullptr);
    if (ret < 0) {
        err = "avformat_find_stream_info failed: " + AvErrToString(ret);
        return false;
    }

    d.video_stream = av_find_best_stream(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d.video_stream < 0) {
        err = "No video stream found: " + AvErrToString(d.video_stream);
        return false;
    }

    AVStream* stream = d.fmt->streams[d.video_stream];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        err = "No decoder found for codec id " + std::to_string(stream->codecpar->codec_id);
        return false;
    }

    d.codec = avcodec_alloc_context3(codec);
    if (!d.codec) {
        err = "avcodec_alloc_context3 failed.";
        return false;
    }

    ret = avcodec_parameters_to_context(d.codec, stream->codecpar);
    if (ret < 0) {
        err = "avcodec_parameters_to_context failed: " + AvErrToString(ret);
        return false;
    }

    d.codec->thread_count = 1;
    d.codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d.codec->flags2 |= AV_CODEC_FLAG2_FAST;

    ret = avcodec_open2(d.codec, codec, nullptr);
    if (ret < 0) {
        err = "avcodec_open2 failed: " + AvErrToString(ret);
        return false;
    }

    d.pkt = av_packet_alloc();
    d.frame = av_frame_alloc();
    if (!d.pkt || !d.frame) {
        err = "Failed to allocate packet/frame.";
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    Args args;
    if (!ParseArgs(argc, argv, args)) {
        if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
            return 0;
        }
        PrintUsage(argv[0]);
        return 2;
    }

    avformat_network_init();

    std::cout << "wcs_native_vcam starting\n";
    const int effective_timeout_ms = EffectiveUnityTimeoutMs(args.timeout_ms);
    std::cout << "url=" << args.url << " cap=" << args.cap_num;
    if (args.width > 0 && args.height > 0) std::cout << " out=" << args.width << "x" << args.height;
    if (args.fps > 0) std::cout << " fps_cap=" << args.fps;
    std::cout << " resize=" << (args.resize_linear ? "linear" : "disabled");
    std::cout << " timeout_ms=" << args.timeout_ms << " (effective=" << effective_timeout_ms << ")";
    std::cout << "\n";

    SharedImageMemory sender(args.cap_num);
    auto last_not_ready_log = std::chrono::steady_clock::now() - std::chrono::seconds(5);

    while (!g_stop.load()) {
        Decoder dec;
        std::string err;
        if (!OpenDecoder(dec, args, err)) {
            std::cerr << "Decoder open failed: " << err << "\n";
            CloseDecoder(dec);
            std::this_thread::sleep_for(std::chrono::milliseconds(args.reconnect_ms));
            continue;
        }

        std::cout << "Input connected.\n";
        using clock = std::chrono::steady_clock;
        const auto frame_interval = (args.fps > 0) ? std::chrono::microseconds(1'000'000 / args.fps) : std::chrono::microseconds(0);
        auto next_send = clock::now();
        auto stats_at = clock::now() + std::chrono::seconds(1);
        int stats_decoded = 0;
        int stats_sent = 0;
        int stats_warn_skip = 0;
        int stats_drop_fps = 0;

        bool reconnect = false;
        while (!g_stop.load() && !reconnect) {
            const int read = av_read_frame(dec.fmt, dec.pkt);
            if (read < 0) {
                if (read != AVERROR_EOF && read != AVERROR(EAGAIN)) {
                    std::cerr << "av_read_frame: " << AvErrToString(read) << "\n";
                }
                reconnect = true;
                continue;
            }

            if (dec.pkt->stream_index != dec.video_stream) {
                av_packet_unref(dec.pkt);
                continue;
            }

            const int send_pkt = avcodec_send_packet(dec.codec, dec.pkt);
            av_packet_unref(dec.pkt);
            if (send_pkt < 0 && send_pkt != AVERROR(EAGAIN)) {
                std::cerr << "avcodec_send_packet: " << AvErrToString(send_pkt) << "\n";
                reconnect = true;
                continue;
            }

            while (!g_stop.load()) {
                const int recv = avcodec_receive_frame(dec.codec, dec.frame);
                if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) {
                    break;
                }
                if (recv < 0) {
                    std::cerr << "avcodec_receive_frame: " << AvErrToString(recv) << "\n";
                    reconnect = true;
                    break;
                }

                ++stats_decoded;

                if (!RebuildScalerIfNeeded(dec, dec.frame, args, err)) {
                    std::cerr << "Scaler error: " << err << "\n";
                    reconnect = true;
                    break;
                }

                sws_scale(dec.sws, dec.frame->data, dec.frame->linesize, 0, dec.frame->height, dec.dst_data, dec.dst_linesize);

                if (args.fps > 0) {
                    const auto now = clock::now();
                    if (now < next_send) {
                        ++stats_drop_fps;
                        continue;
                    }
                    while (next_send <= now) {
                        next_send += frame_interval;
                    }
                }

                if (!sender.SendIsReady()) {
                    const auto now = clock::now();
                    if (now - last_not_ready_log >= std::chrono::seconds(2)) {
                        last_not_ready_log = now;
                        std::cout << "UnityCapture inactive (open 'Unity Video Capture' in a receiver app).\n";
                    }
                    continue;
                }

                const DWORD data_size = static_cast<DWORD>(dec.dst_linesize[0] * dec.dst_h);
                const auto send_res = sender.Send(
                    dec.dst_w,
                    dec.dst_h,
                    dec.dst_linesize[0] / 4,
                    data_size,
                    SharedImageMemory::FORMAT_UINT8,
                    args.resize_linear ? SharedImageMemory::RESIZEMODE_LINEAR : SharedImageMemory::RESIZEMODE_DISABLED,
                    SharedImageMemory::MIRRORMODE_DISABLED,
                    effective_timeout_ms,
                    dec.rgba.data());

                if (send_res == SharedImageMemory::SENDRES_TOOLARGE) {
                    std::cerr << "UnityCapture send failed: frame too large (max 4K RGBA).\n";
                    reconnect = true;
                    break;
                }

                ++stats_sent;
                if (send_res == SharedImageMemory::SENDRES_WARN_FRAMESKIP) {
                    ++stats_warn_skip;
                }
            }

            const auto now = clock::now();
            if (now >= stats_at) {
                std::cout << "decoded:" << stats_decoded
                          << " sent:" << stats_sent
                          << " warn_skip:" << stats_warn_skip
                          << " drop_fps:" << stats_drop_fps
                          << " out:" << (dec.dst_w > 0 ? dec.dst_w : 0) << "x" << (dec.dst_h > 0 ? dec.dst_h : 0)
                          << "\n";
                stats_decoded = 0;
                stats_sent = 0;
                stats_warn_skip = 0;
                stats_drop_fps = 0;
                stats_at = now + std::chrono::seconds(1);
            }
        }

        CloseDecoder(dec);
        if (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(args.reconnect_ms));
        }
    }

    avformat_network_deinit();
    std::cout << "wcs_native_vcam stopped.\n";
    return 0;
}
