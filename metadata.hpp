#ifndef METADATA_HPP
#define METADATA_HPP

#include <string>
#include <vector>
#include <cstdint>

// 歌曲元数据结构体
struct TrackMetadata {
    std::string filename;
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    int year = 0;
    int track_number = 0;
    int duration = 0;  // 时长（秒）
    std::vector<uint8_t> cover_art; // 专辑封面二进制数据
    std::string lyrics;
    std::string file_path; // 完整文件路径

    // 默认构造函数
    TrackMetadata() = default;

    // 从文件路径构造，自动提取元数据
    TrackMetadata(const std::string& file_path);

    // 检查元数据是否为空
    bool is_empty() const;

    // 获取显示名称（优先使用title，如果没有则使用文件名）
    std::string get_display_name() const;

    // 重置元数据
    void clear();
};

// 元数据管理器类
class MetadataManager {
public:
    // 从音频文件提取完整元数据
    static TrackMetadata extract_metadata(const std::string& file_path);

    // 检查文件是否支持元数据提取
    static bool is_supported_format(const std::string& filename);

    // 安全的文件名处理（防止中文乱码）
    static std::string safe_filename(const std::string& filename);

    // 使用FFmpeg获取音频时长
    static int get_duration_via_ffmpeg(const std::string& file_path);
};

#endif // METADATA_HPP