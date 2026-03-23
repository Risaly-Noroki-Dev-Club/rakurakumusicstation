# RakurakuRadioServer

一个高性能、轻量级的 C++ 流媒体广播服务器，基于 Apache License 2.0 协议开源。

本项目专为低延迟音频分发设计，结合了 Linux 原生 epoll 事件驱动模型与现代化的 Web 管理界面。它能将本地媒体目录转化为一个实时流媒体电台，支持多客户端同步收听及远程文件管理。

## ✨ 功能特性

*   **双引擎架构**：
    *   **核心流引擎**：基于 epoll 的非阻塞 I/O，支持上千路并发连接，极低 CPU 占用。
    *   **Web 管理引擎**：基于 Crow 框架，提供 RESTful API 与交互式 UI。
*   **全能环境适配**：内置自动识别脚本，完美适配 Arch Linux 与 Debian/Ubuntu (含 WSL)。
*   **自动环境修复**：自动检测并配置 `zh_CN.UTF-8` 本地化环境，彻底解决因特殊字符文件名导致的 FFmpeg 管道崩溃 (`revents=16`) 问题。（感谢知夏）
*   **极致性能优化**：构建脚本默认开启 `-O3`、`-flto` 及 `-march=native` 硬件原生指令集优化。

## 🛠️ 技术栈

*   **内核**：C++17 标准
*   **底层 I/O**：Linux epoll, POSIX Threads, Pipes
*   **Web 框架**：Crow
*   **编解码后端**：FFmpeg
*   **加密/通讯**： Libcrypto

## 🚀 快速开始

### 1. 一键构建 (推荐)

为了简化操作，项目提供了一个全能构建脚本。无论你在 Arch 还是 Ubuntu 上，它都会自动处理所有依赖。

```bash
# 赋予脚本执行权限
chmod +x build.sh

# 执行自动化构建
./build.sh
```

脚本会自动完成以下操作：
*   识别发行版并安装 `g++`, `openssl`, `boost`, `ffmpeg` 等依赖。
*   生成并配置中文语言环境 (Locale)。
*   执行 Release 级优化编译。
*   在 `dist/` 目录下生成完整的运行环境。

### 2. 部署与运行

构建完成后，所有产物都集中在 `dist` 文件夹中：

```bash
cd dist

# 1. 放入音乐文件（支持 .mp3, .m4a, .flac, .wav 等）
# 脚本已为你自动创建了 media 目录
cp /path/to/your/music/* ./media/

# 2. 使用管理脚本启动
./start.sh
```

### 3. 访问方式

*   **Web 管理平台**: `http://你的IP:2240` (点歌、监控状态、上传文件)
*   **音频流挂载点**: `http://你的IP:2241` (使用浏览器、VLC 或 PotPlayer 打开此链接即可收听)

## 📂 项目架构

*   **StreamServer**: 负责维护客户端 Socket 队列，通过 epoll 轮询实现高效的数据分发。
*   **AudioPlayer**: 负责读取音频文件，通过 popen 管道调用 FFmpeg。已优化为 `-re` 实时流模式，提供更平滑的听感。
*   **RingBuffer**: 生产者-消费者模型缓存，平衡 FFmpeg 解码速度与网络传输波动的差值。

## ⚠️ 疑难解答 (Troubleshooting)

**Q: 遇到 `Pipe error: revents=16` 怎么办？**

**A:** 这通常是由于文件名包含特殊字符（如 `/`, `()`, `【】`）且系统缺少中文 Locale 导致的。
*   最新版本的 `build.sh` 会自动修复此问题。
*   请确保通过 `dist/start.sh` 启动程序，它会强制注入 UTF-8 环境变量。

## 📜 开源协议

本项目采用 Apache License 2.0 协议开源。详情请参阅项目中的 LICENSE 文件。

## 🤝 鸣谢

本项目在开发过程中得到了许多支持，在此特别鸣谢 **Cynun**。
感谢 ta 提供的技术支持与灵感。

Copyright © 2026. Licensed under the Apache License, Version 2.0.