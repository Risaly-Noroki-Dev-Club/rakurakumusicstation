#!/bin/bash

# =============================================================================
# Rakuraku Music Station - 自动化构建与部署脚本
# =============================================================================

set -e  # 遇到错误立即退出

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
NC='\033[0m' 

echo -e "${BLUE}====================================================${NC}"
echo -e "${BLUE}   Rakuraku Music Station Release Builder v2.0      ${NC}"
echo -e "${BLUE}====================================================${NC}\n"

# 1. 环境依赖深度检查
echo -e "${YELLOW}[1/6] 检查系统依赖...${NC}"

check_cmd() {
    if ! command -v $1 &> /dev/null; then
        echo -e "${RED}✗ 错误: 未安装 $1${NC}"
        return 1
    fi
    echo -e "${GREEN}✓ 已检测到 $1${NC}"
    return 0
}

check_cmd g++ || { echo "请执行: sudo apt install build-essential"; exit 1; }
check_cmd ffmpeg || { echo "请执行: sudo apt install ffmpeg"; exit 1; }

# 检查 Locale 环境 (防止你的 zh_CN 代码崩溃)
if ! locale -a | grep -qi "zh_CN.utf8"; then
    echo -e "${PURPLE}! 提醒: 系统未发现 zh_CN.UTF-8，程序运行时可能报错。${NC}"
    echo -e "  建议执行: sudo locale-gen zh_CN.UTF-8 && sudo update-locale"
fi

# 2. 清理与准备
echo -e "\n${YELLOW}[2/6] 准备工作目录...${NC}"
BUILD_DIR="build_tmp"
RELEASE_DIR="dist"

rm -rf $BUILD_DIR $RELEASE_DIR
mkdir -p $BUILD_DIR
mkdir -p $RELEASE_DIR/media  # 存放音乐
echo -e "${GREEN}✓ 目录已重置${NC}"

# 3. 极致性能编译 (Release 模式)
echo -e "\n${YELLOW}[3/6] 开始极致优化编译 (O3 + LTO)...${NC}"

# 参数说明:
# -O3: 最高级优化
# -flto: 链接时优化，能跨文件优化二进制流
# -s: 移除符号表
# -DNDEBUG: 禁用断言
# -march=native: 针对当前CPU生成指令(若需跨机分发请移除此项)
g++ radioserver.cpp -o $RELEASE_DIR/radioserver \
    -std=c++17 \
    -O3 -flto -DNDEBUG \
    -lpthread -lssl -lcrypto \
    -I. -w

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ 核心程序编译成功${NC}"
else
    echo -e "${RED}✗ 编译失败，请检查 C++ 语法错误${NC}"
    exit 1
fi

# 4. 二进制精简
echo -e "\n${YELLOW}[4/6] 二进制体积精简...${NC}"
BEFORE_SIZE=$(ls -lh $RELEASE_DIR/radioserver | awk '{print $5}')
strip $RELEASE_DIR/radioserver
AFTER_SIZE=$(ls -lh $RELEASE_DIR/radioserver | awk '{print $5}')
echo -e "${GREEN}✓ 体积压缩完成: $BEFORE_SIZE -> $AFTER_SIZE${NC}"

# 5. 生成辅助脚本
echo -e "\n${YELLOW}[5/6] 生成管理脚本...${NC}"

# 生成启动脚本
cat <<EOF > $RELEASE_DIR/start.sh
#!/bin/bash
# 自动创建媒体目录
mkdir -p media
# 检查ffmpeg
if ! command -v ffmpeg &> /dev/null; then
    echo "错误: 运行环境缺少 ffmpeg，无法解码音乐。"
    exit 1
fi
echo "电台启动中... Web界面: http://localhost:2240"
nohup ./radioserver > server.log 2>&1 &
echo \$! > .server.pid
echo "服务已进入后台运行，PID: \$(cat .server.pid)"
EOF

# 生成停止脚本
cat <<EOF > $RELEASE_DIR/stop.sh
#!/bin/bash
if [ -f .server.pid ]; then
    PID=\$(cat .server.pid)
    kill \$PID && rm .server.pid
    echo "服务已停止 (PID: \$PID)"
else
    pkill radioserver
    echo "已尝试停止所有 radioserver 进程"
fi
EOF

chmod +x $RELEASE_DIR/*.sh
echo -e "${GREEN}✓ 管理脚本 (start.sh/stop.sh) 已生成${NC}"

# 6. 发布总结
echo -e "\n${BLUE}====================================================${NC}"
echo -e "${GREEN}        构建成功! 产物已存至: ./$RELEASE_DIR         ${NC}"
echo -e "${BLUE}====================================================${NC}"
echo -e "${YELLOW}使用说明:${NC}"
echo -e "1. 将音乐文件放入 ${RELEASE_DIR}/media 文件夹"
echo -e "2. 执行 ${RELEASE_DIR}/start.sh 启动服务"
echo -e "3. 访问 Web 页面: http://$(hostname -I | awk '{print $1}'):2240"
echo -e "${BLUE}====================================================${NC}\n"
if [ $? -ne 0 ]; then
    echo -e "${RED}✗ 编译失败${NC}"
    exit 1
fi
echo -e "${GREEN}✓ 编译成功${NC}"

# 5. 检查文件
echo -e "\n${YELLOW}[5/6] 验证编译产物...${NC}"
if [ -f "radioserver_release" ]; then
    SIZE=$(ls -lh radioserver_release | awk '{print $5}')
    echo -e "${GREEN}✓ 可执行文件: radioserver_release (${SIZE})${NC}"
    
    # 显示文件信息
    file radioserver_release
    
    # 显示依赖的动态库
    echo -e "\n依赖的动态库:"
    ldd radioserver_release | grep -E "libc|libpthread|libssl"
else
    echo -e "${RED}✗ 编译失败，未生成可执行文件${NC}"
    exit 1
fi

# 6. 创建媒体目录
echo -e "\n${YELLOW}[6/6] 设置运行环境...${NC}"
mkdir -p ../media
echo -e "${GREEN}✓ 媒体目录已创建: ../media${NC}"

# 完成
echo -e "\n${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${GREEN}✓ Release 构建完成！${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}\n"

echo -e "${YELLOW}运行服务器:${NC}"
echo "  cd release"
echo "  ./radioserver_release"
echo ""
echo -e "${YELLOW}Web 界面:${NC}"
echo "  http://localhost:2240"
echo ""
echo -e "${YELLOW}音频流:${NC}"
echo "  http://localhost:2241"
echo ""
