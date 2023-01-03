//
// libavtest-cpp
// Copyright (c) 2023 Filip Szczerek <ga.software@yahoo.com>
//
// This code is licensed under the terms of the MIT license
// (see the LICENSE file for details).
//

/*
TODO: why the first few frames have terrible quality even with v. high bitrate?


*/

#include <array>
#include <cmath>
#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#include <memory>
#include <stdexcept>

namespace Deleters
{
    struct avformatcontext { void operator ()(AVFormatContext* ptr) const { avformat_free_context(ptr); } };
    struct aviocontext { void operator ()(AVIOContext* ptr) const { avio_close(ptr); } };
    struct avcodeccontext { void operator()(AVCodecContext* ptr) const { avcodec_free_context(&ptr); } };
    struct avframe { void operator()(AVFrame* ptr) const { av_frame_free(&ptr); } };
    struct swscontext { void operator()(SwsContext* ptr) const { sws_freeContext(ptr); } };
}

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
    constexpr int FRAMERATE = 60;
    constexpr int NUM_FRAMES = 500;
    constexpr int WIDTH = 640;
    constexpr int HEIGHT = 480;

    std::unique_ptr<AVFormatContext, Deleters::avformatcontext> fmt_context{avformat_alloc_context()};
    AVOutputFormat output_fmt = []() {
        const auto* guessed_output_fmt = av_guess_format("mp4", nullptr, "video/mp4");
        if (!guessed_output_fmt)
        {
            throw std::runtime_error("av_guess_format failed");
        }
        AVOutputFormat result = *guessed_output_fmt;
        result.audio_codec = AV_CODEC_ID_NONE;
        result.video_codec = AV_CODEC_ID_H264;
        return result;
    }();
    fmt_context->oformat = &output_fmt;

    AVStream* stream = avformat_new_stream(fmt_context.get(), nullptr);
    if (!stream)
    {
        throw std::runtime_error("avformat_new_stream failed");
    }
    stream->sample_aspect_ratio = AVRational{1, 1};
    stream->time_base = AVRational{1, FRAMERATE};
    {
        //TODO: some of this we also set in codec context; do we have to do it here?
        auto* cp = stream->codecpar;
        cp->codec_type = AVMEDIA_TYPE_VIDEO;
        cp->codec_id = AV_CODEC_ID_H264;
        cp->format = AV_PIX_FMT_YUV420P;
        // needed? cp->bit_rate = 1'000'000;
        cp->width = WIDTH;
        cp->height = HEIGHT;
        cp->sample_aspect_ratio = AVRational{1, 1};
    }

    std::unique_ptr<AVIOContext, Deleters::aviocontext> avio_context{[]() {
        AVIOContext* ptr{nullptr};
        if (avio_open2(&ptr, "file:output.mp4", AVIO_FLAG_WRITE, nullptr, nullptr/*?*/) < 0)
        {
            throw std::runtime_error("avio_open2 failed");
        }
        return ptr;
    }()};
    fmt_context->pb = avio_context.get();

    if (avformat_write_header(fmt_context.get(), nullptr) < 0)
    {
        throw std::runtime_error("avformat_write_header failed");
    }

    std::cout << "muxer set the stream time base to " << stream->time_base.num << "/" << stream->time_base.den << "\n";

    // initialize codec --------------------------------

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec)
    {
        throw std::runtime_error("avcodec_find_encoder_by_name failed");
    }
    std::unique_ptr<AVCodecContext, Deleters::avcodeccontext> codec_ctx{avcodec_alloc_context3(codec)};
    codec_ctx->time_base = stream->time_base;
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx->width = WIDTH;
    codec_ctx->height = HEIGHT;
    {
        AVDictionary* opts{nullptr};
        av_dict_set(&opts, "b", "5M", 0); //TODO: which is actually needed, this one or `AVStream::codecpar::bit_rate`?
        if (avcodec_open2(codec_ctx.get(), codec, &opts) < 0)
        {
            throw std::runtime_error("avcodec_open2 failed");
        }
    }

    //TESTING #####################
    // const auto* yuv_descr = av_pix_fmt_desc_get(AV_PIX_FMT_YUV420P);
    // const auto* rgb_descr = av_pix_fmt_desc_get(AV_PIX_FMT_RGB24);
    //END TESTING #################

    // send frames to codec, get compressed packets from codec, send them to output file -----------------------

    int aligned_width = WIDTH;
    int aligned_height = HEIGHT;
    avcodec_align_dimensions(codec_ctx.get(), &aligned_width, &aligned_height);
    if (aligned_width != WIDTH || aligned_height != HEIGHT)
    {
        std::cout << "codec aligned frame size to " << aligned_width << "x" << aligned_height << std::endl;
    }

    const auto frame_data_rgb = std::make_unique<std::uint8_t[]>(aligned_width * aligned_height * 3);

    // RGB frame stores all 3 data planes in one buffer (as per `av_pix_fmt_desc_get(AV_PIX_FMT_RGB24)`)
    std::unique_ptr<AVFrame, Deleters::avframe> avframe_rgb{av_frame_alloc()};
    for (int i = 0; i < 3; ++i)
    {
        avframe_rgb->data[i] = frame_data_rgb.get();
        avframe_rgb->linesize[i] = 3 * aligned_width;
    }
    avframe_rgb->extended_data = avframe_rgb->data;
    avframe_rgb->width = WIDTH;
    avframe_rgb->height = HEIGHT;
    avframe_rgb->format = AV_PIX_FMT_RGB24;
    avframe_rgb->sample_aspect_ratio = AVRational{1, 1};
    avframe_rgb->quality = 1;
    avframe_rgb->color_range = AVCOL_RANGE_JPEG;
    avframe_rgb->color_primaries = AVCOL_PRI_UNSPECIFIED; //TODO
    avframe_rgb->color_trc = AVCOL_TRC_LINEAR; // ?? AVCOL_TRC_IEC61966_2_1; //TODO
    avframe_rgb->colorspace = AVCOL_SPC_RGB; // ?
    avframe_rgb->chroma_location = AVCHROMA_LOC_LEFT;

    // YUV frame stores its 3 data planes in separate buffers (as per `av_pix_fmt_desc_get(AV_PIX_FMT_YUV420P)`)
    std::unique_ptr<AVFrame, Deleters::avframe> avframe_yuv{av_frame_alloc()};
    const auto frame_data_y = std::make_unique<std::uint8_t[]>(aligned_width * aligned_height);
    const auto frame_data_u = std::make_unique<std::uint8_t[]>(aligned_width * aligned_height);
    const auto frame_data_v = std::make_unique<std::uint8_t[]>(aligned_width * aligned_height);
    avframe_yuv->data[0] = frame_data_y.get();
    avframe_yuv->data[1] = frame_data_u.get();
    avframe_yuv->data[2] = frame_data_v.get();
    for (int i = 0; i < 3; ++i)
    {
        avframe_yuv->linesize[i] = aligned_width;
    }
    avframe_yuv->extended_data = avframe_yuv->data;
    avframe_yuv->width = WIDTH;
    avframe_yuv->height = HEIGHT;
    avframe_yuv->format = AV_PIX_FMT_YUV420P;
    avframe_yuv->sample_aspect_ratio = AVRational{1, 1};
    avframe_yuv->quality = 1;
    avframe_yuv->color_range = AVCOL_RANGE_JPEG; //?
    avframe_yuv->color_primaries = AVCOL_PRI_UNSPECIFIED; //TODO
    avframe_yuv->color_trc = AVCOL_TRC_LINEAR; // ?? AVCOL_TRC_IEC61966_2_1; //TODO
    avframe_yuv->colorspace = AVCOL_SPC_RGB; // ?
    avframe_yuv->chroma_location = AVCHROMA_LOC_LEFT;

    std::unique_ptr<SwsContext, Deleters::swscontext> sws_ctx{sws_getContext(
        WIDTH,
        HEIGHT,
        static_cast<AVPixelFormat>(avframe_rgb->format),
        WIDTH,
        HEIGHT,
        static_cast<AVPixelFormat>(avframe_yuv->format),
        0,
        nullptr,
        nullptr,
        nullptr
    )};

    enum class EncodingState
    {
        SendFrameToEncoder,
        ReceivePacketFromEncoder
    };

    int num_encoded_frames = 0;
    auto state = EncodingState::SendFrameToEncoder;
    while (true)
    {
        if (EncodingState::SendFrameToEncoder == state)
        {
            fill_frame_rgb(num_encoded_frames, frame_data_rgb.get(), aligned_width, aligned_height);

            if (0 > sws_scale(
                sws_ctx.get(),
                avframe_rgb->data,
                avframe_rgb->linesize,
                0,
                avframe_rgb->height,
                avframe_yuv->data,
                avframe_yuv->linesize
            ))
            {
                throw std::runtime_error("sws_scale failed");
            }

            avframe_yuv->pts = num_encoded_frames * static_cast<std::int64_t>(av_q2d(av_div_q(AVRational{1, FRAMERATE}, stream->time_base)));
            avframe_yuv->coded_picture_number = num_encoded_frames;
            avframe_yuv->display_picture_number = num_encoded_frames;

            int result = avcodec_send_frame(codec_ctx.get(), avframe_yuv.get());
            switch (result)
            {
            case 0:
                num_encoded_frames += 1;
                if (num_encoded_frames < NUM_FRAMES)
                {
                    state = EncodingState::ReceivePacketFromEncoder; //?
                }
                else
                {
                    // flush the encoder
                    if (avcodec_send_frame(codec_ctx.get(), nullptr) < 0)
                    {
                        throw std::runtime_error("flushing the encoder failed");
                    }
                    state = EncodingState::ReceivePacketFromEncoder;
                }
                break;

            case AVERROR(EAGAIN):
                state = EncodingState::ReceivePacketFromEncoder;
                break;

            default: throw std::runtime_error("error sending frames to encoder");
            }
        }
        else if (EncodingState::ReceivePacketFromEncoder == state)
        {
            bool encoder_flushed{false};
            while (true)
            {
                AVPacket packet{};
                int result = avcodec_receive_packet(codec_ctx.get(), &packet);
                if (0 == result)
                {
                    if (av_interleaved_write_frame(fmt_context.get(), &packet) < 0)
                    {
                        throw std::runtime_error("av_interleaved_write_frame failed");
                    }
                    av_packet_unref(&packet);
                }
                else if (AVERROR(EAGAIN) == result)
                {
                    state = EncodingState::SendFrameToEncoder;
                    break;
                }
                else if (AVERROR_EOF == result)
                {
                    encoder_flushed = true;
                    break;
                }
                else
                {
                    throw std::runtime_error("avcodec_receive_packet failed");
                }
            }

            if (encoder_flushed)
            {
                if (av_write_trailer(fmt_context.get()) < 0)
                {
                    throw std::runtime_error("av_write_trailer failed");
                }
                break;
            }
        }
        else
        {
            throw std::logic_error("unrecognized state");
        }
    }

    std::cout << "Finished successfully." << std::endl;

    return 0;
}
