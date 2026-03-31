#!/bin/bash
if [ -f .server.pid ]; then
    PID=$(cat .server.pid)
    kill $PID && rm .server.pid
    echo "服务已停止 (PID: $PID)"
else
    pkill radioserver
    echo "已尝试停止所有 radioserver 进程"
fi
