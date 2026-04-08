#include <opendriver/core/ipc.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <algorithm>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>
#include <iostream>
#include <errno.h>

namespace opendriver::core {

// ============================================================================
// HELPERS — partial read/write safety for large H264 frames
// ============================================================================

static constexpr size_t MAX_IPC_MSG_SIZE = 4 * 1024 * 1024; // 4 MB max

/// Reads exactly `len` bytes from fd; returns true on success.
static bool read_full(int fd, void* buf, size_t len) {
    uint8_t* ptr = reinterpret_cast<uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = read(fd, ptr, remaining);
        if (n <= 0) {
            if (n == -1 && (errno == EINTR)) continue;
            return false; // EOF or error
        }
        ptr += n;
        remaining -= n;
    }
    return true;
}

/// Writes exactly `len` bytes to fd; returns true on success.
static bool write_full(int fd, const void* buf, size_t len) {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, ptr, remaining);
        if (n <= 0) {
            if (n == -1 && (errno == EINTR || errno == EAGAIN)) continue;
            return false;
        }
        ptr += n;
        remaining -= n;
    }
    return true;
}

// ============================================================================
// LINUX IPC SERVER (Using Unix Domain Sockets)
// ============================================================================

class LinuxIPCServer : public IIPCServer {
public:
    ~LinuxIPCServer() { Stop(); }

    bool Start(const std::string& address) override {
        server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd == -1) return false;

        // Set non-blocking for accept loop
        fcntl(server_fd, F_SETFL, O_NONBLOCK);

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, address.c_str(), sizeof(addr.sun_path) - 1);

        unlink(address.c_str());
        if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            close(server_fd);
            return false;
        }

        if (listen(server_fd, 5) == -1) {
            close(server_fd);
            return false;
        }

        is_running = true;
        accept_thread = std::thread(&LinuxIPCServer::AcceptLoop, this);
        return true;
    }

    void Stop() override {
        is_running = false;
        if (accept_thread.joinable()) accept_thread.join();
        
        std::lock_guard<std::mutex> lock(client_mutex);
        for (int fd : clients) {
            close(fd);
        }
        clients.clear();

        if (server_fd != -1) {
            close(server_fd);
            server_fd = -1;
        }
    }

    void Broadcast(const IPCMessage& msg) override {
        std::lock_guard<std::mutex> lock(client_mutex);
        
        // Header: Type (4B), Size (4B)
        uint32_t header[2];
        header[0] = static_cast<uint32_t>(msg.type);
        header[1] = static_cast<uint32_t>(msg.data.size());

        auto it = clients.begin();
        while (it != clients.end()) {
            if (!write_full(*it, header, sizeof(header)) ||
                (!msg.data.empty() && !write_full(*it, msg.data.data(), msg.data.size()))) {
                close(*it);
                it = clients.erase(it);
                continue;
            }
            ++it;
        }
    }

    bool Receive(IPCMessage& msg, int timeout_ms = -1) override {
        std::lock_guard<std::mutex> lock(client_mutex);
        if (clients.empty()) return false;

        std::vector<struct pollfd> pfds;
        for (int fd : clients) {
            pfds.push_back({fd, POLLIN, 0});
        }

        int res = poll(pfds.data(), pfds.size(), timeout_ms);
        if (res <= 0) return false;

        for (size_t i = 0; i < pfds.size(); ++i) {
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(pfds[i].fd);
                clients.erase(std::find(clients.begin(), clients.end(), pfds[i].fd));
                continue;
            }
            if (pfds[i].revents & POLLIN) {
                uint32_t header[2];
                if (!read_full(pfds[i].fd, header, sizeof(header))) {
                    close(pfds[i].fd);
                    clients.erase(std::find(clients.begin(), clients.end(), pfds[i].fd));
                    continue;
                }
                msg.type = static_cast<IPCMessageType>(header[0]);
                uint32_t size = header[1];
                
                if (size > MAX_IPC_MSG_SIZE) {
                    close(pfds[i].fd);
                    clients.erase(std::find(clients.begin(), clients.end(), pfds[i].fd));
                    continue;
                }
                
                if (size > 0) {
                    msg.data.resize(size);
                    if (!read_full(pfds[i].fd, msg.data.data(), size)) {
                        close(pfds[i].fd);
                        clients.erase(std::find(clients.begin(), clients.end(), pfds[i].fd));
                        continue;
                    }
                } else {
                    msg.data.clear();
                }
                return true;
            }
        }
        return false;
    }

    bool HasClients() const override {
        std::lock_guard<std::mutex> lock(client_mutex);
        return !clients.empty();
    }

private:
    void AcceptLoop() {
        while (is_running) {
            struct sockaddr_un client_addr;
            socklen_t len = sizeof(client_addr);
            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &len);
            
            if (client_fd != -1) {
                // Client sockets stay BLOCKING for reliable large reads/writes
                std::lock_guard<std::mutex> lock(client_mutex);
                clients.push_back(client_fd);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    int server_fd = -1;
    std::atomic<bool> is_running{false};
    std::thread accept_thread;
    std::vector<int> clients;
    mutable std::mutex client_mutex;
};

// ============================================================================
// LINUX IPC CLIENT
// ============================================================================

class LinuxIPCClient : public IIPCClient {
public:
    ~LinuxIPCClient() { Disconnect(); }

    bool Connect(const std::string& address) override {
        client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client_fd == -1) return false;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, address.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            close(client_fd);
            client_fd = -1;
            return false;
        }

        // Socket stays blocking for proper large-frame handling
        return true;
    }

    bool Send(const IPCMessage& msg) override {
        std::lock_guard<std::mutex> lock(send_mutex);
        if (client_fd == -1) return false;

        uint32_t header[2];
        header[0] = static_cast<uint32_t>(msg.type);
        header[1] = static_cast<uint32_t>(msg.data.size());

        if (!write_full(client_fd, header, sizeof(header))) {
            Disconnect();
            return false;
        }

        if (!msg.data.empty()) {
            if (!write_full(client_fd, msg.data.data(), msg.data.size())) {
                Disconnect();
                return false;
            }
        }
        return true;
    }

    void Disconnect() override {
        if (client_fd != -1) {
            close(client_fd);
            client_fd = -1;
        }
    }

    bool Receive(IPCMessage& msg, int timeout_ms) override {
        if (client_fd == -1) return false;

        struct pollfd pfd;
        pfd.fd = client_fd;
        pfd.events = POLLIN;

        int res = poll(&pfd, 1, timeout_ms);
        if (res <= 0) return false;

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            Disconnect();
            return false;
        }

        uint32_t header[2];
        if (!read_full(client_fd, header, sizeof(header))) {
            Disconnect();
            return false;
        }

        msg.type = static_cast<IPCMessageType>(header[0]);
        uint32_t size = header[1];
        
        if (size > MAX_IPC_MSG_SIZE) {
            Disconnect();
            return false;
        }
        
        if (size > 0) {
            msg.data.resize(size);
            if (!read_full(client_fd, msg.data.data(), size)) {
                Disconnect();
                return false;
            }
        } else {
            msg.data.clear();
        }

        return true;
    }

    bool IsConnected() const override {
        return client_fd != -1;
    }

private:
    int client_fd = -1;
    std::mutex send_mutex; // driver Present() calls Send from render thread
};

// ============================================================================
// FACTORY
// ============================================================================

std::unique_ptr<IIPCServer> CreateIPCServer() {
    return std::make_unique<LinuxIPCServer>();
}

std::unique_ptr<IIPCClient> CreateIPCClient() {
    return std::make_unique<LinuxIPCClient>();
}

} // namespace opendriver::core
