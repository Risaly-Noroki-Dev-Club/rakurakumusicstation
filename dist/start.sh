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
