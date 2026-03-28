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

namespace opendriver::core {

// ============================================================================
// LINUX IPC SERVER (Using Unix Domain Sockets)
// ============================================================================

class LinuxIPCServer : public IIPCServer {
public:
    ~LinuxIPCServer() { Stop(); }

    bool Start(const std::string& address) override {
        server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd == -1) return false;

        // Set non-blocking
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
            // Write header
            if (write(*it, header, sizeof(header)) == -1) {
                close(*it);
                it = clients.erase(it);
                continue;
            }
            // Write data
            if (!msg.data.empty()) {
                if (write(*it, msg.data.data(), msg.data.size()) == -1) {
                    close(*it);
                    it = clients.erase(it);
                    continue;
                }
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

        for (auto const& pfd : pfds) {
            if (pfd.revents & POLLIN) {
                // Read from this client
                uint32_t header[2];
                if (read(pfd.fd, header, sizeof(header)) == sizeof(header)) {
                    msg.type = static_cast<IPCMessageType>(header[0]);
                    uint32_t size = header[1];
                    if (size > 0) {
                        msg.data.resize(size);
                        read(pfd.fd, msg.data.data(), size);
                    } else {
                        msg.data.clear();
                    }
                    return true;
                }
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
                std::lock_guard<std::mutex> lock(client_mutex);
                clients.push_back(client_fd);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

        return true;
    }

    bool Send(const IPCMessage& msg) override {
        if (client_fd == -1) return false;

        uint32_t header[2];
        header[0] = static_cast<uint32_t>(msg.type);
        header[1] = static_cast<uint32_t>(msg.data.size());

        if (write(client_fd, header, sizeof(header)) == -1) {
            Disconnect();
            return false;
        }

        if (!msg.data.empty()) {
            if (write(client_fd, msg.data.data(), msg.data.size()) == -1) {
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

        uint32_t header[2];
        ssize_t n = read(client_fd, header, sizeof(header));
        if (n != sizeof(header)) {
            Disconnect();
            return false;
        }

        msg.type = static_cast<IPCMessageType>(header[0]);
        uint32_t size = header[1];
        
        if (size > 0) {
            msg.data.resize(size);
            n = read(client_fd, msg.data.data(), size);
            if (n != (ssize_t)size) {
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
