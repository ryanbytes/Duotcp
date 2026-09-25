#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <sdrplay_api.h>

#include "rtl_tcp_protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
std::atomic<bool> g_stop{false};

BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

enum class TunerId { A, B };

const char* tuner_name(TunerId id) { return id == TunerId::A ? "A" : "B"; }

std::wstring read_sdrplay_install_dir() {
    constexpr const wchar_t* keys[] = {
        L"SOFTWARE\\SDRplay\\Service\\API",
        L"SOFTWARE\\WOW6432Node\\SDRplay\\Service\\API"
    };

    for (const auto* key_name : keys) {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_name, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            continue;
        }
        std::array<wchar_t, 1024> buf{};
        DWORD type = 0;
        DWORD bytes = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
        const auto rc = RegQueryValueExW(key, L"Install_Dir", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(buf.data()), &bytes);
        RegCloseKey(key);
        if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            return std::wstring(buf.data());
        }
    }
    return {};
}

class SdrplayApi {
public:
    decltype(&sdrplay_api_Open) Open{};
    decltype(&sdrplay_api_Close) Close{};
    decltype(&sdrplay_api_ApiVersion) ApiVersion{};
    decltype(&sdrplay_api_LockDeviceApi) LockDeviceApi{};
    decltype(&sdrplay_api_UnlockDeviceApi) UnlockDeviceApi{};
    decltype(&sdrplay_api_GetDevices) GetDevices{};
    decltype(&sdrplay_api_SelectDevice) SelectDevice{};
    decltype(&sdrplay_api_ReleaseDevice) ReleaseDevice{};
    decltype(&sdrplay_api_GetDeviceParams) GetDeviceParams{};
    decltype(&sdrplay_api_Init) Init{};
    decltype(&sdrplay_api_Uninit) Uninit{};
    decltype(&sdrplay_api_Update) Update{};
    decltype(&sdrplay_api_GetErrorString) GetErrorString{};

    bool load() {
        if (m_module) return true;

        std::vector<std::wstring> candidates;
        if (const wchar_t* env = _wgetenv(L"SDRPLAY_API_DLL")) candidates.emplace_back(env);
        const auto install = read_sdrplay_install_dir();
        if (!install.empty()) {
            candidates.push_back(install + L"\\x64\\sdrplay_api.dll");
            candidates.push_back(install + L"\\API\\x64\\sdrplay_api.dll");
        }
        candidates.emplace_back(L"C:\\Program Files\\SDRplay\\API\\x64\\sdrplay_api.dll");
        candidates.emplace_back(L"sdrplay_api.dll");

        for (const auto& path : candidates) {
            m_module = LoadLibraryW(path.c_str());
            if (m_module) {
                std::wcout << L"Loaded SDRplay API: " << path << L"\n";
                break;
            }
        }
        if (!m_module) {
            std::cerr << "Unable to load sdrplay_api.dll. Install SDRplay API 3.15 or set SDRPLAY_API_DLL.\n";
            return false;
        }

        return bind(Open, "sdrplay_api_Open") && bind(Close, "sdrplay_api_Close") &&
               bind(ApiVersion, "sdrplay_api_ApiVersion") &&
               bind(LockDeviceApi, "sdrplay_api_LockDeviceApi") &&
               bind(UnlockDeviceApi, "sdrplay_api_UnlockDeviceApi") &&
               bind(GetDevices, "sdrplay_api_GetDevices") &&
               bind(SelectDevice, "sdrplay_api_SelectDevice") &&
               bind(ReleaseDevice, "sdrplay_api_ReleaseDevice") &&
               bind(GetDeviceParams, "sdrplay_api_GetDeviceParams") &&
               bind(Init, "sdrplay_api_Init") && bind(Uninit, "sdrplay_api_Uninit") &&
               bind(Update, "sdrplay_api_Update") &&
               bind(GetErrorString, "sdrplay_api_GetErrorString");
    }

    void unload() {
        if (m_module) FreeLibrary(m_module);
        m_module = nullptr;
    }

private:
    template <typename T>
    bool bind(T& fn, const char* name) {
        fn = reinterpret_cast<T>(GetProcAddress(m_module, name));
        if (!fn) std::cerr << "Missing SDRplay API symbol: " << name << "\n";
        return fn != nullptr;
    }

    HMODULE m_module{};
};

class Endpoint {
public:
    using CommandHandler = std::function<void(TunerId, std::uint8_t, std::uint32_t)>;

    Endpoint(TunerId id, std::uint16_t port, CommandHandler handler)
        : m_id(id), m_port(port), m_handler(std::move(handler)) {}

    ~Endpoint() { stop(); }

    bool start() {
        if (m_running.exchange(true)) return true;
        m_thread = std::thread(&Endpoint::accept_loop, this);
        return true;
    }

    void stop() {
        if (!m_running.exchange(false)) return;
        drop_client();
        if (m_listen != INVALID_SOCKET) {
            closesocket(m_listen);
            m_listen = INVALID_SOCKET;
        }
        m_queue_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    void set_hardware_ready(bool ready) {
        m_hardware_ready = ready;
        if (!ready) drop_client();
    }

    void publish(const short* xi, const short* xq, unsigned int count) {
        if (m_client.load() == INVALID_SOCKET || !m_hardware_ready.load()) return;
        std::vector<std::uint8_t> bytes;
        bytes.resize(static_cast<std::size_t>(count) * 2);
        for (unsigned int i = 0; i < count; ++i) {
            bytes[2 * i] = duotcp::sample_to_u8(xi[i]);
            bytes[2 * i + 1] = duotcp::sample_to_u8(xq[i]);
        }
        {
            std::lock_guard lock(m_queue_mutex);
            while (m_queue.size() >= kMaxQueuedChunks) {
                m_queue.pop_front();
                ++m_dropped_chunks;
            }
            m_queue.emplace_back(std::move(bytes));
        }
        m_queue_cv.notify_one();
    }

    void drop_client() {
        const SOCKET s = m_client.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
            shutdown(s, SD_BOTH);
            closesocket(s);
        }
        {
            std::lock_guard lock(m_queue_mutex);
            m_queue.clear();
        }
        m_queue_cv.notify_all();
    }

private:
    static constexpr std::size_t kMaxQueuedChunks = 32;

    static bool send_all(SOCKET s, const std::uint8_t* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            const int n = send(s, reinterpret_cast<const char*>(data + sent),
                               static_cast<int>(std::min<std::size_t>(size - sent, INT_MAX)), 0);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    bool recv_full(SOCKET s, std::uint8_t* data, std::size_t size) {
        std::size_t got = 0;
        while (got < size && m_running && m_hardware_ready && m_client.load() == s) {
            const int n = recv(s, reinterpret_cast<char*>(data + got),
                               static_cast<int>(size - got), 0);
            if (n <= 0) return false;
            got += static_cast<std::size_t>(n);
        }
        return got == size;
    }

    void sender_loop(SOCKET s) {
        while (m_running && m_hardware_ready && m_client.load() == s) {
            std::vector<std::uint8_t> chunk;
            {
                std::unique_lock lock(m_queue_mutex);
                m_queue_cv.wait_for(lock, 500ms, [&] {
                    return !m_queue.empty() || !m_running || !m_hardware_ready || m_client.load() != s;
                });
                if (!m_running || !m_hardware_ready || m_client.load() != s) break;
                if (m_queue.empty()) continue;
                chunk = std::move(m_queue.front());
                m_queue.pop_front();
            }
            if (!send_all(s, chunk.data(), chunk.size())) {
                drop_client();
                break;
            }
        }
    }

    void command_loop(SOCKET s) {
        std::array<std::uint8_t, 5> packet{};
        while (m_running && m_hardware_ready && m_client.load() == s) {
            if (!recv_full(s, packet.data(), packet.size())) break;
            m_handler(m_id, packet[0], duotcp::read_be32(packet.data() + 1));
        }
    }

    void accept_loop() {
        m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listen == INVALID_SOCKET) {
            std::cerr << "Tuner " << tuner_name(m_id) << ": socket() failed\n";
            m_running = false;
            return;
        }

        BOOL reuse = TRUE;
        setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
            listen(m_listen, 4) == SOCKET_ERROR) {
            std::cerr << "Tuner " << tuner_name(m_id) << ": cannot bind 127.0.0.1:" << m_port
                      << " (WSA " << WSAGetLastError() << ")\n";
            closesocket(m_listen);
            m_listen = INVALID_SOCKET;
            m_running = false;
            return;
        }

        std::cout << "Tuner " << tuner_name(m_id) << " listening on 127.0.0.1:" << m_port << "\n";

        while (m_running) {
            if (!m_hardware_ready) {
                std::this_thread::sleep_for(200ms);
                continue;
            }

            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(m_listen, &rfds);
            timeval tv{0, 250000};
            const int rc = select(0, &rfds, nullptr, nullptr, &tv);
            if (!m_running) break;
            if (rc <= 0 || !FD_ISSET(m_listen, &rfds)) continue;

            SOCKET s = accept(m_listen, nullptr, nullptr);
            if (s == INVALID_SOCKET) continue;
            if (!m_hardware_ready) {
                closesocket(s);
                continue;
            }

            BOOL nodelay = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
            int sndbuf = 1 << 20;
            setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

            drop_client();
            m_client = s;
            {
                std::lock_guard lock(m_queue_mutex);
                m_queue.clear();
            }

            const auto hello = duotcp::rtl_tcp_handshake();
            if (!send_all(s, hello.data(), hello.size())) {
                drop_client();
                continue;
            }

            std::cout << "Tuner " << tuner_name(m_id) << ": SDRTrunk connected\n";
            std::thread sender(&Endpoint::sender_loop, this, s);
            command_loop(s);
            drop_client();
            if (sender.joinable()) sender.join();
            std::cout << "Tuner " << tuner_name(m_id) << ": client disconnected";
            if (m_dropped_chunks.exchange(0) > 0) std::cout << " (sample queue overrun occurred)";
            std::cout << "\n";
        }
    }

    TunerId m_id;
    std::uint16_t m_port;
    CommandHandler m_handler;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_hardware_ready{false};
    std::thread m_thread;
    SOCKET m_listen{INVALID_SOCKET};
    std::atomic<SOCKET> m_client{INVALID_SOCKET};
    std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;
    std::deque<std::vector<std::uint8_t>> m_queue;
    std::atomic<std::uint64_t> m_dropped_chunks{0};
};

class Bridge {
public:
    Bridge(std::uint16_t port_a, std::uint16_t port_b, std::string serial)
        : m_serial(std::move(serial)),
          m_a(TunerId::A, port_a, [this](auto t, auto c, auto p) { handle_command(t, c, p); }),
          m_b(TunerId::B, port_b, [this](auto t, auto c, auto p) { handle_command(t, c, p); }) {}

    int run() {
        m_a.start();
        m_b.start();

        while (!g_stop) {
            m_device_lost = false;
            if (!open_hardware()) {
                close_hardware();
                if (!g_stop) {
                    std::cerr << "RSPduo unavailable; retrying in 2 seconds\n";
                    std::this_thread::sleep_for(2s);
                }
                continue;
            }

            while (!g_stop && !m_device_lost) std::this_thread::sleep_for(200ms);
            close_hardware();
            if (!g_stop) {
                std::cerr << "RSPduo lost; waiting for replug/recovery\n";
                std::this_thread::sleep_for(1s);
            }
        }

        close_hardware();
        m_a.stop();
        m_b.stop();
        return 0;
    }

private:
    sdrplay_api_TunerSelectT api_tuner(TunerId id) const {
        return id == TunerId::A ? sdrplay_api_Tuner_A : sdrplay_api_Tuner_B;
    }

    sdrplay_api_RxChannelParamsT* channel(TunerId id) const {
        if (!m_params) return nullptr;
        return id == TunerId::A ? m_params->rxChannelA : m_params->rxChannelB;
    }

    const char* err(sdrplay_api_ErrT e) const {
        return m_api.GetErrorString ? m_api.GetErrorString(e) : "unknown";
    }

    bool open_hardware() {
        if (!m_api.load()) return false;
        auto rc = m_api.Open();
        if (rc != sdrplay_api_Success) {
            std::cerr << "sdrplay_api_Open failed: " << err(rc) << "\n";
            return false;
        }
        m_api_open = true;

        float version = 0.0f;
        rc = m_api.ApiVersion(&version);
        if (rc != sdrplay_api_Success) return fail("ApiVersion", rc);
        std::cout << "SDRplay API version " << version << "\n";

        rc = m_api.LockDeviceApi();
        if (rc != sdrplay_api_Success) return fail("LockDeviceApi", rc);

        std::array<sdrplay_api_DeviceT, SDRPLAY_MAX_DEVICES> devices{};
        unsigned int count = static_cast<unsigned int>(devices.size());
        rc = m_api.GetDevices(devices.data(), &count, static_cast<unsigned int>(devices.size()));
        if (rc != sdrplay_api_Success) {
            m_api.UnlockDeviceApi();
            return fail("GetDevices", rc);
        }

        std::optional<sdrplay_api_DeviceT> found;
        for (unsigned int i = 0; i < count; ++i) {
            auto& d = devices[i];
            if (!d.valid || d.hwVer != SDRPLAY_RSPduo_ID) continue;
            if (!m_serial.empty() && m_serial != d.SerNo) continue;
            if ((d.rspDuoMode & sdrplay_api_RspDuoMode_Dual_Tuner) != sdrplay_api_RspDuoMode_Dual_Tuner) continue;
            if ((d.tuner & sdrplay_api_Tuner_Both) != sdrplay_api_Tuner_Both) continue;
            found = d;
            break;
        }
        if (!found) {
            m_api.UnlockDeviceApi();
            return false;
        }

        m_device = *found;
        m_device.tuner = sdrplay_api_Tuner_Both;
        m_device.rspDuoMode = sdrplay_api_RspDuoMode_Dual_Tuner;
        m_device.rspDuoSampleFreq = duotcp::kAdcSampleRate;

        rc = m_api.SelectDevice(&m_device);
        m_api.UnlockDeviceApi();
        if (rc != sdrplay_api_Success) return fail("SelectDevice", rc);
        m_selected = true;

        rc = m_api.GetDeviceParams(m_device.dev, &m_params);
        if (rc != sdrplay_api_Success || !m_params || !m_params->devParams ||
            !m_params->rxChannelA || !m_params->rxChannelB) {
            if (rc != sdrplay_api_Success) return fail("GetDeviceParams", rc);
            std::cerr << "SDRplay returned incomplete RSPduo parameter blocks\n";
            return false;
        }

        m_params->devParams->fsFreq.fsHz = duotcp::kAdcSampleRate;
        configure_channel(m_params->rxChannelA, m_frequency_a);
        configure_channel(m_params->rxChannelB, m_frequency_b);

        sdrplay_api_CallbackFnsT callbacks{};
        callbacks.StreamACbFn = &Bridge::stream_a;
        callbacks.StreamBCbFn = &Bridge::stream_b;
        callbacks.EventCbFn = &Bridge::event_cb;

        rc = m_api.Init(m_device.dev, &callbacks, this);
        if (rc != sdrplay_api_Success) return fail("Init", rc);
        m_initialized = true;

        if (m_frequency_b != m_frequency_a) {
            m_params->rxChannelB->tunerParams.rfFreq.rfHz = static_cast<double>(m_frequency_b);
            rc = m_api.Update(m_device.dev, sdrplay_api_Tuner_B, sdrplay_api_Update_Tuner_Frf,
                              sdrplay_api_Update_Ext1_None);
            if (rc != sdrplay_api_Success) return fail("Update tuner B frequency", rc);
        }

        m_ready = true;
        m_a.set_hardware_ready(true);
        m_b.set_hardware_ready(true);
        std::cout << "RSPduo " << m_device.SerNo << " ready in dual-tuner mode: "
                  << duotcp::kOutputSampleRate << " samples/s per tuner\n";
        return true;
    }

    void configure_channel(sdrplay_api_RxChannelParamsT* ch, std::uint32_t frequency) {
        ch->tunerParams.rfFreq.rfHz = static_cast<double>(frequency);
        ch->tunerParams.bwType = sdrplay_api_BW_1_536;
        ch->tunerParams.ifType = sdrplay_api_IF_2_048;
        ch->tunerParams.gain.gRdB = 40;
        ch->tunerParams.gain.LNAstate = 3;
        ch->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
        ch->ctrlParams.dcOffset.DCenable = 1;
        ch->ctrlParams.dcOffset.IQenable = 1;
        ch->ctrlParams.decimation.enable = 0;
        ch->ctrlParams.decimation.decimationFactor = 1;
    }

    bool fail(const char* where, sdrplay_api_ErrT rc) {
        std::cerr << where << " failed: " << err(rc) << " (" << static_cast<int>(rc) << ")\n";
        return false;
    }

    void close_hardware() {
        m_ready = false;
        m_a.set_hardware_ready(false);
        m_b.set_hardware_ready(false);

        std::lock_guard lock(m_api_mutex);
        if (m_initialized && m_api.Uninit) m_api.Uninit(m_device.dev);
        m_initialized = false;
        if (m_selected && m_api.ReleaseDevice) m_api.ReleaseDevice(&m_device);
        m_selected = false;
        m_params = nullptr;
        if (m_api_open && m_api.Close) m_api.Close();
        m_api_open = false;
        m_api.unload();
    }

    void handle_command(TunerId id, std::uint8_t command, std::uint32_t param) {
        if (!m_ready) return;
        std::lock_guard lock(m_api_mutex);
        if (!m_initialized || !m_params) return;

        auto* ch = channel(id);
        const auto tuner = api_tuner(id);
        sdrplay_api_ErrT rc = sdrplay_api_Success;

        switch (static_cast<duotcp::RtlCommand>(command)) {
        case duotcp::RtlCommand::SetFrequency:
            if (param < 1'000 || param > 2'000'000'000u) return;
            ch->tunerParams.rfFreq.rfHz = static_cast<double>(param);
            if (id == TunerId::A) m_frequency_a = param; else m_frequency_b = param;
            rc = m_api.Update(m_device.dev, tuner, sdrplay_api_Update_Tuner_Frf,
                              sdrplay_api_Update_Ext1_None);
            break;

        case duotcp::RtlCommand::SetSampleRate:
            if (param != duotcp::kOutputSampleRate) {
                std::cerr << "Tuner " << tuner_name(id) << ": requested sample rate " << param
                          << "; dual mode is fixed at " << duotcp::kOutputSampleRate << "\n";
            }
            return;

        case duotcp::RtlCommand::SetGainMode:
            ch->ctrlParams.agc.enable = (param == 0) ? sdrplay_api_AGC_50HZ : sdrplay_api_AGC_DISABLE;
            rc = m_api.Update(m_device.dev, tuner, sdrplay_api_Update_Ctrl_Agc,
                              sdrplay_api_Update_Ext1_None);
            break;

        case duotcp::RtlCommand::SetGain: {
            const int requested_db = static_cast<int>(param) / 10;
            ch->tunerParams.gain.gRdB = std::clamp(59 - requested_db, 20, 59);
            rc = m_api.Update(m_device.dev, tuner, sdrplay_api_Update_Tuner_Gr,
                              sdrplay_api_Update_Ext1_None);
            break;
        }

        case duotcp::RtlCommand::SetBiasTee:
            if (id == TunerId::B) {
                ch->rspDuoTunerParams.biasTEnable = param ? 1 : 0;
                rc = m_api.Update(m_device.dev, tuner, sdrplay_api_Update_RspDuo_BiasTControl,
                                  sdrplay_api_Update_Ext1_None);
            }
            break;

        case duotcp::RtlCommand::SetFrequencyCorrection:
            return;

        default:
            return;
        }

        if (rc != sdrplay_api_Success) {
            std::cerr << "Tuner " << tuner_name(id) << ": command 0x" << std::hex
                      << static_cast<int>(command) << std::dec << " failed: " << err(rc) << "\n";
        }
    }

    static void stream_a(short* xi, short* xq, sdrplay_api_StreamCbParamsT*,
                         unsigned int num_samples, unsigned int, void* ctx) {
        auto* self = static_cast<Bridge*>(ctx);
        if (self && self->m_ready) self->m_a.publish(xi, xq, num_samples);
    }

    static void stream_b(short* xi, short* xq, sdrplay_api_StreamCbParamsT*,
                         unsigned int num_samples, unsigned int, void* ctx) {
        auto* self = static_cast<Bridge*>(ctx);
        if (self && self->m_ready) self->m_b.publish(xi, xq, num_samples);
    }

    static void event_cb(sdrplay_api_EventT event_id, sdrplay_api_TunerSelectT tuner,
                         sdrplay_api_EventParamsT* params, void* ctx) {
        auto* self = static_cast<Bridge*>(ctx);
        if (!self) return;

        if (event_id == sdrplay_api_PowerOverloadChange && self->m_initialized) {
            self->m_api.Update(self->m_device.dev, tuner, sdrplay_api_Update_Ctrl_OverloadMsgAck,
                               sdrplay_api_Update_Ext1_None);
            return;
        }

        if (event_id == sdrplay_api_DeviceRemoved || event_id == sdrplay_api_DeviceFailure) {
            std::cerr << "SDRplay event: device "
                      << (event_id == sdrplay_api_DeviceRemoved ? "removed" : "failure") << "\n";
            self->m_ready = false;
            self->m_a.set_hardware_ready(false);
            self->m_b.set_hardware_ready(false);
            self->m_device_lost = true;
            return;
        }

        (void)params;
    }

    std::string m_serial;
    SdrplayApi m_api;
    Endpoint m_a;
    Endpoint m_b;
    std::mutex m_api_mutex;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_device_lost{false};
    bool m_api_open{false};
    bool m_selected{false};
    bool m_initialized{false};
    sdrplay_api_DeviceT m_device{};
    sdrplay_api_DeviceParamsT* m_params{};
    std::uint32_t m_frequency_a{100'000'000};
    std::uint32_t m_frequency_b{100'000'000};
};

void usage() {
    std::cout << "DuoTCP - RSPduo dual-tuner rtl_tcp bridge for SDRTrunk\n"
              << "Usage: duotcp.exe [--port-a 1240] [--port-b 1241] [--serial SERIAL]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::uint16_t port_a = duotcp::kDefaultPortA;
    std::uint16_t port_b = duotcp::kDefaultPortB;
    std::string serial;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (++i >= argc) { usage(); std::exit(2); }
            return argv[i];
        };
        if (arg == "--port-a") port_a = static_cast<std::uint16_t>(std::stoul(next()));
        else if (arg == "--port-b") port_b = static_cast<std::uint16_t>(std::stoul(next()));
        else if (arg == "--serial") serial = next();
        else if (arg == "--help" || arg == "-h") { usage(); return 0; }
        else { std::cerr << "Unknown argument: " << arg << "\n"; usage(); return 2; }
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    SetConsoleCtrlHandler(console_handler, TRUE);

    std::cout << "DuoTCP starting. Tuner A -> 127.0.0.1:" << port_a
              << ", Tuner B -> 127.0.0.1:" << port_b << "\n";
    Bridge bridge(port_a, port_b, serial);
    const int rc = bridge.run();
    WSACleanup();
    return rc;
}
