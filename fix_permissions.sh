#!/bin/bash
# 修复dist目录权限问题

echo "正在修复dist目录权限..."

# 进入dist目录
cd dist

# 创建必要的目录和设置权限
mkdir -p media
chmod 755 media

# 修复文件权限
touch server.log
chmod 644 server.log

touch .server.pid
chmod 644 .server.pid

echo "权限修复完成"
echo "现在可以使用 ./start.sh 启动服务器"