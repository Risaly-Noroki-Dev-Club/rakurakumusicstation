# 🎵 Rakuraku Music Station

一个轻量级、高性能的 C++ 流媒体广播服务器，支持实时音频流传输和 Web 管理界面。

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)
![C++](https://img.shields.io/badge/C++-17-orange.svg)

## ✨ 特性

- 🚀 **高性能**：基于 Linux epoll 的非阻塞 I/O，支持大量并发连接
- 🎧 **多格式支持**：MP3, WAV, FLAC, OGG, M4A, AAC 等常见音频格式
- 🌐 **Web 界面**：现代化的管理界面，支持文件上传和播放列表管理
- 🔐 **权限控制**：基于会话的认证系统，区分普通用户和管理员
- 📱 **流媒体兼容**：标准 HTTP 流媒体协议，支持各种音频播放器
- 🛠️ **易于部署**：一键式构建脚本，无需复杂配置

## 📋 系统要求

- **操作系统**: Linux (Ubuntu/Debian/Arch Linux)
- **依赖**: 
  - GCC/G++ 7.0+ 或 Clang
  - FFmpeg (音频处理)
  - OpenSSL (加密支持)
  - Boost C++ Libraries
  - Crow Framework (Web 框架)

## 🚀 快速开始

### 1. 自动构建（推荐）

### 第 2 步：一键构建
```bash
# 运行构建脚本（自动处理依赖）
./build_release.sh

# 进入发布目录
cd dist

# 添加音乐文件（支持中文文件名）
cp /path/to/your/music/*.mp3 ./media/

# 启动服务器
./start.sh
```

### 2. 手动构建

### 第 4 步：启动服务器
```bash
# 安装依赖（Ubuntu/Debian）
sudo apt-get install build-essential ffmpeg libavcodec-extra libssl-dev libboost-all-dev wget

# 下载 Crow 框架
wget https://github.com/CrowCpp/Crow/releases/download/v1.3.2/crow_all.h

# 编译
mkdir -p dist/media
g++ radioserver.cpp -o dist/radioserver -std=c++17 -O3 -lpthread -lssl -lcrypto -I.

# 运行
cd dist
./radioserver
```

## 🎮 使用方法

### 启动服务器

```bash
cd dist
./start.sh
```

启动后可以通过以下地址访问：

- **Web 管理界面**: http://localhost:2240
- **流媒体端点**: http://localhost:2241
- **播放器兼容**: 兼容 VLC, Chrome, Firefox 等主流播放器

### 基本操作

1. **添加音乐**：将音频文件放入 `media/` 目录，服务器会自动扫描并添加到播放列表
2. **Web 管理**：访问 http://localhost:2240 进行播放控制和文件管理
3. **管理员登录**：点击"管理面板"，使用默认密码 `admin123`

### 配置文件

编辑 `settings.json` 自定义服务器设置：

```json
{
    "station_name": "我的音乐电台",
    "subtitle": "个性化描述",
    "primary_color": "#764ba2",
    "secondary_color": "#667eea", 
    "bg_color": "#f4f4f9",
    "admin_password": "你的密码",
    "allow_guest_skip": false
}
```

## 📁 项目结构

```
rakurakumusicstation/
├── radioserver.cpp          # 主服务器实现
├── authmiddleware.hpp       # 认证中间件
├── sessionmanager.hpp       # 会话管理
├── settings.json            # 配置文件
├── build_release.sh         # 自动化构建脚本
├── README.md               # 项目文档
└── dist/                   # 发布目录
    ├── radioserver         # 可执行文件
    ├── start.sh           # 启动脚本
    ├── stop.sh            # 停止脚本
    ├── settings.json      # 配置文件
    ├── index.html         # Web 界面模板
    └── media/             # 音乐文件目录
```

## 🔧 技术架构

### 核心组件

- **StreamServer**: 基于 epoll 的异步流媒体服务器
- **AudioPlayer**: FFmpeg 音频解码和转码引擎
- **BroadcastBuffer**: 线程安全的环形缓冲区
- **WebServer**: Crow 框架 Web 管理界面
- **SessionManager**: 会话管理和认证系统

### 端口配置

- **Web 端口**: 2240 (HTTP 管理界面)
- **流媒体端口**: 2241 (音频流传输)

## 🐛 故障排除

### 常见问题

**1. 编译失败**
```bash
# 确保安装所有依赖
sudo apt-get update
sudo apt-get install build-essential ffmpeg libssl-dev libboost-all-dev
```

**2. 中文文件名乱码**
```bash
# 启用中文语言环境
sudo locale-gen zh_CN.UTF-8
sudo update-locale LANG=zh_CN.UTF-8
```

**3. 端口被占用**
```bash
# 检查端口使用情况
netstat -tulpn | grep :2240
# 或更换端口（修改 settings.json）
```

**4. 音频无法播放**
- 确保 FFmpeg 支持 MP3 编码
- 验证音频文件格式是否为支持的类型
- 检查文件权限和路径

### 日志查看

```bash
# 实时查看日志
tail -f dist/server.log

# 查看错误信息
cat dist/server.log | grep ERROR
```

## 🔒 安全建议

1. **修改默认密码**：生产环境务必修改 `admin_password`
2. **防火墙配置**：限制对 2240/2241 端口的访问
3. **文件上传限制**：避免上传大文件或不安全内容
4. **HTTPS 支持**：如需公网访问，建议配置 SSL/TLS

## 🤝 贡献

欢迎提交 Issues 和 Pull Requests！

### 开发环境设置

```bash
# 调试构建
g++ radioserver.cpp -o radioserver -std=c++17 -g -O0 -lpthread -lssl -lcrypto -I.

# 运行测试
./radioserver
```

## 📄 许可证

本项目基于 MIT 许可证开源 - 查看 [LICENSE](LICENSE) 文件了解详情。

## 🙏 致谢

- [Crow C++](https://github.com/CrowCpp/Crow) - 轻量级 Web 框架
- [FFmpeg](https://ffmpeg.org/) - 强大的多媒体处理库
- [Boost C++ Libraries](https://www.boost.org/) - C++ 扩展库
- [Cynun] - 你不知道她是谁，她也许只想呆在这里。但是如果没有她，这个项目不可能存在
- [知夏] - 如果没有ta，这个项目走不到今天。祝他身体健康。

---

<div align="center">
  <p><em>如果本项目对你有帮助，请点个 ⭐ Star 支持！</em></p>
</div>
