## 📻 RakurakuRadioServer
一个高性能、轻量级的 C++ 流媒体广播服务器，基于 Apache License 2.0 协议开源。
本项目专为低延迟音频分发设计，结合了 Linux 原生 epoll 事件驱动模型与现代化的 Web 管理界面。它能将本地媒体目录转化为一个实时流媒体电台，支持多客户端同步收听及远程文件管理。
#### ✨ 功能特性
 * 双引擎架构：
   * 核心流引擎：基于 epoll 的非阻塞 I/O，支持上千路并发连接，极低 CPU 占用。
   * Web 管理引擎：基于 Crow 框架，提供 RESTful API 与交互式 UI。
 * 实时流化：调用 ffmpeg 动态转码，确保音频以恒定的比特率传输，适配各类播放器。
 * 智能播放列表：自动扫描 ./media 目录，支持动态更新及上传新曲目。
 * 优雅停机：完善的信号处理机制，确保在服务器关闭时正确释放 Socket 和文件句柄。

#### 🛠️ 技术栈
 * 内核：C++17 标准
 * 底层 I/O：Linux epoll, POSIX Threads, Pipes
 * Web 框架：Crow (Header-only)
 * 编解码后端：FFmpeg
 * 加密/通讯：OpenSSL, Libcrypto

#### 🚀 编译与运行
### 1. 准备环境
确保您的 Linux 环境已安装以下依赖：
Ubuntu/Debian
sudo apt update
sudo apt install g++ cmake libssl-dev ffmpeg

### 注意：请确保 crow_all.h 位于当前目录或系统包含路径中。
### 2. 编译
由于项目使用了 C++17 的文件系统库 (std::filesystem) 和多线程库，请使用以下编译命令：
g++ radioserver.cpp -o radioserver \
    -std=c++17 \
    -lpthread \
    -lssl \
    -lcrypto \
    -I. \
    -O3

-O3 选项可开启编译器优化，提升流处理性能。
### 3. 配置与启动
 * 创建媒体文件夹：mkdir -p media
 * 放置音频文件（支持 .mp3, .wav, .flac 等）。
 * 运行服务器：
   ./radioserver

### 4. 访问方式
 * Web 管理平台: http://你的IP:2240 (用于点歌、查看状态、上传文件)
 * 音频流挂载点: http://你的IP:2241 (直接在浏览器或 VLC 中打开此链接即可收听)

#### 📂 项目架构
 * StreamServer: 负责维护客户端 Socket 队列，通过 epoll 轮询实现高效的数据分发。
 * AudioPlayer: 负责读取音频文件，通过匿名管道 (popen) 将解码后的流推送到缓冲区。
 * RingBuffer: 生产者-消费者模型缓存，平衡解码速度与网络传输波动的差值。

#### 📜 开源协议
本项目采用 Apache License 2.0 协议开源。详情请参阅项目中的 LICENSE 文件。

#### 🤝 鸣谢 (Acknowledgments)
本项目在开发过程中得到了许多支持，在此特别鸣谢 Cynun。
感谢 ta 提供的想法支持。要是没有 Cynun，我完不成这个计划。
Copyright © 2026. Licensed under the Apache License, Version 2.0.
