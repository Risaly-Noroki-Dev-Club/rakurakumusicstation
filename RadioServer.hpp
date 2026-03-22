#include <crow.h>
#include <sys/epoll.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <fstream>
#include <vector>
#include <queue>
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

namespace fs = std::filesystem;

class BroadcastRing {
public:
    explicit BroadcastRing(size_t cap = 256 * 1024) 
        : capacity_(cap), mask_(cap - 1), buffer_(cap) 
    {
        if ((cap & mask_) != 0) {
            throw std::runtime_error("Capacity must be power of two");
        }
    }

    // 生产者写入
    void push(const char* data, size_t len) {
        if (len == 0 || len > capacity_) return;
        
        size_t current_wp = write_pos_.load(std::memory_order_relaxed);
        size_t new_wp = (current_wp + len) & mask_;
        
        // 环形写入
        size_t first_seg = std::min(len, capacity_ - (current_wp & mask_));
        std::memcpy(&buffer_[current_wp & mask_], data, first_seg);
        if (first_seg < len) {
            std::memcpy(&buffer_[0], data + first_seg, len - first_seg);
        }
        
        // 原子更新写位置
        write_pos_.store(new_wp, std::memory_order_release);
        
        // 新数据
        notify_counter_.fetch_add(1, std::memory_order_relaxed);
        cv_.notify_all();
    }

    // 不修改全局状态，返回可用数据量
    size_t read(size_t& consume_pos, char* dest, size_t max_len) const {
        size_t wp = write_pos_.load(std::memory_order_acquire);
        size_t rp = consume_pos;

        if (rp == wp) return 0;
        
        // 计算可用数据
        size_t avail = (wp > rp) ? (wp - rp) : (capacity_ - rp + wp);
        size_t to_read = std::min(avail, max_len);
        
        // 环形读取
        size_t first_seg = std::min(to_read, capacity_ - (rp & mask_));
        std::memcpy(dest, &buffer_[rp & mask_], first_seg);
        if (first_seg < to_read) {
            std::memcpy(dest + first_seg, &buffer_[0], to_read - first_seg);
        }
        
        // 更新独立位置
        consume_pos = (rp + to_read) & mask_;
        return to_read;
    }

    // 等待新数据通知
    bool wait_for_data(uint64_t& last_counter, int timeout_ms = 100) {
        std::unique_lock lock(cv_mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return notify_counter_.load(std::memory_order_relaxed) != last_counter;
        });
    }

private:
    const size_t capacity_;
    const size_t mask_;
    std::vector<char> buffer_;
    
    std::atomic<size_t> write_pos_{0};
    std::atomic<uint64_t> notify_counter_{0};
    
    mutable std::mutex cv_mutex_;
    std::condition_variable cv_;
};

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
    ClientSession(int fd, BroadcastRing* broadcast)
        : fd_(fd)
        , broadcast_(broadcast)
        , consume_pos_(0)
        , shutdown_(false)
    {
        // 设置非阻塞 + TCP_NODELAY
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        
        int yes = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        
        // 准备 ICY 响应头
        prepare_header();
    }

    ~ClientSession() {
        shutdown();
    }

    void shutdown() {
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd() const { return fd_; }
    bool is_shutdown() const { return shutdown_; }

    // 填充发送缓冲区
    void fill_buffer() {
        if (send_buf_.empty()) {
            char audio[8192];
            size_t bytes = broadcast_->read(consume_pos_, audio, sizeof(audio));
            if (bytes > 0) {
                send_buf_.resize(bytes);
                std::memcpy(send_buf_.data(), audio, bytes);
            }
        }
    }

    // 处理可写事件 
    bool handle_write() {
        // 先发送头部
        if (!header_sent_) {
            const char* header = 
                "ICY 200 OK\r\n"
                "icy-name:My Radio\r\n"
                "icy-genre:Various\r\n"
                "icy-url:http://localhost:2241\r\n"
                "icy-metaint:16384\r\n"
                "icy-bitrate:192\r\n"
                "Content-Type:audio/mpeg\r\n"
                "\r\n";
            
            ssize_t sent = ::send(fd_, header, strlen(header), MSG_NOSIGNAL);
            if (sent > 0) {
                header_sent_ = true;
            } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return false;
            }
            return true;
        }
        
        // 发送音频数据
        if (send_buf_.empty()) {
            fill_buffer();
        }
        
        if (!send_buf_.empty()) {
            ssize_t sent = ::send(fd_, send_buf_.data(), send_buf_.size(), 
                                  MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent > 0) {
                send_buf_.erase(0, sent);
            } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return false;
            }
        }
        
        return true;
    }

private:
    int fd_;
    BroadcastRing* broadcast_;
    size_t consume_pos_;          // 消费者独立位置
    std::vector<char> send_buf_;  // 发送缓冲区
    std::atomic<bool> shutdown_;
    bool header_sent_{false};
};

struct RadioState {
    std::mutex playlist_mutex;
    std::vector<std::string> playlist;
    std::atomic<size_t> current_track{0};
    std::atomic<bool> running{true};
    std::atomic<bool> skip_track{false};
    
    std::mutex clients_mutex;
    std::unordered_map<int, std::shared_ptr<ClientSession>> clients;
    
    BroadcastRing broadcast{256 * 1024};
    
    int epoll_fd{-1};
    int server_fd{-1};
} state;

void signal_handler(int sig) {
    (void)sig;
    state.running = false;
}

// 流媒体服务器
void stream_server() {
    // 创建监听 socket
    state.server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (state.server_fd < 0) {
        perror("socket");
        return;
    }
    
    int reuse = 1;
    setsockopt(state.server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(2241);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(state.server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return;
    }
    
    if (listen(state.server_fd, 1024) < 0) {
        perror("listen");
        return;
    }
    
    // 设置非阻塞
    int flags = fcntl(state.server_fd, F_GETFL, 0);
    fcntl(state.server_fd, F_SETFL, flags | O_NONBLOCK);
    
    // 创建 epoll
    state.epoll_fd = epoll_create1(0);
    if (state.epoll_fd < 0) {
        perror("epoll_create1");
        return;
    }
    
    // 注册 server_fd
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = state.server_fd;
    epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.server_fd, &ev);
    
    const int MAX_EVENTS = 1024;
    epoll_event events[MAX_EVENTS];
    
    auto& bcast = state.broadcast;
    
    // 主事件循环
    while (state.running) {
        int n = epoll_wait(state.epoll_fd, events, MAX_EVENTS, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t evts = events[i].events;
            
            // 新连接
            if (fd == state.server_fd) {
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept(state.server_fd, (sockaddr*)&client_addr, &len);
                    
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }
                    
                    // 创建客户端会话
                    auto client = std::make_shared<ClientSession>(client_fd, &bcast);
                    
                    // 注册到 epoll
                    epoll_event client_ev{};
                    client_ev.events = EPOLLOUT | EPOLLRDHUP | EPOLLERR | EPOLLET;
                    client_ev.data.fd = client_fd;
                    
                    if (epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0) {
                        perror("epoll_ctl");
                        continue;
                    }
                    
                    // 保存客户端
                    {
                        std::lock_guard<std::mutex> lock(state.clients_mutex);
                        state.clients[client_fd] = client;
                    }
                }
                continue;
            }
            
            // 客户端事件
            if (evts & (EPOLLERR | EPOLLRDHUP | EPOLLHUP)) {
                // 断开连接
                {
                    std::lock_guard<std::mutex> lock(state.clients_mutex);
                    state.clients.erase(fd);
                }
                epoll_ctl(state.epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                continue;
            }
            
            // 可写事件
            if (evts & EPOLLOUT) {
                std::shared_ptr<ClientSession> client;
                {
                    std::lock_guard<std::mutex> lock(state.clients_mutex);
                    auto it = state.clients.find(fd);
                    if (it != state.clients.end()) {
                        client = it->second;
                    }
                }
                
                if (client && !client->handle_write()) {
                    std::lock_guard<std::mutex> lock(state.clients_mutex);
                    state.clients.erase(fd);
                    epoll_ctl(state.epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                }
            }
        }
        
        // 空闲时唤醒部分客户端尝试发送
        if (n == 0) {
            std::vector<int> need_wake;
            {
                std::lock_guard<std::mutex> lock(state.clients_mutex);
                for (auto& [fd, client] : state.clients) {
                    client->fill_buffer();
                }
            }
        }
    }
    
    // 清理
    close(state.server_fd);
    close(state.epoll_fd);
    
    {
        std::lock_guard<std::mutex> lock(state.clients_mutex);
        state.clients.clear();
    }
}

// =============================================================================
// 音频管道
// =============================================================================
void audio_pipeline() {
    signal(SIGPIPE, SIG_IGN);
    
    auto& bcast = state.broadcast;
    
    while (state.running) {
        std::string current_file;
        
        // 获取当前曲目
        {
            std::lock_guard<std::mutex> lock(state.playlist_mutex);
            if (state.playlist.empty() || 
                state.current_track >= state.playlist.size()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            current_file = "./media/" + state.playlist[state.current_track];
        }
        
        printf("Playing: %s\n", current_file.c_str());
        
        // 检查文件
        if (!fs::exists(current_file)) {
            printf("File not found: %s\n", current_file.c_str());
            state.current_track = (state.current_track + 1) % state.playlist.size();
            continue;
        }
        
        // FFmpeg 命令
        std::string cmd = "ffmpeg -v quiet -re -i \"" + current_file + "\" "
                          "-c:a libmp3lame -b:a 192k -f mp3 -";
        
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            perror("popen");
            state.current_track = (state.current_track + 1) % state.playlist.size();
            continue;
        }
        
        // 使用 poll 超时控制
        int pipe_fd = fileno(pipe);
        struct pollfd pfd = {pipe_fd, POLLIN, 0};
        
        char buffer[16384];
        state.skip_track = false;
        
        // 主循环
        while (!state.skip_track && state.running) {
            int ret = poll(&pfd, 1, 200);  // 200ms 超时
            
            if (ret > 0) {
                if (pfd.revents & POLLIN) {
                    ssize_t bytes = read(pipe_fd, buffer, sizeof(buffer));
                    
                    if (bytes > 0) {
                        bcast.push(buffer, bytes);
                    } else if (bytes == 0) {
                        break;  // 文件结束
                    }
                }
            } else if (ret < 0) {
                if (errno == EINTR) continue;
                perror("poll");
                break;
            }
            // 超时继续循环，检查 skip_track
        }
        
        pclose(pipe);
        
        // 切换下一首
        size_t next = (state.current_track + 1) % state.playlist.size();
        state.current_track.store(next, std::memory_order_release);
    }
}

// =============================================================================
// Web 服务器
// =============================================================================
void web_server() {
    crow::SimpleApp app;
    fs::create_directories("./media");
    
    // 首页
    CROW_ROUTE(app, "/")
    ([]{
        std::string html = R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>Radio Server</title>
    <style>
        body { font-family: Arial; max-width: 800px; margin: 50px auto; padding: 20px; }
        .track { padding: 10px; margin: 5px 0; background: #f0f0f0; cursor: pointer; }
        .track:hover { background: #e0e0e0; }
        .current { background: #90EE90; }
    </style>
</head>
<body>
    <h1>网络电台</h1>
    <audio id="player" controls>
        <source src="http://localhost:2241" type="audio/mpeg">
    </audio>
    <h2>播放列表</h2>
    <div id="playlist"></div>
    <h3>上传音乐</h3>
    <form action="/upload" method="post" enctype="multipart/form-data">
        <input type="file" name="file" accept="audio/*">
        <button type="submit">上传</button>
    </form>
    <script>
        function loadPlaylist() {
            fetch('/api/playlist')
                .then(r => r.json())
                .then(data => {
                    let html = '';
                    data.playlist.forEach((t, i) => {
                        html += '<div class="track" onclick="play('+i+')">'+t+'</div>';
                    });
                    document.getElementById('playlist').innerHTML = html;
                });
        }
        function play(i) { fetch('/api/play/'+i); }
        loadPlaylist();
        setInterval(loadPlaylist, 5000);
    </script>
</body>
</html>
)";
        return crow::response(html);
    });
    
    // API: 播放列表
    CROW_ROUTE(app, "/api/playlist")
    ([]{
        crow::json::wvalue result;
        std::lock_guard<std::mutex> lock(state.playlist_mutex);
        result["playlist"] = state.playlist;
        result["current"] = state.current_track.load();
        return result;
    });
    
    // API: 播放指定曲目
    CROW_ROUTE(app, "/api/play/<int>")
    ([](size_t index) {
        std::lock_guard<std::mutex> lock(state.playlist_mutex);
        if (index < state.playlist.size()) {
            state.skip_track = true;
            state.current_track = index;
            return crow::response("OK");
        }
        return crow::response(404, "Not found");
    });
    
    // API: 下一首
    CROW_ROUTE(app, "/api/next")
    ([]{
        state.skip_track = true;
        return crow::response("OK");
    });
    
    // API: 统计
    CROW_ROUTE(app, "/api/stats")
    ([]{
        crow::json::wvalue r;
        {
            std::lock_guard<std::mutex> lock(state.clients_mutex);
            r["clients"] = state.clients.size();
        }
        return r;
    });
    
    // 上传
    CROW_ROUTE(app, "/upload").methods("POST"_method)
    ([](const crow::request& req) {
        auto& file = req.files[0];
        std::string filename = file.filename;
        
        // 安全检查
        if (filename.find('/') != std::string::npos || 
            filename.find('\\') != std::string::npos ||
            filename.find("..") != std::string::npos) {
            return crow::response(400, "Invalid filename");
        }
        
        std::string ext = fs::path(filename).extension();
        if (ext != ".mp3" && ext != ".wav" && ext != ".flac" && 
            ext != ".ogg" && ext != ".m4a") {
            return crow::response(400, "Unsupported format");
        }
        
        std::string target = "./media/" + filename;
        std::ofstream out(target, std::ios::binary);
        if (out) {
            out.write(file.content.data(), file.content.length());
            out.close();
            
            {
                std::lock_guard<std::mutex> lock(state.playlist_mutex);
                state.playlist.push_back(filename);
            }
            return crow::response(200, "OK");
        }
        return crow::response(500, "Save failed");
    });
    
    printf("Web server running on http://localhost:2240\n");
    app.port(2240).multithreaded().run_async();
}

// =============================================================================
// Main
// =============================================================================
int main() {
    printf("=== Radio Server Starting ===\n");
    printf("Stream : http://localhost:2241\n");
    printf("Web    : http://localhost:2240\n");
    
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::thread t_streamer(stream_server);
    std::thread t_audio(audio_pipeline);
    
    web_server();  // 主线程运行
    
    // 等待退出
    state.running = false;
    
    t_streamer.join();
    t_audio.join();
    
    printf("=== Radio Server Stopped ===\n");
    return 0;
}
