int mp4_encoder_open(char *filename, AVStream *video_stream_ptr, char *x264_args, AVStream *audio_stream_ptr, char *faac_args);
int mp4_encoder_process_video(AVFrame *frame_ptr);
int mp4_encoder_process_audio(void *sample_buffer, size_t sample_buffer_size);
int mp4_encoder_close();