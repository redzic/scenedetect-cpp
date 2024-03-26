#include "decode.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/packet.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "resource.h"

std::variant<DecodeContext, DecoderCreationError>
DecodeContext::open(const char* url) {
    auto pkt = make_resource<AVPacket, av_packet_alloc, av_packet_free>();

    // this should work with the way smart pointers work right?
    if (pkt == nullptr) {
        return DecoderCreationError{
            .type = DecoderCreationError::AllocationFailure};
    }

    // avformat_open_input automatically frees on failure so we construct
    // the smart pointer AFTER this expression.
    AVFormatContext* raw_demuxer = nullptr;
    int ret = avformat_open_input(&raw_demuxer, url, nullptr, nullptr);
    if (ret < 0) {
        return {DecoderCreationError{.type = DecoderCreationError::AVError,
                                     .averror = ret}};
    }
    DvAssert(raw_demuxer != nullptr);
    auto demuxer =
        std::unique_ptr<AVFormatContext, decltype([](AVFormatContext* ctx) {
                            avformat_close_input(&ctx);
                        })>(raw_demuxer);

    DvAssert(avformat_find_stream_info(demuxer.get(), nullptr) >= 0);

    int stream_idx = av_find_best_stream(demuxer.get(), AVMEDIA_TYPE_VIDEO, -1,
                                         -1, nullptr, 0);

    if (stream_idx < 0) {
        // TODO fix up error handling here
        // because it could also return no decoder available.
        return {DecoderCreationError{
            .type = DecoderCreationError::NoVideoStream,
        }};
    }

    // index is stored in AVStream->index
    auto* stream = demuxer->streams[stream_idx];

    const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        return {DecoderCreationError{
            .type = DecoderCreationError::NoDecoderAvailable,
        }};
    }

    auto decoder =
        std::unique_ptr<AVCodecContext, decltype([](AVCodecContext* ctx) {
                            avcodec_free_context(&ctx);
                        })>(avcodec_alloc_context3(codec));

    {
        int ret =
            avcodec_parameters_to_context(decoder.get(), stream->codecpar);
        if (ret < 0) {
            return {DecoderCreationError{.type = DecoderCreationError::AVError,
                                         .averror = ret}};
        }
    }

    // set automatic threading
    decoder->thread_count = 0;

    // AVCodecContext allocated with alloc context
    // previously was allocated with non-NULL codec,
    // so we can pass NULL here.
    ret = avcodec_open2(decoder.get(), nullptr, nullptr);
    if (ret < 0) [[unlikely]] {
        return {DecoderCreationError{.type = DecoderCreationError::AVError,
                                     .averror = ret}};
    }

    // auto frame1 = make_resource<AVFrame, av_frame_alloc, av_frame_free>();

    // TODO properly clean up resources on alloc failure
    FrameBuf frame_buffer{};

    for (size_t i = 0; i < frame_buffer.size(); i++) {
        frame_buffer[i] = av_frame_alloc();
        if (frame_buffer[i] == nullptr) [[unlikely]] {
            // free previously allocated frames
            for (size_t j = 0; j < i; j++) {
                av_frame_free(&frame_buffer[i]);
            }
            return DecoderCreationError{
                .type = DecoderCreationError::AllocationFailure};
        }
    }

    return std::variant<DecodeContext, DecoderCreationError>{
        std::in_place_type<DecodeContext>,
        demuxer.release(),
        decoder.release(),
        pkt.release(),
        frame_buffer,
        stream_idx};
}

int run_decoder(DecodeContext& dc, size_t framebuf_offset, size_t max_frames) {
    // TODO could possibly add a check/method in DecodeContext to ensure
    // it's initialized fully before use.

    DvAssert(max_frames > 0);
    DvAssert(max_frames <= dc.framebuf.size());
    // TODO I think the second part of this bounds check is redundant.
    if ((framebuf_offset + max_frames - 1) >= dc.framebuf.size() ||
        framebuf_offset >= dc.framebuf.size()) [[unlikely]] {
        // bounds check failed
        return -1;
    }

    size_t output_index = 0;

    // returns 0 on success, or negative averror
    auto receive_frames = [&dc, max_frames, framebuf_offset,
                           &output_index]() mutable {
        // receive last frames
        while (output_index < max_frames) {
            // here it's equal to the index of the current
            int ret = avcodec_receive_frame(
                dc.decoder, dc.framebuf[framebuf_offset + output_index]);
            if (ret < 0) [[unlikely]] {
                return ret;
            }

            // so the output index here is equal to the number of frames that
            // have been outputted so far.

            output_index++;
        }
        return 0;
    };

    while (true) {
        // Flush any frames currently in the decoder
        //
        // This is needed in case we send a packet, read some of its frames and
        // stop because of max_frames, and need to keep reading frames from the
        // decoder on the next chunk.

        // TODO deduplicate this code
        int ret = receive_frames();
        if (ret == AVERROR_EOF) [[unlikely]] {
            return (int)output_index;
        } else if (ret < 0 && ret != AVERROR(EAGAIN)) [[unlikely]] {
            return ret;
        }

        // Get packet (compressed data) from demuxer
        ret = av_read_frame(dc.demuxer, dc.pkt);
        // EOF in compressed data
        if (ret < 0) [[unlikely]] {
            break;
        }

        // skip packets other than the ones we're interested in
        if (dc.pkt->stream_index != dc.video_index) [[unlikely]] {
            av_packet_unref(dc.pkt);
            continue;
        }

        // Send the compressed data to the decoder
        ret = avcodec_send_packet(dc.decoder, dc.pkt);
        if (ret < 0) [[unlikely]] {
            // Error decoding frame
            av_packet_unref(dc.pkt);

            printf("Error decoding frame!\n");

            return ret;
        } else {
            av_packet_unref(dc.pkt);
        }

        // receive as many frames as possible up until max size
        // TODO check error?
        // TODO check on godbolt if you can return an optional with no overhead.
        ret = receive_frames();
        if (ret == AVERROR_EOF) [[unlikely]] {
            return (int)output_index;
        } else if (ret < 0 && ret != AVERROR(EAGAIN)) [[unlikely]] {
            return ret;
        }

        if (output_index >= max_frames) [[unlikely]] {
            return (int)output_index;
        }
    }

    // once control flow reached here, it is guaranteed that more frames need to
    // be received to fill the buffer send flush packet
    // TODO error handling here as well
    avcodec_send_packet(dc.decoder, nullptr);

    int ret = receive_frames();
    if (ret == AVERROR_EOF || ret == 0) {
        return (int)output_index;
    } else {
        return ret;
    }
}

// TODO check if there's some kind of abstraction layer where I can
// do like generalizeed shit with bitflags and stuff. I mean
// ig there is and that's kinda built into the language or whatever.

// jesus dude this is insanely messy
// TODO FIX
// maybe copy pts too

// by counting packets
CountFramesResult count_video_packets(DecodeContext& dc) {
    // TODO fix memory leaks

    // TODO cache this/use cached value.

    DvAssert(dc.pkt != nullptr);

    unsigned int pkt_count = 0;
    unsigned int nb_discarded = 0;
    // TODO error handling
    while (av_read_frame(dc.demuxer, dc.pkt) == 0) {
        if (dc.pkt->stream_index != dc.video_index) {
            av_packet_unref(dc.pkt);
            continue;
        }
        if ((dc.pkt->flags & AV_PKT_FLAG_DISCARD) != 0) {
            nb_discarded++;
        } else {
            pkt_count++;
        }
        // So it was really just this huh...
        // "packet MUST be unreffed when no longer needed"
        av_packet_unref(dc.pkt);
    }

    return CountFramesResult{
        .error_occurred = false,
        .nb_discarded = nb_discarded,
        .frame_count = pkt_count,
    };
}
