extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_video.h>
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

    AVCodecParameters* videoParams = formatCtx->streams[videoStreamIndex]->codecpar;
    const AVCodec* videoCodec = avcodec_find_decoder(videoParams->codec_id);
    AVCodecContext* videoCtx = avcodec_alloc_context3(videoCodec);
    avcodec_parameters_to_context(videoCtx, videoParams);
    avcodec_open2(videoCtx, videoCodec, nullptr);

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

    SDL_DisplayMode displayMode;
    if (SDL_GetCurrentDisplayMode(0, &displayMode) != 0) {
        std::cerr << "SDL_GetCurrentDisplayMode failed: " << SDL_GetError() << std::endl;
        return -1;
    }
    int displayW = displayMode.w;
    int displayH = displayMode.h;


    const int maxWindowWidth = static_cast<int>(displayW * 0.8);
    const int maxWindowHeight = static_cast<int>(displayH * 0.8);

    int origVideoW = videoCtx->width;
    int origVideoH = videoCtx->height;

    const int baseToolbarHeight = 40;

    float scaleX = static_cast<float>(maxWindowWidth) / origVideoW;
    float scaleY = static_cast<float>(maxWindowHeight - baseToolbarHeight) / origVideoH;
    float scale = (scaleX < scaleY) ? scaleX : scaleY; 

    int windowVideoW = static_cast<int>(origVideoW * scale);
    int windowVideoH = static_cast<int>(origVideoH * scale);
    int toolbarHeight = static_cast<int>(baseToolbarHeight * scale);

    int windowW = windowVideoW;
    int windowH = windowVideoH + toolbarHeight;

    SDL_Window* window = SDL_CreateWindow("MediaPlayer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, windowW, windowH, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 0);

    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, origVideoW, origVideoH);

    SDL_Rect toolbarRect = {0, windowVideoH, windowW, toolbarHeight};
    SDL_Rect playButton = {5, windowVideoH + 5, (windowW / 2) - 10, toolbarHeight - 10};
    SDL_Rect pauseButton = {windowW / 2 + 5, windowVideoH + 5, (windowW / 2) - 10, toolbarHeight - 10};

    SwsContext* swsCtx = sws_getContext(origVideoW, origVideoH, videoCtx->pix_fmt, origVideoW, origVideoH, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);

    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();
    AVFrame* audioFrame = av_frame_alloc();
    uint8_t* buffer = (uint8_t*)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_RGB24, origVideoW, origVideoH, 1));
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, buffer, AV_PIX_FMT_RGB24, origVideoW, origVideoH, 1);

    bool quit = false;
    bool paused = false;

    AVPacket packet;
    SDL_Event event;
    auto start_time = std::chrono::steady_clock::now();
    AVRational time_base = formatCtx->streams[videoStreamIndex]->time_base;

    SDL_Rect destRect = {0, 0, windowVideoW, windowVideoH};

    while (!quit) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = true;
                break;
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN) {
                int x = event.button.x;
                int y = event.button.y;
                if (x >= playButton.x && x < playButton.x + playButton.w &&
                    y >= playButton.y && y < playButton.y + playButton.h) {
                    paused = false;
                    std::cout << "Play pressed\n";
                }
                else if (x >= pauseButton.x && x < pauseButton.x + pauseButton.w &&
                         y >= pauseButton.y && y < pauseButton.y + pauseButton.h) {
                    paused = true;
                    std::cout << "Pause pressed\n";
                }
            }
        }

        if (!paused) {
            if (av_read_frame(formatCtx, &packet) >= 0) {
                if (packet.stream_index == videoStreamIndex) {
                    avcodec_send_packet(videoCtx, &packet);
                    while (avcodec_receive_frame(videoCtx, frame) == 0) {
                        sws_scale(swsCtx, frame->data, frame->linesize, 0, origVideoH, rgbFrame->data, rgbFrame->linesize);

                        int64_t pts = frame->best_effort_timestamp;
                        double frame_time_ms = pts * av_q2d(time_base) * 1000;
                        auto now = std::chrono::steady_clock::now();
                        double elapsed_ms = std::chrono::duration<double, std::milli>(now - start_time).count();

                        if (frame_time_ms > elapsed_ms)
                            SDL_Delay(static_cast<Uint32>(frame_time_ms - elapsed_ms));

                        SDL_UpdateTexture(texture, nullptr, rgbFrame->data[0], rgbFrame->linesize[0]);
                        SDL_RenderClear(renderer);
                        SDL_RenderCopy(renderer, texture, nullptr, &destRect);
                    }
                }
                else if (packet.stream_index == audioStreamIndex) {
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
            }
            else {
                quit = true;
            }
        }

        SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
        SDL_RenderFillRect(renderer, &toolbarRect);

        SDL_SetRenderDrawColor(renderer, 0, 200, 0, 255);
        SDL_RenderFillRect(renderer, &playButton);

        SDL_SetRenderDrawColor(renderer, 200, 0, 0, 255);
        SDL_RenderFillRect(renderer, &pauseButton);

        SDL_RenderPresent(renderer);
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
