#include <libavformat/avformat.h>
#include "../queue.h"

typedef struct {
	// Options
	char *input_file;
	int video_stream_index, audio_stream_index
	char *start_time;
	char *stop_time;
	
	// Output queues
	queue_t out;
} demuxer_t;

void* demuxer_main(demuxer_t *options);

typedef struct {
	AVStream *video_stream_ptr, *audio_stream_ptr;
} demuxer_streams_t;

typedef struct {
	AVPacket *packet_ptr;
} demuxer_packet_t;