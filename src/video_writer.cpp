#include "video_writer.h"

#include <array>
#include <cstddef>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h> //TODO: needed?
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
    //TODO: use unabbreviated names
    std::unique_ptr<AVFormatContext, Deleters::avformatcontext> fmt_context;
    AVOutputFormat                                              output_fmt{};
    std::unique_ptr<AVIOContext, Deleters::aviocontext>         avio_context;
    std::unique_ptr<AVCodecContext, Deleters::avcodeccontext>   codec_ctx;
    std::unique_ptr<std::uint8_t[]>                             frame_data_rgb;
    std::unique_ptr<AVFrame, Deleters::avframe>                 avframe_rgb;
    std::array<std::unique_ptr<std::uint8_t[]>, 3>              frame_data_yuv;
    std::unique_ptr<AVFrame, Deleters::avframe>                 avframe_yuv;
    std::unique_ptr<SwsContext, Deleters::swscontext>           sws_context;
};


std::optional<VideoWriter> VideoWriter::Create(
    std::filesystem::path& output_file,
    unsigned frame_width,
    unsigned frame_height,
    unsigned frame_rate,
    unsigned bit_rate,
    PixelFormat pixel_format
)
{
    auto data = std::make_unique<VideoWriterData>();

    data->fmt_context.reset(avformat_alloc_context());
    if (!data->fmt_context)
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

    AVStream* stream = avformat_new_stream(data->fmt_context.get(), nullptr);
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
    data->fmt_context->pb = data->avio_context.get();

    if (0 > avformat_write_header(data->fmt_context.get(), nullptr))
    {
        av_log(nullptr, AV_LOG_FATAL, "avformat_write_header failed");
        return std::nullopt;
    }

    if (stream->time_base.num != 1 || stream->time_base.den != static_cast<int>(frame_rate))
    {
        av_log(nullptr, AV_LOG_VERBOSE, "muxer set stream time base to %d/%d s", stream->time_base.num, stream->time_base.den);
    }

    {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec)
        {
            av_log(nullptr, AV_LOG_FATAL, "avcodec_find_encoder with H264 failed");
            return std::nullopt;
        }
        data->codec_ctx.reset(avcodec_alloc_context3(codec));
        data->codec_ctx->time_base = stream->time_base;
        data->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        data->codec_ctx->width = frame_width;
        data->codec_ctx->height = frame_height;

        AVDictionary* opts{nullptr};
        const auto bit_rate_str = std::to_string(bit_rate);
        av_dict_set(&opts, "b", bit_rate_str.c_str(), 0);
        if (0 > avcodec_open2(data->codec_ctx.get(), codec, &opts))
        {
            av_log(nullptr, AV_LOG_FATAL, "avcodec_open2 failed");
            return std::nullopt;
        }
    }

    {
        int aligned_width = frame_width;
        int aligned_height = frame_height;
        avcodec_align_dimensions(data->codec_ctx.get(), &aligned_width, &aligned_height);
        if (aligned_width != frame_width || aligned_height != frame_height)
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
    Finalize();
}

bool VideoWriter::AddFrame(std::uint8_t* data, std::ptrdiff_t* line_stride)
{
}

bool VideoWriter::Finalize()
{
}
