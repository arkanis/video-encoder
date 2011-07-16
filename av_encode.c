/**
 * Please be aware: The following code is UGLY. It was hacked together in a
 * few days to learn libavformat, libavcodec, libavfilter, x264, faac, libmp4v2
 * and the MPEG4 spec.
 */

#include <stdio.h>
#include <stdbool.h>

// For option parsing
#include <getopt.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/vsrc_buffer.h>
#include <libavfilter/vsink_buffer.h>
#include <libavfilter/avcodec.h>

#include <x264.h>
#include <faac.h>
#include <mp4v2/mp4v2.h>

/**
 * Small helper function that prints an error message followed by a description of
 * the specified AV error code. Inspired by the POSIX perror() function.
 */
void av_perror(char *prefix, int error){
	char message[255];
	
	if (av_strerror(error, message, 255) == 0)
		fprintf(stderr, "%s: av error: %s\n", prefix, message);
	else
		fprintf(stderr, "%s: unknown av error, code: %d\n", prefix, error);
}

/**
 * Opens the specified file and returns a format context pointer in `format_context_dptr`.
 * The file is scanned for additional information. This is necessary since DV files do not
 * contain much of a header. We need to scan DV files in order to get an proper audio and
 * video stream.
 */
bool reader_open_file(char *filename, AVFormatContext **format_context_dptr){
	int error = 0;
	
	error = avformat_open_input(format_context_dptr, filename, NULL, NULL);
	if (error != 0){
		av_perror("avformat_open_input", error);
		return false;
	}
	
	error = av_find_stream_info(*format_context_dptr);
	if (error < 0){
		av_perror("av_find_stream_info", error);
		return false;
	}
	
	av_dump_format(*format_context_dptr, 0, filename, 0);
	return true;
}

/**
 * Searches and opens a codec for the specified stream.
 */
bool reader_open_codec(AVFormatContext *format_context_ptr, int stream_index, enum AVMediaType required_stream_type, AVCodecContext **codec_context_dptr, AVCodec **codec_dptr){
	if (stream_index < 0 || stream_index >= format_context_ptr->nb_streams){
		fprintf(stderr, "Stream %d is an invalid stream index. Valid stream indices: 0 - %d\n",
			stream_index, format_context_ptr->nb_streams - 1);
		return false;
	}
	
	enum AVMediaType stream_type = format_context_ptr->streams[stream_index]->codec->codec_type;
	if (required_stream_type != stream_type){
		char *type_names[] = {"unknown", "video", "audio", "data", "subtitle", "attachment", "nb"};
		fprintf(stderr, "Tried to use steam %d as %s steam but it is an %s steam!\n",
			stream_index, type_names[required_stream_type+1], type_names[stream_type+1]);
		return false;
	}
	
	*codec_context_dptr = format_context_ptr->streams[stream_index]->codec;
	printf("Searching decoder for stream %d... ", stream_index);
	*codec_dptr = avcodec_find_decoder((*codec_context_dptr)->codec_id);
	if (*codec_dptr == NULL){
		fprintf(stderr, "no matching codec found!\n");
		return false;
	}
	
	if ( avcodec_open(*codec_context_dptr, *codec_dptr) != 0 ){
		fprintf(stderr, "initialization of %s failed!\n", (*codec_dptr)->name);
		return false;
	}
	
	printf("%s initialized\n", (*codec_dptr)->name);
	return true;
}

//
// Global stuff shared between main(), store_nal() and the *filter_graph() functions.
//
int width = 0, height = 0;
AVRational time_base, sample_aspect_ratio;

AVFilterGraph *filter_graph_ptr = NULL;
AVFilterContext *src_filter_context_ptr = NULL, *sink_filter_context_ptr = NULL;

void setup_filter_graph(AVCodecContext *video_codec_context_ptr, const char *filters){
	char filter_args[255];
	int error = 0;
	
	filter_graph_ptr = avfilter_graph_alloc();
	
	// Build the gateway (source) into the filter pipeline
	snprintf(filter_args, sizeof(filter_args), "%d:%d:%d:%d:%d:%d:%d",
		width, height, video_codec_context_ptr->pix_fmt, time_base.num, time_base.den,
		sample_aspect_ratio.num, sample_aspect_ratio.den);
	
	src_filter_context_ptr = NULL;
	error = avfilter_graph_create_filter(&src_filter_context_ptr, avfilter_get_by_name("buffer"), "src", filter_args, NULL, filter_graph_ptr);
	if (error < 0){
		av_perror("avfilter_graph_create_filter", error);
		exit(8);
	}
	
	// Build the output sink of the filter pipeline
	enum PixelFormat pix_fmts[] = { video_codec_context_ptr->pix_fmt, PIX_FMT_NONE };
	sink_filter_context_ptr = NULL;
	error = avfilter_graph_create_filter(&sink_filter_context_ptr, avfilter_get_by_name("buffersink"), "sink", "",  pix_fmts, filter_graph_ptr);
	if (error < 0){
		av_perror("avfilter_graph_create_filter", error);
		exit(8);
	}
	
	// Build the use defined filter graph between the two ends
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs = avfilter_inout_alloc();
	
	outputs->name = av_strdup("in");
	outputs->filter_ctx = src_filter_context_ptr;
	outputs->pad_idx = 0;
	outputs->next = NULL;
	
	inputs->name = av_strdup("out");
	inputs->filter_ctx = sink_filter_context_ptr;
	inputs->pad_idx = 0;
	inputs->next = NULL;
	
	error = avfilter_graph_parse(filter_graph_ptr, filters, &inputs, &outputs, NULL);
	if (error != 0){
		av_perror("avfilter_graph_parse", error);
		exit(8);
	}
	
	error = avfilter_graph_config(filter_graph_ptr, NULL);
	if (error < 0){
		av_perror("avfilter_graph_config", error);
		exit(8);
	}
}

void cleanup_filter_graph(){
	avfilter_graph_free(&filter_graph_ptr);
}


typedef struct {
	uint8_t *data;
	uint32_t size;
	x264_picture_t pic;
	x264_nal_t nals[8];
	int nal_count;
} x264_output_t;


/**
 * Stores the output NALs of the x264 encoder. More or less the general output function that
 * takes care of writing the output videos.
 */
void store_nal(int frame_size, x264_nal_t *nals, int nal_count, FILE *video_stream_dump, MP4FileHandle container, MP4TrackId *video_track_ptr, x264_picture_t *pic_ptr){
	static x264_output_t prev_output = {.data = NULL, .size = 0};
	
	printf(" - x264");
	
	// Write the NALs directly to the raw dump
	if (nals != NULL)
		fwrite(nals[0].p_payload, frame_size, 1, video_stream_dump);
	
	if (prev_output.data != NULL) {
		// We got a previous x264 output so we can calculate a proper decoding delta for the MP4 sample
		int64_t decode_delta, composition_offset;
		decode_delta = pic_ptr->i_dts - prev_output.pic.i_dts;
		composition_offset = prev_output.pic.i_pts - prev_output.pic.i_dts;
		printf(" - x264, prev: (dts: %ld, pts: %ld), curr: (dts: %ld, pts: %ld), dec delta: %ld, comp offset: %ld NALs:",
			prev_output.pic.i_dts, prev_output.pic.i_pts, pic_ptr->i_dts, pic_ptr->i_pts, decode_delta, composition_offset);
		
		x264_nal_t* nal_ptr = NULL;
		int i;
		for(i = 0; i < prev_output.nal_count; i++){
			nal_ptr = &prev_output.nals[i];
			printf(" %d", nal_ptr->i_type);
			switch(nal_ptr->i_type){
				case NAL_SPS:
					// If we have not created a video track yet, do so based on the first sequence parameter set
					if (*video_track_ptr == MP4_INVALID_TRACK_ID){
						uint8_t profile_idc, profile_compat, level_idc;
						
						// Extract some information from the sequence parameter set and use them
						// to add a video track to the mp4 container.
						// 
						// SPS layout:
						// 	byte 0 - 3	startcode (for Annex-B bytestreams) or payload size
						//	byte 4		NAL header (for SPS NALs just the forbidden_zero_bit, nal_ref_idc and nal_unit_type)
						// 	byte 5		profile_idc
						//	byte 6		constraint set flags (profile compatibility flags)
						//	byte 7		level_idc
						//int j;
						//printf("\nSPS size: %d header:", nal_ptr->i_payload);
						//for(j = 0; j < 8; j++)
						//	printf(" %02x", nal_ptr->p_payload[j]);
						
						profile_idc = nal_ptr->p_payload[5];
						profile_compat = nal_ptr->p_payload[6];
						level_idc = nal_ptr->p_payload[7];
						//printf("... track: profile_idc %d, profile_compat %x, level_idc: %d\n", profile_idc, profile_compat, level_idc);
						
						// MP4 muxing does not work with Annex-B streams! If b_annexb is fals x264
						// puts the payload length into the first 4 byte of the NALs. This is perfect for
						// MP4. Therefore we set the sampleLenFieldSizeMinusOne parameter to 3.
						// 
						// Note: MP4_INVALID_SAMPLE_DURATION might work well for ASF streams
						*video_track_ptr = MP4AddH264VideoTrack(container, time_base.num * time_base.den, MP4_INVALID_DURATION, width, height,
							profile_idc, profile_compat, level_idc, 3);
						if (*video_track_ptr == MP4_INVALID_TRACK_ID){
							fprintf(stderr, "failed to create video track in mp4 file!\n");
							exit(7);
						}
						
						MP4AddPixelAspectRatio(container, *video_track_ptr, sample_aspect_ratio.num, sample_aspect_ratio.den);
					}
					
					// Put the sequence parameter set into the MP4 container. Framing is provided
					// by the container, therefore we don't need the leading 4 bytes (the payload size).
					MP4AddH264SequenceParameterSet(container, *video_track_ptr, nal_ptr->p_payload + 4, nal_ptr->i_payload - 4);
					break;
				case NAL_PPS:
					// Put the picture parameter set into the MP4 container. Framing is provided
					// by the container, therefore we don't need the leading 4 bytes (the payload size).
					MP4AddH264PictureParameterSet(container, *video_track_ptr, nal_ptr->p_payload + 4, nal_ptr->i_payload - 4);
					break;
				case NAL_FILLER:
					// Throw filler data away (AVC spec wants it)
					break;
				default:
					// Every thing else is data for the video track. Collect all remaining NALs and
					// put them into one MP4 sample.
					if (*video_track_ptr != MP4_INVALID_TRACK_ID) {
						int remaining_nals = prev_output.nal_count - i;
						uint8_t *start_ptr = prev_output.nals[i].p_payload;
						int size = prev_output.size - ((void*)start_ptr - (void*)(prev_output.nals[0].p_payload));
						
						bool is_sync_sample = prev_output.pic.b_keyframe;
						printf(" storing %d NALs, at %p, %d bytes", remaining_nals, start_ptr, size);
						if ( MP4WriteSample(container, *video_track_ptr, start_ptr, size, decode_delta, composition_offset, is_sync_sample) != true)
							printf(" MP4WriteSample (NAL %d) failed", i);
						
						i += remaining_nals;
					} else {
						printf(" ignoring because there is no video track! ");
					}
					break;
			}
		}		
	}
	
	// Store the current x264 output as the previous output so we can calculate the decoding delta of the next sample
	if (nals != NULL) {
		printf(", buffering output");
		if (prev_output.data != NULL)
			free(prev_output.data);
		prev_output.data = (uint8_t*) malloc(frame_size);
		if (prev_output.data == NULL){
			fprintf(stderr, "store_nal: failed to allcoate buffer for the x264 output!\n");
			return;
		}
		memcpy(prev_output.data, nals[0].p_payload, frame_size);
		prev_output.size = frame_size;
		
		prev_output.pic = *pic_ptr;
		prev_output.nal_count = nal_count;
		if (nal_count >= 8){
			fprintf(stderr, "store_nal: received more NALs than the output buffer can hold!\n");
			return;
		}
		
		for(int i = 0; i < nal_count; i++){
			prev_output.nals[i] = nals[i];
			int offset = nals[i].p_payload - nals[0].p_payload;
			prev_output.nals[i].p_payload = prev_output.data + offset;
		}
	} else {
		printf(", flushing");
	}
}

/**
 * Structure that contains the parsed command line options.
 */
typedef struct {
	// Flag for extensive logging output
	bool verbose;
	// If this flag is set we just output the information about the input video and exit. This
	// provides a way to know what streams are inside the file.
	bool analyze;
	
	// Name of the input file that will be read from
	char *input_file;
	// Index of the video stream that is encoded
	int video_stream_index;
	// Index of the audio stream that is encoded
	int audio_stream_index;
	
	// If not negative sets a limit of frames that will be read from the input video. Useful
	// for testing purpose to encode just the first few hundred frames.
	int64_t frame_limit;
	
	// The text representation of the filter graph the video is piped though. The string
	// is parsed by avfilter_graph_parse().
	char *video_filter;
} cli_options_t;

/**
 * Parses the command line options using `getopt_long()`. All encountered values are stored
 * in the specified cli_options_t struct.
 * 
 * Returns `true` on sucess or `false` if no input file was specified.
 */
bool parse_cli_options(cli_options_t *options_ptr, int argc, char **argv){
	// First fill with default options
	cli_options_t defaults = {
		.verbose = false,
		.analyze = false,
		.input_file = NULL,
		.video_stream_index = -1,
		.audio_stream_index = -1,
		.frame_limit = -1,
		.video_filter = NULL
	};
	*options_ptr = defaults;
	
	// Now parse the command line arguments
	struct option long_opts[] = {
		{"verbose", no_argument, NULL, 2},
		{"analyze", no_argument, NULL, 3},
		{"video-stream", required_argument, NULL, 'v'},
		{"audio-stream", required_argument, NULL, 'a'},
		{"frame-limit", required_argument, NULL, 'l'},
		{"filters", required_argument, NULL, 'f'},
		{NULL, 0, NULL, 0}
	};
	
	int long_opt_index = 0, opt_abbr = 0;
	while(true){
		opt_abbr = getopt_long(argc, argv, "v:a:l:f:", long_opts, &long_opt_index);
		if (opt_abbr == -1)
			break;
		
		switch(opt_abbr){
			case 2:
				options_ptr->verbose = true;
				break;
			case 3:
				options_ptr->analyze = true;
				break;
			case 'v':
				options_ptr->video_stream_index = strtol(optarg, NULL, 10);
				break;
			case 'a':
				options_ptr->audio_stream_index = strtol(optarg, NULL, 10);
				break;
			case 'l':
				options_ptr->frame_limit = strtoll(optarg, NULL, 10);
				break;
			case 'f':
				options_ptr->video_filter = optarg;
				break;
			default:
				// Error message is already printed by `getopt_long()`
				//TODO: show cli help?
				return false;
		}
	}
	
	if (optind < argc)
		options_ptr->input_file = argv[optind];
	
	if (options_ptr->input_file == NULL){
		fprintf(stderr, "no input file specified!\n");
		return false;
	}
	
	printf("verbose: %d\nanalyze: %d\ninput_file: %s\nvideo_stream_index: %d\naudio_stream_index: %d\nframe_limit: %ld\nvideo_filter: %s\n",
		options_ptr->verbose, options_ptr->analyze, options_ptr->input_file,
		options_ptr->video_stream_index, options_ptr->audio_stream_index,
		options_ptr->frame_limit, options_ptr->video_filter);
	
	return true;
}

int main(int argc, char **argv){
	cli_options_t opts;
	if ( ! parse_cli_options(&opts, argc, argv) )
		return 1;
	
	// Init libavformat and register all codecs
	av_register_all();
	avfilter_register_all();
	
	// Open the video to get a format context
	AVFormatContext *format_context_ptr = NULL;
	if ( ! reader_open_file(opts.input_file, &format_context_ptr) )
		return 2;
	
	// Search for the first video and audio streams if the stream indecies are -1 (auto detect)
	if (opts.video_stream_index == -1){
		int selected_bitrate = -1;
		for(int i = 0; i < format_context_ptr->nb_streams; i++){
			AVCodecContext *codec_context_ptr = format_context_ptr->streams[i]->codec;
			if ( codec_context_ptr->codec_type == AVMEDIA_TYPE_VIDEO && codec_context_ptr->bit_rate > selected_bitrate ){
				opts.video_stream_index = i;
				selected_bitrate = codec_context_ptr->bit_rate;
			}
		}
	}
	
	if (opts.audio_stream_index == -1){
		int selected_bitrate = -1;
		for(int i = 0; i < format_context_ptr->nb_streams; i++){
			AVCodecContext *codec_context_ptr = format_context_ptr->streams[i]->codec;
			if ( codec_context_ptr->codec_type == AVMEDIA_TYPE_AUDIO && codec_context_ptr->bit_rate > selected_bitrate ){
				opts.audio_stream_index = i;
				selected_bitrate = codec_context_ptr->bit_rate;
			}
		}
	}
	
	printf("Using video steam %d and audio steam %d\n", opts.video_stream_index, opts.audio_stream_index);
	
	
	// Open a decoder for the specified video stream
	AVCodec *video_codec_ptr = NULL, *audio_codec_ptr = NULL;
	AVCodecContext *video_codec_context_ptr = NULL, *audio_codec_context_ptr = NULL;
	
	if ( ! reader_open_codec(format_context_ptr, opts.video_stream_index, AVMEDIA_TYPE_VIDEO, &video_codec_context_ptr, &video_codec_ptr) )
		return 3;
	if ( ! reader_open_codec(format_context_ptr, opts.audio_stream_index, AVMEDIA_TYPE_AUDIO, &audio_codec_context_ptr, &audio_codec_ptr) )
		return 3;
	
	width = video_codec_context_ptr->width;
	height = video_codec_context_ptr->height;
	time_base = video_codec_context_ptr->time_base;
	sample_aspect_ratio = format_context_ptr->streams[opts.video_stream_index]->sample_aspect_ratio; //video_codec_context_ptr->sample_aspect_ratio;
	
	printf("Decoding video: width: %d, height: %d, time base: (%d/%d), fps: (%d/%d) %lf, sample aspect ratio: (%d/%d)\n",
		width, height, time_base.num, time_base.den, time_base.den, time_base.num, 1 / av_q2d(time_base), sample_aspect_ratio.num, sample_aspect_ratio.den);
	
	// Init the filter graph
	char *filters = NULL;
	if (argc >= 5)
		filters = argv[4];
	else
		filters = "hqdn3d,yadif";
	setup_filter_graph(video_codec_context_ptr, filters);
	
	// Init the x264 encoder
	x264_param_t params;
	if ( x264_param_default_preset(&params, "slow", "film") != 0 )	// use "zerolatency" tune to avoid out of order frames
		fprintf(stderr, "x264: failed to set preset defaults\n");
	
	params.i_width = width;
	params.i_height = height;
	params.b_annexb = false;
	// fps is the reciprocal of the time base
	params.i_fps_num = time_base.den;
	params.i_fps_den = time_base.num;
	// Set the sample aspect ratio for the video stream since this information is also present in the h264 stream
	params.vui.i_sar_width = sample_aspect_ratio.num;
	params.vui.i_sar_height = sample_aspect_ratio.den;
	
	params.rc.i_rc_method = X264_RC_CRF;
	params.rc.f_rf_constant = 20;
	params.rc.f_rf_constant = 25;
	
	
	if ( x264_param_apply_profile(&params, "high") != 0 )
		fprintf(stderr, "x264: failed to apply profile\n");
	x264_t *encoder_ptr = x264_encoder_open(&params);
	if (encoder_ptr == NULL){
		fprintf(stderr, "x264: failed to initialize encoder\n");
		return 5;
	}
	
	// Init the FAAC encoder
	unsigned long faac_input_sample_count, faac_max_output_byte_count;
	faacEncHandle faac_encoder = faacEncOpen(audio_codec_context_ptr->sample_rate, audio_codec_context_ptr->channels, &faac_input_sample_count, &faac_max_output_byte_count);
	if (faac_encoder == NULL){
		fprintf(stderr, "faac: failed to initialize encoder\n");
		return 6;
	}
	
	// Calculate the number of samples one frame is long (for one channel)
	uint32_t aac_frame_size = faac_input_sample_count / audio_codec_context_ptr->channels;
	
	// Allocate buffers for the libavcodec output (the raw samples) and for the FAAC output (the AAC bitstream)
	int sample_buffer_size = 2 * AVCODEC_MAX_AUDIO_FRAME_SIZE;
	int sample_buffer_used = 0, sample_size = sizeof(int16_t);
	int16_t *sample_buffer_ptr = (int16_t*) av_mallocz(sample_buffer_size);
	
	int aac_buffer_size = faac_max_output_byte_count;
	uint8_t *aac_buffer_ptr = (uint8_t*) av_mallocz(faac_max_output_byte_count);
	
	if (sample_buffer_ptr == NULL || aac_buffer_ptr == NULL){
		fprintf(stderr, "faac: failed to malloc buffer\n");
		return 6;
	}
	
	// Detail config of the encoder
	faacEncConfigurationPtr faac_config_ptr = faacEncGetCurrentConfiguration(faac_encoder);
	faac_config_ptr->mpegVersion = MPEG4;  // for Windows Media Player. It only accpets mpeg4 audio
	faac_config_ptr->aacObjectType = LOW;  // for apple, these things can only play low profile
	faac_config_ptr->inputFormat = FAAC_INPUT_16BIT;  // matches the raw output of the audio decoder (pcm_s16le)
	faacEncSetConfiguration(faac_encoder, faac_config_ptr);
	
	
	// Open the video stream dump file and the MP4 file
	FILE *video_stream_dump = fopen("video.h264", "wb");
	
	MP4FileHandle container = MP4Create("video.mp4", 0);
	if (container == MP4_INVALID_FILE_HANDLE){
		fprintf(stderr, "MP4Create failed\n");
		return 5;
	}
	//MP4SetTimeScale(container, time_base.num * time_base.den);
	MP4SetAudioProfileLevel(container, 0x0f);
	//MP4SetMetadataTool(container, "HdM encoder");
	
	// Define the video track. Changed later on when the first SPS (sequence parameter set) NAL is received
	MP4TrackId video_track = MP4_INVALID_TRACK_ID;
	
	// Build the audio track
	MP4TrackId audio_track = MP4AddAudioTrack(container, audio_codec_context_ptr->sample_rate, MP4_INVALID_DURATION, MP4_MPEG4_AUDIO_TYPE);
	if (audio_track == MP4_INVALID_TRACK_ID){
		fprintf(stderr, "MP4AddAudioTrack failed\n");
		return 5;
	}
	
	/* Leads to files that can not be played with Totem (gstreamer)
	uint8_t *aac_config_ptr = NULL;
	unsigned long aac_config_length = 0;
	faacEncGetDecoderSpecificInfo(faac_encoder, &aac_config_ptr, &aac_config_length);
	MP4SetTrackESConfiguration(container, audio_track, aac_config_ptr, aac_config_length);
	free(aac_config_ptr);
	*/
	
	// Read the packages of the container
	AVFrame *frame_ptr = avcodec_alloc_frame();
	int frame_decoded;
	AVPacket packet;
	
	int nal_count;
	x264_nal_t *nals;
	x264_picture_t pic_in, pic_out;
	int frame_size, i, error;
	uint64_t processed_frames = 0;
	if ( x264_picture_alloc(&pic_in, X264_CSP_I420, width, height) != 0 )
		fprintf(stderr, "x264: could not allocate picture\n");
	
	struct SwsContext* scaler = sws_getContext(width, height, video_codec_context_ptr->pix_fmt,
		width, height, PIX_FMT_YUV420P,
		SWS_FAST_BILINEAR, NULL, NULL, NULL);
	
	while( av_read_frame(format_context_ptr, &packet) >= 0 ){
		if (packet.stream_index == opts.video_stream_index){
			printf("VIDEO packet pts: %5ld, dts: %5ld", packet.pts, packet.dts);
			
			int bytes_decompressed = avcodec_decode_video2(video_codec_context_ptr, frame_ptr, &frame_decoded, &packet);
			if (bytes_decompressed < 0)
				av_perror("avcodec_decode_video2", bytes_decompressed);
			
			if (frame_decoded){
				//printf("frame: pts: %ld, coded_picture_number: %d, display_picture_number: %d, quality: %d, age: %d\n",
				//	frame_ptr->pts, frame_ptr->coded_picture_number, frame_ptr->display_picture_number, frame_ptr->quality, frame_ptr->age);
				
				// Put the frame into the filter pipeline
				error = av_vsrc_buffer_add_frame(src_filter_context_ptr, frame_ptr, AV_VSRC_BUF_FLAG_OVERWRITE);
				if (error < 0)
					av_perror("av_vsrc_buffer_add_frame", error);
			}
			
			// Loop over all frames that the filter pipeline finished
			while((error = avfilter_poll_frame(sink_filter_context_ptr->inputs[0])) > 0){
				AVFilterBufferRef *buffer_ref_ptr = NULL;
				
				// Get the frame out of the pipeline
				error = av_vsink_buffer_get_video_buffer_ref(sink_filter_context_ptr, &buffer_ref_ptr, 0);
				if (error < 0)
					av_perror("av_vsink_buffer_get_video_buffer_ref", error);
				error = avfilter_fill_frame_from_video_buffer_ref(frame_ptr, buffer_ref_ptr);
				if (error < 0)
					av_perror("avfilter_fill_frame_from_video_buffer_ref", error);
				
				// Give it to x264 to encode
				pic_in.i_type = X264_TYPE_AUTO;
				//pic_in.i_type = X264_TYPE_I;
				pic_in.i_pts = packet.pts;
				sws_scale(scaler, (const uint8_t * const*)frame_ptr->data, frame_ptr->linesize,
					0, height, pic_in.img.plane, pic_in.img.i_stride);
				
				frame_size = x264_encoder_encode(encoder_ptr, &nals, &nal_count, &pic_in, &pic_out);
				if ( frame_size < 0 )
					fprintf(stderr, "x264: encoder error");
				
				if (frame_size > 0)
					store_nal(frame_size, nals, nal_count, video_stream_dump, container, &video_track, &pic_out);
				
				// Free the buffer reference we got from the filter pipeline
				avfilter_unref_buffer(buffer_ref_ptr);
				
				processed_frames++;
			}
			
			// TODO: what if the decoder finished emmiting frames but frames are still left in the filter pipeline?
			// Take care of delayed frames in the filter pipeline.
			
			if (error < 0)
				av_perror("avfilter_poll_frame", error);
			
			printf("\n");
			
			if (processed_frames > opts.frame_limit)
				break;
		}
		
		if (packet.stream_index == opts.audio_stream_index){
			int sample_buffer_free = sample_buffer_size - sample_buffer_used;
			int bytes_consumed = avcodec_decode_audio3(audio_codec_context_ptr, sample_buffer_ptr + (sample_buffer_used / sample_size), &sample_buffer_free, &packet);
			printf("AUDIO packet: size: %d, bytes consumed: %d, bytes uncompessed: %d\n", packet.size, bytes_consumed, sample_buffer_free);
			
			if (bytes_consumed > 0){
				// sample_buffer_free now contains the number of bytes written into it by avcodec_decode_audio3()
				sample_buffer_used += sample_buffer_free;
				
				//printf("  sample_buffer_used: %d, faac_input_sample_count: %ld, ",
				//	sample_buffer_used, faac_input_sample_count);
				
				//printf("faac batches:");
				int samples_to_encode = sample_buffer_used / sample_size;
				int buffer_encoded = 0;
				while (samples_to_encode >= faac_input_sample_count){
					//printf(" %ld", faac_input_sample_count);
					int encoded_bytes = faacEncEncode(faac_encoder,
						(int32_t*)(sample_buffer_ptr + buffer_encoded / sample_size),
						faac_input_sample_count,
						aac_buffer_ptr, aac_buffer_size);
					
					samples_to_encode -= faac_input_sample_count;
					buffer_encoded += faac_input_sample_count * sample_size;
					
					if (encoded_bytes < 0)
						fprintf(stderr, "faac: faacEncEncode() failed\n");
					
					if (encoded_bytes > 0){
						// Ugly hack to test something...
						static bool first_time = true;
						MP4Duration offset = 0;
						if (first_time){
							offset = aac_frame_size;
							first_time = false;
						}
						
						bool result = MP4WriteSample(container, audio_track, aac_buffer_ptr, encoded_bytes, aac_frame_size, offset, true);
						if (result != true)
							fprintf(stderr, "faac: MP4WriteSample() failed\n");
					}
				}
				//printf("\n");
				
				// If not all data of the buffer was encoded move the remaining data to the front again
				if (buffer_encoded > 0 && buffer_encoded < sample_buffer_used){
					//printf("  moving %d bytes from %d to the front\n", sample_buffer_used - buffer_encoded, buffer_encoded);
					memmove(sample_buffer_ptr, sample_buffer_ptr + buffer_encoded / sample_size, sample_buffer_used - buffer_encoded);
				}
				
				sample_buffer_used -= buffer_encoded;
			}
			
			if (bytes_consumed < 0){
				fprintf(stderr, "faac: encoder error");
			}
		}
	}
	
	// Process any buffered frames that are still in the encoder
	while( x264_encoder_delayed_frames(encoder_ptr) > 0 ){
		printf("delayed x264 frame");
		frame_size = x264_encoder_encode(encoder_ptr, &nals, &nal_count, NULL, &pic_out);
		if ( frame_size < 0 )
			fprintf(stderr, "x264: encoder error");
		
		if (frame_size > 0)
			store_nal(frame_size, nals, nal_count, video_stream_dump, container, &video_track, &pic_out);
		
		printf("\n");
	}
	
	// Flush the NAL store function, it buffers one sample
	store_nal(0, NULL, 0, video_stream_dump, container, &video_track, &pic_out);
	
	// Process any remaining unencoded samples in the sample buffer
	if (sample_buffer_used > 0){
		printf("delayed unencoded sample buffer: %d bytes\n", sample_buffer_used);
		int encoded_bytes = faacEncEncode(faac_encoder,
			(int32_t*)sample_buffer_ptr,
			faac_input_sample_count,
			aac_buffer_ptr, aac_buffer_size);
		
		if (encoded_bytes < 0)
			fprintf(stderr, "faac: faacEncEncode() failed\n");
		
		if (encoded_bytes > 0){
			bool result = MP4WriteSample(container, audio_track, aac_buffer_ptr, encoded_bytes, aac_frame_size, 0, true);
			if (result != true)
				fprintf(stderr, "faac: MP4WriteSample() failed\n");
		}
	}
	
	// Process any buffered AAC frames still in the encoder
	int encoded_bytes = 0;
	while ( (encoded_bytes = faacEncEncode(faac_encoder, NULL, 0, aac_buffer_ptr, aac_buffer_size)) > 0 ){
		printf("delayed AAC frame\n");
		bool result = MP4WriteSample(container, audio_track, aac_buffer_ptr, encoded_bytes, aac_frame_size, 0, true);
		if (result != true)
			fprintf(stderr, "faac: MP4WriteSample() failed\n");
	}
	
	// Clean up
	fclose(video_stream_dump);
	MP4Close(container, 0);
	//MP4MakeIsmaCompliant("video.mp4", mp4_verbosity, true);
	
	av_free(aac_buffer_ptr);
	faacEncClose(faac_encoder);
	
	x264_picture_clean(&pic_in);
	x264_encoder_close(encoder_ptr);
	
	cleanup_filter_graph();
	avcodec_close(video_codec_context_ptr);
	av_close_input_file(format_context_ptr);
	
	avfilter_uninit();
	
	return 0;
}