# 🎵 Rakuraku Music Station

A lightweight, high-performance C++ streaming broadcast server for low-latency audio distribution, with a built-in web admin panel.

一个轻量级、高性能的 C++ 流媒体广播服务器，支持低延迟音频分发，内置 Web 管理面板。

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)
![C++](https://img.shields.io/badge/C++-17-orange.svg)

**Languages / 语言**: [English](#english) · [中文](#中文)

---

## English

### Overview

Rakuraku Music Station broadcasts the same audio stream to every connected listener, radio-style. A single `ffmpeg`-backed decoder feeds a thread-safe ring buffer, and a Linux `epoll` loop fans the bytes out to HTTP clients on port **2241**. The Crow-based web panel on port **2240** handles playlist management, uploads, and playback control.

### Features

- **High performance** — non-blocking I/O via Linux `epoll`; many concurrent listeners on one thread.
- **Radio-style broadcast** — a single decoder feeds a shared ring buffer; every client hears the same stream in sync.
- **Format support** — MP3, WAV, FLAC, OGG, M4A, AAC via FFmpeg.
- **Hot-reload playlist** — uploaded files appear in the playlist immediately; no restart needed.
- **Metadata / cover / lyrics APIs** — per-track endpoints for title, artist, duration, embedded cover art, and lyrics.
- **Session auth** — cookie-based admin sessions; optional guest skip permission.
- **Templated UI** — `{{VAR}}` substitution from `settings.json` (station name, colors, subtitle).
- **One-shot build** — `build_release.sh` installs deps, fetches Crow, and emits a self-contained `dist/`.

### Requirements

- Linux (Arch, Debian, or Ubuntu — the build script auto-detects)
- GCC/G++ 7+ with C++17, or Clang
- FFmpeg, OpenSSL, Boost, Asio

### Quick Start

```bash
# Build — installs dependencies and produces dist/
./build_release.sh

# Add audio files (Chinese / Japanese filenames are fine)
cp /path/to/music/*.mp3 dist/media/

# Run
cd dist
./start.sh
```

Then open:

- Web admin: <http://localhost:2240>
- Audio stream: <http://localhost:2241> (VLC, browsers, any HTTP-capable player)

Stop with `./stop.sh`.

### Manual Build (Development)

```bash
# Debian/Ubuntu deps
sudo apt-get install build-essential ffmpeg libavcodec-extra libssl-dev \
                     libboost-all-dev libasio-dev wget locales

# Crow header
wget https://github.com/CrowCpp/Crow/releases/download/v1.3.2/crow_all.h

# Debug build
g++ radioserver.cpp metadata.cpp -o radioserver \
    -std=c++17 -g -O0 -lpthread -lssl -lcrypto -I.
```

### Configuration — `settings.json`

```json
{
    "station_name": "Rakuraku Music Station",
    "subtitle": "Your tagline here",
    "primary_color": "#764ba2",
    "secondary_color": "#667eea",
    "bg_color": "#f4f4f9",
    "admin_password": "change_me",
    "allow_guest_skip": false
}
```

- `admin_password` — falls back to `admin123` if unset. **Change it in production.**
- `allow_guest_skip` — if `true`, unauthenticated clients can POST `/api/next` and `/api/prev`.

### HTTP API

Public:

| Method | Path                   | Description                                    |
| ------ | ---------------------- | ---------------------------------------------- |
| GET    | `/`                    | Listener page (or admin panel if logged in)    |
| GET    | `/api/playlist`        | Playlist + per-track metadata                  |
| GET    | `/api/stats`           | Current listener count                         |
| GET    | `/api/metadata/<idx>`  | Full metadata for track `idx`                  |
| GET    | `/api/cover/<idx>`     | Embedded cover art (or placeholder)            |
| GET    | `/api/lyrics/<idx>`    | Lyrics, if available                           |

Admin (session cookie required):

| Method | Path                 | Description                         |
| ------ | -------------------- | ----------------------------------- |
| POST   | `/admin/login`       | `{ "password": "..." }` → cookie    |
| POST   | `/admin/logout`      | Destroy session                     |
| POST   | `/upload`            | Multipart upload (≤ 50 MB)          |
| POST   | `/api/next`          | Skip forward (guest-allowed opt)    |
| POST   | `/api/prev`          | Skip backward (guest-allowed opt)   |
| POST   | `/api/play/<idx>`    | Jump to track `idx`                 |
| POST   | `/api/delete/<idx>`  | Remove track `idx`                  |

### Architecture

| Component          | Role                                                                  |
| ------------------ | --------------------------------------------------------------------- |
| `StreamServer`     | `epoll` loop fanning out audio bytes to listeners on port 2241        |
| `AudioPlayer`      | Spawns FFmpeg with `-re`, pipes decoded audio into the buffer         |
| `BroadcastBuffer`  | Power-of-two ring buffer; one producer, many consumers                |
| `WebServer`        | Crow app on port 2240 serving UI and REST APIs                        |
| `SessionManager`   | In-memory session table with 24-hour expiry                           |
| `AuthMiddleware`   | Crow middleware that resolves `session_id` cookies to admin state     |

### Ports

- `2240` — web admin / API
- `2241` — audio stream

### Project Layout

```
├── radioserver.cpp        # Main server
├── metadata.{hpp,cpp}     # Audio metadata extraction
├── authmiddleware.hpp     # Crow auth middleware
├── sessionmanager.hpp     # Session store
├── build_release.sh       # One-shot build script
├── settings.json          # Runtime config
├── templates/             # HTML templates (optional)
└── dist/                  # Build output
    ├── radioserver
    ├── start.sh / stop.sh
    ├── media/             # Audio files live here
    └── templates/
```

### Troubleshooting

- **Garbled non-ASCII filenames** — regenerate locale: `sudo locale-gen zh_CN.UTF-8 && sudo update-locale LANG=zh_CN.UTF-8`, or start via `./start.sh` which sets it for you.
- **Port already in use** — `ss -ltnp | grep -E ':2240|:2241'` and free the port, or edit `Config::WEB_PORT` / `Config::STREAM_PORT` in `radioserver.cpp`.
- **Pipe error `revents=16`** — usually a locale issue; see above.
- **No audio plays** — confirm `ffmpeg` is on `PATH` and the file extension is in the supported list.

Logs live at `dist/server.log`.

### Security Notes

- Change `admin_password` before exposing the server.
- Sessions are cookie-only (`HttpOnly`, `SameSite=Lax`) — put a TLS-terminating proxy in front for public deployments.
- Uploads cap at 50 MB and reject unsupported extensions, but the admin endpoint is your trust boundary.

### License

MIT — see [LICENSE](LICENSE).

### Credits

- [Crow](https://github.com/CrowCpp/Crow) — header-only C++ web framework
- [FFmpeg](https://ffmpeg.org/) — audio decoding
- [Boost](https://www.boost.org/) / [Asio](https://think-async.com/Asio/) — networking primitives

---

## 中文

### 概述

Rakuraku Music Station 以电台的方式，将同一路音频流推送给所有连接的听众。由 `ffmpeg` 驱动的单一解码器向线程安全的环形缓冲区写入数据，Linux `epoll` 循环把字节分发到 **2241** 端口上的 HTTP 客户端；**2240** 端口上基于 Crow 的 Web 面板负责播放列表管理、上传与播控。

### 特性

- **高性能** — 基于 `epoll` 的非阻塞 I/O，一个线程即可承载大量并发听众。
- **电台式广播** — 单一解码器写入共享环形缓冲区，所有客户端同步听到同一路流。
- **多格式支持** — 通过 FFmpeg 支持 MP3、WAV、FLAC、OGG、M4A、AAC。
- **热重载播放列表** — 上传文件后立即出现在播放列表中，无需重启。
- **元数据 / 封面 / 歌词 API** — 按曲目提供标题、艺术家、时长、内嵌封面和歌词接口。
- **会话认证** — 基于 Cookie 的管理员会话，可选开放游客切歌权限。
- **模板化 UI** — 从 `settings.json` 注入 `{{VAR}}` 变量（台名、颜色、副标题）。
- **一键构建** — `build_release.sh` 自动安装依赖、拉取 Crow，并生成独立的 `dist/`。

### 系统要求

- Linux（Arch / Debian / Ubuntu，构建脚本自动识别）
- 支持 C++17 的 GCC/G++ 7+ 或 Clang
- FFmpeg、OpenSSL、Boost、Asio

### 快速开始

```bash
# 构建 — 自动安装依赖并生成 dist/
./build_release.sh

# 放入音频文件（中文 / 日文文件名都支持）
cp /path/to/music/*.mp3 dist/media/

# 启动
cd dist
./start.sh
```

访问：

- Web 管理：<http://localhost:2240>
- 音频流：<http://localhost:2241>（VLC、浏览器或任意支持 HTTP 的播放器）

使用 `./stop.sh` 停止服务。

### 手动构建（开发）

```bash
# Debian/Ubuntu 依赖
sudo apt-get install build-essential ffmpeg libavcodec-extra libssl-dev \
                     libboost-all-dev libasio-dev wget locales

# 下载 Crow 头文件
wget https://github.com/CrowCpp/Crow/releases/download/v1.3.2/crow_all.h

# 调试构建
g++ radioserver.cpp metadata.cpp -o radioserver \
    -std=c++17 -g -O0 -lpthread -lssl -lcrypto -I.
```

### 配置 — `settings.json`

```json
{
    "station_name": "Rakuraku Music Station",
    "subtitle": "你的副标题",
    "primary_color": "#764ba2",
    "secondary_color": "#667eea",
    "bg_color": "#f4f4f9",
    "admin_password": "change_me",
    "allow_guest_skip": false
}
```

- `admin_password` — 未设置时回退到 `admin123`，**生产环境务必修改**。
- `allow_guest_skip` — 为 `true` 时，未登录用户也可以 POST `/api/next` 和 `/api/prev`。

### HTTP API

公开接口：

| 方法 | 路径                   | 说明                                 |
| ---- | ---------------------- | ------------------------------------ |
| GET  | `/`                    | 听众页（已登录则显示管理面板）        |
| GET  | `/api/playlist`        | 播放列表 + 每首曲目的元数据           |
| GET  | `/api/stats`           | 当前在线听众数                        |
| GET  | `/api/metadata/<idx>`  | 曲目 `idx` 的完整元数据               |
| GET  | `/api/cover/<idx>`     | 内嵌封面（无则返回占位图）            |
| GET  | `/api/lyrics/<idx>`    | 歌词（如有）                          |

管理员接口（需要 session cookie）：

| 方法 | 路径                 | 说明                              |
| ---- | -------------------- | --------------------------------- |
| POST | `/admin/login`       | `{ "password": "..." }` → cookie  |
| POST | `/admin/logout`      | 销毁会话                          |
| POST | `/upload`            | 分块上传（≤ 50 MB）               |
| POST | `/api/next`          | 下一首（可放宽给游客）            |
| POST | `/api/prev`          | 上一首（可放宽给游客）            |
| POST | `/api/play/<idx>`    | 跳转到曲目 `idx`                  |
| POST | `/api/delete/<idx>`  | 删除曲目 `idx`                    |

### 架构

| 组件               | 职责                                                        |
| ------------------ | ----------------------------------------------------------- |
| `StreamServer`     | 使用 `epoll` 将音频字节分发到 2241 端口上的听众             |
| `AudioPlayer`      | 以 `-re` 模式调起 FFmpeg，把解码后的音频写入缓冲区          |
| `BroadcastBuffer`  | 2 的幂次容量的环形缓冲区，单生产者、多消费者                |
| `WebServer`        | 运行在 2240 端口上的 Crow 应用，提供 UI 与 REST API         |
| `SessionManager`   | 内存态会话表，24 小时过期                                   |
| `AuthMiddleware`   | Crow 中间件，将 `session_id` Cookie 解析为管理员状态        |

### 端口

- `2240` — Web 管理 / API
- `2241` — 音频流

### 项目结构

```
├── radioserver.cpp        # 主服务器
├── metadata.{hpp,cpp}     # 音频元数据提取
├── authmiddleware.hpp     # Crow 认证中间件
├── sessionmanager.hpp     # 会话存储
├── build_release.sh       # 一键构建脚本
├── settings.json          # 运行时配置
├── templates/             # HTML 模板（可选）
└── dist/                  # 构建产物
    ├── radioserver
    ├── start.sh / stop.sh
    ├── media/             # 音频文件放这里
    └── templates/
```

### 故障排除

- **非 ASCII 文件名乱码** — 重新生成 locale：`sudo locale-gen zh_CN.UTF-8 && sudo update-locale LANG=zh_CN.UTF-8`，或直接使用 `./start.sh`（内部已设置）。
- **端口被占用** — `ss -ltnp | grep -E ':2240|:2241'` 查占用进程，或修改 `radioserver.cpp` 中的 `Config::WEB_PORT` / `Config::STREAM_PORT`。
- **管道错误 `revents=16`** — 通常是 locale 问题，参考上一条。
- **没有声音** — 确认 `ffmpeg` 在 `PATH` 中，且文件扩展名在支持列表内。

日志位于 `dist/server.log`。

### 安全建议

- 对外部署前务必修改 `admin_password`。
- 会话仅走 Cookie（`HttpOnly`、`SameSite=Lax`），公网部署请在前面挂 TLS 反向代理。
- 上传限制 50 MB 并校验扩展名，但管理员接口本身才是信任边界。

### 许可证

MIT — 详见 [LICENSE](LICENSE)。

### 致谢

- [Crow](https://github.com/CrowCpp/Crow) — 轻量级 C++ Web 框架
- [FFmpeg](https://ffmpeg.org/) — 音频解码
- [Boost](https://www.boost.org/) / [Asio](https://think-async.com/Asio/) — 网络原语

---

<div align="center"><em>如果本项目对你有帮助，欢迎点亮 ⭐ Star / Star the repo if you find it useful.</em></div>
