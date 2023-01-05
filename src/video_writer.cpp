// TODO: why the first few frames are poor-quality even with v. high bitrate?

#include "video_writer.h"

#include <array>
#include <cstddef>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}
#include <string>
#include <tuple>

// private definitions
namespace
{

namespace Deleters
{
    struct avformatcontext { void operator ()(AVFormatContext* ptr) const { avformat_free_context(ptr); } };
    struct aviocontext { void operator ()(AVIOContext* ptr) const { avio_close(ptr); } };
    struct avcodeccontext { void operator()(AVCodecContext* ptr) const { avcodec_free_context(&ptr); } };
    struct avframe { void operator()(AVFrame* ptr) const { av_frame_free(&ptr); } };
    struct swscontext { void operator()(SwsContext* ptr) const { sws_freeContext(ptr); } };
}

/// Returns {RGB buffer, AVFrame}.
std::tuple<
    std::unique_ptr<std::uint8_t[]>,
    std::unique_ptr<AVFrame, Deleters::avframe>
> allocate_rgb_frame(int width, int height, int aligned_width, int aligned_height)
{
    auto frame_data_rgb = std::make_unique<std::uint8_t[]>(aligned_width * aligned_height * 3);

    // RGB frame stores all 3 data planes in one buffer (as per `av_pix_fmt_desc_get(AV_PIX_FMT_RGB24)`)
    std::unique_ptr<AVFrame, Deleters::avframe> avframe_rgb{av_frame_alloc()};
    for (int i = 0; i < 3; ++i)
    {
        avframe_rgb->data[i] = frame_data_rgb.get();
        avframe_rgb->linesize[i] = 3 * aligned_width;
    }
    avframe_rgb->extended_data = avframe_rgb->data;
    avframe_rgb->width = width;
    avframe_rgb->height = height;
    avframe_rgb->format = AV_PIX_FMT_RGB24;
    avframe_rgb->sample_aspect_ratio = AVRational{1, 1};
    avframe_rgb->quality = 1;
    avframe_rgb->color_range = AVCOL_RANGE_JPEG;
    avframe_rgb->color_primaries = AVCOL_PRI_UNSPECIFIED; //TODO: what should be here?
    avframe_rgb->color_trc = AVCOL_TRC_LINEAR; //TODO: what should be here?
    avframe_rgb->colorspace = AVCOL_SPC_RGB; //TODO: what should be here?
    avframe_rgb->chroma_location = AVCHROMA_LOC_LEFT;

    return std::make_tuple(std::move(frame_data_rgb), std::move(avframe_rgb));
}

/// Returns {{Y buffer, U buffer, V buffer}, AVFrame}.
std::tuple<
    std::array<std::unique_ptr<std::uint8_t[]>, 3>,
    std::unique_ptr<AVFrame, Deleters::avframe>
> allocate_yuv_frame(int width, int height, int aligned_width, int aligned_height)
{
    // YUV frame stores its 3 data planes in separate buffers (as per `av_pix_fmt_desc_get(AV_PIX_FMT_YUV420P)`)
    std::unique_ptr<AVFrame, Deleters::avframe> avframe_yuv{av_frame_alloc()};
    auto frame_data_y = std::make_unique<std::uint8_t[]>(aligned_width * aligned_height);
    auto frame_data_u = std::make_unique<std::uint8_t[]>(aligned_width * aligned_height);
    auto frame_data_v = std::make_unique<std::uint8_t[]>(aligned_width * aligned_height);
    avframe_yuv->data[0] = frame_data_y.get();
    avframe_yuv->data[1] = frame_data_u.get();
    avframe_yuv->data[2] = frame_data_v.get();
    for (int i = 0; i < 3; ++i)
    {
        avframe_yuv->linesize[i] = aligned_width;
    }
    avframe_yuv->extended_data = avframe_yuv->data;
    avframe_yuv->width = width;
    avframe_yuv->height = height;
    avframe_yuv->format = AV_PIX_FMT_YUV420P;
    avframe_yuv->sample_aspect_ratio = AVRational{1, 1};
    avframe_yuv->quality = 1;
    avframe_yuv->color_range = AVCOL_RANGE_JPEG; //TODO: what should be here?
    avframe_yuv->color_primaries = AVCOL_PRI_UNSPECIFIED; //TODO: what should be here?
    avframe_yuv->color_trc = AVCOL_TRC_LINEAR; //TODO: what should be here?
    avframe_yuv->colorspace = AVCOL_SPC_RGB; //TODO: what should be here?
    avframe_yuv->chroma_location = AVCHROMA_LOC_LEFT;

    return std::make_tuple(
        std::array<std::unique_ptr<std::uint8_t[]>, 3>{
            std::move(frame_data_y),
            std::move(frame_data_u),
            std::move(frame_data_v)
        },
        std::move(avframe_yuv)
    );
}

} // end of private definitions

struct VideoWriterData
{
    std::unique_ptr<AVFormatContext, Deleters::avformatcontext> format_context;
    std::unique_ptr<AVIOContext, Deleters::aviocontext>         avio_context;
    std::unique_ptr<AVCodecContext, Deleters::avcodeccontext>   codec_context;
    std::unique_ptr<std::uint8_t[]>                             frame_data_rgb;
    std::unique_ptr<AVFrame, Deleters::avframe>                 avframe_rgb;
    std::array<std::unique_ptr<std::uint8_t[]>, 3>              frame_data_yuv;
    std::unique_ptr<AVFrame, Deleters::avframe>                 avframe_yuv;
    std::unique_ptr<SwsContext, Deleters::swscontext>           sws_context;

    AVOutputFormat output_fmt{};
    std::size_t num_encoded_frames{0};
    int frame_rate{30};
    AVRational stream_time_base{1, 30};
    bool finalized{false};
};


std::optional<VideoWriter> VideoWriter::create(
    const std::filesystem::path& output_file,
    unsigned frame_width,
    unsigned frame_height,
    unsigned frame_rate,
    unsigned bit_rate,
    PixelFormat pixel_format
)
{
    if (pixel_format != PixelFormat::RGB24)
    {
        av_log(nullptr, AV_LOG_FATAL, "only RGB24 pixel format is currently supported");
        return std::nullopt;
    }

    auto data = std::make_unique<VideoWriterData>();

    data->frame_rate = frame_rate;

    data->format_context.reset(avformat_alloc_context());
    if (!data->format_context)
    {
        av_log(nullptr, AV_LOG_FATAL, "avformat_alloc_context failed");
        return std::nullopt;
    }

    {
        const auto* guessed_output_fmt = av_guess_format("mp4", nullptr, "video/mp4");
        if (!guessed_output_fmt)
        {
            av_log(nullptr, AV_LOG_FATAL, "av_guess_format with \"video/mp4\" failed");
            return std::nullopt;
        }
        data->output_fmt = *guessed_output_fmt;
    }
    data->output_fmt.audio_codec = AV_CODEC_ID_NONE;
    data->output_fmt.video_codec = AV_CODEC_ID_H264;
    data->format_context->oformat = &data->output_fmt;

    AVStream* stream = avformat_new_stream(data->format_context.get(), nullptr);
    if (!stream)
    {
        av_log(nullptr, AV_LOG_FATAL, "avformat_new_stream failed");
        return std::nullopt;
    }
    stream->sample_aspect_ratio = AVRational{1, 1};
    stream->time_base = AVRational{1, static_cast<int>(frame_rate)};
    {
        //TODO: some of this we also set in codec context; do we have to do it here?
        auto* cp = stream->codecpar;
        cp->codec_type = AVMEDIA_TYPE_VIDEO;
        cp->codec_id = AV_CODEC_ID_H264;
        cp->format = AV_PIX_FMT_YUV420P;
        cp->width = frame_width;
        cp->height = frame_height;
        cp->sample_aspect_ratio = AVRational{1, 1};
    }

    {
        AVIOContext* ptr{nullptr};
        if (0 > avio_open2(&ptr, (std::string{"file:"} + output_file.string()).c_str(), AVIO_FLAG_WRITE, nullptr, nullptr))
        {
            av_log(nullptr, AV_LOG_FATAL, "avio_open2 failed");
            return std::nullopt;
        }
        data->avio_context.reset(ptr);
    }
    data->format_context->pb = data->avio_context.get();

    if (0 > avformat_write_header(data->format_context.get(), nullptr))
    {
        av_log(nullptr, AV_LOG_FATAL, "avformat_write_header failed");
        return std::nullopt;
    }

    if (stream->time_base.num != 1 || stream->time_base.den != static_cast<int>(frame_rate))
    {
        av_log(nullptr, AV_LOG_VERBOSE, "muxer set stream time base to %d/%d s", stream->time_base.num, stream->time_base.den);
        data->stream_time_base = stream->time_base;
    }

    {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec)
        {
            av_log(nullptr, AV_LOG_FATAL, "avcodec_find_encoder with H264 failed");
            return std::nullopt;
        }
        data->codec_context.reset(avcodec_alloc_context3(codec));
        data->codec_context->time_base = stream->time_base;
        data->codec_context->pix_fmt = AV_PIX_FMT_YUV420P;
        data->codec_context->width = frame_width;
        data->codec_context->height = frame_height;

        AVDictionary* opts{nullptr};
        const auto bit_rate_str = std::to_string(bit_rate);
        av_dict_set(&opts, "b", bit_rate_str.c_str(), 0);
        if (0 > avcodec_open2(data->codec_context.get(), codec, &opts))
        {
            av_log(nullptr, AV_LOG_FATAL, "avcodec_open2 failed");
            return std::nullopt;
        }
    }

    {
        int aligned_width = frame_width;
        int aligned_height = frame_height;
        avcodec_align_dimensions(data->codec_context.get(), &aligned_width, &aligned_height);
        if (aligned_width != static_cast<int>(frame_width) || aligned_height != static_cast<int>(frame_height))
        {
            av_log(nullptr, AV_LOG_VERBOSE, "codec aligned frame size to %dx%d", aligned_width, aligned_height);
        }

        std::tie(data->frame_data_rgb, data->avframe_rgb) =
            allocate_rgb_frame(frame_width, frame_height, aligned_width, aligned_height);

        std::tie(data->frame_data_yuv, data->avframe_yuv) =
            allocate_yuv_frame(frame_width, frame_height, aligned_width, aligned_height);
    }

    data->sws_context.reset(sws_getContext(
        frame_width,
        frame_height,
        static_cast<AVPixelFormat>(data->avframe_rgb->format),
        frame_width,
        frame_height,
        static_cast<AVPixelFormat>(data->avframe_yuv->format),
        0,
        nullptr,
        nullptr,
        nullptr
    ));
    if (!data->sws_context)
    {
        av_log(nullptr, AV_LOG_FATAL, "sws_getContext failed");
        return std::nullopt;
    }

    return VideoWriter(std::move(data));
}

VideoWriter::VideoWriter(std::unique_ptr<VideoWriterData> data)
: _data(std::move(data))
{}

VideoWriter::~VideoWriter()
{
    finalize();
}

bool VideoWriter::add_frame(const std::uint8_t* frame_contents, std::ptrdiff_t line_stride)
{
    const std::uint8_t* src_line = frame_contents;
    std::uint8_t* dest_line = _data->frame_data_rgb.get();
    const int width = _data->avframe_rgb->width;
    for (int y = 0; y < _data->avframe_rgb->height; ++y)
    {
        memcpy(dest_line, src_line, 3 * width);
        src_line += line_stride;
        dest_line += _data->avframe_rgb->linesize[0];
    }

    if (0 > sws_scale(
        _data->sws_context.get(),
        _data->avframe_rgb->data,
        _data->avframe_rgb->linesize,
        0,
        _data->avframe_rgb->height,
        _data->avframe_yuv->data,
        _data->avframe_yuv->linesize
    ))
    {
        av_log(nullptr, AV_LOG_FATAL, "sws_scale failed");
        return false;
    }

    _data->avframe_yuv->pts = _data->num_encoded_frames *
        static_cast<std::int64_t>(av_q2d(av_div_q(AVRational{1, _data->frame_rate}, _data->stream_time_base)));
    _data->avframe_yuv->coded_picture_number = _data->num_encoded_frames;
    _data->avframe_yuv->display_picture_number = _data->num_encoded_frames;

    bool must_resend_frame{false};

    while (true)
    {
        switch (avcodec_send_frame(_data->codec_context.get(), _data->avframe_yuv.get()))
        {
        case 0:
            _data->num_encoded_frames += 1;
            break;

        case AVERROR(EAGAIN):
            must_resend_frame = true;
            break;

        default:
            av_log(nullptr, AV_LOG_FATAL, "avcodec_send_frame failed");
            return false;
        }

        if (!write_out_encoded_packets())
        {
            return false;
        }

        if (!must_resend_frame) { break; }
    }


    return true;
}

bool VideoWriter::finalize()
{
    if (!_data) { return false; }
    if (_data->finalized) { return true; }

    // flush the encoder
    if (0 > avcodec_send_frame(_data->codec_context.get(), nullptr))
    {
        av_log(nullptr, AV_LOG_FATAL, "flushing the encoder failed");
        return false;
    }

    if (!write_out_encoded_packets())
    {
        return false;
    }

    if (0 > av_write_trailer(_data->format_context.get()))
    {
        return false;
    }

    _data->finalized = true;

    return true;
}

bool VideoWriter::write_out_encoded_packets()
{
    while (true)
    {
        AVPacket packet{};
        int result = avcodec_receive_packet(_data->codec_context.get(), &packet);
        if (0 == result)
        {
            if (0 > av_interleaved_write_frame(_data->format_context.get(), &packet))
            {
                av_log(nullptr, AV_LOG_FATAL, "av_interleaved_write_frame failed");
                return false;
            }
            av_packet_unref(&packet);
        }
        else if (AVERROR(EAGAIN) == result || AVERROR_EOF == result)
        {
            break;
        }
        else
        {
            av_log(nullptr, AV_LOG_FATAL, "avcodec_receive_packet failed");
            return false;
        }
    }

    return true;
}
