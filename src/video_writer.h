#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>

struct VideoWriterData;

class VideoWriter
{
public:
    enum class PixelFormat
    {
        RGB24,
        Mono8
    };

    /// Returns `std::nullopt` on error.
    static std::optional<VideoWriter> Create(
        std::filesystem::path& output_file,
        unsigned frame_width,
        unsigned frame_height,
        unsigned frame_rate,
        unsigned bit_rate,
        PixelFormat pixel_format
    );

    VideoWriter(const VideoWriter&) = default;
    VideoWriter(VideoWriter&&) = default;

    VideoWriter& operator=(const VideoWriter&) = default;
    VideoWriter& operator=(VideoWriter&&) = default;

    /// Calls `Finalize`.
    ~VideoWriter();

    /// Encodes and writes a frame; returns `false` on error.
    bool AddFrame(std::uint8_t* data, std::ptrdiff_t* line_stride);

    bool Finalize();

private:
    VideoWriter(std::unique_ptr<VideoWriterData> data);

    std::unique_ptr<VideoWriterData> _data;
};
