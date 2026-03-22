#include <crow.h>
#include <sys/epoll.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <fstream>
#include <vector>
#include <deque>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <unordered_map>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <regex>

namespace fs = std::filesystem;

// =============================================================================
// 配置常量
// =============================================================================
namespace Config {
    constexpr int WEB_PORT = 2240;
    constexpr int STREAM_PORT = 2241;
    constexpr size_t BUFFER_CAPACITY = 256 * 1024;  // 256KB
    constexpr size_t AUDIO_CHUNK_SIZE = 8192;       // 8KB
    constexpr int EPOLL_TIMEOUT_MS = 100;
    constexpr int POLL_TIMEOUT_MS = 200;
    constexpr int MAX_EVENTS = 1024;
    constexpr int MAX_CONNECTIONS = 1024;
    
    // 支持的音频格式
    const std::vector<std::string> SUPPORTED_FORMATS = {
        ".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac"
    };
    
    // 最大上传文件大小 (50MB)
    constexpr size_t MAX_UPLOAD_SIZE = 50 * 1024 * 1024;
}

// =============================================================================
// 线程安全的环形缓冲区
// =============================================================================
class BroadcastBuffer {
public:
    explicit BroadcastBuffer(size_t capacity = Config::BUFFER_CAPACITY) 
        : capacity_(capacity), mask_(capacity - 1), buffer_(capacity) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            throw std::runtime_error("Capacity must be power of two");
        }
    }

    // 生产者写入
    void push(const char* data, size_t len) {
        if (len == 0 || len > capacity_) return;
        
        std::lock_guard<std::mutex> lock(write_mutex_);
        
        size_t current_wp = write_pos_.load(std::memory_order_relaxed);
        size_t new_wp = (current_wp + len) & mask_;
        
        // 环形写入
        size_t first_seg = std::min(len, capacity_ - (current_wp & mask_));
        std::memcpy(&buffer_[current_wp & mask_], data, first_seg);
        if (first_seg < len) {
            std::memcpy(&buffer_[0], data + first_seg, len - first_seg);
        }
        
        write_pos_.store(new_wp, std::memory_order_release);
        data_cv_.notify_all();
    }

    // 消费者读取
    size_t read(size_t& consume_pos, char* dest, size_t max_len) {
        size_t wp = write_pos_.load(std::memory_order_acquire);
        size_t rp = consume_pos;
        
        if (rp == wp) return 0;
        
        size_t avail = (wp > rp) ? (wp - rp) : (capacity_ - rp + wp);
        size_t to_read = std::min(avail, max_len);
        
        size_t first_seg = std::min(to_read, capacity_ - (rp & mask_));
        std::memcpy(dest, &buffer_[rp & mask_], first_seg);
        if (first_seg < to_read) {
            std::memcpy(dest + first_seg, &buffer_[0], to_read - first_seg);
        }
        
        consume_pos = (rp + to_read) & mask_;
        return to_read;
    }

    // 等待数据到达
    bool wait_for_data(size_t& consume_pos, int timeout_ms = 100) {
        std::unique_lock<std::mutex> lock(cv_mutex_);
        size_t wp = write_pos_.load(std::memory_order_acquire);
        
        if (consume_pos != wp) return true;
        
        return data_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
            [this, &consume_pos]() {
                return consume_pos != write_pos_.load(std::memory_order_acquire);
            });
    }

private:
    const size_t capacity_;
    const size_t mask_;
    std::vector<char> buffer_;
    
    std::atomic<size_t> write_pos_{0};
    std::mutex write_mutex_;
    
    std::mutex cv_mutex_;
    std::condition_variable data_cv_;
};

// =============================================================================
// 客户端连接管理
// =============================================================================
class ClientConnection {
public:
    ClientConnection(int fd, BroadcastBuffer* buffer)
        : fd_(fd), buffer_(buffer), consume_pos_(0), shutdown_(false) {
        // 设置非阻塞
        int flags = fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        
        // 禁用Nagle算法
        int yes = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        
        // 设置发送超时
        struct timeval tv{1, 0};  // 1秒
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    ~ClientConnection() {
        close_socket();
    }

    void close_socket() {
        if (!shutdown_.exchange(true)) {
            if (fd_ >= 0) {
                ::shutdown(fd_, SHUT_RDWR);
                ::close(fd_);
                fd_ = -1;
            }
        }
    }

    int fd() const { return fd_; }
    bool is_shutdown() const { return shutdown_; }

    bool send_header() {
        if (shutdown_ || header_sent_) return true;
        
        const std::string header = 
            "ICY 200 OK\r\n"
            "icy-name:Stream Radio\r\n"
            "icy-genre:Music\r\n"
            "icy-url:http://localhost:" + std::to_string(Config::STREAM_PORT) + "\r\n"
            "icy-metaint:16384\r\n"
            "icy-bitrate:192\r\n"
            "Content-Type:audio/mpeg\r\n"
            "Connection: close\r\n"
            "\r\n";
        
        ssize_t sent = ::send(fd_, header.c_str(), header.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            header_sent_ = true;
            return true;
        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
        return true;
    }

    bool send_audio() {
        if (shutdown_) return false;
        
        char audio[Config::AUDIO_CHUNK_SIZE];
        size_t bytes = buffer_->read(consume_pos_, audio, sizeof(audio));
        
        if (bytes == 0) return true;  // 没有数据，但不是错误
        
        ssize_t sent = ::send(fd_, audio, bytes, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;  // 缓冲区满，稍后再试
            }
            return false;  // 真正的错误
        }
        
        return true;
    }

private:
    int fd_;
    BroadcastBuffer* buffer_;
    size_t consume_pos_;
    std::atomic<bool> shutdown_{false};
    bool header_sent_{false};
};

// =============================================================================
// 流媒体服务器
// =============================================================================
class StreamServer {
public:
    StreamServer(BroadcastBuffer* buffer) 
        : buffer_(buffer), running_(false), epoll_fd_(-1), server_fd_(-1) {}
    
    ~StreamServer() {
        stop();
    }
    
    bool start() {
        if (running_) return false;
        
        // 创建服务器socket
        server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            perror("socket");
            return false;
        }
        
        // 设置socket选项
        int reuse = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        
        // 绑定地址
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(Config::STREAM_PORT);
        addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind");
            close(server_fd_);
            return false;
        }
        
        if (listen(server_fd_, Config::MAX_CONNECTIONS) < 0) {
            perror("listen");
            close(server_fd_);
            return false;
        }
        
        // 设置非阻塞
        int flags = fcntl(server_fd_, F_GETFL, 0);
        fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);
        
        // 创建epoll实例
        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ < 0) {
            perror("epoll_create1");
            close(server_fd_);
            return false;
        }
        
        // 注册服务器socket
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = server_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev);
        
        running_ = true;
        thread_ = std::thread(&StreamServer::worker_loop, this);
        
        std::cout << "[Stream] Server started on port " << Config::STREAM_PORT << std::endl;
        return true;
    }
    
    void stop() {
        if (!running_.exchange(false)) return;
        
        if (thread_.joinable()) thread_.join();
        
        // 关闭所有客户端连接
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& client : clients_) {
            client.second->close_socket();
        }
        clients_.clear();
        
        // 关闭文件描述符
        if (epoll_fd_ >= 0) {
            close(epoll_fd_);
            epoll_fd_ = -1;
        }
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
        
        std::cout << "[Stream] Server stopped" << std::endl;
    }
    
    size_t client_count() const {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        return clients_.size();
    }

private:
    void worker_loop() {
        epoll_event events[Config::MAX_EVENTS];
        
        while (running_) {
            int n = epoll_wait(epoll_fd_, events, Config::MAX_EVENTS, Config::EPOLL_TIMEOUT_MS);
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("epoll_wait");
                break;
            }
            
            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                uint32_t events_mask = events[i].events;
                
                if (fd == server_fd_) {
                    handle_new_connections();
                } else if (events_mask & (EPOLLERR | EPOLLRDHUP | EPOLLHUP)) {
                    remove_client(fd);
                } else if (events_mask & EPOLLOUT) {
                    handle_client_write(fd);
                }
            }
            
            // 定期清理无效连接
            static int cleanup_counter = 0;
            if (++cleanup_counter >= 100) {  // 每10秒清理一次
                cleanup_counter = 0;
                cleanup_dead_clients();
            }
        }
    }
    
    void handle_new_connections() {
        while (running_) {
            sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &addr_len);
            
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                perror("accept");
                break;
            }
            
            // 检查连接数限制
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                if (clients_.size() >= Config::MAX_CONNECTIONS) {
                    close(client_fd);
                    std::cout << "[Stream] Connection refused: limit reached" << std::endl;
                    continue;
                }
            }
            
            auto client = std::make_shared<ClientConnection>(client_fd, buffer_);
            
            // 注册到epoll
            epoll_event ev{};
            ev.events = EPOLLOUT | EPOLLRDHUP | EPOLLET;
            ev.data.fd = client_fd;
            
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                perror("epoll_ctl");
                close(client_fd);
                continue;
            }
            
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                clients_[client_fd] = client;
            }
            
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
            std::cout << "[Stream] New client: " << ip_str << ":" 
                      << ntohs(client_addr.sin_port) 
                      << " (total: " << clients_.size() << ")" << std::endl;
        }
    }
    
    void handle_client_write(int fd) {
        std::shared_ptr<ClientConnection> client;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto it = clients_.find(fd);
            if (it == clients_.end()) return;
            client = it->second;
        }
        
        if (!client) return;
        
        try {
            if (!client->send_header()) {
                remove_client(fd);
                return;
            }
            
            if (!client->send_audio()) {
                remove_client(fd);
                return;
            }
        } catch (const std::exception& e) {
            std::cerr << "[Stream] Error handling client " << fd << ": " << e.what() << std::endl;
            remove_client(fd);
        }
    }
    
    void remove_client(int fd) {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(fd);
        if (it != clients_.end()) {
            it->second->close_socket();
            clients_.erase(it);
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
            std::cout << "[Stream] Client disconnected: " << fd 
                      << " (remaining: " << clients_.size() << ")" << std::endl;
        }
    }
    
    void cleanup_dead_clients() {
        std::vector<int> dead_clients;
        
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (const auto& [fd, client] : clients_) {
                if (client->is_shutdown()) {
                    dead_clients.push_back(fd);
                }
            }
        }
        
        for (int fd : dead_clients) {
            remove_client(fd);
        }
    }
    
    BroadcastBuffer* buffer_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    
    int epoll_fd_;
    int server_fd_;
    
    mutable std::mutex clients_mutex_;
    std::unordered_map<int, std::shared_ptr<ClientConnection>> clients_;
};

// =============================================================================
// 音频播放管理器
// =============================================================================
class AudioPlayer {
public:
    AudioPlayer(BroadcastBuffer* buffer, std::vector<std::string>* playlist, 
                std::atomic<size_t>* current_track)
        : buffer_(buffer), playlist_(playlist), current_track_(current_track),
          running_(false), skip_track_(false) {}
    
    ~AudioPlayer() {
        stop();
    }
    
    bool start() {
        if (running_) return false;
        
        signal(SIGPIPE, SIG_IGN);
        running_ = true;
        thread_ = std::thread(&AudioPlayer::worker_loop, this);
        
        std::cout << "[Audio] Player started" << std::endl;
        return true;
    }
    
    void stop() {
        if (!running_.exchange(false)) return;
        
        skip_track_ = true;
        if (thread_.joinable()) thread_.join();
        
        std::cout << "[Audio] Player stopped" << std::endl;
    }
    
    void skip_current_track() {
        skip_track_ = true;
    }
    
private:
    void worker_loop() {
        while (running_) {
            // 等待播放列表中有音乐
            {
                std::lock_guard<std::mutex> lock(*playlist_mutex_);
                if (playlist_->empty()) {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }
            }
            
            play_next_track();
        }
    }
    
    void play_next_track() {
        std::string filename;
        size_t track_idx;
        
        {
            std::lock_guard<std::mutex> lock(*playlist_mutex_);
            if (playlist_->empty()) return;
            
            track_idx = current_track_->load() % playlist_->size();
            filename = "./media/" + playlist_->at(track_idx);
        }
        
        // 检查文件是否存在
        if (!fs::exists(filename)) {
            std::cerr << "[Audio] File not found: " << filename << std::endl;
            (*current_track_)++;
            return;
        }
        
        std::cout << "[Audio] Playing: " << playlist_->at(track_idx) 
                  << " (" << track_idx + 1 << "/" << playlist_->size() << ")" << std::endl;
        
        // 构建FFmpeg命令
        std::string cmd = "ffmpeg -v quiet -re -i \"" + filename + "\" "
                          "-c:a libmp3lame -b:a 192k -f mp3 -";
        
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            std::cerr << "[Audio] Failed to start FFmpeg for: " << filename << std::endl;
            (*current_track_)++;
            return;
        }
        
        // 使用poll监控管道
        int pipe_fd = fileno(pipe);
        struct pollfd pfd = {pipe_fd, POLLIN, 0};
        
        char buffer[Config::AUDIO_CHUNK_SIZE];
        skip_track_ = false;
        
        bool error_occurred = false;
        
        while (!skip_track_ && running_ && !error_occurred) {
            int ret = poll(&pfd, 1, Config::POLL_TIMEOUT_MS);
            
            if (ret > 0) {
                if (pfd.revents & POLLIN) {
                    ssize_t bytes = read(pipe_fd, buffer, sizeof(buffer));
                    
                    if (bytes > 0) {
                        buffer_->push(buffer, bytes);
                    } else if (bytes == 0) {
                        break;  // EOF
                    } else if (bytes < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            std::cerr << "[Audio] Read error: " << strerror(errno) << std::endl;
                            error_occurred = true;
                        }
                    }
                } else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    std::cerr << "[Audio] Pipe error: revents=" << pfd.revents << std::endl;
                    error_occurred = true;
                }
            } else if (ret < 0) {
                if (errno == EINTR) continue;
                std::cerr << "[Audio] Poll error: " << strerror(errno) << std::endl;
                error_occurred = true;
            }
            // 超时继续检查条件
        }
        
        // 关闭管道
        if (pipe) {
            int status = pclose(pipe);
            if (status != 0 && !error_occurred) {
                std::cerr << "[Audio] FFmpeg exited with status: " << status << std::endl;
            }
        }
        
        // 切换到下一首（除非用户指定了其他曲目）
        if (!skip_track_) {
            (*current_track_)++;
        }
    }
    
    BroadcastBuffer* buffer_;
    std::vector<std::string>* playlist_;
    std::atomic<size_t>* current_track_;
    std::mutex* playlist_mutex_;
    
    std::atomic<bool> running_{false};
    std::atomic<bool> skip_track_{false};
    std::thread thread_;
};

// =============================================================================
// Web服务器管理器
// =============================================================================
class WebServer {
public:
    WebServer(std::vector<std::string>* playlist, std::atomic<size_t>* current_track,
              StreamServer* stream_server, AudioPlayer* audio_player)
        : playlist_(playlist), current_track_(current_track),
          stream_server_(stream_server), audio_player_(audio_player),
          running_(false) {}
    
    ~WebServer() {
        stop();
    }
    
    bool start() {
        if (running_) return false;
        
        setup_routes();
        
        running_ = true;
        thread_ = std::thread([this]() {
            try {
                std::cout << "[Web] Server starting on port " << Config::WEB_PORT << std::endl;
                app_.port(Config::WEB_PORT).multithreaded().run();
            } catch (const std::exception& e) {
                std::cerr << "[Web] Error: " << e.what() << std::endl;
            }
        });
        
        return true;
    }
    
    void stop() {
        if (!running_.exchange(false)) return;
        
        app_.stop();
        if (thread_.joinable()) thread_.join();
        
        std::cout << "[Web] Server stopped" << std::endl;
    }

private:
    void setup_routes() {
        // 首页
        CROW_ROUTE(app_, "/")
        ([this]() {
            std::stringstream html;
            html << R"(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>流媒体电台</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            padding: 20px;
            color: #333;
        }
        .container {
            max-width: 1000px;
            margin: 0 auto;
            background: rgba(255, 255, 255, 0.95);
            border-radius: 20px;
            padding: 30px;
            box-shadow: 0 20px 60px rgba(0, 0, 0, 0.3);
            backdrop-filter: blur(10px);
        }
        header {
            text-align: center;
            margin-bottom: 40px;
        }
        h1 {
            font-size: 3em;
            color: #764ba2;
            margin-bottom: 10px;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 15px;
        }
        h1 .icon { font-size: 1.2em; }
        .subtitle {
            color: #666;
            font-size: 1.2em;
            margin-bottom: 30px;
        }
        .player-section {
            background: #f8f9fa;
            border-radius: 15px;
            padding: 25px;
            margin-bottom: 30px;
            border: 1px solid #e9ecef;
        }
        .player-section h2 {
            color: #495057;
            margin-bottom: 20px;
            display: flex;
            align-items: center;
            gap: 10px;
        }
        audio {
            width: 100%;
            margin-bottom: 20px;
            border-radius: 10px;
        }
        .controls {
            display: flex;
            gap: 15px;
            align-items: center;
            flex-wrap: wrap;
        }
        button {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            border: none;
            padding: 12px 25px;
            border-radius: 8px;
            cursor: pointer;
            font-size: 1em;
            font-weight: 600;
            transition: all 0.3s ease;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        button:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 15px rgba(102, 126, 234, 0.4);
        }
        button:active {
            transform: translateY(0);
        }
        #currentTrack {
            font-size: 1.2em;
            color: #495057;
            font-weight: 600;
            background: white;
            padding: 10px 20px;
            border-radius: 8px;
            flex-grow: 1;
            border: 2px solid #e9ecef;
        }
        .playlist-section {
            background: white;
            border-radius: 15px;
            padding: 25px;
            margin-bottom: 30px;
            border: 1px solid #e9ecef;
        }
        .playlist-section h2 {
            color: #495057;
            margin-bottom: 20px;
            display: flex;
            align-items: center;
            gap: 10px;
        }
        #playlist {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
            gap: 12px;
            max-height: 400px;
            overflow-y: auto;
            padding-right: 10px;
        }
        .track {
            background: #f8f9fa;
            padding: 15px;
            border-radius: 8px;
            cursor: pointer;
            transition: all 0.2s ease;
            border-left: 4px solid transparent;
        }
        .track:hover {
            background: #e9ecef;
            transform: translateX(5px);
        }
        .track.current {
            background: linear-gradient(135deg, rgba(102, 126, 234, 0.1) 0%, rgba(118, 75, 162, 0.1) 100%);
            border-left-color: #667eea;
        }
        .track-number {
            display: inline-block;
            width: 25px;
            text-align: center;
            background: #667eea;
            color: white;
            border-radius: 4px;
            margin-right: 10px;
            font-size: 0.9em;
            padding: 2px 5px;
        }
        .upload-section {
            background: #f8f9fa;
            border-radius: 15px;
            padding: 25px;
            border: 1px solid #e9ecef;
        }
        .upload-section h2 {
            color: #495057;
            margin-bottom: 20px;
            display: flex;
            align-items: center;
            gap: 10px;
        }
        #uploadForm {
            display: flex;
            gap: 15px;
            align-items: center;
            flex-wrap: wrap;
        }
        input[type="file"] {
            flex-grow: 1;
            padding: 12px;
            border: 2px solid #e9ecef;
            border-radius: 8px;
            font-size: 1em;
            background: white;
        }
        #uploadStatus {
            margin-top: 15px;
            padding: 12px;
            border-radius: 8px;
            font-weight: 600;
            text-align: center;
        }
        .success { background: #d4edda; color: #155724; }
        .error { background: #f8d7da; color: #721c24; }
        .info { background: #d1ecf1; color: #0c5460; }
        .stats {
            display: flex;
            gap: 20px;
            margin-top: 30px;
            flex-wrap: wrap;
        }
        .stat-box {
            background: white;
            padding: 20px;
            border-radius: 10px;
            flex: 1;
            min-width: 200px;
            border: 1px solid #e9ecef;
            text-align: center;
        }
        .stat-value {
            font-size: 2.5em;
            font-weight: bold;
            color: #667eea;
            margin: 10px 0;
        }
        .stat-label {
            color: #6c757d;
            font-size: 0.9em;
        }
        footer {
            text-align: center;
            margin-top: 40px;
            color: #6c757d;
            font-size: 0.9em;
            padding-top: 20px;
            border-top: 1px solid #e9ecef;
        }
        @media (max-width: 768px) {
            .container { padding: 15px; }
            h1 { font-size: 2em; }
            .controls { flex-direction: column; }
            #playlist { grid-template-columns: 1fr; }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1><span class="icon">🎵</span> 流媒体电台</h1>
            <div class="subtitle">在线音乐广播，随时随地享受高品质音乐</div>
        </header>
        
        <section class="player-section">
            <h2><span class="icon">▶️</span> 当前播放</h2>
            <audio id="player" controls autoplay>
                <source src="http://localhost:)" << Config::STREAM_PORT << R"(" type="audio/mpeg">
                您的浏览器不支持音频播放
            </audio>
            <div class="controls">
                <button onclick="playNext()">
                    <span class="icon">⏭️</span> 下一首
                </button>
                <div id="currentTrack">正在加载播放列表...</div>
            </div>
        </section>
        
        <section class="playlist-section">
            <h2><span class="icon">📋</span> 播放列表 (<span id="trackCount">0</span> 首歌曲)</h2>
            <div id="playlist">加载中...</div>
        </section>
        
        <section class="upload-section">
            <h2><span class="icon">📤</span> 上传音乐</h2>
            <form id="uploadForm">
                <input type="file" id="fileInput" name="file" accept=".mp3,.wav,.flac,.ogg,.m4a,.aac" required>
                <button type="submit">
                    <span class="icon">⬆️</span> 上传
                </button>
            </form>
            <div id="uploadStatus"></div>
        </section>
        
        <div class="stats">
            <div class="stat-box">
                <div class="stat-label">在线听众</div>
                <div class="stat-value" id="clientCount">0</div>
            </div>
            <div class="stat-box">
                <div class="stat-label">总曲目</div>
                <div class="stat-value" id="totalTracks">0</div>
            </div>
            <div class="stat-box">
                <div class="stat-label">当前曲目</div>
                <div class="stat-value" id="currentIndex">-</div>
            </div>
        </div>
        
        <footer>
            <p>© 2024 流媒体电台 | 服务器运行中 | 端口: )" << Config::WEB_PORT << " (Web), " << Config::STREAM_PORT << " (Stream)</p>
        </footer>
    </div>

    <script>
        let currentTrackIndex = 0;
        let totalTracks = 0;
        
        // 加载播放列表
        async function loadPlaylist() {
            try {
                const response = await fetch('/api/playlist');
                const data = await response.json();
                
                const playlist = data.playlist || [];
                const currentIndex = data.current || 0;
                
                currentTrackIndex = currentIndex % (playlist.length || 1);
                totalTracks = playlist.length;
                
                // 更新UI
                document.getElementById('currentTrack').textContent = 
                    playlist.length > 0 ? `正在播放: ${playlist[currentTrackIndex]}` : '播放列表为空';
                document.getElementById('trackCount').textContent = totalTracks;
                document.getElementById('totalTracks').textContent = totalTracks;
                document.getElementById('currentIndex').textContent = playlist.length > 0 ? currentTrackIndex + 1 : '-';
                
                // 生成播放列表HTML
                let html = '';
                if (playlist.length === 0) {
                    html = '<div style="text-align: center; padding: 40px; color: #6c757d;">'
                         + '<div style="font-size: 3em; margin-bottom: 20px;">🎵</div>'
                         + '<div>播放列表为空，请上传音乐文件</div>'
                         + '</div>';
                } else {
                    playlist.forEach((track, index) => {
                        const isCurrent = index === currentTrackIndex;
                        html += `
                            <div class="track ${isCurrent ? 'current' : ''}" onclick="playTrack(${index})">
                                <span class="track-number">${index + 1}</span>
                                ${track}
                                ${isCurrent ? ' <span style="color: #667eea;">▶️</span>' : ''}
                            </div>
                        `;
                    });
                }
                document.getElementById('playlist').innerHTML = html;
                
            } catch (error) {
                console.error('加载播放列表失败:', error);
                document.getElementById('currentTrack').textContent = '加载失败，请刷新页面';
            }
        }
        
        // 加载统计信息
        async function loadStats() {
            try {
                const response = await fetch('/api/stats');
                const data = await response.json();
                document.getElementById('clientCount').textContent = data.clients || 0;
            } catch (error) {
                console.error('加载统计失败:', error);
            }
        }
        
        // 播放指定曲目
        async function playTrack(index) {
            try {
                await fetch('/api/play/' + index);
                await new Promise(resolve => setTimeout(resolve, 500)); // 等待切换
                loadPlaylist();
                document.getElementById('player').load(); // 重新加载音频流
            } catch (error) {
                console.error('切换曲目失败:', error);
                showMessage('切换曲目失败: ' + error.message, 'error');
            }
        }
        
        // 播放下一首
        async function playNext() {
            try {
                await fetch('/api/next');
                await new Promise(resolve => setTimeout(resolve, 500));
                loadPlaylist();
                document.getElementById('player').load();
            } catch (error) {
                console.error('下一首失败:', error);
                showMessage('切换失败: ' + error.message, 'error');
            }
        }
        
        // 显示消息
        function showMessage(message, type = 'info') {
            const statusEl = document.getElementById('uploadStatus');
            statusEl.textContent = message;
            statusEl.className = type;
            if (type !== 'info') {
                setTimeout(() => {
                    statusEl.textContent = '';
                    statusEl.className = '';
                }, 5000);
            }
        }
        
        // 处理文件上传
        document.getElementById('uploadForm').addEventListener('submit', async (e) => {
            e.preventDefault();
            
            const fileInput = document.getElementById('fileInput');
            if (!fileInput.files[0]) {
                showMessage('请选择要上传的文件', 'error');
                return;
            }
            
            const file = fileInput.files[0];
            const maxSize = 50 * 1024 * 1024; // 50MB
            
            if (file.size > maxSize) {
                showMessage('文件太大，最大支持50MB', 'error');
                return;
            }
            
            const formData = new FormData();
            formData.append('file', file);
            
            showMessage('上传中...', 'info');
            
            try {
                const response = await fetch('/upload', {
                    method: 'POST',
                    body: formData
                });
                
                if (response.ok) {
                    const text = await response.text();
                    showMessage('✅ ' + text, 'success');
                    fileInput.value = '';
                    setTimeout(() => {
                        loadPlaylist();
                    }, 1000);
                } else {
                    const text = await response.text();
                    showMessage('❌ ' + text, 'error');
                }
            } catch (error) {
                showMessage('❌ 上传失败: ' + error.message, 'error');
            }
        });
        
        // 初始化加载
        loadPlaylist();
        loadStats();
        
        // 定期更新
        setInterval(loadPlaylist, 3000);
        setInterval(loadStats, 2000);
    </script>
</body>
</html>
)";
            return crow::response(html.str());
        });
        
        // API: 播放列表
        CROW_ROUTE(app_, "/api/playlist")
        ([this]() {
            std::lock_guard<std::mutex> lock(playlist_mutex_);
            crow::json::wvalue result;
            result["playlist"] = *playlist_;
            result["current"] = current_track_->load();
            return result;
        });
        
        // API: 播放指定曲目
        CROW_ROUTE(app_, "/api/play/<int>")
        ([this](int index) {
            std::lock_guard<std::mutex> lock(playlist_mutex_);
            if (index >= 0 && index < playlist_->size()) {
                current_track_->store(index);
                if (audio_player_) audio_player_->skip_current_track();
                return crow::response(200, "OK");
            }
            return crow::response(404, "Invalid track index");
        });
        
        // API: 下一首
        CROW_ROUTE(app_, "/api/next")
        ([this]() {
            if (audio_player_) audio_player_->skip_current_track();
            return crow::response(200, "OK");
        });
        
        // API: 统计信息
        CROW_ROUTE(app_, "/api/stats")
        ([this]() {
            crow::json::wvalue result;
            result["clients"] = stream_server_ ? stream_server_->client_count() : 0;
            {
                std::lock_guard<std::mutex> lock(playlist_mutex_);
                result["tracks"] = playlist_->size();
                result["current"] = current_track_->load();
            }
            return result;
        });
        
        // 文件上传
        CROW_ROUTE(app_, "/upload").methods("POST"_method)
        ([this](const crow::request& req) {
            try {
                // 解析multipart表单数据
                crow::multipart::message msg(req);
                
                if (msg.parts.empty()) {
                    return crow::response(400, "未选择文件");
                }
                
                // 获取第一个文件部分
                const auto& part = msg.parts[0];
                auto header_it = part.headers.find("Content-Disposition");
                if (header_it == part.headers.end()) {
                    return crow::response(400, "无效的文件格式");
                }
                
                auto params = header_it->second.params;
                auto filename_it = params.find("filename");
                if (filename_it == params.end()) {
                    return crow::response(400, "文件名缺失");
                }
                
                std::string filename = filename_it->second;
                
                // 安全检查
                if (filename.empty()) {
                    return crow::response(400, "文件名为空");
                }
                
                // 防止路径遍历攻击
                if (filename.find("..") != std::string::npos ||
                    filename.find('/') != std::string::npos ||
                    filename.find('\\') != std::string::npos) {
                    return crow::response(400, "无效的文件名");
                }
                
                // 检查文件扩展名
                std::string ext = fs::path(filename).extension();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                
                bool supported = false;
                for (const auto& supported_ext : Config::SUPPORTED_FORMATS) {
                    if (ext == supported_ext) {
                        supported = true;
                        break;
                    }
                }
                
                if (!supported) {
                    std::string msg = "不支持的文件格式，支持: ";
                    for (size_t i = 0; i < Config::SUPPORTED_FORMATS.size(); ++i) {
                        if (i > 0) msg += ", ";
                        msg += Config::SUPPORTED_FORMATS[i];
                    }
                    return crow::response(400, msg);
                }
                
                // 检查文件大小
                if (part.body.size() > Config::MAX_UPLOAD_SIZE) {
                    return crow::response(413, "文件太大，最大支持50MB");
                }
                
                // 确保media目录存在
                fs::create_directories("./media");
                
                // 生成安全的文件名（避免覆盖）
                std::string safe_filename = filename;
                int counter = 1;
                while (fs::exists("./media/" + safe_filename)) {
                    std::string stem = fs::path(filename).stem();
                    safe_filename = stem + " (" + std::to_string(counter++) + ")" + ext;
                }
                
                // 保存文件
                std::string target_path = "./media/" + safe_filename;
                std::ofstream out(target_path, std::ios::binary);
                if (!out) {
                    return crow::response(500, "保存文件失败");
                }
                
                out.write(part.body.data(), part.body.size());
                out.close();
                
                // 添加到播放列表
                {
                    std::lock_guard<std::mutex> lock(playlist_mutex_);
                    playlist_->push_back(safe_filename);
                    std::sort(playlist_->begin(), playlist_->end());
                }
                
                std::cout << "[Web] File uploaded: " << safe_filename 
                          << " (" << part.body.size() / 1024 << " KB)" << std::endl;
                
                return crow::response(200, "文件上传成功: " + safe_filename);
                
            } catch (const std::exception& e) {
                std::cerr << "[Web] Upload error: " << e.what() << std::endl;
                return crow::response(500, "上传过程中发生错误");
            }
        });
        
        // 静态文件服务
        CROW_ROUTE(app_, "/static/<string>")
        ([this](const std::string& filename) {
            std::string filepath = "./media/" + filename;
            
            if (!fs::exists(filepath)) {
                return crow::response(404, "文件不存在");
            }
            
            auto response = crow::response();
            response.set_static_file_info(filepath);
            
            // 设置Content-Type
            std::string ext = fs::path(filename).extension();
            if (ext == ".mp3") response.set_header("Content-Type", "audio/mpeg");
            else if (ext == ".wav") response.set_header("Content-Type", "audio/wav");
            else if (ext == ".flac") response.set_header("Content-Type", "audio/flac");
            else if (ext == ".ogg") response.set_header("Content-Type", "audio/ogg");
            else if (ext == ".m4a") response.set_header("Content-Type", "audio/mp4");
            else if (ext == ".aac") response.set_header("Content-Type", "audio/aac");
            
            return response;
        });
    }
    
    crow::SimpleApp app_;
    std::vector<std::string>* playlist_;
    std::atomic<size_t>* current_track_;
    StreamServer* stream_server_;
    AudioPlayer* audio_player_;
    mutable std::mutex playlist_mutex_;
    
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// =============================================================================
// 应用程序主类
// =============================================================================
class RadioServer {
public:
    RadioServer() {
        // 初始化播放列表
        init_playlist();
    }
    
    ~RadioServer() {
        stop();
    }
    
    bool start() {
        if (running_) return false;
        
        std::cout << "\n"
            "╔══════════════════════════════════════════╗\n"
            "║        流媒体电台服务器启动中...        ║\n"
            "╚══════════════════════════════════════════╝\n" << std::endl;
        
        // 启动各个组件
        bool success = true;
        
        stream_server_ = std::make_unique<StreamServer>(&buffer_);
        audio_player_ = std::make_unique<AudioPlayer>(&buffer_, &playlist_, &current_track_);
        web_server_ = std::make_unique<WebServer>(&playlist_, &current_track_, 
                                                  stream_server_.get(), audio_player_.get());
        
        success &= stream_server_->start();
        success &= audio_player_->start();
        success &= web_server_->start();
        
        if (success) {
            running_ = true;
            std::cout << "\n"
                "╔══════════════════════════════════════════╗\n"
                "║        服务器启动成功！                 ║\n"
                "║                                          ║\n"
                "║  Web界面: http://localhost:" << Config::WEB_PORT << "     ║\n"
                "║  流媒体: http://localhost:" << Config::STREAM_PORT << "    ║\n"
                "║                                          ║\n"
                "║  按 Ctrl+C 停止服务器                  ║\n"
                "╚══════════════════════════════════════════╝\n" << std::endl;
        }
        
        return success;
    }
    
    void stop() {
        if (!running_.exchange(false)) return;
        
        std::cout << "\n[System] 正在停止服务器..." << std::endl;
        
        // 按依赖顺序停止组件
        if (web_server_) web_server_->stop();
        if (audio_player_) audio_player_->stop();
        if (stream_server_) stream_server_->stop();
        
        // 等待所有组件停止
        web_server_.reset();
        audio_player_.reset();
        stream_server_.reset();
        
        std::cout << "[System] 服务器已停止" << std::endl;
    }
    
    void wait_for_shutdown() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

private:
    void init_playlist() {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        
        // 创建media目录
        fs::create_directories("./media");
        
        // 扫描音频文件
        try {
            for (const auto& entry : fs::directory_iterator("./media")) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    std::string ext = fs::path(filename).extension();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    
                    for (const auto& supported_ext : Config::SUPPORTED_FORMATS) {
                        if (ext == supported_ext) {
                            playlist_.push_back(filename);
                            break;
                        }
                    }
                }
            }
            
            std::sort(playlist_.begin(), playlist_.end());
            
            if (!playlist_.empty()) {
                // 随机选择起始曲目
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(0, playlist_.size() - 1);
                current_track_ = dis(gen);
            }
            
            std::cout << "[Init] 在 ./media/ 目录中找到 " << playlist_.size() << " 个音频文件" << std::endl;
            
        } catch (const fs::filesystem_error& e) {
            std::cerr << "[Init] 扫描目录时出错: " << e.what() << std::endl;
        }
    }
    
    BroadcastBuffer buffer_{Config::BUFFER_CAPACITY};
    std::vector<std::string> playlist_;
    std::atomic<size_t> current_track_{0};
    mutable std::mutex playlist_mutex_;
    
    std::unique_ptr<StreamServer> stream_server_;
    std::unique_ptr<AudioPlayer> audio_player_;
    std::unique_ptr<WebServer> web_server_;
    
    std::atomic<bool> running_{false};
};

// =============================================================================
// 全局信号处理器
// =============================================================================
RadioServer* g_server_instance = nullptr;

void signal_handler(int sig) {
    std::cout << "\n[System] 收到信号 " << sig << "，正在关闭服务器..." << std::endl;
    
    if (g_server_instance) {
        g_server_instance->stop();
    }
}

// =============================================================================
// 主函数
// =============================================================================
int main() {
    // 设置本地化
    std::locale::global(std::locale("zh_CN.UTF-8"));
    
    // 设置信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);  // 忽略管道断开信号
    
    try {
        RadioServer server;
        g_server_instance = &server;
        
        if (!server.start()) {
            std::cerr << "[System] 服务器启动失败" << std::endl;
            return 1;
        }
        
        // 等待服务器运行
        server.wait_for_shutdown();
        
        g_server_instance = nullptr;
        
    } catch (const std::exception& e) {
        std::cerr << "[System] 致命错误: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "[System] 服务器已退出" << std::endl;
    return 0;
}