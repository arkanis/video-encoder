#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include <pulse/simple.h>
#include <pulse/error.h>

void print_av_error(char *prefix, int error){
	char message[255];
	
	if (av_strerror(error, message, 255) == 0)
		fprintf(stderr, "%s: av error: %s\n", prefix, message);
	else
		fprintf(stderr, "%s: unknown av error, code: %d\n", prefix, error);
}

void main(int argc, char **argv){
	if (argc < 2){
		fprintf(stderr, "invalid args\nusage: %s filename", argv[0]);
		exit(-1);
	}
	
	//
	// Initialize libraries
	//
	
	// libavformat
	av_register_all();
	
	//
	// Open the video file and read the stream information (based on data from the
	// header _and_ content). DV files don't have much header information so we
	// need to scan the content.
	//
	AVFormatContext *format_context_ptr;
	int error = 0;
	error = avformat_open_input(&format_context_ptr, argv[1], NULL, NULL);
	if (error != 0)
		print_av_error("avformat_open_input", error);
	
	error = av_find_stream_info(format_context_ptr);
	if (error < 0)
		print_av_error("av_find_stream_info", error);
	
	av_dump_format(format_context_ptr, 0, argv[1], 0);
	
	
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
	//AVCodecContext *video_context_ptr = format_context_ptr->streams[video_stream]->codec;
	AVCodecContext *audio_context_ptr = format_context_ptr->streams[audio_stream]->codec;
	
	
	//
	// Init video decoder
	//
	/*
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
	*/
	
	
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
	// Init pulse audio playback
	//
	int pa_error = 0;
	pa_sample_spec ss = {
		.format = PA_SAMPLE_S16LE,
		.rate = audio_context_ptr->sample_rate, // 44100,
		.channels = audio_context_ptr->channels // 2
	};
	
	pa_simple *pas_ptr = pa_simple_new(NULL, "av_listen", PA_STREAM_PLAYBACK, NULL, "Video sound", &ss, NULL, NULL, &pa_error);
	if (pas_ptr == NULL){
		fprintf(stderr, __FILE__": pa_simple_new() failed: %s\n", pa_strerror(pa_error));
	}
	
	setenv("PULSE_PROP_media.role", "video", 1);
	
	//
	// Read data
	//
	
	// Taken from ffplay.c line 154
	DECLARE_ALIGNED(16, uint8_t, audio_buffer_ptr)[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
	
	int audio_buffer_size;
	AVPacket packet;
	
	while(av_read_frame(format_context_ptr, &packet) >= 0){
		if (packet.stream_index == video_stream) {
			//printf("v");
		} else if (packet.stream_index == audio_stream) {
			//printf("a");
			
			audio_buffer_size = sizeof(audio_buffer_ptr);
			error = avcodec_decode_audio3(audio_context_ptr, (int16_t*) audio_buffer_ptr, &audio_buffer_size, &packet);
			if (error < 0)
				fprintf(stderr, "error while decoding audio frame: %d\n", error);
			
			if (audio_buffer_size > 0){
				pa_usec_t latency;
				
				if ((latency = pa_simple_get_latency(pas_ptr, &pa_error)) == (pa_usec_t) -1) {
				    fprintf(stderr, __FILE__": pa_simple_get_latency() failed: %s\n", pa_strerror(pa_error));
				}
				
				fprintf(stderr, "%0.0f msec    \r", (float)latency / 1000.0f);
				
				if ( pa_simple_write(pas_ptr, audio_buffer_ptr, audio_buffer_size, &pa_error) < 0 ){
					fprintf(stderr, __FILE__": pa_simple_write() failed: %s\n", pa_strerror(pa_error));
				}
			}
		}
		
		av_free_packet(&packet);
		fflush(stdout);
	}
	
	
	//
	// Clean up
	//
	
	//avcodec_close(video_context_ptr);
	avcodec_close(audio_context_ptr);
	av_close_input_file(format_context_ptr);
	
	if ( pa_simple_drain(pas_ptr, &pa_error) < 0 ){
		fprintf(stderr, __FILE__": pa_simple_drain() failed: %s\n", pa_strerror(pa_error));
	}
	pa_simple_free(pas_ptr);
}
