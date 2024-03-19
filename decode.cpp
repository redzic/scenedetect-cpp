#include "decode.h"

#include <cassert>

std::variant<DecodeContext, DecoderCreationError>
DecodeContext::open(const char* url) {
    auto pkt = make_resource<AVPacket, av_packet_alloc, av_packet_free>();

    // this should work with the way smart pointers work right?
    if (pkt == nullptr) {
        return DecoderCreationError{
            .type = DecoderCreationError::AllocationFailure};
    }

    // auto frame1 = make_managed<AVFrame, av_frame_alloc, av_frame_free>();

    // TODO properly clean up resources on alloc failure
    FrameBuf frame_buffer{};

    for (auto& frame : frame_buffer) {
        frame = av_frame_alloc();
        if (frame == nullptr) {
            return DecoderCreationError{
                .type = DecoderCreationError::AllocationFailure};
        }
    }

    AVFormatContext* raw_demuxer = nullptr;

    // avformat_open_input automatically frees on failure so we construct
    // the smart pointer AFTER this expression.
    {
        int ret = avformat_open_input(&raw_demuxer, url, nullptr, nullptr);
        if (ret < 0) {
            return {DecoderCreationError{.type = DecoderCreationError::AVError,
                                         .averror = ret}};
        }
    }

    assert(raw_demuxer != nullptr);
    auto demuxer =
        std::unique_ptr<AVFormatContext, decltype([](AVFormatContext* ctx) {
                            avformat_close_input(&ctx);
                        })>(raw_demuxer);

    avformat_find_stream_info(demuxer.get(), nullptr);

    // find stream idx of video stream
    int stream_idx = [](AVFormatContext* demuxer) {
        for (unsigned int stream_idx = 0; stream_idx < demuxer->nb_streams;
             stream_idx++) {
            if (demuxer->streams[stream_idx]->codecpar->codec_type ==
                AVMEDIA_TYPE_VIDEO) {
                return static_cast<int>(stream_idx);
            }
        }
        return -1;
    }(demuxer.get());

    if (stream_idx < 0) {
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

    return std::variant<DecodeContext, DecoderCreationError>{
        std::in_place_type<DecodeContext>,
        demuxer.release(),
        stream,
        decoder.release(),
        pkt.release(),
        frame_buffer};
}

int run_decoder(DecodeContext& dc, size_t framebuf_offset, size_t max_frames) {
    assert(max_frames > 0);
    assert(max_frames <= dc.framebuf.size());
    if ((framebuf_offset + max_frames - 1) >= dc.framebuf.size() ||
        framebuf_offset >= dc.framebuf.size()) {
        // bounds check failed
        return -1;
    }

    // AVCodecContext allocated with alloc context
    // previously was allocated with non-NULL codec,
    // so we can pass NULL here.
    int ret = avcodec_open2(dc.decoder, nullptr, nullptr);
    if (ret < 0) [[unlikely]] {
        return ret;
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
        ret = receive_frames();
        if (ret == AVERROR_EOF) [[unlikely]] {
            return (int)output_index;
        } else if (ret < 0 && ret != AVERROR(EAGAIN)) [[unlikely]] {
            return ret;
        } else [[likely]] {
        }

        // Get packet (compressed data) from demuxer
        ret = av_read_frame(dc.demuxer, dc.pkt);
        // EOF in compressed data
        if (ret < 0) [[unlikely]] {
            break;
        }

        // skip packets other than the ones we're interested in
        if (dc.pkt->stream_index != dc.stream->index) [[unlikely]] {
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
        } else [[likely]] {
        }

        if (output_index >= max_frames) [[unlikely]] {
            return (int)output_index;
        }
    }

    // once control flow reached here, it is guaranteed that more frames need to
    // be received to fill the buffer send flush packet
    // TODO error handling here as well
    avcodec_send_packet(dc.decoder, nullptr);

    ret = receive_frames();
    if (ret == AVERROR_EOF || ret == 0) {
        return (int)output_index;
    } else {
        return ret;
    }
}
