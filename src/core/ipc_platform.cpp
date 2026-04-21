#include <opendriver/core/ipc.h>
#include <opendriver/core/platform.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// ============================================================================
// Platform-specific includes OUTSIDE of any namespace to prevent symbol
// pollution (e.g. ::socket must resolve to the global POSIX function, not
// opendriver::core::socket from a sys/socket.h inside our namespace)
// ============================================================================
#if defined(OD_PLATFORM_WINDOWS)
  // Already included through platform.h (windows.h)
#else
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
  #include <poll.h>
  #include <fcntl.h>
  #include <cerrno>
#endif

namespace opendriver::core {

static constexpr size_t MAX_IPC_MSG_SIZE = 4 * 1024 * 1024; // 4 MB

// Wire format: [type: uint32_t][size: uint32_t][data: size bytes]

#if defined(OD_PLATFORM_WINDOWS)
// ============================================================================
// WINDOWS — Named Pipes
// ============================================================================

static bool pipe_read_all(HANDLE h, void* buf, DWORD len) {
    DWORD total = 0;
    while (total < len) {
        DWORD n = 0;
        if (!::ReadFile(h, static_cast<char*>(buf) + total, len - total, &n, nullptr))
            return false;
        if (n == 0) return false;
        total += n;
    }
    return true;
}

static bool pipe_write_all(HANDLE h, const void* buf, DWORD len) {
    DWORD total = 0;
    while (total < len) {
        DWORD n = 0;
        if (!::WriteFile(h, static_cast<const char*>(buf) + total, len - total, &n, nullptr))
            return false;
        if (n == 0) return false;
        total += n;
    }
    return true;
}

// ---- Server ----------------------------------------------------------------

class WinIPCServer : public IIPCServer {
public:
    ~WinIPCServer() { Stop(); }

    bool Start(const std::string& address) override {
        pipe_name_ = address;
        is_running_ = true;
        accept_thread_ = std::thread(&WinIPCServer::AcceptLoop, this);
        return true;
    }

    void Stop() override {
        is_running_ = false;
        HANDLE dummy = ::CreateFileA(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (dummy != INVALID_HANDLE_VALUE) ::CloseHandle(dummy);
        if (accept_thread_.joinable()) accept_thread_.join();

        std::lock_guard<std::mutex> lock(client_mutex_);
        for (auto h : clients_) ::CloseHandle(h);
        clients_.clear();
    }

    void Broadcast(const IPCMessage& msg) override {
        std::lock_guard<std::mutex> lock(client_mutex_);
        uint32_t header[2] = {
            static_cast<uint32_t>(msg.type),
            static_cast<uint32_t>(msg.data.size())
        };
        auto it = clients_.begin();
        while (it != clients_.end()) {
            bool ok = pipe_write_all(*it, header, sizeof(header));
            if (ok && !msg.data.empty())
                ok = pipe_write_all(*it, msg.data.data(), (DWORD)msg.data.size());
            if (!ok) { ::CloseHandle(*it); it = clients_.erase(it); }
            else ++it;
        }
    }

    bool Receive(IPCMessage& msg, int /*timeout_ms*/) override {
        std::lock_guard<std::mutex> lock(client_mutex_);
        for (auto it = clients_.begin(); it != clients_.end(); ) {
            HANDLE h = *it;
            DWORD avail = 0;
            if (!::PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
                ::CloseHandle(h); it = clients_.erase(it); continue;
            }
            if (avail < sizeof(uint32_t) * 2) { ++it; continue; }

            uint32_t header[2] = {};
            if (!pipe_read_all(h, header, sizeof(header))) {
                ::CloseHandle(h); it = clients_.erase(it); continue;
            }
            msg.type = static_cast<IPCMessageType>(header[0]);
            uint32_t size = header[1];
            if (size > MAX_IPC_MSG_SIZE) { ::CloseHandle(h); it = clients_.erase(it); continue; }
            msg.data.clear();
            if (size > 0) {
                msg.data.resize(size);
                if (!pipe_read_all(h, msg.data.data(), size)) {
                    ::CloseHandle(h); it = clients_.erase(it); continue;
                }
            }
            return true;
        }
        return false;
    }

    bool HasClients() const override {
        std::lock_guard<std::mutex> lock(client_mutex_);
        return !clients_.empty();
    }

private:
    void AcceptLoop() {
        while (is_running_) {
            HANDLE pipe = ::CreateNamedPipeA(
                pipe_name_.c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                4 * 1024 * 1024, 4 * 1024 * 1024, 0, nullptr
            );
            if (pipe == INVALID_HANDLE_VALUE) { ::Sleep(50); continue; }
            BOOL ok = ::ConnectNamedPipe(pipe, nullptr)
                ? TRUE : (::GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
            if (ok && is_running_) {
                std::lock_guard<std::mutex> lock(client_mutex_);
                clients_.push_back(pipe);
            } else {
                ::CloseHandle(pipe);
            }
        }
    }

    std::string pipe_name_;
    std::atomic<bool> is_running_{false};
    std::thread accept_thread_;
    std::vector<HANDLE> clients_;
    mutable std::mutex client_mutex_;
};

// ---- Client ----------------------------------------------------------------

class WinIPCClient : public IIPCClient {
public:
    ~WinIPCClient() { Disconnect(); }

    bool Connect(const std::string& address) override {
        for (int i = 0; i < 10; ++i) {
            pipe_ = ::CreateFileA(address.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) return true;
            if (::GetLastError() != ERROR_PIPE_BUSY) break;
            ::WaitNamedPipeA(address.c_str(), 1000);
        }
        pipe_ = INVALID_HANDLE_VALUE;
        return false;
    }

    void Disconnect() override {
        if (pipe_ != INVALID_HANDLE_VALUE) { ::CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; }
    }

    bool Send(const IPCMessage& msg) override {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (pipe_ == INVALID_HANDLE_VALUE) return false;
        uint32_t header[2] = {
            static_cast<uint32_t>(msg.type),
            static_cast<uint32_t>(msg.data.size())
        };
        if (!pipe_write_all(pipe_, header, sizeof(header))) { Disconnect(); return false; }
        if (!msg.data.empty() && !pipe_write_all(pipe_, msg.data.data(), (DWORD)msg.data.size())) {
            Disconnect(); return false;
        }
        return true;
    }

    bool Receive(IPCMessage& msg, int /*timeout_ms*/) override {
        if (pipe_ == INVALID_HANDLE_VALUE) return false;
        DWORD avail = 0;
        if (!::PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) { Disconnect(); return false; }
        if (avail == 0) return false;

        uint32_t header[2] = {};
        if (!pipe_read_all(pipe_, header, sizeof(header))) { Disconnect(); return false; }
        msg.type = static_cast<IPCMessageType>(header[0]);
        uint32_t size = header[1];
        if (size > MAX_IPC_MSG_SIZE) { Disconnect(); return false; }
        msg.data.clear();
        if (size > 0) {
            msg.data.resize(size);
            if (!pipe_read_all(pipe_, msg.data.data(), size)) { Disconnect(); return false; }
        }
        return true;
    }

    bool IsConnected() const override { return pipe_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::mutex send_mutex_;
};

#else
// ============================================================================
// LINUX/macOS — Unix Domain Sockets
// Headers are included above the namespace to keep global POSIX symbols clean
// ============================================================================

static bool read_full(int fd, void* buf, size_t len) {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t rem = len;
    while (rem > 0) {
        ssize_t n = read(fd, ptr, rem);  // plain read, no ::  — we're outside namespace
        if (n <= 0) { if (n == -1 && errno == EINTR) continue; return false; }
        ptr += n; rem -= n;
    }
    return true;
}

static bool write_full(int fd, const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t rem = len;
    while (rem > 0) {
        ssize_t n = write(fd, ptr, rem);
        if (n <= 0) { if (n == -1 && (errno == EINTR || errno == EAGAIN)) continue; return false; }
        ptr += n; rem -= n;
    }
    return true;
}

class LinuxIPCServer : public IIPCServer {
public:
    ~LinuxIPCServer() { Stop(); }

    bool Start(const std::string& address) override {
        server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd_ == -1) return false;
        fcntl(server_fd_, F_SETFL, O_NONBLOCK);

        struct sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, address.c_str(), sizeof(addr.sun_path) - 1);
        unlink(address.c_str());

        if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) == -1 ||
            listen(server_fd_, 5) == -1) {
            close(server_fd_); server_fd_ = -1; return false;
        }
        is_running_ = true;
        accept_thread_ = std::thread(&LinuxIPCServer::AcceptLoop, this);
        return true;
    }

    void Stop() override {
        is_running_ = false;
        if (accept_thread_.joinable()) accept_thread_.join();
        std::lock_guard<std::mutex> lock(client_mutex_);
        for (int fd : clients_) close(fd);
        clients_.clear();
        if (server_fd_ != -1) { close(server_fd_); server_fd_ = -1; }
    }

    void Broadcast(const IPCMessage& msg) override {
        std::lock_guard<std::mutex> lock(client_mutex_);
        uint32_t header[2] = {
            static_cast<uint32_t>(msg.type),
            static_cast<uint32_t>(msg.data.size())
        };
        auto it = clients_.begin();
        while (it != clients_.end()) {
            bool ok = write_full(*it, header, sizeof(header));
            if (ok && !msg.data.empty())
                ok = write_full(*it, msg.data.data(), msg.data.size());
            if (!ok) { close(*it); it = clients_.erase(it); }
            else ++it;
        }
    }

    bool Receive(IPCMessage& msg, int timeout_ms) override {
        std::lock_guard<std::mutex> lock(client_mutex_);
        if (clients_.empty()) return false;

        std::vector<struct pollfd> pfds;
        pfds.reserve(clients_.size());
        for (int fd : clients_) pfds.push_back({fd, POLLIN, 0});

        if (poll(pfds.data(), (nfds_t)pfds.size(), timeout_ms) <= 0) return false;

        for (size_t i = 0; i < pfds.size(); ++i) {
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(pfds[i].fd);
                clients_.erase(std::find(clients_.begin(), clients_.end(), pfds[i].fd));
                continue;
            }
            if (pfds[i].revents & POLLIN) {
                uint32_t header[2] = {};
                if (!read_full(pfds[i].fd, header, sizeof(header))) {
                    close(pfds[i].fd);
                    clients_.erase(std::find(clients_.begin(), clients_.end(), pfds[i].fd));
                    continue;
                }
                msg.type = static_cast<IPCMessageType>(header[0]);
                uint32_t size = header[1];
                if (size > MAX_IPC_MSG_SIZE) {
                    close(pfds[i].fd);
                    clients_.erase(std::find(clients_.begin(), clients_.end(), pfds[i].fd));
                    continue;
                }
                msg.data.clear();
                if (size > 0) {
                    msg.data.resize(size);
                    if (!read_full(pfds[i].fd, msg.data.data(), size)) {
                        close(pfds[i].fd);
                        clients_.erase(std::find(clients_.begin(), clients_.end(), pfds[i].fd));
                        continue;
                    }
                }
                return true;
            }
        }
        return false;
    }

    bool HasClients() const override {
        std::lock_guard<std::mutex> lock(client_mutex_);
        return !clients_.empty();
    }

private:
    void AcceptLoop() {
        while (is_running_) {
            struct sockaddr_un client_addr = {};
            socklen_t len = sizeof(client_addr);
            int cfd = accept(server_fd_, (struct sockaddr*)&client_addr, &len);
            if (cfd != -1) {
                std::lock_guard<std::mutex> lock(client_mutex_);
                clients_.push_back(cfd);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    int server_fd_ = -1;
    std::atomic<bool> is_running_{false};
    std::thread accept_thread_;
    std::vector<int> clients_;
    mutable std::mutex client_mutex_;
};

class LinuxIPCClient : public IIPCClient {
public:
    ~LinuxIPCClient() { Disconnect(); }

    bool Connect(const std::string& address) override {
        client_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client_fd_ == -1) return false;

        struct sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, address.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(client_fd_, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            close(client_fd_); client_fd_ = -1; return false;
        }
        return true;
    }

    void Disconnect() override {
        if (client_fd_ != -1) { close(client_fd_); client_fd_ = -1; }
    }

    bool Send(const IPCMessage& msg) override {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (client_fd_ == -1) return false;
        uint32_t header[2] = {
            static_cast<uint32_t>(msg.type),
            static_cast<uint32_t>(msg.data.size())
        };
        if (!write_full(client_fd_, header, sizeof(header))) { Disconnect(); return false; }
        if (!msg.data.empty() && !write_full(client_fd_, msg.data.data(), msg.data.size())) {
            Disconnect(); return false;
        }
        return true;
    }

    bool Receive(IPCMessage& msg, int timeout_ms) override {
        if (client_fd_ == -1) return false;
        struct pollfd pfd = {client_fd_, POLLIN, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) return false;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { Disconnect(); return false; }

        uint32_t header[2] = {};
        if (!read_full(client_fd_, header, sizeof(header))) { Disconnect(); return false; }
        msg.type = static_cast<IPCMessageType>(header[0]);
        uint32_t size = header[1];
        if (size > MAX_IPC_MSG_SIZE) { Disconnect(); return false; }
        msg.data.clear();
        if (size > 0) {
            msg.data.resize(size);
            if (!read_full(client_fd_, msg.data.data(), size)) { Disconnect(); return false; }
        }
        return true;
    }

    bool IsConnected() const override { return client_fd_ != -1; }

private:
    int client_fd_ = -1;
    std::mutex send_mutex_;
};

#endif // OD_PLATFORM_WINDOWS / else

// ============================================================================
// FACTORY
// ============================================================================

std::unique_ptr<IIPCServer> CreateIPCServer() {
#if defined(OD_PLATFORM_WINDOWS)
    return std::make_unique<WinIPCServer>();
#else
    return std::make_unique<LinuxIPCServer>();
#endif
}

std::unique_ptr<IIPCClient> CreateIPCClient() {
#if defined(OD_PLATFORM_WINDOWS)
    return std::make_unique<WinIPCClient>();
#else
    return std::make_unique<LinuxIPCClient>();
#endif
}

} // namespace opendriver::core
