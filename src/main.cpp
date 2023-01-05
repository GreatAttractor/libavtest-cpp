//
// libavtest-cpp
// Copyright (c) 2023 Filip Szczerek <ga.software@yahoo.com>
//
// This code is licensed under the terms of the MIT license
// (see the LICENSE file for details).
//

#include "video_writer.h"

#include <array>
#include <cstring>
#include <cmath>
#include <iostream>
#include <memory>


/// Fills frame with colored circles.
void fill_frame_rgb(int index, std::uint8_t* data, int width, int height)
{
    const auto make_disk = [&](int cx, int cy, int r, std::array<std::uint8_t, 3> color) {
        for (int y = cy - r; y <= cy + r; ++y)
        {
            for (int x = cx - r; x <= cx + r; ++x)
            {
                if (y >= 0 && y < height && x >= 0 && x < width)
                {
                    const auto d_sq = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                    if (d_sq <= r * r)
                    {
                        for (int ch = 0; ch < 3; ++ch)
                        {
                            data[3 * (y * width + x) + ch] = color[ch];
                        }
                    }
                }
            }
        }
    };

    constexpr double PI = 3.1415926;

    memset(data, 0x80, 3 * width * height);
    for (int i = 0; i < 10; ++i)
    {
        const double phase = i * PI / 10;
        constexpr double SPEED = 120.0;

        make_disk(20 + i * 50, height * (0.5 + 0.5 * sin(2 * PI * index / SPEED + phase             )), 15, {0xFF, 0, 0});
        make_disk(30 + i * 50, height * (0.5 + 0.5 * sin(2 * PI * index / SPEED + phase + PI / 3    )), 15, {0, 0x80, 0});
        make_disk(40 + i * 50, height * (0.5 + 0.5 * sin(2 * PI * index / SPEED + phase + 2 * PI / 3)), 15, {0x10, 0x10, 0xFF});
    }
}

int main(int argc, char* argv[])
{
    constexpr int FRAME_RATE = 60;
    constexpr int BIT_RATE = 1'000'000;
    constexpr int NUM_FRAMES = 100;
    constexpr int WIDTH = 640;
    constexpr int HEIGHT = 480;

    auto video_writer = VideoWriter::create(
        "output.mp4",
        WIDTH,
        480,
        FRAME_RATE,
        BIT_RATE,
        VideoWriter::PixelFormat::RGB24
    );
    if (!video_writer.has_value())
    {
        std::cout << "Failed to initialize video writer.\n";
        return 1;
    }

    auto frame_contents = std::make_unique<std::uint8_t[]>(3 * WIDTH * HEIGHT);

    for (int i = 0; i < NUM_FRAMES; ++i)
    {
        fill_frame_rgb(i, frame_contents.get(), WIDTH, HEIGHT);
        if (!video_writer->add_frame(frame_contents.get(), 3 * WIDTH))
        {
            std::cout << "Error encoding frame.\n";
            return 2;
        }
    }

    if (!video_writer->finalize())
    {
        std::cout << "Error finalizing video file.\n";
        return 3;
    }
    else
    {
        std::cout << "Finished successfully." << std::endl;
    }

    return 0;
}
