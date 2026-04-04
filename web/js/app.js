// 动态注入音频源（确保audio标签的src正确）
document.addEventListener("DOMContentLoaded", () => {
    document.getElementById("player").src = "http://localhost:2241";
});

// 加载播放列表
async function loadPlaylist() {
    try {
        const response = await fetch('/api/playlist');
        const data = await response.json();
        
        const playlist = data.playlist || [];
        const currentIndex = data.current || 0;
        
        // 更新UI
        document.getElementById('currentTrack').textContent = 
            playlist.length > 0 ? `正在播放: ${playlist[currentIndex]}` : '播放列表为空';
        document.getElementById('trackCount').textContent = playlist.length;
        document.getElementById('totalTracks').textContent = playlist.length;
        document.getElementById('currentIndex').textContent = playlist.length > 0 ? currentIndex + 1 : '-';
        
        // 生成播放列表HTML
        let html = '';
        if (playlist.length === 0) {
            html = '<div style="text-align: center; padding: 40px; color: #6c757d;">'
                 + '<div style="font-size: 3em; margin-bottom: 20px;">🎵</div>'
                 + '<div>播放列表为空，请上传音乐文件</div>'
                 + '</div>';
        } else {
            playlist.forEach((track, index) => {
                const isCurrent = index === currentIndex;
                html += `
                    <div class="track ${isCurrent ? 'current' : ''}" onclick="playTrack(${index})">
                        <span class="track-number">${index + 1}</span>
                        ${escapeHtml(track)}
                        ${isCurrent ? ' <span style="color: #4169E1;">▶️</span>' : ''}
                    </div>
                `;
            });
        }
        document.getElementById('playlist').innerHTML = html;
        
    } catch (error) {
        console.error('加载播放列表失败:', error);
        document.getElementById('currentTrack').textContent = '加载失败，请刷新页面';
    }
}

// 简单的转义函数防止XSS
function escapeHtml(str) {
    return str.replace(/[&<>]/g, function(m) {
        if (m === '&') return '&amp;';
        if (m === '<') return '&lt;';
        if (m === '>') return '&gt;';
        return m;
    });
}

// 加载统计信息
async function loadStats() {
    try {
        const response = await fetch('/api/stats');
        const data = await response.json();
        document.getElementById('clientCount').textContent = data.clients || 0;
    } catch (error) {
        console.error('加载统计失败:', error);
    }
}

// 播放指定曲目
async function playTrack(index) {
    try {
        await fetch('/api/play/' + index);
        await new Promise(resolve => setTimeout(resolve, 500)); // 等待切换
        loadPlaylist();
        document.getElementById('player').load(); // 重新加载音频流
    } catch (error) {
        console.error('切换曲目失败:', error);
        showMessage('切换曲目失败: ' + error.message, 'error');
    }
}

// 播放下一首
async function playNext() {
    try {
        await fetch('/api/next');
        await new Promise(resolve => setTimeout(resolve, 500));
        loadPlaylist();
        document.getElementById('player').load();
    } catch (error) {
        console.error('下一首失败:', error);
        showMessage('切换失败: ' + error.message, 'error');
    }
}

// 显示消息
function showMessage(message, type = 'info') {
    const statusEl = document.getElementById('uploadStatus');
    statusEl.textContent = message;
    statusEl.className = type;
    if (type !== 'info') {
        setTimeout(() => {
            statusEl.textContent = '';
            statusEl.className = '';
        }, 5000);
    }
}

// 处理文件上传
document.getElementById('uploadForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    
    const fileInput = document.getElementById('fileInput');
    if (!fileInput.files[0]) {
        showMessage('请选择要上传的文件', 'error');
        return;
    }
    
    const file = fileInput.files[0];
    const maxSize = 50 * 1024 * 1024; // 50MB
    
    if (file.size > maxSize) {
        showMessage('文件太大，最大支持50MB', 'error');
        return;
    }
    
    const formData = new FormData();
    formData.append('file', file);
    
    showMessage('上传中...', 'info');
    
    try {
        const response = await fetch('/upload', {
            method: 'POST',
            body: formData
        });
        
        if (response.ok) {
            const text = await response.text();
            showMessage('✅ ' + text, 'success');
            fileInput.value = '';
            setTimeout(() => {
                loadPlaylist();
            }, 1000);
        } else {
            const text = await response.text();
            showMessage('❌ ' + text, 'error');
        }
    } catch (error) {
        showMessage('❌ 上传失败: ' + error.message, 'error');
    }
});

// 初始化加载
loadPlaylist();
loadStats();

// 定期更新
setInterval(loadPlaylist, 3000);
setInterval(loadStats, 2000);
