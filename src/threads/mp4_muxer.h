#include "../queue.h"

typedef struct {
	// Options
	char *output_file;
	
	// Input queues
	queue_t in;
} mp4_muxer_t;

void* mp4_muxer_main(mp4_muxer_t *options);