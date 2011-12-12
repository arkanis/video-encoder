/**
 * Please be aware: The following code is UGLY. It was hacked together in a
 * few days to learn libavformat, libavcodec, libavfilter, x264, faac, libmp4v2
 * and the MPEG4 spec.
 */

// This is for time.h to include struct timespec (since it's from POSIX)
#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>

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

/*
on tty: progress info (time and percent)
as batch job: start, important events, end (everything with timestamp)

good DV filter pipeline: hqdn3d,yadif

usage output for x264 parameters:
	// x264_preset_names[]
	// x264_tune_names[]
	// x264_profile_names[]

*/

//
// Comand line option stuff
//

/**
 * Structure that contains the parsed command line options.
 */
typedef struct {
	// Flag to disable progress output. Per default this is only on for TTYs, we don't want
	// progress lines flooding log files.
	bool silent;
	// Flag to enable debug output.
	bool debug;
	
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
	
	// Name of the output file that will be written
	char *output_file;
	
	// x264 configuration options
	char *preset;
	char *tune;
	float quality; 
	char *profile;
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
		.silent = ! isatty(STDIN_FILENO),
		.debug = false,
		.input_file = NULL,
		.video_stream_index = -1,
		.audio_stream_index = -1,
		.frame_limit = -1,
		.video_filter = NULL,
		
		.output_file = NULL,
		
		.preset = "medium",
		.tune = "film",
		.quality = 20.0,
		.profile = NULL
	};
	*options_ptr = defaults;
	
	// Now parse the command line arguments
	struct option long_opts[] = {
		{"silent", no_argument, NULL, 's'},
		{"debug", no_argument, NULL, 'd'},
		{"video-stream", required_argument, NULL, 'v'},
		{"audio-stream", required_argument, NULL, 'a'},
		{"frame-limit", required_argument, NULL, 'l'},
		{"filters", required_argument, NULL, 'f'},
		
		{"preset", required_argument, NULL, 1},
		{"tune", required_argument, NULL, 2},
		{"quality", required_argument, NULL, 3},
		{"profile", required_argument, NULL, 4},
		
		{NULL, 0, NULL, 0}
	};
	
	int long_opt_index = 0, opt_abbr = 0;
	while(true){
		opt_abbr = getopt_long(argc, argv, "sdv:a:l:f:", long_opts, &long_opt_index);
		if (opt_abbr == -1)
			break;
		
		switch(opt_abbr){
			case 's':
				options_ptr->silent = true;
				break;
			case 'd':
				options_ptr->debug = true;
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
			
			case 1:
				options_ptr->preset = optarg;
				break;
			case 2:
				options_ptr->tune = optarg;
				break;
			case 3:
				options_ptr->quality = strtof(optarg, NULL);
				break;
			case 4:
				options_ptr->profile = optarg;
				break;
			
			default:
				// Error message is already printed by `getopt_long()`
				//TODO: show cli help?
				return false;
		}
	}
	
	if (optind < argc) {
		options_ptr->input_file = argv[optind];
		optind++;
	} else {
		fprintf(stderr, "no input file specified!\n");
		return false;
	}
	
	if (optind < argc) {
		options_ptr->output_file = argv[optind];
		optind++;
	} else {
		fprintf(stderr, "no output file specified!\n");
		return false;
	}
	
	printf("silent: %d \ndebug: %d \ninput_file: %s \noutput_file: %s \nvideo_stream_index: %d \naudio_stream_index: %d \nframe_limit: %ld \nvideo_filter: %s \npreset: %s \ntune: %s \nquality: %f \nprofile: %s\n",
		options_ptr->silent, options_ptr->debug, options_ptr->input_file, options_ptr->output_file,
		options_ptr->video_stream_index, options_ptr->audio_stream_index,
		options_ptr->frame_limit, options_ptr->video_filter,
		options_ptr->preset, options_ptr->tune, options_ptr->quality, options_ptr->profile
	);
	
	return true;
}


//
// General output stuff
//

bool debug_show = false;

void debug(const char *format, ...){
	if (!debug_show)
		return;
	
	va_list args;
	
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

/**
 * Just a small helper function that will return `-1` for an unkown PTS value.
 * This is better readable than the minimum int64_t value. Could also be done
 * with a macro, but well... not really worth it.
 */
int64_t format_pts(int64_t pts){
	return pts == AV_NOPTS_VALUE ? -1 : pts;
}


typedef struct {
	uint16_t hours;
	uint8_t minutes, seconds;
	double entire_seconds;
} display_time_t;

display_time_t display_time_from_secs(double entire_seconds){
	int64_t seconds = entire_seconds;
	
	uint16_t hours = seconds / (60LL * 60LL);
	seconds -= hours * (60LL * 60LL);
	uint8_t minutes = seconds / 60LL;
	seconds -= minutes * 60LL;
	
	return (display_time_t){ .hours = hours, .minutes = minutes, .seconds = seconds, .entire_seconds = entire_seconds };
}

display_time_t display_time(int64_t time_stamp, AVRational time_base){
	return display_time_from_secs( time_stamp * av_q2d(time_base) );
}


//
// Common libav stuff
//

/**
 * Small helper function that prints an error message followed by a description of
 * the specified AV error code. Inspired by the POSIX perror() function.
 */
void enc_av_perror(char *prefix, int error){
	char message[255];
	
	if (av_strerror(error, message, 255) == 0)
		fprintf(stderr, "%s: av error: %s\n", prefix, message);
	else
		fprintf(stderr, "%s: unknown av error, code: %d\n", prefix, error);
}


//
//  libavformat stuff
//

/**
 * Opens the specified file and returns a format context pointer in `format_context_dptr`.
 * The file is scanned for additional information. This is necessary since DV files do not
 * contain much of a header. We need to scan DV files in order to get an proper audio and
 * video stream.
 */
bool enc_avformat_open_file(const char *filename, AVFormatContext **format_context_dptr){
	int error = 0;
	
	error = avformat_open_input(format_context_dptr, filename, NULL, NULL);
	if (error != 0){
		enc_av_perror("avformat_open_input", error);
		return false;
	}
	
	error = av_find_stream_info(*format_context_dptr);
	if (error < 0){
		enc_av_perror("av_find_stream_info", error);
		return false;
	}
	
	return true;
}

/**
 * If a stream index is `-1` this function selects the video and/or audio stream with the highest bitrate.
 * 
 * The purpose of this function is to autodetect the best streams in the old WMV archive files. They often
 * contain many streams and manually searching for the indecies an be a tedious task.
 */
bool enc_avformat_select_streams(const AVFormatContext *format_context_ptr, int *video_stream_index, int *audio_stream_index){
	if (*video_stream_index == -1){
		int selected_bitrate = -1;
		for(int i = 0; i < format_context_ptr->nb_streams; i++){
			AVCodecContext *codec_context_ptr = format_context_ptr->streams[i]->codec;
			if ( codec_context_ptr->codec_type == AVMEDIA_TYPE_VIDEO && codec_context_ptr->bit_rate > selected_bitrate ){
				*video_stream_index = i;
				selected_bitrate = codec_context_ptr->bit_rate;
			}
		}
	}
	
	if (*audio_stream_index == -1){
		int selected_bitrate = -1;
		for(int i = 0; i < format_context_ptr->nb_streams; i++){
			AVCodecContext *codec_context_ptr = format_context_ptr->streams[i]->codec;
			if ( codec_context_ptr->codec_type == AVMEDIA_TYPE_AUDIO && codec_context_ptr->bit_rate > selected_bitrate ){
				*audio_stream_index = i;
				selected_bitrate = codec_context_ptr->bit_rate;
			}
		}
	}
	
	if (*video_stream_index < 0 || *video_stream_index >= format_context_ptr->nb_streams || *audio_stream_index < 0 || *audio_stream_index >= format_context_ptr->nb_streams){
		fprintf(stderr, "Could not find a proper video or audio stream, sorry.\nVideo stream index: %d, audio steam index: %d\n",
			*video_stream_index, *audio_stream_index);
		return false;
	}
	
	return true;
}


//
// libavcodec stuff
//

/**
 * Searches and opens a codec for the specified stream.
 */
bool enc_avcodec_open(AVFormatContext *format_context_ptr, int stream_index, enum AVMediaType required_stream_type, AVCodecContext **codec_context_dptr, AVCodec **codec_dptr){
	enum AVMediaType stream_type = format_context_ptr->streams[stream_index]->codec->codec_type;
	if (required_stream_type != stream_type){
		char *type_names[] = {"unknown", "video", "audio", "data", "subtitle", "attachment", "nb"};
		fprintf(stderr, "Tried to use steam %d as %s steam but it is an %s steam!\n",
			stream_index, type_names[required_stream_type+1], type_names[stream_type+1]);
		return false;
	}
	
	*codec_context_dptr = format_context_ptr->streams[stream_index]->codec;
	*codec_dptr = avcodec_find_decoder((*codec_context_dptr)->codec_id);
	if (*codec_dptr == NULL){
		fprintf(stderr, "Found no matching codec for stream %d!\n", stream_index);
		return false;
	}
	
	if ( avcodec_open(*codec_context_dptr, *codec_dptr) != 0 ){
		fprintf(stderr, "Initialization of codec %s failed!\n", (*codec_dptr)->name);
		return false;
	}
	
	return true;
}


//
// x264 stuff
//

typedef struct {
	x264_t *encoder;
	x264_picture_t pic_in, pic_out;
	struct SwsContext* scaler;
	x264_nal_t* nals;
	int nal_count;
	int payload_size;
} x264_context_t;

bool enc_x264_open(
	AVCodecContext *video_codec_context_ptr, AVRational sample_aspect_ratio,
	const char *preset, const char *tune, int quality, const char *profile, x264_context_t *x264_ptr
){
	x264_param_t params;
	// use tune "zerolatency" tune to avoid out of order frames
	if ( x264_param_default_preset(&params, preset, tune) != 0 ){
		fprintf(stderr, "x264: failed to set preset %s and tune %s\n", preset, tune);
		return false;
	}
	
	params.i_width = video_codec_context_ptr->width;
	params.i_height = video_codec_context_ptr->height;
	// We're muxing the h264 stream into an MP4 container, so we don't want an AnnexB stream
	params.b_annexb = false;
	// fps is the reciprocal of the time base
	params.i_fps_num = video_codec_context_ptr->time_base.den;
	params.i_fps_den = video_codec_context_ptr->time_base.num;
	// Set the sample aspect ratio for the video stream since this information is also present in the h264 stream
	params.vui.i_sar_width = sample_aspect_ratio.num;
	params.vui.i_sar_height = sample_aspect_ratio.den;
	
	params.rc.i_rc_method = X264_RC_CRF;
	params.rc.f_rf_constant = quality;
	
	if ( x264_param_apply_profile(&params, profile) != 0 ){
		fprintf(stderr, "x264: failed to apply profile %s\n", profile);
		return false;
	}
	
	x264_ptr->encoder = x264_encoder_open(&params);
	if (x264_ptr->encoder == NULL){
		fprintf(stderr, "x264: failed to initialize encoder\n");
		return false;
	}
	
	// Allocate the x264 input buffer (input "picture")
	if ( x264_picture_alloc(&x264_ptr->pic_in, X264_CSP_I420, video_codec_context_ptr->width, video_codec_context_ptr->height) != 0 ){
		fprintf(stderr, "x264: could not allocate input picture\n");
		return false;
	}
	
	// Initialize the output picture pts to 0 so we can use it to calculate the progress.
	// Otherwise the random value will screw up our status message.
	x264_ptr->pic_out.i_pts = 0;
	
	// The software scaler to copy the raw decoder output into the x264 input picture.
	// Those buffers use different paddings so we pretty much only use the software scaler
	// perform an efficient copy.
	x264_ptr->scaler = sws_getContext(
		video_codec_context_ptr->width, video_codec_context_ptr->height, video_codec_context_ptr->pix_fmt,
		video_codec_context_ptr->width, video_codec_context_ptr->height, PIX_FMT_YUV420P,
		SWS_FAST_BILINEAR, NULL, NULL, NULL);
	
	if (x264_ptr->scaler == NULL){
		fprintf(stderr, "failed to create software scaler to copy frames to x264\n");
		return false;
	}
	
	return true;
}

bool enc_x264_close(x264_context_t *x264){
	sws_freeContext(x264->scaler);
	x264_picture_clean(&x264->pic_in);
	x264_encoder_close(x264->encoder);
}


//
// libavfilter stuff
//

bool enc_avfilter_build_graph(
	AVCodecContext *video_codec_context_ptr, AVRational sample_aspect_ratio, const char *filters,
	AVFilterGraph **filter_graph_dptr, AVFilterContext **src_filter_context_dptr, AVFilterContext **sink_filter_context_dptr
){
	char filter_args[255];
	int error = 0;
	
	*filter_graph_dptr = avfilter_graph_alloc();
	
	// Build the gateway (source) into the filter pipeline
	snprintf(filter_args, sizeof(filter_args), "%d:%d:%d:%d:%d:%d:%d",
		video_codec_context_ptr->width, video_codec_context_ptr->height, video_codec_context_ptr->pix_fmt,
		video_codec_context_ptr->time_base.num, video_codec_context_ptr->time_base.den,
		sample_aspect_ratio.num, sample_aspect_ratio.den);
	
	*src_filter_context_dptr = NULL;
	error = avfilter_graph_create_filter(src_filter_context_dptr, avfilter_get_by_name("buffer"), "src", filter_args, NULL, *filter_graph_dptr);
	if (error < 0){
		enc_av_perror("avfilter_graph_create_filter", error);
		return false;
	}
	
	// Build the output sink of the filter pipeline
	enum PixelFormat pix_fmts[] = { video_codec_context_ptr->pix_fmt, PIX_FMT_NONE };
	*sink_filter_context_dptr = NULL;
	error = avfilter_graph_create_filter(sink_filter_context_dptr, avfilter_get_by_name("buffersink"), "sink", "",  pix_fmts, *filter_graph_dptr);
	if (error < 0){
		enc_av_perror("avfilter_graph_create_filter", error);
		return false;
	}
	
	// A NULL string will crash avfilter_graph_parse(). If we got no filter string to wire up
	// just connect the source with the sink.
	if (filters != NULL && strlen(filters) > 0) {
		// Build the use defined filter graph between the two ends
		AVFilterInOut *outputs = avfilter_inout_alloc();
		AVFilterInOut *inputs = avfilter_inout_alloc();
		
		outputs->name = av_strdup("in");
		outputs->filter_ctx = *src_filter_context_dptr;
		outputs->pad_idx = 0;
		outputs->next = NULL;
		
		inputs->name = av_strdup("out");
		inputs->filter_ctx = *sink_filter_context_dptr;
		inputs->pad_idx = 0;
		inputs->next = NULL;
		
		error = avfilter_graph_parse(*filter_graph_dptr, filters, &inputs, &outputs, NULL);
		if (error != 0){
			enc_av_perror("avfilter_graph_parse", error);
			return false;
		}
	} else {
		error = avfilter_link(*src_filter_context_dptr, 0, *sink_filter_context_dptr, 0);
		if (error != 0){
			enc_av_perror("avfilter_link", error);
			return false;
		}
	}
	
	error = avfilter_graph_config(*filter_graph_dptr, NULL);
	if (error < 0){
		enc_av_perror("avfilter_graph_config", error);
		return false;
	}
	
	return true;
}

/**
 * Tries to pull one frame out of the filter pipeline and copy it into the input picture of the x264 context
 * input pixture. Returns `true` if a frame was copied to the x264 context, `false` if the pipeline is empty.
 */
bool enc_avfilter_pull_to_x264_context(AVFilterContext *sink_ptr, AVFrame *frame_ptr, x264_context_t *x264_ptr){
	int error;
	AVFilterBufferRef *buffer_ref_ptr = NULL;
	
	error = avfilter_poll_frame(sink_ptr->inputs[0]);
	if (error > 0) {
		// A frame is ready, get it out of the pipeline
		error = av_vsink_buffer_get_video_buffer_ref(sink_ptr, &buffer_ref_ptr, 0);
		if (error < 0)
			enc_av_perror("av_vsink_buffer_get_video_buffer_ref", error);
		error = avfilter_fill_frame_from_video_buffer_ref(frame_ptr, buffer_ref_ptr);
		if (error < 0)
			enc_av_perror("avfilter_fill_frame_from_video_buffer_ref", error);
		
		debug("  filtered frame: pts: %ld, packet pts: %ld, packet dts: %ld\n", format_pts(frame_ptr->pts),
			format_pts(frame_ptr->pkt_pts), frame_ptr->pkt_dts);
		
		// Copy it into the x264 context input picture
		x264_ptr->pic_in.i_type = X264_TYPE_AUTO;
		x264_ptr->pic_in.i_pts = frame_ptr->pts;
		sws_scale(x264_ptr->scaler, (const uint8_t * const*)frame_ptr->data,
			frame_ptr->linesize, 0, frame_ptr->height,
			x264_ptr->pic_in.img.plane, x264_ptr->pic_in.img.i_stride);
		
		// Free the buffer reference we got from the filter pipeline
		avfilter_unref_buffer(buffer_ref_ptr);
	} else if (error == 0) {
		// Pipeline empty
		return false;
	} else {
		// Negative values are error codes
		enc_av_perror("avfilter_poll_frame", error);
	}
	
	return true;
}


//
// FAAC stuff
//

typedef struct {
	faacEncHandle encoder;
	unsigned long input_sample_count;
	// Number of samples on AAC frame is long (only one channel). This is useful for MP4 muxing.
	uint32_t frame_length;
	// Buffer for the FAAC output (the AAC bitstream)
	int buffer_size;
	uint8_t *buffer_ptr;
} faac_context_t;

bool enc_faac_open(AVCodecContext *audio_codec_context_ptr, faac_context_t *faac){
	unsigned long max_output_byte_count;
	
	faac->encoder = faacEncOpen(audio_codec_context_ptr->sample_rate, audio_codec_context_ptr->channels,
		&faac->input_sample_count, &max_output_byte_count);
	
	if (faac->encoder == NULL){
		fprintf(stderr, "faac: failed to initialize encoder\n");
		return false;
	}
	
	faac->frame_length = faac->input_sample_count / audio_codec_context_ptr->channels;
	faac->buffer_size = max_output_byte_count;
	faac->buffer_ptr = (uint8_t*) av_mallocz(max_output_byte_count);
	
	if (faac->buffer_ptr == NULL){
		fprintf(stderr, "faac: failed to allocate AAC buffer\n");
		return false;
	}
	
	// Detail config of the encoder
	faacEncConfigurationPtr faac_config_ptr = faacEncGetCurrentConfiguration(faac->encoder);
	faac_config_ptr->mpegVersion = MPEG4;  // for Windows Media Player. It only accpets mpeg4 audio
	faac_config_ptr->aacObjectType = LOW;  // for apple, these things can only play low profile
	faac_config_ptr->inputFormat = FAAC_INPUT_16BIT;  // matches the raw output of the audio decoder (pcm_s16le)
	faacEncSetConfiguration(faac->encoder, faac_config_ptr);
	
	return true;
}


//
// MP4 stuff
//

bool enc_mp4_open(
	const char *filename, AVCodecContext *video_codec_context_ptr, AVRational sample_aspect_ratio, AVCodecContext  *audio_codec_context_ptr,
	MP4FileHandle *container_ptr, MP4TrackId *video_track_ptr, MP4TrackId *audio_track_ptr
){
	*container_ptr = MP4Create(filename, 0);
	if (*container_ptr == MP4_INVALID_FILE_HANDLE){
		fprintf(stderr, "mp4v2: failed to create mp4 file %s\n", filename);
		return false;
	}
	
	// TODO: Not sure if this has any advantage for file that contain a audio _and_ video stream. A look into the spec might clear things up.
	//MP4SetTimeScale(*container_ptr, video_codec_context_ptr->time_base.num * video_codec_context_ptr->time_base.den);
	// TODO: The man page of MP4SetAudioProfileLevel() does not list 0x0f. Look into the spec profile and level this is (maybe low profile?)
	MP4SetAudioProfileLevel(*container_ptr, 0x0f);
	// TODO: Depricated, look how to do it properly if it's really necessary
	//MP4SetMetadataTool(*container_ptr, "HdM encoder");

	// Add the video track to the container. Use the product of the timebase numerator and denumerator as time scale
	// (the number of ticks per second). Then we only have to multiply each PTS with the numerator. The sample duration
	// is set for each sample since the duration of frames generated by x264 can vary.
	// The profile_idc, profile_compat and level_idc are set to 0 for now but are updated with proper values as soon as
	// the first SPS (sequence parameter set) NAL is received from x264. x264 puts the payload length into the first 4 byte
	// before each NAL. This is perfect for MP4 (to be more exact AVC1 encapsulation in an MP4 container). Therefore we
	// set the sampleLenFieldSizeMinusOne parameter to 3.
	*video_track_ptr = MP4AddH264VideoTrack(*container_ptr, video_codec_context_ptr->time_base.num * video_codec_context_ptr->time_base.den,
		MP4_INVALID_DURATION, video_codec_context_ptr->width, video_codec_context_ptr->height,
		0, 0, 0, 3);
	if (*video_track_ptr == MP4_INVALID_TRACK_ID){
		fprintf(stderr, "mp4v2: failed to add video track to container\n");
		return false;
	}

	MP4AddPixelAspectRatio(*container_ptr, *video_track_ptr, sample_aspect_ratio.num, sample_aspect_ratio.den);

	// Add the audio track to the container
	*audio_track_ptr = MP4AddAudioTrack(*container_ptr, audio_codec_context_ptr->sample_rate, MP4_INVALID_DURATION, MP4_MPEG4_AUDIO_TYPE);
	if (*audio_track_ptr == MP4_INVALID_TRACK_ID){
		fprintf(stderr, "mp4v2: failed to add audio track to container\n");
		return false;
	}

	/* TODO: Leads to files that can not be played with Totem (gstreamer). Figure out why and what this should do in the first place.
	uint8_t *aac_config_ptr = NULL;
	unsigned long aac_config_length = 0;
	faacEncGetDecoderSpecificInfo(faac_encoder, &aac_config_ptr, &aac_config_length);
	MP4SetTrackESConfiguration(container, audio_track, aac_config_ptr, aac_config_length);
	free(aac_config_ptr);
	*/
	
	return true;
}

/**
 * Writes a video sample to the mp4 video track. If the track is not configured some codec details of the video track are updated
 * based on the first SPS NAL received.
 */
void enc_mp4_write_video_sample(
	MP4FileHandle container, MP4TrackId video_track,
	x264_nal_t *nals, int nal_count, size_t payload_size,
	bool is_sync_sample, int64_t decode_delta, int64_t composition_offset
){
	static bool video_track_configured = false;
	x264_nal_t* nal_ptr = NULL;
	
	debug("    writing NALs:");
	for(int i = 0; i < nal_count; i++){
		nal_ptr = &nals[i];
		debug(" %d", nal_ptr->i_type);
		switch(nal_ptr->i_type){
			case NAL_SPS:
				// If the codec details of the video track are not yet set to valid values do so based on the first
				// sequence parameter set.
				if (!video_track_configured){
					uint8_t profile_idc, profile_compat, level_idc;
					
					// Extract some information from the sequence parameter set and use them
					// to configure the video track of the mp4 container.
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
					debug(" (configuring video track: profile_idc %d, profile_compat %x, level_idc: %d)", profile_idc, profile_compat, level_idc);
					
					// Update the codec details of the video track. Taken from MP4File::AddH264VideoTrack(),
					// mp4file.cpp line 1858 of libmp4v2.
					MP4SetTrackIntegerProperty(container, video_track,
						"mdia.minf.stbl.stsd.avc1.avcC.AVCProfileIndication", profile_idc);
					MP4SetTrackIntegerProperty(container, video_track,
						"mdia.minf.stbl.stsd.avc1.avcC.profile_compatibility", profile_compat);
					MP4SetTrackIntegerProperty(container, video_track,
						"mdia.minf.stbl.stsd.avc1.avcC.AVCLevelIndication", level_idc);
					
					video_track_configured = true;
				}
				
				// Put the sequence parameter set into the MP4 container. Framing is provided
				// by the container, therefore we don't need the leading 4 bytes (the payload size).
				MP4AddH264SequenceParameterSet(container, video_track, nal_ptr->p_payload + 4, nal_ptr->i_payload - 4);
				break;
			case NAL_PPS:
				// Put the picture parameter set into the MP4 container. Framing is provided
				// by the container, therefore we don't need the leading 4 bytes (the payload size).
				MP4AddH264PictureParameterSet(container, video_track, nal_ptr->p_payload + 4, nal_ptr->i_payload - 4);
				break;
			case NAL_FILLER:
				// Throw filler data away (AVC spec wants it)
				break;
			default:
				// Every thing else is data for the video track. Collect all remaining NALs and
				// put them into one MP4 sample.
				{
					int remaining_nals = nal_count - i;
					uint8_t *start_ptr = nals[i].p_payload;
					int size = payload_size - ((void*)start_ptr - (void*)(nals[0].p_payload));
					
					debug(" storing %d NALs, %d bytes", remaining_nals, size);
					if ( MP4WriteSample(container, video_track, start_ptr, size, decode_delta, composition_offset, is_sync_sample) != true)
						fprintf(stderr, "enc_mp4_write_video_sample: MP4WriteSample (NAL %d) failed\n", i);
					
					i += remaining_nals;
				}
				break;
		}
	}

	debug("\n");
}


typedef struct {
	uint8_t *payload_data;
	size_t payload_size;
	x264_nal_t *nal_data;
	size_t nal_count;
	x264_picture_t pic;
} x264_frame_t;

bool enc_mp4_mux_video(MP4FileHandle container, MP4TrackId video_track, x264_context_t *x264_ptr){
	static x264_frame_t prev_frame = {
		.payload_data = NULL, .payload_size = 0,
		.nal_data = NULL, .nal_count = 0
	};
	
	if (x264_ptr->payload_size > 0) {
		// We got a fresh frame from the encoder
		
		// If we already have a previous frame buffered we can calculate the decoding delta and composition offset. Otherwise
		// just buffer the current frame (it's the first one then).
		if (prev_frame.payload_size > 0) {
			int64_t decode_delta, composition_offset;
			decode_delta = x264_ptr->pic_out.i_dts - prev_frame.pic.i_dts;
			composition_offset = prev_frame.pic.i_pts - prev_frame.pic.i_dts;
			
			debug("  writing mp4 sample: dec delta: %ld, comp offset: %ld, prev: (dts: %ld, pts: %ld), curr: (dts: %ld, pts: %ld)\n",
				decode_delta, composition_offset, prev_frame.pic.i_dts, prev_frame.pic.i_pts,
				x264_ptr->pic_out.i_dts, x264_ptr->pic_out.i_pts);
			
			enc_mp4_write_video_sample(container, video_track, prev_frame.nal_data, prev_frame.nal_count,
				prev_frame.payload_size, prev_frame.pic.b_keyframe, decode_delta, composition_offset);
		}
		
		// Buffer the current frame for the next time
		debug("  buffering x264 frame\n");
		
		if (prev_frame.payload_data != NULL)
			free(prev_frame.payload_data);
		prev_frame.payload_data = (uint8_t*) malloc(x264_ptr->payload_size);
		
		if (prev_frame.nal_data != NULL)
			free(prev_frame.nal_data);
		prev_frame.nal_data = (x264_nal_t*) malloc(x264_ptr->nal_count * sizeof(x264_nal_t));
		
		if (prev_frame.payload_data == NULL || prev_frame.nal_data == NULL){
			fprintf(stderr, "enc_mp4_mux_video: failed to allocate buffers for x264 frame\n");
			return false;
		}
		
		memcpy(prev_frame.payload_data, x264_ptr->nals[0].p_payload, x264_ptr->payload_size);
		prev_frame.payload_size = x264_ptr->payload_size;
		
		prev_frame.pic = x264_ptr->pic_out;
		
		prev_frame.nal_count = x264_ptr->nal_count;
		for(int i = 0; i < x264_ptr->nal_count; i++){
			prev_frame.nal_data[i] = x264_ptr->nals[i];
			int offset = x264_ptr->nals[i].p_payload - x264_ptr->nals[0].p_payload;
			prev_frame.nal_data[i].p_payload = prev_frame.payload_data + offset;
		}
	} else {
		// No new frame data, then this is the last call to flush the buffers. The last frame is allowed
		// to have a decode delta (duration) of 0.
		int64_t decode_delta, composition_offset;
		decode_delta = 1;
		composition_offset = prev_frame.pic.i_pts - prev_frame.pic.i_dts;
		
		debug("  flushing mp4 buffer, writing last sample: dec delta: %ld, comp offset: %ld, prev: (dts: %ld, pts: %ld)\n",
			decode_delta, composition_offset, prev_frame.pic.i_dts, prev_frame.pic.i_pts);
		
		enc_mp4_write_video_sample(container, video_track, prev_frame.nal_data, prev_frame.nal_count,
			prev_frame.payload_size, prev_frame.pic.b_keyframe, decode_delta, composition_offset);
		
		// Clean up the buffered output data
		free(prev_frame.payload_data);
		prev_frame.payload_data = NULL;
		free(prev_frame.nal_data);
		prev_frame.nal_data = NULL;
	}
	
	return true;
}


//
// The main "pupetmaster" function coordinating all libraries
//
int main(int argc, char **argv){
	// Parse the CLI options
	cli_options_t opts;
	if ( ! parse_cli_options(&opts, argc, argv) )
		return 1;
	
	// Set the global debug flag to show or hide all debug output
	debug_show = opts.debug;
	
	// Init libavformat and register all codecs
	av_register_all();
	avfilter_register_all();
	
	// Open the video to get a format context
	AVFormatContext *format_context_ptr = NULL;
	if ( ! enc_avformat_open_file(opts.input_file, &format_context_ptr) )
		return 2;
	
	// Show some nice information about the container and its streams
	av_dump_format(format_context_ptr, 0, opts.input_file, 0);
	
	// Select the best video and audio stream if the user didn't select some manually
	if ( ! enc_avformat_select_streams(format_context_ptr, &opts.video_stream_index, &opts.audio_stream_index) )
		return 3;
	
	// Open decoders for the selected video and audio streams
	AVCodec *video_codec_ptr = NULL, *audio_codec_ptr = NULL;
	AVCodecContext *video_codec_context_ptr = NULL, *audio_codec_context_ptr = NULL;
	
	if ( ! enc_avcodec_open(format_context_ptr, opts.video_stream_index, AVMEDIA_TYPE_VIDEO, &video_codec_context_ptr, &video_codec_ptr) )
		return 4;
	if ( ! enc_avcodec_open(format_context_ptr, opts.audio_stream_index, AVMEDIA_TYPE_AUDIO, &audio_codec_context_ptr, &audio_codec_ptr) )
		return 5;
	
	// Use the sample aspect ratio from the video stream. If it's unknown use the ratio from the container.
	AVRational sample_aspect_ratio = video_codec_context_ptr->sample_aspect_ratio;
	if (sample_aspect_ratio.num == 0)
		sample_aspect_ratio = format_context_ptr->streams[opts.video_stream_index]->sample_aspect_ratio;
	
	// Show the streams selected for encoding
	printf("Streams selected for encoding:\n");
	printf("  video steam %d: decoder: %s, %dx%d, timebase: (%d/%d), sample aspect ratio: (%d/%d)\n",
		opts.video_stream_index, video_codec_ptr->name, video_codec_context_ptr->width, video_codec_context_ptr->height,
		video_codec_context_ptr->time_base.num, video_codec_context_ptr->time_base.den, sample_aspect_ratio.num, sample_aspect_ratio.den);
	printf("  audio steam %d: %d Hz, %d channels\n",
		opts.audio_stream_index, audio_codec_context_ptr->sample_rate, audio_codec_context_ptr->channels);
	
	// Build the filter graph
	AVFilterGraph *filter_graph_ptr = NULL;
	AVFilterContext *src_filter_context_ptr = NULL, *sink_filter_context_ptr = NULL;
	if ( ! enc_avfilter_build_graph(video_codec_context_ptr, sample_aspect_ratio, opts.video_filter, &filter_graph_ptr, &src_filter_context_ptr, &sink_filter_context_ptr) )
		return 6;
	
	// Init the x264 encoder
	x264_context_t x264;
	if ( ! enc_x264_open(video_codec_context_ptr, sample_aspect_ratio, opts.preset, opts.tune, opts.quality, opts.profile, &x264) )
		return 7;
	
	// Init the FAAC encoder
	faac_context_t faac;
	if ( ! enc_faac_open(audio_codec_context_ptr, &faac) )
		return 8;
	
	// Init the MP4 muxer
	MP4FileHandle mp4_container = NULL;
	MP4TrackId mp4_video_track = MP4_INVALID_TRACK_ID, mp4_audio_track = MP4_INVALID_TRACK_ID;
	if ( ! enc_mp4_open(opts.output_file, video_codec_context_ptr, sample_aspect_ratio, audio_codec_context_ptr, &mp4_container, &mp4_video_track, &mp4_audio_track) )
		return 9;
	
	//
	// Allocate the decode and encode buffers and stuff
	//
	AVPacket packet;
	
	// Video decoder frame
	AVFrame *decoded_frame_ptr = avcodec_alloc_frame();
	int decoded_frame_available;
	
	// Audio decoder output buffer (the raw audio samples)
	int sample_buffer_size = 2 * AVCODEC_MAX_AUDIO_FRAME_SIZE;
	int sample_buffer_used = 0, sample_size = sizeof(int16_t);
	int16_t *sample_buffer_ptr = (int16_t*) av_mallocz(sample_buffer_size);
	
	if (decoded_frame_ptr == NULL || sample_buffer_ptr == NULL){
		fprintf(stderr, "failed to allocate decoding buffers\n");
		return 10;
	}
	
	// Read all packages from the input file
	printf("Initialization completed, starting decoding and encoding...\n");
	
	int64_t encoded_video_pts = 0, encoded_audio_pts = 0;
	double duration_sec = format_context_ptr->duration / (double) AV_TIME_BASE;
	
	uint64_t start_video_pts = 0, start_audio_pts = 0;
	struct timespec start, now;
	clock_gettime(CLOCK_REALTIME, &start);
	
	//double encoded_video_sec = 0.0, encoded_audio_sec = 0.0;
	//bool status_changed = true;
	
	int error;
	while( av_read_frame(format_context_ptr, &packet) >= 0 )
	{
		if (packet.stream_index == opts.video_stream_index)
		{
			debug("video packet: pts: %ld, dts: %ld\n", format_pts(packet.pts), packet.dts);
			
			int bytes_decompressed = avcodec_decode_video2(video_codec_context_ptr, decoded_frame_ptr, &decoded_frame_available, &packet);
			if (bytes_decompressed < 0)
				enc_av_perror("avcodec_decode_video2", bytes_decompressed);
			
			if (decoded_frame_available){
				// Use the container (packet) PTS if the frame has no valid PTS on its own. This is the case for
				// DV video files. We have to use the frame PTS so the filter pipeline gets the right PTS from the
				// start. The packet PTS and DTS are still stored in the frame but it is unclear if the filter
				// pipeline uses them.
				int64_t original_pts = decoded_frame_ptr->pts;
				if (decoded_frame_ptr->pts == AV_NOPTS_VALUE || decoded_frame_ptr->pts == 0)
					decoded_frame_ptr->pts = packet.pts;
				
				debug("  decoded frame: pts: %ld, used pts: %ld\n", format_pts(original_pts), format_pts(decoded_frame_ptr->pts));
				
				// Put the frame into the filter pipeline
				error = av_vsrc_buffer_add_frame(src_filter_context_ptr, decoded_frame_ptr, AV_VSRC_BUF_FLAG_OVERWRITE);
				if (error < 0)
					enc_av_perror("av_vsrc_buffer_add_frame", error);
			}
			
			// Pull all finished frames from the filter pipeline and encode them with x264
			while( enc_avfilter_pull_to_x264_context(sink_filter_context_ptr, decoded_frame_ptr, &x264) )
			{
				x264.payload_size = x264_encoder_encode(x264.encoder, &x264.nals, &x264.nal_count, &x264.pic_in, &x264.pic_out);
				if (x264.payload_size > 0)
					enc_mp4_mux_video(mp4_container, mp4_video_track, &x264);
				else if ( x264.payload_size < 0 )
					fprintf(stderr, "x264: encoder error\n");
			}
			
			// The x264 context contains the latest output picture. In there is the PTS of the latest encoded frame.
			// Use it to update the video encoding progress.
			encoded_video_pts = x264.pic_out.i_pts;
			/*
			double current_sec = x264.pic_out.i_pts * av_q2d(video_codec_context_ptr->time_base);
			if (current_sec > encoded_video_sec){
				encoded_video_sec = current_sec;
				status_changed = true;
			}
			*/
		}
		else if (packet.stream_index == opts.audio_stream_index)
		{
			int sample_buffer_free = sample_buffer_size - sample_buffer_used;
			int bytes_consumed = avcodec_decode_audio3(audio_codec_context_ptr, sample_buffer_ptr + (sample_buffer_used / sample_size), &sample_buffer_free, &packet);
			
			debug("audio packet: pts: %5ld, dts: %5ld size: %d, bytes uncompessed: %d\n",
				packet.pts, packet.dts, packet.size, sample_buffer_free);
			
			if (bytes_consumed > 0) {
				// sample_buffer_free now contains the number of bytes written into it by avcodec_decode_audio3()
				sample_buffer_used += sample_buffer_free;
				
				int samples_to_encode = sample_buffer_used / sample_size;
				int buffer_encoded = 0;
				
				debug("  samples to encode: %d, encoding batches: ", samples_to_encode);
				while (samples_to_encode >= faac.input_sample_count){
					debug(" %ld", faac.input_sample_count);
					int encoded_bytes = faacEncEncode(faac.encoder,
						(int32_t*)(sample_buffer_ptr + buffer_encoded / sample_size), faac.input_sample_count,
						faac.buffer_ptr, faac.buffer_size);
					
					samples_to_encode -= faac.input_sample_count;
					buffer_encoded += faac.input_sample_count * sample_size;
					
					if (encoded_bytes > 0) {
						if ( ! MP4WriteSample(mp4_container, mp4_audio_track, faac.buffer_ptr, encoded_bytes, faac.frame_length, 0, true) )
							fprintf(stderr, "    faac: MP4WriteSample() failed\n    ");
						
						// Update the audio encoding progress
						encoded_audio_pts += faac.frame_length;
						/*
						encoded_audio_sec += faac.frame_length / (double)audio_codec_context_ptr->sample_rate;
						status_changed = true;
						*/
					} else if (encoded_bytes < 0) {
						fprintf(stderr, "    faac: faacEncEncode() failed\n    ");
					}
				}
				debug("\n");
				
				// If not all data of the buffer was encoded move the remaining data to the front again
				if (buffer_encoded > 0 && buffer_encoded < sample_buffer_used){
					debug("  moving %d bytes from position %d to the front\n", sample_buffer_used - buffer_encoded, buffer_encoded);
					memmove(sample_buffer_ptr, sample_buffer_ptr + buffer_encoded / sample_size, sample_buffer_used - buffer_encoded);
				}
				
				sample_buffer_used -= buffer_encoded;
			} else if (bytes_consumed < 0) {
				enc_av_perror("avcodec_decode_audio3", bytes_consumed);
			}
		}
		
		// Print new status information to the terminal (unless we are in silent mode)
		if (!opts.silent){
			clock_gettime(CLOCK_REALTIME, &now);
			double interval_duration = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1000000000.0;
			if (interval_duration > 1){
				start = now;
				// Refresh the progress every second
				display_time_t video_time, audio_time;
				video_time = display_time(encoded_video_pts, video_codec_context_ptr->time_base);
				audio_time = display_time(encoded_audio_pts, (AVRational){ .num = 1, .den = audio_codec_context_ptr->sample_rate });
				
				double delta_video_sec = (encoded_video_pts - start_video_pts) * av_q2d(video_codec_context_ptr->time_base);
				start_video_pts = encoded_video_pts;
				double delta_audio_sec = (encoded_audio_pts - start_audio_pts) / (double)audio_codec_context_ptr->sample_rate;
				start_audio_pts = encoded_audio_pts;
				
				double left_video_sec = duration_sec - video_time.entire_seconds;
				double left_audio_sec = duration_sec - audio_time.entire_seconds;
				double left_encoding_time_sec = (left_video_sec / delta_video_sec + left_audio_sec / delta_audio_sec) * interval_duration;
				display_time_t left_time = display_time_from_secs(left_encoding_time_sec);
				
				printf("\rvideo: %d:%02d:%02d (%.1lf%%) audio: %d:%02d:%02d (%.1lf%%) - time left: %d:%02d:%02d",
					video_time.hours, video_time.minutes, video_time.seconds, video_time.entire_seconds / duration_sec * 100,
					audio_time.hours, audio_time.minutes, audio_time.seconds, audio_time.entire_seconds / duration_sec * 100,
					left_time.hours, left_time.minutes, left_time.seconds);
				if (debug_show)
					printf("\n");
				else
					fflush(stdout);
			}
		}
	}
	
	if (!opts.silent)
		printf("\nDecoding finished, flushing encoders...\n");
	
	// Process any buffered frames that are still in the encoder
	while( x264_encoder_delayed_frames(x264.encoder) > 0 ){
		debug("x264 delayed output frame\n");
		x264.payload_size = x264_encoder_encode(x264.encoder, &x264.nals, &x264.nal_count, NULL, &x264.pic_out);
		if (x264.payload_size > 0)
			enc_mp4_mux_video(mp4_container, mp4_video_track, &x264);
		else if ( x264.payload_size < 0 )
			fprintf(stderr, "x264: encoder error");
	}
	
	// enc_mp4_mux_video() buffers one frame, flush it
	x264.payload_size = 0;
	enc_mp4_mux_video(mp4_container, mp4_video_track, &x264);
	
	// Feed any remaining unencoded samples in the sample buffer to the FAAC encoder
	if (sample_buffer_used > 0){
		debug("delayed unencoded sample buffer: %d bytes\n", sample_buffer_used);
		int encoded_bytes = faacEncEncode(faac.encoder,
			(int32_t*)sample_buffer_ptr, faac.input_sample_count,
			faac.buffer_ptr, faac.buffer_size);
		
		if (encoded_bytes > 0) {
			if ( ! MP4WriteSample(mp4_container, mp4_audio_track, faac.buffer_ptr, encoded_bytes, faac.frame_length, 0, true) )
				fprintf(stderr, "  faac: MP4WriteSample() failed\n");
		} else if (encoded_bytes < 0) {
			fprintf(stderr, "  faac: faacEncEncode() failed\n");
		}
	}
	
	// Flush any buffered AAC frames still in the encoder
	int encoded_bytes = 0;
	while ( (encoded_bytes = faacEncEncode(faac.encoder, NULL, 0, faac.buffer_ptr, faac.buffer_size)) > 0 ){
		debug("FAAC delayed frame\n");
		if ( ! MP4WriteSample(mp4_container, mp4_audio_track, faac.buffer_ptr, encoded_bytes, faac.frame_length, 0, true) )
			fprintf(stderr, "  faac: MP4WriteSample() failed\n");
	}
	
	// Clean up
	MP4Close(mp4_container, 0);
	//MP4MakeIsmaCompliant("video.mp4", mp4_verbosity, true);
	
	av_free(faac.buffer_ptr);
	faacEncClose(faac.encoder);
	
	enc_x264_close(&x264);
	
	avfilter_graph_free(&filter_graph_ptr);
	avcodec_close(video_codec_context_ptr);
	av_close_input_file(format_context_ptr);
	
	avfilter_uninit();
	
	return 0;
}