#include <stdio.h>
#include "asf.h"

void main(int argc, char **argv){
	asf_file_t *file_ptr;
	
	file_ptr = asf_open_file(argv[1]);
	asf_init(file_ptr);
	
	int streams = asf_get_stream_count(file_ptr);
	printf("streams: %d\n", streams);
	
	int i;
	for(i = 0; i < streams; i++){
		asf_stream_t *stream_ptr = asf_get_stream(file_ptr, i);
		
		// Output the stuff that is present for all kind of streams
		char *type_name = NULL;
		switch(stream_ptr->type){
			case ASF_STREAM_TYPE_NONE:
				type_name = "none";
				break;
			case ASF_STREAM_TYPE_AUDIO:
				type_name = "audio";
				break;
			case ASF_STREAM_TYPE_VIDEO:
				type_name = "video";
				break;
			case ASF_STREAM_TYPE_COMMAND:
				type_name = "command";
				break;
			default:
				type_name = "unknown";
				break;
		}
		
		printf("stream %d: %s, flags: ", i, type_name);
		if ((stream_ptr->flags & ASF_STREAM_FLAG_AVAILABLE) == ASF_STREAM_FLAG_AVAILABLE)
			printf("AVAILABLE ");
		if ((stream_ptr->flags & ASF_STREAM_FLAG_HIDDEN) == ASF_STREAM_FLAG_HIDDEN)
			printf("HIDDEN ");
		if ((stream_ptr->flags & ASF_STREAM_FLAG_EXTENDED) == ASF_STREAM_FLAG_EXTENDED)
			printf("EXTENDED ");
		printf("\n");
		
		// Output properties structure if available
		if ((stream_ptr->flags & ASF_STREAM_FLAG_AVAILABLE) == ASF_STREAM_FLAG_AVAILABLE){
			if (stream_ptr->type == ASF_STREAM_TYPE_AUDIO) {
				asf_waveformatex_t *audio_ptr = stream_ptr->properties;
				printf("  properties (waveformatex):\n");
				printf("    wFormatTag: %hu\n", audio_ptr->wFormatTag);
				printf("    nChannels: %hu\n", audio_ptr->nChannels);
				printf("    nSamplesPerSec: %u\n", audio_ptr->nSamplesPerSec);
				printf("    nAvgBytesPerSec: %u\n", audio_ptr->nAvgBytesPerSec);
				printf("    nBlockAlign: %hu\n", audio_ptr->nBlockAlign);
				printf("    wBitsPerSample: %hu\n", audio_ptr->wBitsPerSample);
				printf("    cbSize: %hu\n", audio_ptr->cbSize);
				printf("    data: %p\n", audio_ptr->data);
			} else if (stream_ptr->type == ASF_STREAM_TYPE_VIDEO) {
				asf_bitmapinfoheader_t *video_ptr = stream_ptr->properties;
				printf("  properties (bitmapinfoheader):\n");
				printf("    biSize: %u\n", video_ptr->biSize);
				printf("    biWidth: %u\n", video_ptr->biWidth);
				printf("    biHeight: %u\n", video_ptr->biHeight);
				printf("    biPlanes: %hu\n", video_ptr->biPlanes);
				printf("    biBitCount: %hu\n", video_ptr->biBitCount);
				printf("    biCompression: %u\n", video_ptr->biCompression);
				printf("    biSizeImage: %u\n", video_ptr->biSizeImage);
				printf("    biXPelsPerMeter: %u\n", video_ptr->biXPelsPerMeter);
				printf("    biYPelsPerMeter: %u\n", video_ptr->biYPelsPerMeter);
				printf("    biClrUsed: %u\n", video_ptr->biClrUsed);
				printf("    biClrImportant: %u\n", video_ptr->biClrImportant);
				printf("    data: %p\n", video_ptr->data);
			}
		}
		
		if ((stream_ptr->flags & ASF_STREAM_FLAG_EXTENDED) == ASF_STREAM_FLAG_EXTENDED){
			asf_stream_extended_properties_t *ext_ptr = stream_ptr->extended_properties;
			printf("  extended properties:\n");
			printf("    start_time: %lu\n", ext_ptr->start_time);
			printf("    end_time: %lu\n", ext_ptr->end_time);
			printf("    data_bitrate: %u\n", ext_ptr->data_bitrate);
			printf("    buffer_size: %u\n", ext_ptr->buffer_size);
			printf("    initial_buf_fullness: %u\n", ext_ptr->initial_buf_fullness);
			printf("    data_bitrate2: %u\n", ext_ptr->data_bitrate2);
			printf("    buffer_size2: %u\n", ext_ptr->buffer_size2);
			printf("    initial_buf_fullness2: %u\n", ext_ptr->initial_buf_fullness2);
			printf("    max_obj_size: %u\n", ext_ptr->max_obj_size);
			printf("    flags: %u\n", ext_ptr->flags);
			printf("    stream_num: %hu\n", ext_ptr->stream_num);
			printf("    lang_idx: %hu\n", ext_ptr->lang_idx);
			printf("    avg_time_per_frame: %lu\n", ext_ptr->avg_time_per_frame);
			printf("    stream_name_count: %hu\n", ext_ptr->stream_name_count);
			printf("    num_payload_ext: %hu\n", ext_ptr->num_payload_ext);
		}
	}
	
	asf_close(file_ptr);
}
