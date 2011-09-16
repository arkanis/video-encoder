#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>

#include <x264.h>
#include <faac.h>
#include <mp4v2/mp4v2.h>

#include "mp4_encoder.h"
#include "status.h"


// Video encoding stuff
struct SwsContext* video_scaler_ptr = NULL;
x264_t *x264_encoder_ptr = NULL;
x264_picture_t x264_pic_in, x264_pic_out;

typedef struct {
	uint8_t *data;
	uint32_t size;
	x264_picture_t pic;
	x264_nal_t nals[8];
	int nal_count;
} x264_output_t;

x264_output_t x264_prev_output = {.data = NULL, .size = 0};

// Audio encoding stuff
faacEncHandle faac_encoder = NULL;
uint8_t faac_sample_size = 0;
uint32_t faac_input_buffer_size = 0, faac_output_buffer_size = 0;
uint32_t faac_input_buffer_used = 0;
uint8_t *faac_input_buffer_ptr = NULL, *faac_output_buffer_ptr = NULL;
MP4Duration faac_frame_duration = MP4_INVALID_DURATION;

// Container stuff for muxing
MP4FileHandle mp4_container = NULL;
MP4TrackId mp4_video_track = MP4_INVALID_TRACK_ID, mp4_audio_track = MP4_INVALID_TRACK_ID;
bool mp4_video_track_configured = false;


/**
 * Initializes the MP4 encoder. That is x264, FAAC and the mp4v2 library.
 * 
 * ToDo for encoder options:
 * - x264: preset name, preset tuning, profile name, x264_param_parse() for options (might also work for presets?)
 * - faac: mpeg version (2 or 4), profile
 */
int mp4_encoder_open(char *filename, AVStream *video_stream_ptr, char *x264_args, AVStream *audio_stream_ptr, char *faac_args){
	//
	// Init the software scaler to convert any incomming video stream to the format x264 requires. Usually
	// this should just adjust the different stride sizes and maybe do a color space conversation. Resizing
	// of the image is not intended.
	//
	video_scaler_ptr = sws_getContext(
		video_stream_ptr->codec->width, video_stream_ptr->codec->height, video_stream_ptr->codec->pix_fmt,
		video_stream_ptr->codec->width, video_stream_ptr->codec->height, PIX_FMT_YUV420P,
		SWS_FAST_BILINEAR, NULL, NULL, NULL
	);
	if (!video_scaler_ptr){
		status_info("sws: failed to init the software scaler\n");
		return -1;
	}
	
	
	//
	// Init the x264 encoder
	//
	x264_param_t params;
	if ( x264_param_default_preset(&params, "slow", "film") != 0 )	// use "zerolatency" tune to avoid out of order frames
		status_info("x264: failed to set preset defaults\n");
	
	params.i_width = video_stream_ptr->codec->width;
	params.i_height = video_stream_ptr->codec->height;
	// No Annex-B stream since we want to mux the stream into an MP4 file. Therefore we need the NAL length
	// in the first 4 byte, not the start value of an Annex-B stream.
	params.b_annexb = false;
	// fps is the reciprocal of the time base, therefore swap numerator and denumerator
	params.i_fps_num = video_stream_ptr->codec->time_base.den;
	params.i_fps_den = video_stream_ptr->codec->time_base.num;
	// Set the sample aspect ratio for the video stream since this information is also present in the h264 stream
	params.vui.i_sar_width = video_stream_ptr->sample_aspect_ratio.num;
	params.vui.i_sar_height = video_stream_ptr->sample_aspect_ratio.den;
	
	params.rc.i_rc_method = X264_RC_CRF;
	params.rc.f_rf_constant = 20;
	
	if ( x264_param_apply_profile(&params, "high") != 0 )
		status_info("x264: failed to apply profile\n");
	
	x264_encoder_ptr = x264_encoder_open(&params);
	if (x264_encoder_ptr == NULL){
		status_info("x264: failed to initialize encoder\n");
		return -2;
	}
	
	// Alloc an input pixture for x264. The output picture does not need to contain raw data so we do not allocate it, the
	// struct is enough.
	if ( x264_picture_alloc(&x264_pic_in, X264_CSP_I420, video_stream_ptr->codec->width, video_stream_ptr->codec->height) != 0 ){
		status_info("x264: could not allocate input picture\n");
		return -3;
	}
	
	
	//
	// Init the FAAC encoder
	//
	unsigned long faac_input_sample_count, faac_max_output_byte_count;
	faac_encoder = faacEncOpen(audio_stream_ptr->codec->sample_rate, audio_stream_ptr->codec->channels, &faac_input_sample_count, &faac_max_output_byte_count);
	if (faac_encoder == NULL){
		status_info("faac: failed to initialize encoder\n");
		return -4;
	}
	
	faac_sample_size = sizeof(int16_t);  // Have to match the input format of the encoder (see below)
	faac_input_buffer_size = faac_input_sample_count * faac_sample_size;
	faac_output_buffer_size = faac_max_output_byte_count;
	faac_frame_duration = faac_input_sample_count / audio_stream_ptr->codec->channels;
	
	// Allocate buffer for the FAAC input (part of the samples decodec by libavcodec) and output (the AAC bitstream)
	faac_input_buffer_ptr = (uint8_t*) av_mallocz(faac_input_buffer_size);
	faac_input_buffer_used = 0;
	faac_output_buffer_ptr = (uint8_t*) av_mallocz(faac_output_buffer_size);
	
	if (faac_input_buffer_ptr == NULL || faac_output_buffer_ptr == NULL){
		status_info("faac: failed to allocate input or ouput buffer\n");
		return -5;
	}
	
	// Detail config of the encoder
	faacEncConfigurationPtr faac_config_ptr = faacEncGetCurrentConfiguration(faac_encoder);
	faac_config_ptr->mpegVersion = MPEG4;  // for Windows Media Player. It only accpets mpeg4 audio
	faac_config_ptr->aacObjectType = LOW;  // for apple, these things can only play low profile
	faac_config_ptr->inputFormat = FAAC_INPUT_16BIT;  // matches the raw output of the audio decoder (pcm_s16le)
	faacEncSetConfiguration(faac_encoder, faac_config_ptr);
	
	
	//
	// Init the MP4 muxer
	//
	mp4_container = MP4Create(filename, 0);
	if (mp4_container == MP4_INVALID_FILE_HANDLE){
		status_info("mp4v2: failed to create mp4 file %s\n", filename);
		return -6;
	}
	// TODO: Not sure if this has any advantage for file that contain a audio _and_ video stream. A look into the spec might clear things up.
	//MP4SetTimeScale(container, time_base.num * time_base.den);
	// TODO: The man page of MP4SetAudioProfileLevel() does not list 0x0f. Look into the spec profile and level this is (maybe low profile?)
	MP4SetAudioProfileLevel(mp4_container, 0x0f);
	// TODO: Depricated, look how to do it properly if it's really necessary
	//MP4SetMetadataTool(container, "HdM encoder");
	
	// Add the video track to the container. Use the product of the timebase numerator and denumerator as time scale
	// (the number of ticks per second). Then we only have to multiply each PTS with the numerator. The sample duration
	// is set for each sample since the duration of frames generated by x264 can vary.
	// The profile_idc, profile_compat and level_idc are set to 0 for now but are updated with proper values as soon as
	// the first SPS (sequence parameter set) NAL is received from x264. x264 puts the payload length into the first 4 byte
	// before each NAL. This is perfect for MP4 (to be more exact AVC1 encapsulation in an MP4 container). Therefore we
	// set the sampleLenFieldSizeMinusOne parameter to 3.
	mp4_video_track = MP4AddH264VideoTrack(mp4_container, video_stream_ptr->codec->time_base.num * video_stream_ptr->codec->time_base.den,
		MP4_INVALID_DURATION, video_stream_ptr->codec->width, video_stream_ptr->codec->height,
		0, 0, 0, 3);
	if (mp4_video_track == MP4_INVALID_TRACK_ID){
		status_info("mp4v2: failed to add video track to container\n");
		return -7;
	}
	
	MP4AddPixelAspectRatio(mp4_container, mp4_video_track, video_stream_ptr->sample_aspect_ratio.num, video_stream_ptr->sample_aspect_ratio.den);
	mp4_video_track_configured = false;
	
	// Add the audio track to the container
	mp4_audio_track = MP4AddAudioTrack(mp4_container, audio_stream_ptr->codec->sample_rate, MP4_INVALID_DURATION, MP4_MPEG4_AUDIO_TYPE);
	if (mp4_audio_track == MP4_INVALID_TRACK_ID){
		status_info("mp4v2: failed to add audio track to container\n");
		return -8;
	}
	
	/* TODO: Leads to files that can not be played with Totem (gstreamer). Figure out why and what this should do in the first place.
	uint8_t *aac_config_ptr = NULL;
	unsigned long aac_config_length = 0;
	faacEncGetDecoderSpecificInfo(faac_encoder, &aac_config_ptr, &aac_config_length);
	MP4SetTrackESConfiguration(container, audio_track, aac_config_ptr, aac_config_length);
	free(aac_config_ptr);
	*/
	
	status_detail("Encoder initialized\n");
	
	return 0;
}


/**
 * Writes a video sample to the mp4 video track. If the track is not configured some codec details of the video track are updated
 * based on the first SPS NAL received.
 */
static void mp4_encoder_write_video_sample(x264_nal_t *nals, int nal_count, x264_picture_t *pic_ptr, size_t payload_size, int64_t decode_delta, int64_t composition_offset){
	x264_nal_t* nal_ptr = NULL;
	
	status_detail("  muxing NALs:");
	for(int i = 0; i < nal_count; i++){
		nal_ptr = &nals[i];
		status_detail(" %d", nal_ptr->i_type);
		switch(nal_ptr->i_type){
			case NAL_SPS:
				// If the codec details of the video track are not yet set to valid values do so based on the first
				// sequence parameter set.
				if (!mp4_video_track_configured){
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
					//printf("... track: profile_idc %d, profile_compat %x, level_idc: %d\n", profile_idc, profile_compat, level_idc);
					
					// Update the codec details of the video track. Taken from MP4File::AddH264VideoTrack(),
					// mp4file.cpp line 1858 of libmp4v2.
					MP4SetTrackIntegerProperty(mp4_container, mp4_video_track,
						"mdia.minf.stbl.stsd.avc1.avcC.AVCProfileIndication", profile_idc);
					MP4SetTrackIntegerProperty(mp4_container, mp4_video_track,
						"mdia.minf.stbl.stsd.avc1.avcC.profile_compatibility", profile_compat);
					MP4SetTrackIntegerProperty(mp4_container, mp4_video_track,
						"mdia.minf.stbl.stsd.avc1.avcC.AVCLevelIndication", level_idc);
					
					mp4_video_track_configured = true;
				}
				
				// Put the sequence parameter set into the MP4 container. Framing is provided
				// by the container, therefore we don't need the leading 4 bytes (the payload size).
				MP4AddH264SequenceParameterSet(mp4_container, mp4_video_track, nal_ptr->p_payload + 4, nal_ptr->i_payload - 4);
				break;
			case NAL_PPS:
				// Put the picture parameter set into the MP4 container. Framing is provided
				// by the container, therefore we don't need the leading 4 bytes (the payload size).
				MP4AddH264PictureParameterSet(mp4_container, mp4_video_track, nal_ptr->p_payload + 4, nal_ptr->i_payload - 4);
				break;
			case NAL_FILLER:
				// Throw filler data away (AVC spec wants it)
				break;
			default:
				// Every thing else is data for the video track. Collect all remaining NALs and
				// put them into one MP4 sample.
				if (mp4_video_track != MP4_INVALID_TRACK_ID) {
					int remaining_nals = nal_count - i;
					uint8_t *start_ptr = nals[i].p_payload;
					int size = payload_size - ((void*)start_ptr - (void*)(nals[0].p_payload));
					
					bool is_sync_sample = pic_ptr->b_keyframe;
					status_detail(" storing %d NALs, %d bytes", remaining_nals, size);
					if ( MP4WriteSample(mp4_container, mp4_video_track, start_ptr, size, decode_delta, composition_offset, is_sync_sample) != true)
						status_info(" MP4WriteSample (NAL %d) failed", i);
					
					i += remaining_nals;
				} else {
					status_info(" ignoring because there is no video track! ");
				}
				break;
		}
	}
	
	status_detail("\n");
}


/**
 * Expects a decoded frame, encodes it with x264 and puts it into the mp4 container.
 * Set `frame_ptr` to `NULL` to flush the encoder. This is required because x264 buffers some frames for it's encoding work
 * that need to be flushed at the end. We also buffer one frame to calculate the proper frame durations needed by the mp4
 * container.
 * 
 * Returns the number of frames encoded. This is 1 or 0.
 */
int mp4_encoder_process_video(AVFrame *frame_ptr){
	x264_nal_t *nals = NULL;
	int nal_count = 0, payload_size = 0;
	
	if (frame_ptr != NULL) {
		// We got a fresh frame, put it into the encoder.
		x264_pic_in.i_type = X264_TYPE_AUTO;
		x264_pic_in.i_pts = frame_ptr->pts;
		sws_scale(video_scaler_ptr, (const uint8_t * const*)frame_ptr->data, frame_ptr->linesize,
			0, frame_ptr->height, x264_pic_in.img.plane, x264_pic_in.img.i_stride);
		
		payload_size = x264_encoder_encode(x264_encoder_ptr, &nals, &nal_count, &x264_pic_in, &x264_pic_out);
	} else if ( x264_encoder_delayed_frames(x264_encoder_ptr) > 0 ) {
		// Delayed frames are still in the x264 encoder. Get one an process it.
		payload_size = x264_encoder_encode(x264_encoder_ptr, &nals, &nal_count, NULL, &x264_pic_out);
	} else if (x264_prev_output.data == NULL) {
		// We got no input frame, no delayed frame from the encoder and our own buffer is also empty. Nothing
		// more to do, we finished the job. So just return 0 to signal that we did not encode a frame.
		return 0;
	}
	
	if (payload_size < 0){
		status_info("x264: error while encoding frame\n");
		return -1;
	}
	
	if (payload_size > 0) {
		// We got data from the encoder. If we already have the previous x264 output frame buffered we can
		// write the buffered frame to the mp4 container. Necesary because we need the _duration_ (decode
		// delta) of the frame and we can only calculate it when we know the PTS of the next frame.
		if (x264_prev_output.data != NULL) {
			// Calculate a proper decoding delta for the MP4 sample
			int64_t decode_delta, composition_offset;
			decode_delta = x264_pic_out.i_dts - x264_prev_output.pic.i_dts;
			composition_offset = x264_prev_output.pic.i_pts - x264_prev_output.pic.i_dts;
			status_detail("encode video: prev: (dts: %ld, pts: %ld), curr: (dts: %ld, pts: %ld), dec delta: %ld, comp offset: %ld\n",
				x264_prev_output.pic.i_dts, x264_prev_output.pic.i_pts, x264_pic_out.i_dts, x264_pic_out.i_pts,
				decode_delta, composition_offset);
			
			//status_progress(40, "mp4 dec delta: %ld, comp offset: %ld", decode_delta, composition_offset);
			//status_detail("\nx264,  NALs:",
			//	, );
			
			mp4_encoder_write_video_sample(x264_prev_output.nals, x264_prev_output.nal_count,
				&x264_prev_output.pic, x264_prev_output.size, decode_delta, composition_offset);
		}
		
		// Store the current x264 output as the previous output so we can calculate the decoding delta of the next sample
		if (nals != NULL) {
			status_detail("  putting frame in MP4 encoder buffer\n");
			if (x264_prev_output.data != NULL){
				free(x264_prev_output.data);
				x264_prev_output.data = NULL;
			}
			x264_prev_output.data = (uint8_t*) malloc(payload_size);
			if (x264_prev_output.data == NULL){
				status_info("store_nal: failed to allcoate buffer for the x264 output!\n");
				return -2;
			}
			memcpy(x264_prev_output.data, nals[0].p_payload, payload_size);
			x264_prev_output.size = payload_size;
			
			x264_prev_output.pic = x264_pic_out;
			x264_prev_output.nal_count = nal_count;
			if (nal_count >= 8){
				status_info("store_nal: received more NALs than the output buffer can hold!\n");
				return -3;
			}
			
			for(int i = 0; i < nal_count; i++){
				x264_prev_output.nals[i] = nals[i];
				int offset = nals[i].p_payload - nals[0].p_payload;
				x264_prev_output.nals[i].p_payload = x264_prev_output.data + offset;
			}
		} else {
			status_detail("  flushing MP4 encoder buffer\n");
		}
	} else if (frame_ptr == NULL) {
		// No input data was given and we got no delayed data from the encoder. So we should flush out our own buffer and
		// write the final frame. The last frame is allowed to have a decode delta (duration) of 0.
		int64_t decode_delta, composition_offset;
		decode_delta = 1;
		composition_offset = x264_prev_output.pic.i_pts - x264_prev_output.pic.i_dts;
		
		status_detail("encode video: flushing MP4 encoder buffer: (dts: %lu, pts: %lu), dec delta: %ld, comp offset: %ld\n",
			x264_pic_out.i_dts, x264_pic_out.i_pts, decode_delta, composition_offset);
		
		mp4_encoder_write_video_sample(x264_prev_output.nals, x264_prev_output.nal_count,
			&x264_prev_output.pic, x264_prev_output.size, decode_delta, composition_offset);
		
		// Clean up the buffered output data
		free(x264_prev_output.data);
		x264_prev_output.data = NULL;
	}
	
	return 1;
}


/**
 *
 */
int mp4_encoder_process_audio(void *sample_buffer, size_t sample_buffer_size){
	if (sample_buffer) {
		// We got new sample data, feed it to the encoder
		size_t bytes_processed = 0;
		while (bytes_processed < sample_buffer_size){
			// Calculate how many bytes we can put into the next input buffer
			size_t free_input_buffer_size = faac_input_buffer_size - faac_input_buffer_used;
			size_t available_input_size = sample_buffer_size - bytes_processed;
			size_t batch_size = (free_input_buffer_size < available_input_size) ? free_input_buffer_size : available_input_size;
			
			memcpy(faac_input_buffer_ptr + faac_input_buffer_used, sample_buffer + bytes_processed, batch_size);
			faac_input_buffer_used += batch_size;
			
			// If we can not fill the input buffer with enough bytes we should be at the
			// end of the sample buffer. Leave the data in the input buffer until we receive
			// more sample data.
			if (faac_input_buffer_used < faac_input_buffer_size){
				bytes_processed += batch_size;
				break;
			}
			
			int encoded_bytes = faacEncEncode(faac_encoder,
				(int32_t*)faac_input_buffer_ptr, faac_input_buffer_used / faac_sample_size,
				faac_output_buffer_ptr, faac_output_buffer_size);
			
			if (encoded_bytes < 0)
				status_info("faac: failed to encode audio frame\n");
			
			if (encoded_bytes > 0){
				if ( MP4WriteSample(mp4_container, mp4_audio_track, faac_output_buffer_ptr, encoded_bytes, faac_frame_duration, 0, true) != true)
					status_info("faac: failed to write audio frame to mp4 audio track\n");
			}
			
			bytes_processed += batch_size;
			faac_input_buffer_used = 0;
		}
		
		if (bytes_processed != sample_buffer_size)
			status_info("faac: failed to process all sample data! %lu of %lu bytes processed\n", bytes_processed, sample_buffer_size);
		
		return 1;
	} else {
		// No new sample data, flush the encoder
		if (faac_input_buffer_used > 0) {
			// There are still some samples left from a previous call, feed them to the encoder
			int encoded_bytes = faacEncEncode(faac_encoder,
				(int32_t*)faac_input_buffer_ptr, faac_input_buffer_used / faac_sample_size,
				faac_output_buffer_ptr, faac_output_buffer_size);
			
			if (encoded_bytes < 0)
				status_info("faac: failed to encode audio frame\n");
			
			if (encoded_bytes > 0){
				if ( MP4WriteSample(mp4_container, mp4_audio_track, faac_output_buffer_ptr, encoded_bytes, faac_frame_duration, 0, true) != true)
					status_info("faac: failed to write audio frame to mp4 audio track\n");
			}
			
			faac_input_buffer_used = 0;
			return 1;
		} else {
			// No samples left to feed to the encoder, so flush it
			int encoded_bytes = faacEncEncode(faac_encoder, NULL, 0, faac_output_buffer_ptr, faac_output_buffer_size);
			
			if (encoded_bytes < 0)
				status_info("faac: failed to encode audio frame\n");
			
			if (encoded_bytes > 0){
				if ( MP4WriteSample(mp4_container, mp4_audio_track, faac_output_buffer_ptr, encoded_bytes, faac_frame_duration, 0, true) != true)
					status_info("faac: failed to write audio frame to mp4 audio track\n");
			}
			
			return (encoded_bytes > 0);
		}
	}
	
	/*
	faac_input_buffer_ptr = (uint8_t*) av_mallocz(faac_input_buffer_size);
	faac_input_buffer_used = 0;
	faac_output_buffer_ptr = (uint8_t*) av_mallocz(faac_output_buffer_size);
	
	int samples_to_encode = sample_buffer_used / sample_size;
	int buffer_encoded = 0;
	
	
	
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
	*/
}


/**
 * Finishes up the encoding: flush all encoders, write everything to disk, do some cleanup and optimization and finally free used resources.
 */
int mp4_encoder_close(){
	// Flush any buffered video and audio frames from the encoder
	while (mp4_encoder_process_video(NULL) > 0) {};
	while (mp4_encoder_process_audio(NULL, 0) > 0) {};
	
	// Close the FAAC encoder and free the input and output buffers
	faacEncClose(faac_encoder);
	av_free(faac_output_buffer_ptr);
	av_free(faac_input_buffer_ptr);
	
	// Close the x264 encoder and the software scaled used for it
	x264_picture_clean(&x264_pic_in);
	x264_encoder_close(x264_encoder_ptr);
	sws_freeContext(video_scaler_ptr);
	
	// Close the mp4 file. That will write the rest of it to disk.
	MP4Close(mp4_container, 0);
	// TODO: Figure out what exactly this is for.
	//MP4MakeIsmaCompliant("video.mp4", mp4_verbosity, true);
	// TODO: Optimize the file
}