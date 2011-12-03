#include <libavformat/avformat.h>

#include "demuxer.h"

/**
 * Small helper function that prints an error message followed by a description of
 * the specified AV error code. Inspired by the POSIX perror() function.
 */
void print_av_error_and_exit(char *prefix, int error){
	char message[255];
	
	if (av_strerror(error, message, 255) == 0)
		status_error("%s: av error: %s\n", prefix, message);
	else
		status_error("%s: unknown av error, code: %d\n", prefix, error);
	
	exit(-1);
}


static void* buffer_alloc(int type)
{
	switch(type){
		case 'info':
			return malloc(sizeof(demuxer_streams_t));
			break;
		case 'pakt':
			return malloc(sizeof(demuxer_packet_t));
			break;
		case 'stop':
			return NULL;
			break;
		default:
			status_error("demuxer: tried to alloate unknown buffer: %x\n", type);
			error(-1);
			break;
	}
}

static void buffer_cleaner(int type, void *buffer)
{
}

static void buffer_dealloc(int type, void *buffer);
{
	if (type == 'info' || type == 'pakt')
		free(buffer);
}

/**
 * Opens the file specified in `opts->input_file` and scanns it for additional information. This
 * is necessary since DV files do not contain much of a header. We need to scan them in order
 * to get an proper audio and video stream.
 */
void* demuxer_main(demuxer_t *opts)
{
	AVFormatContext *format_context_ptr = NULL;
	
	av_register_all();
	
	// Open the input file
	int error = avformat_open_input(&format_context_ptr, opts->input_file, NULL, NULL);
	if (error != 0)
		print_av_error_and_exit("avformat_open_input", error);
	
	error = av_find_stream_info(format_context_ptr);
	if (error < 0)
		print_av_error_and_exit("av_find_stream_info", error);
	
	av_dump_format(format_context_ptr, 0, opts->input_file, 0);
	
	// Search for the video and audio streams with the highest bitrate if the stream indecies
	// are `-1` (auto detect)
	if (opts->video_stream_index == -1){
		int selected_bitrate = -1;
		for(int i = 0; i < format_context_ptr->nb_streams; i++){
			AVCodecContext *codec_context_ptr = format_context_ptr->streams[i]->codec;
			if ( codec_context_ptr->codec_type == AVMEDIA_TYPE_VIDEO && codec_context_ptr->bit_rate > selected_bitrate ){
				opts->video_stream_index = i;
				selected_bitrate = codec_context_ptr->bit_rate;
			}
		}
	}
	
	if (opts->audio_stream_index == -1){
		int selected_bitrate = -1;
		for(int i = 0; i < format_context_ptr->nb_streams; i++){
			AVCodecContext *codec_context_ptr = format_context_ptr->streams[i]->codec;
			if ( codec_context_ptr->codec_type == AVMEDIA_TYPE_AUDIO && codec_context_ptr->bit_rate > selected_bitrate ){
				opts->audio_stream_index = i;
				selected_bitrate = codec_context_ptr->bit_rate;
			}
		}
	}
	
	status_info("Using video steam %d and audio steam %d\n", opts->video_stream_index, opts->audio_stream_index);
	
	// Attach queues
	queue_attach_producer(opts->out, buffer_alloc, buffer_cleaner, buffer_dealloc);
	queue_preallocate_buffers(opts->out, 1, 'pack');
	
	// Send info buffer
	demuxer_streams_t *info = queue_push_begin(opts->out, 'info');
	info->video_stream_ptr = format_context_ptr->streams[opts->video_stream_index];
	info->audio_stream_ptr = format_context_ptr->streams[opts->audio_stream_index];
	queue_push_end(opts->out);
	
	// Send packet buffers
	int frame_decoded;
	AVPacket packet;
	
	status_info("Demuxer: starting…\n");
	
	while( av_read_frame(format_context_ptr, &packet) >= 0 ){
		if (packet.stream_index == opts.video_stream_index || packet.stream_index == opts.audio_stream_index){
			demuxer_packet_t *packet_buffer = queue_push_begin(opts->out, 'pack');
			packet_buffer->packet_ptr = &packet;
			queue_push_end(opts->out);
		}
	}
	
	status_info("Demuxer: finished…\n");
	
	// Clean up
	queue_detach_producer(opts->out);
	av_close_input_file(format_context_ptr);
	av_free_paket(&packet);
	
	free(opts);
}