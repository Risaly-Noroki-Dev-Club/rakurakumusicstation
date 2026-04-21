#include "metadata.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <regex>

using namespace std;
namespace fs = std::filesystem;

// TrackMetadata 实现

TrackMetadata::TrackMetadata(const std::string& file_path) : file_path(file_path) {
    filename = file_path.substr(file_path.find_last_of("/\\") + 1);

    // 尝试从文件名推测基本信息
    if (title.empty()) {
        title = filename.substr(0, filename.find_last_of('.'));
    }
}

bool TrackMetadata::is_empty() const {
    return title.empty() && artist.empty() && album.empty();
}

std::string TrackMetadata::get_display_name() const {
    if (!title.empty()) {
        return title;
    }
    return filename.substr(0, filename.find_last_of('.'));
}

void TrackMetadata::clear() {
    filename.clear();
    title.clear();
    artist.clear();
    album.clear();
    genre.clear();
    year = 0;
    track_number = 0;
    duration = 0;
    cover_art.clear();
    lyrics.clear();
    file_path.clear();
}

// MetadataManager 实现

TrackMetadata MetadataManager::extract_metadata(const std::string& file_path) {
    TrackMetadata metadata;
    metadata.file_path = file_path;
    metadata.filename = file_path.substr(file_path.find_last_of("/\\") + 1);

    if (!fs::exists(file_path)) {
        cerr << "[Metadata] 文件不存在: " << file_path << endl;
        return metadata;
    }

    // 首先尝试从文件名中提取基本信息
    string name_without_ext = metadata.filename.substr(0, metadata.filename.find_last_of('.'));

    // 常见音乐文件命名模式：艺术家 - 歌曲名
    regex pattern1("(.*)\\s*-\\s*(.*)");
    smatch matches;

    if (regex_match(name_without_ext, matches, pattern1) && matches.size() == 3) {
        metadata.artist = matches[1].str();
        metadata.title = matches[2].str();
        cout << "[Metadata] 从文件名提取: " << metadata.artist << " - " << metadata.title << endl;
    } else {
        // 如果没有匹配到常见模式，直接使用文件名作为标题
        metadata.title = name_without_ext;
    }

    // 使用FFmpeg获取时长信息
    metadata.duration = get_duration_via_ffmpeg(file_path);

    cout << "[Metadata] 成功提取基础元数据: " << metadata.get_display_name()
         << " (" << metadata.duration << "秒)" << endl;

    return metadata;
}

bool MetadataManager::is_supported_format(const std::string& filename) {
    vector<string> supported_formats = {".mp3", ".flac", ".ogg", ".wav", ".m4a", ".aac"};
    string extension = fs::path(filename).extension().string();
    transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    for (const auto& fmt : supported_formats) {
        if (extension == fmt) {
            return true;
        }
    }
    return false;
}

std::string MetadataManager::safe_filename(const std::string& filename) {
    // 简单的文件名清理，移除可能的问题字符
    string safe_name = filename;
    string invalid_chars = "\\/:*?\"<>|";

    for (char c : invalid_chars) {
        replace(safe_name.begin(), safe_name.end(), c, '_');
    }

    return safe_name;
}

int MetadataManager::get_duration_via_ffmpeg(const std::string& file_path) {
    // 构建FFmpeg命令来获取时长
    string command = "ffmpeg -i '" + file_path + "' 2>&1 | grep Duration";

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        cerr << "[Metadata] 无法执行FFmpeg命令" << endl;
        return 0;
    }

    char buffer[256];
    string result;

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }

    pclose(pipe);

    // 解析FFmpeg输出的时长信息
    regex duration_regex("Duration: ([0-9]+):([0-9]+):([0-9]+)\\.([0-9]+)");
    smatch matches;

    if (regex_search(result, matches, duration_regex) && matches.size() >= 4) {
        try {
            int hours = stoi(matches[1].str());
            int minutes = stoi(matches[2].str());
            int seconds = stoi(matches[3].str());

            return hours * 3600 + minutes * 60 + seconds;
        } catch (const exception& e) {
            cerr << "[Metadata] 解析时长失败: " << e.what() << endl;
        }
    }

    // 如果无法解析时长，检查文件大小估算（粗略估计）
    try {
        uintmax_t file_size = fs::file_size(file_path);
        // 简化的时长估算：对于MP3文件，大约 1MB ≈ 1分钟
        if (file_path.find(".mp3") != string::npos) {
            return min(3600, static_cast<int>(file_size / 1024 / 1024)); // 最大1小时
        }
    } catch (const exception& e) {
        cerr << "[Metadata] 无法获取文件大小: " << e.what() << endl;
    }

    return 0;
}