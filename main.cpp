/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <cstdlib>
#include <cstring>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

struct DecodeContext {
    AVFormatContext* demuxer;
    AVStream* stream;
    AVCodecContext* decoder;

    AVPacket* pkt;
    AVFrame* frame;

    int (*process_frame)(struct DecodeContext* dc, AVFrame* frame);
    void* opaque;

    AVDictionary* decoder_opts;
    int max_frames;
};

int print_pixel(DecodeContext* dc, AVFrame* frame) {
    int* numcalled = static_cast<int*>(dc->opaque);

    if (frame != nullptr) {
        printf("data: %d\n", static_cast<int>(frame->data[0][0]));
        *numcalled += 1;
    } else {
        printf("print_pixel() called %d times\n", *numcalled);
    }

    return 0;
}

static int decode_read(DecodeContext* dc, int flush) {
    const int ret_done = flush != 0 ? AVERROR_EOF : AVERROR(EAGAIN);
    int ret = 0;

    while (ret >= 0 &&
           (dc->max_frames == 0 || dc->decoder->frame_num < dc->max_frames)) {
        ret = avcodec_receive_frame(dc->decoder, dc->frame);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                int const err = dc->process_frame(dc, nullptr);
                if (err < 0) {
                    return err;
                }
            }

            return (ret == ret_done) ? 0 : ret;
        }

        ret = dc->process_frame(dc, dc->frame);
        av_frame_unref(dc->frame);
        if (ret < 0) {
            return ret;
        }

        if ((dc->max_frames != 0) && dc->decoder->frame_num == dc->max_frames) {
            return 1;
        }
    }

    return (dc->max_frames == 0 || dc->decoder->frame_num < dc->max_frames) ? 0
                                                                            : 1;
}

int ds_run(DecodeContext* dc) {
    int ret = 0;

    ret = avcodec_open2(dc->decoder, nullptr, &dc->decoder_opts);
    if (ret < 0) {
        return ret;
    }

    while (ret >= 0) {
        ret = av_read_frame(dc->demuxer, dc->pkt);
        if (ret < 0) {
            break;
        }
        if (dc->pkt->stream_index != dc->stream->index) {
            av_packet_unref(dc->pkt);
            continue;
        }

        ret = avcodec_send_packet(dc->decoder, dc->pkt);
        if (ret < 0) {
            fprintf(stderr, "Error decoding: %d\n", ret);
            return ret;
        }
        av_packet_unref(dc->pkt);

        ret = decode_read(dc, 0);
        if (ret < 0) {
            fprintf(stderr, "Error decoding: %d\n", ret);
            return ret;
        } else if (ret > 0) {
            goto finish;
        }
    }

    avcodec_send_packet(dc->decoder, nullptr);
    ret = decode_read(dc, 1);
    if (ret < 0) {
        fprintf(stderr, "Error flushing: %d\n", ret);
        return ret;
    }

finish:
    return dc->process_frame(dc, nullptr);
}

void ds_free(DecodeContext* dc) {
    av_dict_free(&dc->decoder_opts);

    av_frame_free(&dc->frame);
    av_packet_free(&dc->pkt);

    avcodec_free_context(&dc->decoder);
    avformat_close_input(&dc->demuxer);
}

int ds_open(DecodeContext* dc, const char* url) {
    const AVCodec* codec = nullptr;
    int ret = 0;
    int stream_idx = -1;

    auto get_video_stream_idx = [](DecodeContext* dc) -> int {
        for (size_t stream_idx = 0; stream_idx < dc->demuxer->nb_streams;
             stream_idx++) {
            if (dc->demuxer->streams[stream_idx]->codecpar->codec_type ==
                AVMEDIA_TYPE_VIDEO) {
                return static_cast<int>(stream_idx);
            }
        }
        return -1;
    };

    memset(dc, 0, sizeof(*dc));

    dc->pkt = av_packet_alloc();
    dc->frame = av_frame_alloc();
    if ((dc->pkt == nullptr) || (dc->frame == nullptr)) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = avformat_open_input(&dc->demuxer, url, nullptr, nullptr);
    if (ret < 0) {
        fprintf(stderr, "Error opening input file: %d\n", ret);
        return ret;
    }

    // I think this is necessary for it to set the fields and shit
    avformat_find_stream_info(dc->demuxer, nullptr);

    stream_idx = get_video_stream_idx(dc);
    if (stream_idx < 0) {
        return AVERROR_DECODER_NOT_FOUND;
    }

    dc->stream = dc->demuxer->streams[stream_idx];

    codec = avcodec_find_decoder(dc->stream->codecpar->codec_id);
    if (codec == nullptr) {
        return AVERROR_DECODER_NOT_FOUND;
    }

    dc->decoder = avcodec_alloc_context3(codec);
    if (dc->decoder == nullptr) {
        return AVERROR(ENOMEM);
    }

    // set to 0 for automatic thread conut detection
    dc->decoder->thread_count = 0;

    ret = avcodec_parameters_to_context(dc->decoder, dc->stream->codecpar);
    if (ret < 0) {
        goto fail;
    }

    return 0;

fail:
    ds_free(dc);
    return ret;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("usage: scenedetect-cpp <video_file>\n");
        return -1;
    }

    DecodeContext dc{};

    // uhh... error checking maybe?
    int ret = ds_open(&dc, argv[1]);
    if (ret < 0) {
        printf("error occurred!\n");
        ds_free(&dc);
        return -1;
    }

    auto frame_count = std::make_unique<int>(0);
    dc.process_frame = print_pixel;
    dc.opaque = (int*)frame_count.get();
    dc.max_frames = 0;
    ds_run(&dc);

    ds_free(&dc);
}
