#!/bin/bash

# =============================================================================
# Rakuraku Music Station - Build Script v2.0
# =============================================================================

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

echo -e "${BLUE}
══════════════════════════════════════════════
    Rakuraku Music Station 构建工具
══════════════════════════════════════════════${NC}
"

# 1. 检查系统环境
print_status "检测系统环境..."
if [ -f /etc/arch-release ]; then
    OS="Arch Linux"
    PKG_MGR="pacman"
    INSTALL_CMD="sudo pacman -S --needed --noconfirm"
    DEPENDENCIES="base-devel ffmpeg openssl boost cmake wget asio"
elif [ -f /etc/debian_version ] || [ -f /etc/lsb-release ]; then
    OS="Debian/Ubuntu"
    PKG_MGR="apt"
    INSTALL_CMD="sudo apt-get install -y"
    DEPENDENCIES="build-essential ffmpeg libavcodec-extra libssl-dev libboost-all-dev locales wget libasio-dev libtag1-dev"
else
    print_error "不支持的操作系统。仅支持 Arch Linux 和 Debian/Ubuntu 系列"
    exit 1
fi

print_success "检测到系统: $OS"

# 2. 检查并安装依赖
print_status "检查系统依赖..."
if [ "$PKG_MGR" == "apt" ]; then
    sudo apt-get update > /dev/null 2>&1
fi

# 检查 Crow 框架
if [ ! -f "crow_all.h" ]; then
    print_status "下载 Crow 框架..."
    wget -q https://github.com/CrowCpp/Crow/releases/download/v1.3.2/crow_all.h
    if [ $? -eq 0 ]; then
        print_success "Crow 框架下载完成"
    else
        print_error "Crow 框架下载失败"
        exit 1
    fi
else
    print_success "Crow 框架已存在"
fi

# 安装系统依赖
print_status "安装系统依赖包..."
$INSTALL_CMD $DEPENDENCIES > /dev/null 2>&1

# 3. 设置中文环境（可选）
print_status "检查语言环境支持..."
if ! locale -a | grep -qi "zh_CN.utf8"; then
    print_warning "中文语言环境未启用，但不影响基础功能"
else
    print_success "中文环境支持已启用"
fi

# 4. 验证 FFmpeg
print_status "检查 FFmpeg 支持..."
if command -v ffmpeg > /dev/null 2>&1; then
    if ffmpeg -encoders | grep -q "libmp3lame"; then
        print_success "FFmpeg 支持 MP3 编码"
    else
        print_warning "FFmpeg 缺少 MP3 编码支持，将影响音频转码"
    fi
else
    print_error "FFmpeg 未找到，请确保已正确安装"
    exit 1
fi

# 5. 编译项目
print_status "编译服务器程序..."
RELEASE_DIR="dist"
rm -rf $RELEASE_DIR
mkdir -p $RELEASE_DIR/media
mkdir -p $RELEASE_DIR/templates

# 编译参数
CXXFLAGS="-std=c++17 -O3 -flto -march=native -lpthread -lssl -lcrypto -I. -w"

g++ radioserver.cpp metadata.cpp -o $RELEASE_DIR/radioserver $CXXFLAGS

if [ -f "$RELEASE_DIR/radioserver" ]; then
    # 可选：移除调试符号减小体积
    if command -v strip > /dev/null 2>&1; then
        strip $RELEASE_DIR/radioserver
    fi
    print_success "编译成功"
else
    print_error "编译失败"
    exit 1
fi

# 6. 复制配置文件
print_status "准备运行环境..."

# 基础模板文件
if [ -f "index.html" ]; then
    cp index.html $RELEASE_DIR/
else
    # 创建基础模板
    cat > $RELEASE_DIR/index.html << 'EOF'
<!DOCTYPE html>
<html>
<head>
    <title>Rakuraku Music Station</title>
    <meta charset="UTF-8">
    <style>
        body { font-family: Arial, sans-serif; max-width: 800px; margin: 0 auto; padding: 20px; }
        .player { background: #f5f5f5; padding: 20px; border-radius: 8px; margin: 20px 0; }
        button { padding: 10px 20px; margin: 5px; background: #764ba2; color: white; border: none; border-radius: 4px; cursor: pointer; }
    </style>
</head>
<body>
    <h1>🎵 Rakuraku Music Station</h1>
    <div class="player">
        <p>当前正在播放的音乐电台</p>
        <audio id="audioPlayer" controls style="width: 100%;">
            <source src="http://localhost:2241" type="audio/mpeg">
        </audio>
        <div>
            <button onclick="document.getElementById('audioPlayer').play()">播放</button>
            <button onclick="document.getElementById('audioPlayer').pause()">暂停</button>
            <button onclick="window.location.href='/admin'">管理面板</button>
        </div>
    </div>
    <p>将音频文件放入 media/ 目录即可自动播放</p>
</body>
</html>
EOF
fi

# 配置文件和脚本
if [ -f "settings.json" ]; then
    cp settings.json $RELEASE_DIR/
else
    cp > $RELEASE_DIR/settings.json << 'EOF'
{
    "station_name": "Rakuraku Music Station",
    "subtitle": "轻量级流媒体服务器",
    "primary_color": "#764ba2",
    "secondary_color": "#667eea",
    "bg_color": "#f4f4f9",
    "admin_password": "admin123",
    "allow_guest_skip": false
}
EOF
fi

# 创建启动脚本
cat > $RELEASE_DIR/start.sh << 'EOF'
#!/bin/bash

# 设置中文环境支持
if locale -a | grep -qi "zh_CN.utf8"; then
    export LANG=zh_CN.UTF-8
    export LC_ALL=zh_CN.UTF-8
fi

# 确保媒体目录存在
mkdir -p media

echo "🎵 Rakuraku Music Station 启动中..."
echo "========================================"
echo "Web 界面: http://localhost:2240"
echo "流媒体: http://localhost:2241"
echo "========================================"
echo "音乐文件请放置在 media/ 目录"
echo ""

# 后台运行服务器
nohup ./radioserver > server.log 2>&1 &
echo $! > .server.pid

echo "✅ 服务器已启动 (PID: $(cat .server.pid))"
echo "📄 查看日志: tail -f server.log"
echo "🛑 停止服务: ./stop.sh"
EOF

# 创建停止脚本
cat > $RELEASE_DIR/stop.sh << 'EOF'
#!/bin/bash

if [ -f .server.pid ]; then
    PID=$(cat .server.pid)
    if ps -p $PID > /dev/null 2>&1; then
        kill $PID
        rm .server.pid
        echo "✅ 服务器已停止 (PID: $PID)"
    else
        echo "⚠️  PID 文件存在但进程未运行，清理中..."
        rm .server.pid
    fi
else
    # 如果没有 PID 文件，尝试杀死所有 radioserver 进程
    if pkill radioserver 2>/dev/null; then
        echo "✅ 已停止所有 radioserver 进程"
    else
        echo "ℹ️ 没有运行中的 radioserver 进程"
    fi
fi
EOF

chmod +x $RELEASE_DIR/start.sh $RELEASE_DIR/stop.sh

# 7. 完成提示
print_success "构建完成！"
echo -e "${BLUE}
使用方法:
══════════════════════════════════════════════${NC}"
echo "1. 进入目录: cd $RELEASE_DIR"
echo "2. 添加音乐: 将音频文件放入 media/ 目录"
echo "3. 启动服务: ./start.sh"
echo "4. 访问地址: http://localhost:2240"
echo ""
echo "支持格式: MP3, WAV, FLAC, OGG, M4A, AAC"
echo "管理员密码: admin123 (可在 settings.json 中修改)"
echo ""
echo -e "${GREEN}🎵 享受音乐时光！${NC}"