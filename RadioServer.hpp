#pragma once
#include <crow.h>
#include <atomic>
#include <mutex>
#include <deque>
#include <vector>
#include <thread>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <fstream>

class RadioServer {
public:
    RadioServer();
    ~RadioServer();
    void start();

private:
    // Web 服务 (Crow)
    void startWebServer();
    void setupRoutes();
    
    // 流媒体服务
    void startStreamServer();
    void createEpoll();
    void handleConnections();
    void sendICYHeader(int fd);
    void sendMetadata(int fd, const std::string& title);
    void broadcastAudio(const uint8_t* data, size_t len);
    void closeClient(int fd);

    // FFmpeg 音频处理
    void startAudioPipeline();
    bool decodeAudioFile(const std::string& path);
    void encodeToMP3();

    // 播放状态管理
    struct PlayerState {
        std::atomic<int> listeners{0};
        std::string current_song;
        std::deque<std::string> playlist;
        std::mutex playlist_mutex;
    } player_state;

    // 网络参数
    static const int WEB_PORT = 2240;
    static const int STREAM_PORT = 2241;
    static const int MAX_EVENTS = 100;
    static const int META_INTERVAL = 8192;

    // Threads
    std::thread web_thread;
    std::thread stream_thread;
    std::thread audio_thread;
    std::atomic<bool> running{true};

    // Socket/Epoll
    int server_fd = -1;
    int epoll_fd = -1;

    // FFmpeg
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    FILE* mp3_pipe = nullptr;
};
