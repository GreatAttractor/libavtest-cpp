#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>

enum class PixelFormat
{
    RGB24,
    Mono8
};

class VideoWriter
{
public:
    /// Returns `std::nullopt` on error.
    static std::optional<VideoWriter> Create(
        std::filesystem::path& output_file,
        unsigned frame_width,
        unsigned frame_height,
        PixelFormat pixel_format
    );

    ~VideoWriter();

    /// Encodes and writes a frame; returns `false` on error.
    bool AddFrame(std::uint8_t* data, std::ptrdiff_t* line_stride);

private:
    VideoWriter(
        std::filesystem::path& output_file,
        unsigned frame_width,
        unsigned frame_height,
        PixelFormat pixel_format
    );

};
