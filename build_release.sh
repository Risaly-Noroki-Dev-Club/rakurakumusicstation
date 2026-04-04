#!/bin/bash

# =============================================================================
# Rakuraku Music Station - 超智能自动化构建脚本 v6.0 
# =============================================================================

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
NC='\033[0m' 

echo -e "${BLUE}====================================================${NC}"
echo -e "${BLUE}     Rakuraku Music Station - Universal Builder     ${NC}"
echo -e "${BLUE}====================================================${NC}\n"

# 1. 系统环境识别
echo -e "${YELLOW}[1/6] 正在识别系统环境...${NC}"
if [ -f /etc/arch-release ]; then
    OS="Arch"
    PKG_MGR="pacman"
    INSTALL_CMD="sudo pacman -S --needed --noconfirm"
    DEPS="base-devel ffmpeg openssl boost cmake wget asio"
    echo -e "${GREEN}检测到系统类型: Arch Linux${NC}"
elif [ -f /etc/debian_version ] || [ -f /etc/lsb-release ]; then
    OS="Debian"
    PKG_MGR="apt"
    INSTALL_CMD="sudo apt-get install -y"
    DEPS="build-essential ffmpeg libavcodec-extra libssl-dev libboost-all-dev locales wget libasio-dev"
    echo -e "${GREEN}检测到系统类型: Debian/Ubuntu/WSL${NC}"
else
    echo -e "${RED}抱歉，暂不支持此发行版。脚本仅支持 Arch 或 Debian/Ubuntu 系列。${NC}"
    exit 1
fi

# 2. 自动化安装依赖
echo -e "\n${YELLOW}[2/6] 正在同步系统依赖...${NC}"
if [ "$PKG_MGR" == "apt" ]; then sudo apt-get update; fi
if [ ! -f "crow_all.h" ]; then echo -e "${PURPLE}正在下载 Crow 框架核心组件...${NC}"
wget -q https://github.com/CrowCpp/Crow/releases/download/v1.3.2/crow_all.h
    echo -e "${GREEN}✓ Crow 已就绪${NC}"
fi
$INSTALL_CMD $DEPS

# 3. 强制修复 Locale 
echo -e "\n${YELLOW}[3/6] 检查并修复中文语言环境...${NC}"
if ! locale -a | grep -qi "zh_CN.utf8"; then
    echo -e "${PURPLE}正在激活 zh_CN.UTF-8 以支持中文文件名...${NC}"
    if [ "$OS" == "Arch" ]; then
        sudo sed -i '/^#zh_CN.UTF-8 UTF-8/s/^#//' /etc/locale.gen
        sudo locale-gen
    else
        sudo locale-gen zh_CN.UTF-8
        sudo update-locale LANG=zh_CN.UTF-8
    fi
    echo -e "${GREEN}✓ Locale 已修复${NC}"
else
    echo -e "${GREEN}✓ 中文环境已就绪${NC}"
fi

# 4. FFmpeg
echo -e "\n${YELLOW}[4/6] 校验 FFmpeg 解码能力...${NC}"
if ffmpeg -encoders | grep -q "libmp3lame"; then
    echo -e "${GREEN}✓ FFmpeg已就绪 (支持 libmp3lame)${NC}"
else
    echo -e "${RED}! 警告: 即使安装后仍缺少 libmp3lame，正在尝试下载静态全量包...${NC}"
    wget -N https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz
    mkdir -p ffmpeg_tmp && tar -xJf ffmpeg-release-*-static.tar.xz -C ffmpeg_tmp --strip-components=1
    sudo cp ffmpeg_tmp/ffmpeg /usr/local/bin/ffmpeg
    echo -e "${GREEN}✓ 静态全量版 FFmpeg 已部署${NC}"
fi

# 5. 编译
echo -e "\n${YELLOW}[5/6] 编译服务器核心...${NC}"
RELEASE_DIR="dist"
rm -rf $RELEASE_DIR && mkdir -p $RELEASE_DIR/media

# 针对 CPU 进行原生优化 (-march=native)
g++ radioserver.cpp -o $RELEASE_DIR/radioserver -std=c++17 -O3 -flto -march=native -lpthread -lssl -lcrypto -I. -w

if [ -f "$RELEASE_DIR/radioserver" ]; then
    # 移除调试符号以减小体积
    if command -v strip &> /dev/null; then strip $RELEASE_DIR/radioserver; fi
    echo -e "${GREEN}✓ 编译成功!${NC}"
else
    echo -e "${RED}✗ 编译失败，请检查 C++ 源代码。${NC}"
    exit 1
fi

# 【新增】复制前端文件和配置文件到发布目录
echo -e "\n${YELLOW}[5.5/6] 复制前端资源...${NC}"
if [ -f "index.html" ]; then
    cp index.html $RELEASE_DIR/
    echo -e "${GREEN}✓ index.html 已复制${NC}"
else
    echo -e "${YELLOW}! 警告: 未找到 index.html${NC}"
fi

if [ -f "settings.json" ]; then
    cp settings.json $RELEASE_DIR/
    echo -e "${GREEN}✓ settings.json 已复制${NC}"
else
    echo -e "${YELLOW}! 警告: 未找到 settings.json${NC}"
fi

# 6. 生成跨平台运行脚本
echo -e "\n${YELLOW}[6/6] 生成管理脚本...${NC}"

cat <<'EOF' > $RELEASE_DIR/start.sh
#!/bin/bash
# 强制环境为 UTF-8，防止 FFmpeg 在读取特殊字符文件名时崩溃
export LANG=zh_CN.UTF-8
export LC_ALL=zh_CN.UTF-8
export PATH=/usr/local/bin:$PATH

mkdir -p media
echo "=== Rakuraku Music Station ==="
echo "正在启动，请确保音乐已放入 media/ 目录"
echo "Web 控制台: http://localhost:2240"

# 后台运行并记录日志
nohup ./radioserver > server.log 2>&1 &
echo $! > .server.pid
echo "服务已运行，PID: $(cat .server.pid)"
echo "查看运行情况: tail -f server.log"
EOF

cat <<'EOF' > $RELEASE_DIR/stop.sh
#!/bin/bash
if [ -f .server.pid ]; then
    PID=$(cat .server.pid)
    kill $PID && rm .server.pid
    echo "服务已停止 (PID: $PID)"
else
    pkill radioserver
    echo "已尝试停止所有 radioserver 进程"
fi
EOF

chmod +x $RELEASE_DIR/*.sh
echo -e "${GREEN}✓ 管理脚本 (start/stop) 已生成${NC}"

# 完成总结
echo -e "\n${BLUE}====================================================${NC}"
echo -e "${GREEN}        构建大成功！ (Target: $OS)${NC}"
echo -e "${BLUE}====================================================${NC}"
echo -e "${YELLOW}运行指南:${NC}"
echo -e " 1. cd $RELEASE_DIR"
echo -e " 2. 把你的音乐放到 ./media 文件夹里"
echo -e " 3. 执行 ./start.sh"
echo -e " 4. 浏览器访问你的服务器 IP:2240"
echo -e "${BLUE}====================================================${NC}\n"
