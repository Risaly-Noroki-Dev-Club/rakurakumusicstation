#include <crow_all.h>
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
#include <random>
#include <fstream>
#include <sstream>
#include "sessionmanager.hpp"
#include "authmiddleware.hpp"
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
    
    // 检查是否有足够空间
    size_t current_rp = consume_pos_.load(std::memory_order_relaxed);
    size_t current_wp = write_pos_.load(std::memory_order_relaxed);
    size_t used = (current_wp >= current_rp) ? 
                 (current_wp - current_rp) : 
                 (capacity_ - current_rp + current_wp);
    size_t free = capacity_ - used;
    
    if (len > free) {
        // 空间不足，丢弃最旧的数据
        consume_pos_.store((current_rp + (len - free)) & mask_, 
                           std::memory_order_relaxed);
    }
    
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
    std::atomic<size_t> consume_pos_{0};
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

    // 在 ClientConnection 类中修改：
bool send_header() {
    if (shutdown_ || header_sent_) return true;
    
    // 使用标准的 HTTP 1.1 响应头，防止 Chrome 下载
    const std::string header = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: audio/mpeg\r\n"
        "Connection: keep-alive\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "Pragma: no-cache\r\n"
        "Server: Rakuraku-Radio\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    
    ssize_t sent = ::send(fd_, header.c_str(), header.size(), MSG_NOSIGNAL);
    if (sent > 0) {
        header_sent_ = true;
        return true;
    }
    return (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
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
            
            // 向所有客户端广播音频数据
            broadcast_audio();

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
            ev.events = EPOLLOUT | EPOLLRDHUP;
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
    
    void broadcast_audio() {
        std::vector<std::pair<int, std::shared_ptr<ClientConnection>>> snapshot;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            snapshot.assign(clients_.begin(), clients_.end());
        }

        std::vector<int> dead;
        for (auto& [fd, client] : snapshot) {
            if (client->is_shutdown()) {
                dead.push_back(fd);
                continue;
            }
            if (!client->send_header() || !client->send_audio()) {
                dead.push_back(fd);
            }
        }

        for (int fd : dead) {
            remove_client(fd);
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
            std::atomic<size_t>* current_track, std::mutex* playlist_mutex)
    : buffer_(buffer), playlist_(playlist), current_track_(current_track),
      playlist_mutex_(playlist_mutex), running_(false), skip_track_(false) {}

    
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
    BroadcastBuffer* buffer_;
    std::vector<std::string>* playlist_;
    std::atomic<size_t>* current_track_;
    std::mutex* playlist_mutex_;

    std::atomic<bool> running_{false};
    std::atomic<bool> skip_track_{false};
    std::thread thread_;
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
    FILE* pipe = nullptr;
    bool pipe_opened = false;
    
    try {
        size_t playlist_size;
        std::string filename;

        {
            std::lock_guard<std::mutex> lock(*playlist_mutex_);
            playlist_size = playlist_->size();
            if (playlist_size == 0) return;  // 提前返回

            // 安全的模运算
            size_t track_idx = playlist_size > 0 ?
                               current_track_->load() % playlist_size : 0;
            filename = "./media/" + playlist_->at(track_idx);

            std::cout << "[Audio] Playing: " << playlist_->at(track_idx)
                      << " (" << track_idx + 1 << "/" << playlist_->size() << ")" << std::endl;
        }

        // 检查文件是否存在
        if (!fs::exists(filename)) {
            std::cerr << "[Audio] File not found: " << filename << std::endl;
            (*current_track_)++;
            return;
        }
        
        // 构建FFmpeg命令
        std::string cmd = "ffmpeg -re -v error -i \"" + filename + "\" "
                  "-vn -codec:a libmp3lame -b:a 128k -ar 44100 -ac 2 -f mp3 -";
        
        pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            std::cerr << "[Audio] Failed to start FFmpeg" << std::endl;
            return;
        }
        pipe_opened = true;
        
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
                } else if (pfd.revents & POLLHUP) {
                    break;  // Normal EOF: FFmpeg finished encoding
                } else if (pfd.revents & (POLLERR | POLLNVAL)) {
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
        if (pipe && pipe_opened) {
            int status = pclose(pipe);
            pipe = nullptr;
            pipe_opened = false;
            // Track index info already printed earlier
        }
        } catch (const std::exception& e) {
            std::cerr << "[Audio] Error playing track: " << e.what() << std::endl;
        }

        // 切换到下一首（除非用户指定了其他曲目）
        if (!skip_track_) {
            (*current_track_)++;
        }
    }
};

// =============================================================================
// Web服务器管理器
// =============================================================================
// =============================================================================
// Web服务器管理器（更新版，整合认证系统）
// =============================================================================
class WebServer {
public:
    struct Config {
        bool allow_guest_skip = false;  // 是否允许游客切歌
        std::string admin_password = "";
        std::string station_name = "我的音乐电台";
        std::string subtitle = "极简流媒体服务器";
        std::string primary_color = "#764ba2";
        std::string secondary_color = "#667eea";
        std::string bg_color = "#f4f4f9";
        static constexpr int WEB_PORT = 2240;
        static const std::vector<std::string> SUPPORTED_FORMATS;
        static constexpr size_t MAX_UPLOAD_SIZE = 50 * 1024 * 1024;
        
        static Config load_from_settings() {
            Config config;
            std::ifstream conf_file("settings.json");
            if (conf_file.is_open()) {
                std::stringstream ss;
                ss << conf_file.rdbuf();
                auto j = crow::json::load(ss.str());
                if (j) {
                    if (j.has("allow_guest_skip")) 
                        config.allow_guest_skip = j["allow_guest_skip"].b();
                    if (j.has("admin_password")) 
                        config.admin_password = j["admin_password"].s();
                    if (j.has("station_name")) 
                        config.station_name = j["station_name"].s();
                    if (j.has("subtitle")) 
                        config.subtitle = j["subtitle"].s();
                    if (j.has("primary_color")) 
                        config.primary_color = j["primary_color"].s();
                    if (j.has("secondary_color")) 
                        config.secondary_color = j["secondary_color"].s();
                    if (j.has("bg_color")) 
                        config.bg_color = j["bg_color"].s();
                }
            }
            
            // 如果未设置管理员密码，使用默认密码
            if (config.admin_password.empty()) {
                config.admin_password = "admin123";
                std::cout << "[Web] 警告：使用默认管理员密码: admin123" << std::endl;
                std::cout << "[Web] 请在 settings.json 中设置 admin_password" << std::endl;
            }
            
            return config;
        }
    };

    WebServer(std::vector<std::string>* playlist, std::atomic<size_t>* current_track,
          StreamServer* stream_server, AudioPlayer* audio_player, std::mutex* playlist_mutex)
    : config_(Config::load_from_settings()),
      playlist_(playlist), current_track_(current_track),
      stream_server_(stream_server), audio_player_(audio_player),
      playlist_mutex_(playlist_mutex), running_(false) {
// 简单认证系统 - 直接使用 SessionManager
        session_manager_ = std::make_unique<SessionManager>();
        
        std::cout << "[Web] 加载配置文件成功" << std::endl;
        std::cout << "[Web] 管理员密码已配置" << std::endl;
        std::cout << "[Web] 允许游客切歌: " << (config_.allow_guest_skip ? "是" : "否") << std::endl;
    }
    
    ~WebServer() {
        stop();
    }
    
    bool start() {
        if (running_) return false;
        
        setup_routes();
        
        running_ = true;
        
        thread_ = std::thread([this]() {
            try {
                std::cout << "[Web] 服务器启动在端口 " << WebServer::Config::WEB_PORT << std::endl;
                app_.port(WebServer::Config::WEB_PORT).multithreaded().run();
            } catch (const std::exception& e) {
                std::cerr << "[Web] 错误: " << e.what() << std::endl;
            }
            running_ = false;
        });
        
        return true;
    }
    
    void stop() {
        if (!running_.exchange(false)) return;
        
        app_.stop();
        if (thread_.joinable()) thread_.join();
        
        std::cout << "[Web] 服务器已停止" << std::endl;
    }

private:
    // 辅助函数：替换字符串中所有的指定内容
    static void replace_all(std::string& str, const std::string& from, const std::string& to) {
        if(from.empty()) return;
        size_t start_pos = 0;
        while((start_pos = str.find(from, start_pos)) != std::string::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
    }
    
    // 读取HTML模板并替换变量
    std::string render_template(const std::string& filename, 
                       const std::map<std::string, std::string>& context = {},
                       bool is_admin = false) {
        std::string template_path;
        
        // 优先在当前目录查找模板
        template_path = filename;
        std::ifstream html_file(template_path);
        
        if (!html_file.is_open()) {
            // 如果在当前目录找不到，尝试在templates目录查找
            template_path = "templates/" + filename;
            html_file = std::ifstream(template_path);
        }
        
        if (!html_file.is_open()) {
            throw std::runtime_error("无法打开模板文件: " + filename + " 或 templates/" + filename);
        }
        
        std::stringstream ss;
        ss << html_file.rdbuf();
        std::string html_content = ss.str();
        
        // 基本配置替换
        replace_all(html_content, "{{STATION_NAME}}", config_.station_name);
        replace_all(html_content, "{{SUBTITLE}}", config_.subtitle);
        replace_all(html_content, "{{PRIMARY_COLOR}}", config_.primary_color);
        replace_all(html_content, "{{SECONDARY_COLOR}}", config_.secondary_color);
        replace_all(html_content, "{{BG_COLOR}}", config_.bg_color);
        
        // 权限相关的替换
        replace_all(html_content, "{{IS_ADMIN}}", is_admin ? "true" : "false");
        
        // 用户定义的上下文变量
        for (const auto& [key, value] : context) {
            replace_all(html_content, "{{" + key + "}}", value);
        }
        
        return html_content;
    }
    
    void setup_routes() {
        // 简单的认证检查函数
        auto is_authenticated = [this](const crow::request& req) -> bool {
            // 简化认证逻辑，稍后实现
            return false;
        };
        
        // 主页 - 根据是否登录显示不同界面
        CROW_ROUTE(app_, "/")([this](const crow::request& req) {
            bool is_admin = false; // 暂时默认为非管理员

            try {
                if (is_admin) {
                    // 管理员显示管理面板
                    auto admin_context = std::map<std::string, std::string>{
                        {"CLIENT_COUNT", std::to_string(stream_server_->client_count())}
                    };
                    
                    // 获取播放列表信息
                    std::lock_guard<std::mutex> lock(*playlist_mutex_);
                    admin_context["TRACK_COUNT"] = std::to_string(playlist_->size());
                    admin_context["CURRENT_TRACK"] = std::to_string(current_track_->load() + 1);
                    
                    return crow::response(render_template("admin_panel.html", admin_context, true));
                } else {
                    // 普通用户显示收听界面
                    return crow::response(render_template("index.html", {}, false));
                }
            } catch (const std::exception& e) {
                // 如果模板不存在，返回错误
                return crow::response(500, std::string("模板错误: ") + e.what());
            }
        });
        
        // 管理员登录页面
        CROW_ROUTE(app_, "/admin")([this](const crow::request& req) {
            bool is_admin = false;

            if (is_admin) {
                // 如果已经登录，重定向到管理面板
                crow::response res(302);
                res.set_header("Location", "/");
                return res;
            }
            
            try {
                return crow::response(render_template("admin_login.html", {}, false));
            } catch (const std::exception& e) {
                // 如果模板不存在，返回简单登录页面
                std::string simple_login = R"html(
<!DOCTYPE html>
<html>
<head><title>管理员登录</title><style>body{font-family:Arial;text-align:center;padding:50px}</style></head>
<body>
    <h1>管理员登录</h1>
    <div style="max-width:300px;margin:20px auto;">
        <input type="password" id="password" placeholder="密码" style="padding:10px;width:100%;margin:10px 0;">
        <button onclick="login()" style="padding:10px 20px;background:#764ba2;color:white;border:none;cursor:pointer;width:100%;">登录</button>
    </div>
    <script>
    async function login() {
        const password = document.getElementById('password').value;
        const response = await fetch('/admin/login', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({password: password})
        });
        if (response.ok) window.location.href = '/';
        else alert('密码错误');
    }
    </script>
</body>
</html>
                )html";
                replace_all(simple_login, "{{STATION_NAME}}", config_.station_name);
                return crow::response(simple_login);
            }
        });
        
        // 登录API
        CROW_ROUTE(app_, "/admin/login").methods("POST"_method)([this](const crow::request& req) {
            try {
                auto j = crow::json::load(req.body);
                if (!j || !j.has("password")) {
                    return crow::response(400, "缺少密码参数");
                }
                
                std::string password = j["password"].s();
                if (config_.admin_password == password) {
                    auto session = session_manager_->create_admin_session();
                    crow::response res(200);
                    res.set_header("Set-Cookie", 
                        "session_id=" + session->session_id + 
                        "; Path=/; HttpOnly; Max-Age=86400; SameSite=Lax");
                    res.write("登录成功");
                    return res;
                }
                return crow::response(401, "密码错误");
            } catch (const std::exception& e) {
                return crow::response(400, std::string("请求格式错误: ") + e.what());
            }
        });
        
        // 登出API
        CROW_ROUTE(app_, "/admin/logout").methods("POST"_method)([this](const crow::request& req) {
            // 从Cookie中获取session_id
            // 简化认证逻辑
            // session_id = get_session_id_from_cookies(req.get_header_value("Cookie"));
            // session_manager_->destroy_session(session_id);
            
            crow::response res(200);
            res.set_header("Set-Cookie", 
                "session_id=; Path=/; HttpOnly; Max-Age=0; SameSite=Lax");
            res.write("登出成功");
            return res;
        });
        
        // 公开API：播放列表信息
        CROW_ROUTE(app_, "/api/playlist")([this]() {
            std::lock_guard<std::mutex> lock(*playlist_mutex_);
            crow::json::wvalue result;
            result["playlist"] = *playlist_;
            result["current"] = (int)current_track_->load();
            return crow::response(result);
        });
        
        // 公开API：统计信息
        CROW_ROUTE(app_, "/api/stats")([this]() {
            crow::json::wvalue result;
            result["clients"] = (int)stream_server_->client_count();
            return crow::response(result);
        });
        
        // 需要权限的API：上传文件（仅管理员）
        CROW_ROUTE(app_, "/upload").methods("POST"_method)([this](const crow::request& req) {
            bool is_admin = false;
            if (!is_admin) return crow::response(403, "需要管理员权限");
            
            // 检查上传大小
            size_t content_length = 0; // req.content_length not available, use 0 for now
            if (content_length > Config::MAX_UPLOAD_SIZE) {
                return crow::response(413, "文件太大，最大50MB");
            }
            
            return handle_upload(req);
        });
        
        // 需要权限的API：下一首（如果允许游客切歌，则游客也可以使用）
        CROW_ROUTE(app_, "/api/next").methods("POST"_method)([this](const crow::request& req) {
            bool is_admin = false; // 暂时简化认证
            if (!is_admin && !config_.allow_guest_skip) {
                return crow::response(403, "需要登录才能执行此操作");
            }
            
            // 权限检查通过，执行切歌
            std::lock_guard<std::mutex> lock(*playlist_mutex_);
            if (playlist_->empty()) return crow::response{400, "播放列表为空"};
            
            size_t new_index = (current_track_->load() + 1) % playlist_->size();
            current_track_->store(new_index);
            audio_player_->skip_current_track(); // load_file is not implemented, use skip_current_track(*playlist_)[new_index]);
            
            return crow::response{200, "跳到下一首"};
        });
        
        // 需要权限的API：上一首（如果允许游客切歌，则游客也可以使用）
        CROW_ROUTE(app_, "/api/prev").methods("POST"_method)([this](const crow::request& req) {
            bool is_admin = false;
            if (!is_admin && !config_.allow_guest_skip) {
                return crow::response(403, "需要登录才能执行此操作");
            }
            
            // 权限检查通过，执行切歌
            std::lock_guard<std::mutex> lock(*playlist_mutex_);
            if (playlist_->empty()) return crow::response{400, "播放列表为空"};
            
            size_t size = playlist_->size();
            size_t new_index = (current_track_->load() + size - 1) % size;
            current_track_->store(new_index);
            audio_player_->skip_current_track(); // load_file is not implemented, use skip_current_track(*playlist_)[new_index]);
            
            return crow::response{200, "跳到上一首"};
        });
        
        // 需要权限的API：播放指定歌曲（如果允许游客切歌，则游客也可以使用）
        CROW_ROUTE(app_, "/api/play/<int>").methods("POST"_method)([this](const crow::request& req, int idx) {
            bool is_admin = false;
            if (!is_admin && !config_.allow_guest_skip) {
                return crow::response(403, "需要登录才能执行此操作");
            }
            
            // 权限检查通过，执行切歌
            std::lock_guard<std::mutex> lock(*playlist_mutex_);
            if (playlist_->empty()) return crow::response{400, "播放列表为空"};
            
            size_t index = static_cast<size_t>(idx);
            if (index >= playlist_->size()) return crow::response{400, "索引超出范围"};
            
            current_track_->store(index);
            audio_player_->skip_current_track(); // load_file is not implemented, use skip_current_track(*playlist_)[index]);
            
            return crow::response{200, "播放歌曲: " + std::to_string(index)};
        });
        
        // 需要权限的API：删除歌曲（仅管理员）
        CROW_ROUTE(app_, "/api/delete/<int>").methods("POST"_method)([this](const crow::request& req, int idx) {
            bool is_admin = false;
            if (!is_admin) return crow::response(403, "需要管理员权限");
            
            std::lock_guard<std::mutex> lock(*playlist_mutex_);
            if (playlist_->empty()) return crow::response{400, "播放列表为空"};
            
            size_t index = static_cast<size_t>(idx);
            if (index >= playlist_->size()) return crow::response{400, "索引超出范围"};
            
            playlist_->erase(playlist_->begin() + index);
            
            // 调整当前播放索引
            size_t current = current_track_->load();
            if (current >= playlist_->size()) {
                current_track_->store(0);
                if (!playlist_->empty()) {
                    audio_player_->skip_current_track(); // load_file is not implemented, use skip_current_track(*playlist_)[0]);
                }
            }
            
            return crow::response{200, "删除成功"};
        });
    }
    
    // 文件上传处理函数（从原始代码中保留）
    crow::response handle_upload(const crow::request& req) {
        auto boundary_info = req.headers.find("Content-Type");
        if (boundary_info == req.headers.end()) return crow::response(400, "缺少Content-Type");
        
        std::string content_type = boundary_info->second;
        auto boundary_pos = content_type.find("boundary=");
        if (boundary_pos == std::string::npos) return crow::response(400, "无效的Content-Type");
        std::string boundary = content_type.substr(boundary_pos + 9);
        
        std::istringstream body_stream(req.body);
        std::string line;
        size_t total_read = 0;
        
        while (std::getline(body_stream, line)) {
            total_read += line.length() + 1;
            if (line.find("Content-Disposition: form-data;") != std::string::npos &&
                line.find("name=\"file\"") != std::string::npos) {
                // 跳过两行空白行
                std::getline(body_stream, line); total_read += line.length() + 1; // 跳过空行
                std::getline(body_stream, line); total_read += line.length() + 1;
                
                // 读取文件名
                auto filename_start = line.find("filename=\"");
                if (filename_start == std::string::npos) continue;
                filename_start += 10;
                auto filename_end = line.find("\"", filename_start);
                std::string filename = line.substr(filename_start, filename_end - filename_start);
                
                // 读取文件数据
                std::vector<char> file_data;
                while (std::getline(body_stream, line) && line != "--" + boundary + "--") {
                    total_read += line.length() + 1;
                    if (line.find("--" + boundary) == 0) break;
                    
                    line += "\n"; // 恢复被getline移除的换行符
                    file_data.insert(file_data.end(), line.begin(), line.end());
                }
                
                // 移除末尾多余的换行符
                while (!file_data.empty() && (file_data.back() == '\n' || file_data.back() == '\r')) {
                    file_data.pop_back();
                }
                
                // 保存文件
                if (file_data.empty()) {
                    return crow::response(400, "上传的文件为空");
                }
                
                // 检查扩展名
                bool supported = false;
                for (const auto& ext : Config::SUPPORTED_FORMATS) {
                    if (filename.size() >= ext.size() && 
                        filename.compare(filename.size() - ext.size(), ext.size(), ext) == 0) {
                        supported = true;
                        break;
                    }
                }
                
                if (!supported) {
                    std::string supported_formats;
                    for (const auto& ext : Config::SUPPORTED_FORMATS) supported_formats += ext + " ";
                    return crow::response(400, "不支持的文件格式，支持: " + supported_formats);
                }
                
                std::ofstream out_file(filename, std::ios::binary);
                if (!out_file) return crow::response(500, "无法创建文件");
                out_file.write(file_data.data(), file_data.size());
                out_file.close();
                
                // 添加到播放列表
                std::lock_guard<std::mutex> lock(*playlist_mutex_);
                playlist_->push_back(filename);
                
                // 如果是第一首歌，开始播放
                if (playlist_->size() == 1) {
                    current_track_->store(0);
                    audio_player_->skip_current_track(); // load_file is not implemented, use skip_current_trackfilename);
                }
                
                return crow::response(200, "上传成功: " + filename);
            }
        }
        
        return crow::response(400, "未找到文件数据");
    }

private:
    Config config_;
    std::unique_ptr<SessionManager> session_manager_;
    crow::App<> app_;
    std::thread thread_;
    std::atomic<bool> running_;
    
    // 其他成员变量保持不变
    std::vector<std::string>* playlist_;
    std::atomic<size_t>* current_track_;
    StreamServer* stream_server_;
    AudioPlayer* audio_player_;
    std::mutex* playlist_mutex_;
};

// 在类定义外部定义静态成员
const std::vector<std::string> WebServer::Config::SUPPORTED_FORMATS = {".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac"};

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
            "║        Rakuraku启动中...        ║\n"
            "╚══════════════════════════════════════════╝\n" << std::endl;
        
        // 启动各个组件
        bool success = true;
        
        stream_server_ = std::make_unique<StreamServer>(&buffer_);
        audio_player_ = std::make_unique<AudioPlayer>(&buffer_, &playlist_, &current_track_, &playlist_mutex_);
        web_server_   = std::make_unique<WebServer>(&playlist_, &current_track_, 
                                                    stream_server_.get(), audio_player_.get(), &playlist_mutex_);
        
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
    
    // 在 RadioServer 类中：
  void stop() {
    if (!running_.exchange(false)) return;

    std::cout << "[System] 正在停止所有服务..." << std::endl;
    
    // 1. 先停 Web 服务器（它通常是阻塞主线程的元凶）
    if (web_server_) web_server_->stop(); 
    
    // 2. 停止音频播放（停止 FFmpeg 管道）
    if (audio_player_) audio_player_->stop();
    
    // 3. 停止流服务器（断开所有连接）
    if (stream_server_) stream_server_->stop();
    
    std::cout << "[System] 服务器已停止" <<std::endl;
   } // 确保这里括号对应
    
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
