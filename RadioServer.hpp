#include <crow.h>
#include <libshout/shout.h>
#include <sys/epoll.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <fstream>
#include <vector>
#include <queue>
#include <unordered_map>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

// 全局状态管理 (线程安全)
struct RadioState {
    std::mutex playlist_mutex;
    std::vector<std::string> playlist;
    std::atomic<size_t> current_track{0};
    std::atomic<bool> running{true};
    
    // 流媒体客户端管理
    std::mutex client_mutex;
    std::unordered_map<int, shout_t*> clients;
} state;

// 流媒体服务器核心 (端口2241)
void stream_server() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(2241);
    addr.sin_addr.s_addr = INADDR_ANY;

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 128);
    
    // 初始化epoll
    int epoll_fd = epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    const int MAX_EVENTS = 64;
    epoll_event events[MAX_EVENTS];

    while (state.running) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 100);
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == server_fd) {
                // 处理新客户端
                sockaddr_in client_addr{};
                socklen_t len = sizeof(client_addr);
                int client_fd = accept(server_fd, (sockaddr*)&client_addr, &len);
                
                shout_t* shout = shout_new();
                shout_set_host(shout, "localhost");
                shout_set_port(shout, 2241);
                shout_set_password(shout, "pass"); // 鉴权密码
                shout_set_format(shout, SHOUT_FORMAT_MP3);
                
                if (SHOUTERR_SUCCESS == shout_open(shout)) {
                    std::lock_guard<std::mutex> lock(state.client_mutex);
                    state.clients[client_fd] = shout;
                    
                    // 发送协议头
                    shout_send_raw(shout, "ICY 200 OK\r\n", 12);
                    
                    // 配置epoll监听
                    ev.events = EPOLLIN | EPOLLERR;
                    ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
                } else {
                    shout_free(shout);
                    close(client_fd);
                }
            } else {
                // 处理客户端通信（当前仅保持连接）
                // 后续添加元数据更新流
            }
        }
    }
    close(server_fd);
}

// FFmpeg音频处理管道
void audio_pipeline() {
    std::string current_file;
    while (state.running) {
        // 获取当前播放文件
        {
            std::lock_guard<std::mutex> lock(state.playlist_mutex);
            if (state.current_track >= state.playlist.size()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            current_file = state.playlist[state.current_track];
        }
        
        // FFmpeg转码命令（48K MP3示例）
        std::string cmd = "ffmpeg -i \"" + current_file + "\" -c:a libmp3lame -b:a 192k -f mp3 -";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) continue;
        
        // 读取转码数据广播
        char buffer[65536];
        size_t bytes;
        while ((bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
            std::lock_guard<std::mutex> lock(state.client_mutex);
            for (auto& [fd, shout] : state.clients) {
                shout_send_raw(shout, buffer, bytes);
                shout_sync(shout); // 流同步控制
            }
        }
        pclose(pipe);
        
        // 播放下一曲
        state.current_track = (state.current_track + 1) % state.playlist.size();
    }
}

// Web界面服务器 (端口2240)
void web_server() {
    crow::SimpleApp app;
    
    // 播放器HTML页面
    CROW_ROUTE(app, "/player")
    ([]{
        crow::mustache::context ctx;
        return crow::mustache::load("player.html").render();
    });
    
    // 文件上传处理
    CROW_ROUTE(app, "/upload").methods("POST"_method)
    ([](const crow::request& req) {
        auto files = req.file_list();
        {
            std::lock_guard<std::mutex> lock(state.playlist_mutex);
            for (auto& file : files) {
                // 保存文件到media目录
                std::ofstream fout("./media/" + file.filename, std::ios::binary);
                fout.write(file.content.data(), file.content.length());
                state.playlist.push_back(file.filename);
            }
        }
        return crow::response(200);
    });
    
    // 播放控制API
    CROW_ROUTE(app, "/control/<int>")
    ([](size_t index) {
        if (index < state.playlist.size()) {
            state.current_track = index;
            return crow::response(200);
        }
        return crow::response(404);
    });
    
    // 启动服务器
    app.port(2240).multithreaded().run();
}

int main() {
    // 初始化libshout
    shout_init();
    
    // 创建核心线程
    std::thread streamer(stream_server);
    std::thread ffmpeg(audio_pipeline);
    
    // Web服务在主线程运行
    web_server();
    
    // 停止信号处理
    state.running = false;
    streamer.join();
    ffmpeg.join();
    
    shout_shutdown();
    return 0;
}
