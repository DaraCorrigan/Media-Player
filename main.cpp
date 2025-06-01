extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <SDL2/SDL.h>
}

#include <iostream>
#include <chrono>

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
    for (unsigned int i = 0; i < formatCtx->nb_streams; i++) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = i;
            break;
        }
    }

    if (videoStreamIndex == -1) {
        std::cerr << "No video stream found." << std::endl;
        return -1;
    }

    AVCodecParameters* codecParams = formatCtx->streams[videoStreamIndex]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        std::cerr << "Unsupported codec!" << std::endl;
        return -1;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, codecParams);
    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        std::cerr << "Couldn't open codec." << std::endl;
        return -1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        return -1;
    }
    std::cout << "✅ SDL initialized!" << std::endl;

    SDL_Window* window = SDL_CreateWindow(
        "MediaPlayer",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        codecCtx->width,
        codecCtx->height,
        0
    );
    if (!window) {
        std::cerr << "❌ Failed to create SDL window: " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        std::cerr << "❌ Failed to create SDL renderer: " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        codecCtx->width,
        codecCtx->height
    );
    if (!texture) {
        std::cerr << "❌ Failed to create SDL texture: " << SDL_GetError() << std::endl;
        return -1;
    }

    SwsContext* swsCtx = sws_getContext(
        codecCtx->width,
        codecCtx->height,
        codecCtx->pix_fmt,
        codecCtx->width,
        codecCtx->height,
        AV_PIX_FMT_RGB24,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr
    );

    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, codecCtx->width, codecCtx->height, 1);
    uint8_t* buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, buffer, AV_PIX_FMT_RGB24, codecCtx->width, codecCtx->height, 1);

    AVPacket packet;
    SDL_Event event;
    bool quit = false;

    auto start_time = std::chrono::steady_clock::now();
    AVRational time_base = formatCtx->streams[videoStreamIndex]->time_base;

    while (!quit) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = true;
                break;
            }
        }

        if (av_read_frame(formatCtx, &packet) >= 0) {
            if (packet.stream_index == videoStreamIndex) {
                if (avcodec_send_packet(codecCtx, &packet) == 0) {
                    while (avcodec_receive_frame(codecCtx, frame) == 0) {
                        sws_scale(
                            swsCtx,
                            frame->data,
                            frame->linesize,
                            0,
                            codecCtx->height,
                            rgbFrame->data,
                            rgbFrame->linesize
                        );

                        int64_t pts = frame->best_effort_timestamp;
                        double frame_time_ms = (pts * av_q2d(time_base)) * 1000;

                        auto now = std::chrono::steady_clock::now();
                        double elapsed_time_ms = std::chrono::duration<double, std::milli>(now - start_time).count();

                        if (frame_time_ms > elapsed_time_ms) {
                            SDL_Delay(static_cast<Uint32>(frame_time_ms - elapsed_time_ms));
                        }

                        SDL_UpdateTexture(texture, nullptr, rgbFrame->data[0], rgbFrame->linesize[0]);
                        SDL_RenderClear(renderer);
                        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
                        SDL_RenderPresent(renderer);
                    }
                }
            }
            av_packet_unref(&packet);
        } else {
            break;
        }
    }

    av_free(buffer);
    av_frame_free(&rgbFrame);
    av_frame_free(&frame);
    sws_freeContext(swsCtx);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&formatCtx);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
