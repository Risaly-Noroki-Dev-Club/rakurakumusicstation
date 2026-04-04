#define CROW_ENABLE_ASIO
#define ASIO_STANDALONE

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
        std::string cmd = "ffmpeg -re -v error -i \"" + filename + "\" "
                  "-vn -codec:a libmp3lame -b:a 128k -ar 44100 -ac 2 -f mp3 -";
        
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
          StreamServer* stream_server, AudioPlayer* audio_player, std::mutex* playlist_mutex)
    : playlist_(playlist), current_track_(current_track),
      stream_server_(stream_server), audio_player_(audio_player),
      playlist_mutex_(playlist_mutex), running_(false) {}
    
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
    CROW_ROUTE(app_, "/")
([this]() {
    std::ifstream file("web/index.html");
    if (!file.is_open()) {
        return crow::response(404, "index.html not found");
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    crow::response res(buffer.str());
    res.set_header("Content-Type", "text/html; charset=utf-8");
    return res;
});
		
	// 提供 CSS 文件
	CROW_ROUTE(app_, "/css/<string>")
	([this](const std::string& filename) {
	    std::string filepath = "web/css/" + filename;
	    std::ifstream file(filepath, std::ios::binary);
	    if (!file.is_open()) {
	        return crow::response(404, "CSS file not found");
	    }
	    std::stringstream buffer;
	    buffer << file.rdbuf();
	    crow::response res(buffer.str());
	    res.set_header("Content-Type", "text/css");
	    return res;
	});
	
	// 提供 JS 文件
	CROW_ROUTE(app_, "/js/<string>")
	([this](const std::string& filename) {
	    std::string filepath = "web/js/" + filename;
	    std::ifstream file(filepath, std::ios::binary);
	    if (!file.is_open()) {
	        return crow::response(404, "JS file not found");
	    }
	    std::stringstream buffer;
	    buffer << file.rdbuf();
	    crow::response res(buffer.str());
	    res.set_header("Content-Type", "application/javascript");
	    return res;
	});
	
	// API: 播放列表
        CROW_ROUTE(app_, "/api/playlist")
        ([this]() {
            std::lock_guard<std::mutex> lock(*playlist_mutex_);
            crow::json::wvalue result;
            result["playlist"] = *playlist_;
            result["current"] = current_track_->load();
            return result;
        });
        
        // API: 播放指定曲目
        CROW_ROUTE(app_, "/api/play/<int>")
        ([this](int index) {
            std::lock_guard<std::mutex> lock(*playlist_mutex_);
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
                std::lock_guard<std::mutex> lock(*playlist_mutex_);
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
                    std::lock_guard<std::mutex> lock(*playlist_mutex_);
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
    std::mutex* playlist_mutex_;
    
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
