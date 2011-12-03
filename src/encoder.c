#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <getopt.h>
#include <pthread.h>

#include "logger.h"
#include "threads/demuxer.h"
#include "threads/mp4_muxer.h"


/**
 * Structure that contains the parsed command line options.
 */
typedef struct {
	char *input_file;
	char *output_file;
	
	// Indices of the selected audio and video streams
	int video_stream_index, audio_stream_index;
	
	// Begin and end of the timespan that should be encoded. It's converted to PTS
	// values later on.
	char *start_time;
	char *stop_time;
	
	// The text representation of the filter graph the video is piped though. The string
	// is parsed by `avfilter_graph_parse`.
	char *video_filter;
	
	// Log level set by the command line values.
	// TODO: Define levels in logger and map argument to levels: verbose, progress (the default), quiet
	uint8_t log_level;
	
	// If set to `true` an existing output file will be overwritten
	bool force;
} cli_options_t;

/**
 * Prints the CLI help.
 * 
 * TODO: explain time, filters and stream IDs
 */
void show_cli_help(char *name)
{
	fprintf(stderr, "usage: %s [options] <input file> [<output file>]\n"
	"\n"
	"Available options:\n"
	"    -s, --start <time>      start encoding at <time> of input video\n"
	"    -e, --end <time>        end encoding at <time> of input video\n"
	"    -p, --filters <filters> a libavfilter graph used for to post\n"
	"                            processing the input video\n"
	"    --video-stream <id>     ID of the used video stream that will be\n"
	"                            encoded into the output video\n"
	"    --audio-stream <id>     ID of the used audio stream that will be\n"
	"                            encoded into the output video\n"
	"    -q, --quiet             shows only errors and warnings\n"
	"    -v, --verbose           shows everything\n"
	"    -f, --force             overwrite existing output file\n"
	"\n", name);
}

/**
 * Parses the command line options using `getopt_long`. All encountered values are stored
 * in the specified `cli_options_t` struct.
 * 
 * Returns `true` on sucess or `false` if the command line options are invalid (e.g. missing
 * input file):
 */
bool parse_cli_options(cli_options_t *options_ptr, int argc, char **argv)
{
	// First fill with default options
	cli_options_t defaults = {
		.input_file = NULL,
		.output_file = "video.mp4",
		.video_stream_index = -1,
		.audio_stream_index = -1,
		.start_time = NULL,
		.stop_time = NULL,
		.video_filter = NULL,
		.log_level = LOG_LEVEL_PROGRESS,
		.force = false
	};
	*options_ptr = defaults;
	
	// Now parse the command line arguments
	struct option long_opts[] = {
		{"video-stream", required_argument, NULL, 1},
		{"audio-stream", required_argument, NULL, 2},
		{"start", required_argument, NULL, 's'},
		{"end", required_argument, NULL, 'e'},
		{"filters", required_argument, NULL, 'p'},
		{"quiet", no_argument, NULL, 'q'},
		{"verbose", no_argument, NULL, 'v'},
		{"force", required_argument, NULL, 'f'},
		{NULL, 0, NULL, 0}
	};
	
	int long_opt_index = 0, opt_abbr = 0;
	while(true){
		opt_abbr = getopt_long(argc, argv, "s:e:p:qvf", long_opts, &long_opt_index);
		if (opt_abbr == -1)
			break;
		
		switch(opt_abbr){
			case 1:
				options_ptr->video_stream_index = strtol(optarg, NULL, 10);
				break;
			case 2:
				options_ptr->audio_stream_index = strtol(optarg, NULL, 10);
				break;
			case 's':
				options_ptr->start_time = optarg;
				break;
			case 'e':
				options_ptr->stop_time = optarg;
				break;
			case 'p':
				options_ptr->video_filter = optarg;
				break;
			
			case 'q':
				options_ptr->log_level = LOG_LEVEL_WARN;
				break;
			case 'v':
				options_ptr->log_level = LOG_LEVEL_DEBUG;
				break;
			
			case 'f':
				options_ptr->force = true;
				break;
			default:
				// Error message is already printed by `getopt_long`
				show_cli_help(argv[0]);
				return false;
		}
	}
	
	// Read input and optional output file name.
	if (optind < argc)
		options_ptr->input_file = argv[optind];
	optind++;
	
	if (optind < argc)
		options_ptr->output_file = argv[optind];
	
	if (options_ptr->input_file == NULL){
		fprintf(stderr, "no input file specified!\n");
		show_cli_help(argv[0]);
		return false;
	}
	
	return true;
}

int main(int argc, char **argv){
	cli_options_t opts;
	if ( ! parse_cli_options(&opts, argc, argv) )
		return 1;
	
	demuxer_t *dem = malloc(sizeof(demuxer_t));
	dem->input_file = opts.input_file;
	dem->video_stream_index = opts.video_stream_index;
	dem->audio_stream_index = opts.audio_stream_index;
	dem->start_time = opts.start_time;
	dem->stop_time = opts.stop_time;
	
	dem->out = queue_new();
	
	mp4_muxer_t *mux = malloc(sizeof(mp4_muxer_t));
	mux->output_file = opts.output_file;
	
	mux->in = dem->out;
	
	pthread_t dem_thread, mux_thread;
	pthread_create(&dem_thread, NULL, demuxer_main, dem);
	pthread_create(&mux_thread, NULL, mp4_muxer_main, mux);
	
	pthread_join(dem_thread, NULL);
	pthread_join(mux_thread, NULL);
}