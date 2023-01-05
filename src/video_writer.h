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
    static std::optional<VideoWriter> create(
        const std::filesystem::path& output_file,
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

    /// Calls `finalize`.
    ~VideoWriter();

    /// Encodes and writes a frame; returns `false` on error.
    bool add_frame(const std::uint8_t* frame_contents, std::ptrdiff_t line_stride);

    bool finalize();

private:
    VideoWriter(std::unique_ptr<VideoWriterData> data);

    bool write_out_encoded_packets();

    std::unique_ptr<VideoWriterData> _data;
};
