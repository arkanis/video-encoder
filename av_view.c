#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>

#include <SDL/SDL.h>

#define SDL_AUDIO_BUFFER_SIZE 1024

void print_av_error(char *prefix, int error){
	char message[255];
	
	if (av_strerror(error, message, 255) == 0)
		fprintf(stderr, "%s: av error: %s\n", prefix, message);
	else
		fprintf(stderr, "%s: unknown av error, code: %d\n", prefix, error);
}

void sdl_audio_callback(void *userdata, uint8_t *stream, int len){
}

void main(int argc, char **argv){
	if (argc < 2){
		fprintf(stderr, "invalid args\nusage: %s filename", argv[0]);
		exit(-1);
	}
	
	//
	// Initialize libraries
	//
	
	// SDL
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
	// libavformat
	av_register_all();
	
	
	//
	// Open the video file and read the stream information (based on data from the
	// header _and_ content). DV files don't have much header information so we
	// need to scan the content.
	//
	AVFormatContext *format_context_ptr;
	int error = 0;
	error = av_open_input_file(&format_context_ptr, argv[1], NULL, 0, NULL);
	if (error != 0)
		print_av_error("av_open_input_file", error);
	
	error = av_find_stream_info(format_context_ptr);
	if (error < 0)
		print_av_error("av_find_stream_info", error);
	
	dump_format(format_context_ptr, 0, argv[1], 0);
	
	
	//
	// Search for video and audio streams
	//
	int i, video_stream = -1, audio_stream = -1;
	for(i = 0; i < format_context_ptr->nb_streams; i++){
		if (format_context_ptr->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO && video_stream < 0)
			video_stream = i;
		
		if (format_context_ptr->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream < 0)
			audio_stream = i;
	}
	
	printf("Using video stream %d, audio stream %d\n", video_stream, audio_stream);
	AVCodecContext *video_context_ptr = format_context_ptr->streams[video_stream]->codec;
	AVCodecContext *audio_context_ptr = format_context_ptr->streams[audio_stream]->codec;
	
	
	//
	// Init the display
	//
	SDL_Surface *screen = SDL_SetVideoMode(video_context_ptr->width, video_context_ptr->height, 0, 0);
	if (!screen){
		fprintf(stderr, "SDL: could not set video mode - exiting\n");
		exit(-1);
	}
	
	SDL_Overlay *overlay = SDL_CreateYUVOverlay(video_context_ptr->width, video_context_ptr->height, SDL_IYUV_OVERLAY, screen);
	
	
	//
	// Init video decoder
	//
	
	AVCodec *video_codec_ptr = avcodec_find_decoder(video_context_ptr->codec_id);
	if (video_codec_ptr == NULL){
		printf("could not find video codec!\n");
		exit(-1);
	}
	
	error = avcodec_open(video_context_ptr, video_codec_ptr);
	if (error < 0){
		print_av_error("avcodec_open for video codec", error);
		exit(-1);
	}
	
	
	//
	// Init audio output
	//
	SDL_AudioSpec wanted_spec;
	wanted_spec.freq = audio_context_ptr->sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.channels = audio_context_ptr->channels;
	wanted_spec.silence = 0;
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted_spec.callback = sdl_audio_callback;
	wanted_spec.userdata = audio_context_ptr;
	
	if( SDL_OpenAudio(&wanted_spec, NULL) != 0 ){
		fprintf(stderr, "SDL: could not open audio device - exiting\n");
		exit(-1);
	}
	
	
	//
	// Init audio decoder
	//
	
	AVCodec *audio_codec_ptr = avcodec_find_decoder(audio_context_ptr->codec_id);
	if (audio_codec_ptr == NULL){
		printf("could not find audio codec!\n");
		exit(-1);
	}
	
	error = avcodec_open(audio_context_ptr, audio_codec_ptr);
	if (error < 0){
		print_av_error("avcodec_open for audio codec", error);
		exit(-1);
	}
	
	
	//
	// Read data
	//
	
	AVFrame *frame_ptr = avcodec_alloc_frame();
	
	int frame_finished;
	AVPacket packet;
	
	struct SwsContext* scaler = sws_getContext(video_context_ptr->width, video_context_ptr->height, video_context_ptr->pix_fmt,
		video_context_ptr->width, video_context_ptr->height, video_context_ptr->pix_fmt,
		SWS_FAST_BILINEAR, NULL, NULL, NULL);
	
	while(av_read_frame(format_context_ptr, &packet) >= 0){
		if (packet.stream_index == video_stream){
			error = avcodec_decode_video2(video_context_ptr, frame_ptr, &frame_finished, &packet);
			if (error < 0)
				fprintf(stderr, "error while decoding frame: %d\n", error);
			
			if (frame_finished){
				AVPicture pic;
				
				SDL_LockYUVOverlay(overlay);
				int des_stride[3] = {overlay->pitches[0], overlay->pitches[1], overlay->pitches[2]};
				sws_scale(scaler, (const uint8_t * const*)frame_ptr->data, frame_ptr->linesize,
					0, video_context_ptr->height, overlay->pixels, des_stride);
				
				/*
				if (codec_context_ptr->pix_fmt != PIX_FMT_YUV420P) {
					fprintf(stderr, "frame in unsupported format!\n");
				} else {
					int copy_stride_size = (overlay->pitches[0] < frame_ptr->linesize[0]) ? overlay->pitches[0] : frame_ptr->linesize[0];
					memcpy(overlay->pixels[0], frame_ptr->data[0], copy_stride_size * codec_context_ptr->height);
				}
				*/
				/*
				pic.data[0] = overlay->pixels[0];
				pic.data[1] = overlay->pixels[2];
				pic.data[2] = overlay->pixels[1];
				pic.linesize[0] = overlay->pitches[0];
				pic.linesize[1] = overlay->pitches[2];
				pic.linesize[2] = overlay->pitches[1];
				
				img_convert(&pic, PIX_FMT_YUV420P, (AVPicture*) frame_ptr,
					codec_context_ptr->pix_fmt, codec_context_ptr->width, codec_context_ptr->height);
				*/
				SDL_UnlockYUVOverlay(overlay);
				
				SDL_Rect rect;
				rect.x = 0;
				rect.y = 0;
				rect.w = video_context_ptr->width;
				rect.h = video_context_ptr->height;
				SDL_DisplayYUVOverlay(overlay, &rect);
				
				SDL_Delay(30);
			}
		}
		
		SDL_Event event;
		SDL_PollEvent(&event);
		if (event.type == SDL_QUIT)
			break;
		
		av_free_packet(&packet);
	}
	
	av_free(frame_ptr);
	
	
	//
	// Clean up
	//
	
	SDL_FreeYUVOverlay(overlay);
	SDL_Quit();
	
	avcodec_close(video_context_ptr);
	avcodec_close(audio_context_ptr);
	av_close_input_file(format_context_ptr);
}
