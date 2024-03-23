#pragma once

#include "resource.h"

#include <cassert>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

// Something needs to be done about these damn timestamps man.

// So I think after segmenting the ORIGINAL timestamps need to be
// copied from the video.

// or perhaps these should be string_views
// int concat_video(const char* f1, const char* f2) {
[[nodiscard]] inline int concat_video(unsigned int i,
                                      const char* out_filename) {
    assert(i >= 1);

    int ret = 0;
    auto pkt = make_resource<AVPacket, av_packet_alloc, av_packet_free>();

    // this should work with the way smart pointers work right?
    if (pkt == nullptr) {
        return -1;
    }

    // TODO. Deduplicate all this code between demuxers and stuff.

    AVFormatContext* raw_demuxer = nullptr;

    // avformat_open_input automatically frees on failure so we construct
    // the smart pointer AFTER this expression.

    // So I imagine that we do a similar thing here with creating multiple input
    // streams and just opening the concat demuxer.

    const auto* concat = av_find_input_format("concat");
    assert(concat != nullptr);

    // ideally there would be a version of fopen that relies on flags or
    // something to do this instead of this BS string passing mechanism.
    // Is it possible to create my own standard library?
    // I mean, for this project, probably not. Because ffmpeg relies on it.
    // BUT, there's nothing stopping me from creating my own versions
    // of these functions.
    // notice we don't use append mode here
    FILE* concat_file = fopen("concat.txt", "wb");

    // (void)fprintf(concat_file, "file '%s'\n", f1);
    // (void)fprintf(concat_file, "file '%s'\n", f2);
    (void)fprintf(concat_file, "file 'OUTPUT%d.mp4'\n", i - 1);
    (void)fprintf(concat_file, "file 'OUTPUT%d.mp4'\n", i);

    fclose(concat_file);
    // should we delete the file?

    {

        // the open input needs to be of the concat file bruh
        ret = avformat_open_input(&raw_demuxer, "concat.txt", concat, nullptr);
        if (ret < 0) {
            return ret;
        }
    }

    assert(raw_demuxer != nullptr);
    auto ifmt_ctx =
        std::unique_ptr<AVFormatContext, decltype([](AVFormatContext* ctx) {
                            avformat_close_input(&ctx);
                        })>(raw_demuxer);

    avformat_find_stream_info(ifmt_ctx.get(), nullptr);

    int video_idx = av_find_best_stream(ifmt_ctx.get(), AVMEDIA_TYPE_VIDEO, -1,
                                        -1, nullptr, 0);

    if (video_idx < 0) {
        return -1;
    }

    // create output format context

    // now I believe we can read packets and put them all in an output stream.

    // index is stored in AVStream->index

    const AVOutputFormat* ofmt = nullptr;

    AVFormatContext* ofmt_ctx = nullptr;
    avformat_alloc_output_context2(&ofmt_ctx, nullptr, nullptr, out_filename);
    if (ofmt_ctx == nullptr) {
        fprintf(stderr, "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        return ret;
        // goto end;
    }

    ofmt = ofmt_ctx->oformat;
    assert(ofmt != nullptr);

    {
        auto* in_stream = ifmt_ctx->streams[video_idx];
        AVCodecParameters* in_codecpar = in_stream->codecpar;

        assert(in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO);

        auto* out_stream = avformat_new_stream(ofmt_ctx, nullptr);
        if (out_stream == nullptr) {
            fprintf(stderr, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            // goto end;
            // TODO fix memory leak
            return ret;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            // goto end;
            // TODO fix memory leak
            return ret;
        }
        out_stream->codecpar->codec_tag = 0;
    }
    // av_dump_format(ofmt_ctx, 0, out_filename, 1);

    if ((ofmt->flags & AVFMT_NOFILE) == 0) {
        ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'", out_filename);
            // goto end;
            return ret;
            // TODO memory leak
        }
    }

    // yeah so we REALLY can't rely on filenames to correctly
    // detect the number of segments written, unfortunately.

    ret = avformat_write_header(ofmt_ctx, nullptr);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        // goto end;
        return ret;
    }

    while (true) {
        // TODO rename demuxer
        ret = av_read_frame(ifmt_ctx.get(), pkt.get());
        if (ret < 0) {
            break;
        }

        auto* in_stream = ifmt_ctx->streams[pkt->stream_index];
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt.get());
            continue;
        }

        auto* out_stream = ofmt_ctx->streams[pkt->stream_index];

        /* copy packet */
        av_packet_rescale_ts(pkt.get(), in_stream->time_base,
                             out_stream->time_base);
        pkt->pos = -1;

        ret = av_write_frame(ofmt_ctx, pkt.get());
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0) {
            fprintf(stderr, "Error muxing packet\n");
            break;
        }
    }

    // so I believe it's not possible for this here to be
    // incremented more than 1 after the trailer is written.
    // Is that correct??

    av_write_trailer(ofmt_ctx);

    // eh whatever we'll write the code to do all this later

    return 0;
}
