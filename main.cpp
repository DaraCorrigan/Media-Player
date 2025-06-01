extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <SDL2/SDL.h>
}

#include <iostream>
#include <chrono>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: MediaPlayer <video_file>" << std::endl;
        return -1;
    }

    const char* filepath = argv[1];
    avformat_network_init();

    AVFormatContext* formatCtx = nullptr;
    if (avformat_open_input(&formatCtx, filepath, nullptr, nullptr) != 0) {
        std::cerr << "Error opening file." << std::endl;
        return -1;
    }

    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        std::cerr << "Couldn't find stream information." << std::endl;
        return -1;
    }

    int videoStreamIndex = -1;
    int audioStreamIndex = -1;

    for (unsigned int i = 0; i < formatCtx->nb_streams; i++) {
        AVMediaType type = formatCtx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && videoStreamIndex == -1)
            videoStreamIndex = i;
        else if (type == AVMEDIA_TYPE_AUDIO && audioStreamIndex == -1)
            audioStreamIndex = i;
    }

    if (videoStreamIndex == -1 || audioStreamIndex == -1) {
        std::cerr << "Missing video or audio stream." << std::endl;
        return -1;
    }

    // Video
    AVCodecParameters* videoParams = formatCtx->streams[videoStreamIndex]->codecpar;
    const AVCodec* videoCodec = avcodec_find_decoder(videoParams->codec_id);
    AVCodecContext* videoCtx = avcodec_alloc_context3(videoCodec);
    avcodec_parameters_to_context(videoCtx, videoParams);
    avcodec_open2(videoCtx, videoCodec, nullptr);

    // Audio
    AVCodecParameters* audioParams = formatCtx->streams[audioStreamIndex]->codecpar;
    const AVCodec* audioCodec = avcodec_find_decoder(audioParams->codec_id);
    AVCodecContext* audioCtx = avcodec_alloc_context3(audioCodec);
    avcodec_parameters_to_context(audioCtx, audioParams);
    avcodec_open2(audioCtx, audioCodec, nullptr);

    SwrContext* swrCtx = swr_alloc();
    av_channel_layout_default(&audioCtx->ch_layout, 2);

    av_opt_set_chlayout(swrCtx, "in_chlayout", &audioParams->ch_layout, 0);
    av_opt_set_int(swrCtx, "in_sample_rate", audioCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", audioCtx->sample_fmt, 0);

    AVChannelLayout stereoLayout;
    av_channel_layout_default(&stereoLayout, 2);

    av_opt_set_chlayout(swrCtx, "out_chlayout", &stereoLayout, 0);
    av_opt_set_int(swrCtx, "out_sample_rate", audioCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    swr_init(swrCtx);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_AudioSpec wantedSpec, obtainedSpec;
    wantedSpec.freq = audioCtx->sample_rate;
    wantedSpec.format = AUDIO_S16SYS;
    wantedSpec.channels = 2;
    wantedSpec.silence = 0;
    wantedSpec.samples = 1024;
    wantedSpec.callback = nullptr;

    SDL_AudioDeviceID audioDevice = SDL_OpenAudioDevice(nullptr, 0, &wantedSpec, &obtainedSpec, 0);
    if (audioDevice == 0) {
        std::cerr << "Couldn't open SDL audio: " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_PauseAudioDevice(audioDevice, 0);

    SDL_Window* window = SDL_CreateWindow("MediaPlayer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, videoCtx->width, videoCtx->height, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 0);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, videoCtx->width, videoCtx->height);

    SwsContext* swsCtx = sws_getContext(videoCtx->width, videoCtx->height, videoCtx->pix_fmt, videoCtx->width, videoCtx->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);

    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();
    AVFrame* audioFrame = av_frame_alloc();
    uint8_t* buffer = (uint8_t*)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_RGB24, videoCtx->width, videoCtx->height, 1));
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, buffer, AV_PIX_FMT_RGB24, videoCtx->width, videoCtx->height, 1);

    AVPacket packet;
    SDL_Event event;
    auto start_time = std::chrono::steady_clock::now();
    AVRational time_base = formatCtx->streams[videoStreamIndex]->time_base;

    while (av_read_frame(formatCtx, &packet) >= 0) {
        if (packet.stream_index == videoStreamIndex) {
            avcodec_send_packet(videoCtx, &packet);
            while (avcodec_receive_frame(videoCtx, frame) == 0) {
                sws_scale(swsCtx, frame->data, frame->linesize, 0, videoCtx->height, rgbFrame->data, rgbFrame->linesize);

                int64_t pts = frame->best_effort_timestamp;
                double frame_time_ms = pts * av_q2d(time_base) * 1000;
                auto now = std::chrono::steady_clock::now();
                double elapsed_ms = std::chrono::duration<double, std::milli>(now - start_time).count();

                if (frame_time_ms > elapsed_ms)
                    SDL_Delay(static_cast<Uint32>(frame_time_ms - elapsed_ms));

                SDL_UpdateTexture(texture, nullptr, rgbFrame->data[0], rgbFrame->linesize[0]);
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, nullptr, nullptr);
                SDL_RenderPresent(renderer);
            }
        } else if (packet.stream_index == audioStreamIndex) {
            avcodec_send_packet(audioCtx, &packet);
            while (avcodec_receive_frame(audioCtx, audioFrame) == 0) {
                uint8_t** outBuf = nullptr;
                int out_samples = av_rescale_rnd(swr_get_delay(swrCtx, audioCtx->sample_rate) + audioFrame->nb_samples, audioCtx->sample_rate, audioCtx->sample_rate, AV_ROUND_UP);
                av_samples_alloc_array_and_samples(&outBuf, nullptr, 2, out_samples, AV_SAMPLE_FMT_S16, 0);

                int converted = swr_convert(swrCtx, outBuf, out_samples, (const uint8_t**)audioFrame->data, audioFrame->nb_samples);
                int data_size = av_samples_get_buffer_size(nullptr, 2, converted, AV_SAMPLE_FMT_S16, 1);
                SDL_QueueAudio(audioDevice, outBuf[0], data_size);
                av_freep(&outBuf[0]);
                av_freep(&outBuf);
            }
        }
        av_packet_unref(&packet);

        SDL_PollEvent(&event);
        if (event.type == SDL_QUIT)
            break;
    }

    av_free(buffer);
    av_frame_free(&frame);
    av_frame_free(&rgbFrame);
    av_frame_free(&audioFrame);
    sws_freeContext(swsCtx);
    swr_free(&swrCtx);
    avcodec_free_context(&videoCtx);
    avcodec_free_context(&audioCtx);
    avformat_close_input(&formatCtx);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_CloseAudioDevice(audioDevice);
    SDL_Quit();

    return 0;
}
