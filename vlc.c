#include <stdio.h>
#include <stdlib.h>
#include <vlc/vlc.h>

void media_parsed(const struct libvlc_event_t *event, void *data){
	libvlc_media_t *media = (libvlc_media_t*) data;
	
	libvlc_media_track_info_t *track_info_ptr;
	int tracks = libvlc_media_get_tracks_info(media, &track_info_ptr);
	
	fprintf(stderr, "found %d tracks\n", tracks);
	int i;
	for (i = 0; i < tracks; i++){
	char *type_name = NULL;
	switch(track_info_ptr[i].i_type){
		case libvlc_track_audio:
			type_name = "audio";
			break;
		case libvlc_track_video:
			type_name = "video";
			break;
		case libvlc_track_text:
			type_name = "text";
			break;
		default:
			type_name = "unknown";
			break;
	}
	fprintf(stderr, "track: %d, codec: %d, id: %d, type: %s, profile: %d, level: %d\n", i,
		track_info_ptr[i].i_codec, track_info_ptr[i].i_id, type_name, track_info_ptr[i].i_profile, track_info_ptr[i].i_level);
	if (track_info_ptr[i].i_type == libvlc_track_audio)
		fprintf(stderr, "  audio: channels: %u, rate: %u\n", track_info_ptr[i].u.audio.i_channels, track_info_ptr[i].u.audio.i_rate);
	if (track_info_ptr[i].i_type == libvlc_track_video)
		fprintf(stderr, "  video: height: %u, width: %u\n", track_info_ptr[i].u.video.i_height, track_info_ptr[i].u.video.i_width);
	}

	free(track_info_ptr);
}

const char * const vlc_args[] = {"-v"};

int main(int argc, char* argv[])
{
   libvlc_instance_t * inst;
   libvlc_media_player_t *mp;
   libvlc_media_t *m;
   
   /* Load the VLC engine */
   inst = libvlc_new (1, vlc_args);

   /* Create a new item */
   m = libvlc_media_new_path(inst, argv[1]);
   libvlc_media_add_option(m, "sout=#description:dummy");
   libvlc_event_attach(libvlc_media_event_manager(m), libvlc_MediaPlayerPlaying, media_parsed, m);
   libvlc_media_parse(m);
   
   
      
   /* Create a media player playing environement */
   mp = libvlc_media_player_new_from_media (m);
   
   /* No need to keep the media now */
   libvlc_media_release (m);

#if 0
   /* This is a non working code that show how to hooks into a window,
    * if we have a window around */
    libvlc_media_player_set_xdrawable (mp, xdrawable);
   /* or on windows */
    libvlc_media_player_set_hwnd (mp, hwnd);
   /* or on mac os */
    libvlc_media_player_set_nsobject (mp, view);
#endif

   /* play the media_player */
   libvlc_media_player_play (mp);
  
   sleep (10); /* Let it play a bit */
  
   /* Stop playing */
   libvlc_media_player_stop (mp);
		
		
	libvlc_media_track_info_t *track_info_ptr;
	int tracks = libvlc_media_get_tracks_info(m, &track_info_ptr);
	
	fprintf(stderr, "found %d tracks\n", tracks);
	int i;
	for (i = 0; i < tracks; i++){
	char *type_name = NULL;
	switch(track_info_ptr[i].i_type){
		case libvlc_track_audio:
			type_name = "audio";
			break;
		case libvlc_track_video:
			type_name = "video";
			break;
		case libvlc_track_text:
			type_name = "text";
			break;
		default:
			type_name = "unknown";
			break;
	}
	fprintf(stderr, "track: %d, codec: %d, id: %d, type: %s, profile: %d, level: %d\n", i,
		track_info_ptr[i].i_codec, track_info_ptr[i].i_id, type_name, track_info_ptr[i].i_profile, track_info_ptr[i].i_level);
	if (track_info_ptr[i].i_type == libvlc_track_audio)
		fprintf(stderr, "  audio: channels: %u, rate: %u\n", track_info_ptr[i].u.audio.i_channels, track_info_ptr[i].u.audio.i_rate);
	if (track_info_ptr[i].i_type == libvlc_track_video)
		fprintf(stderr, "  video: height: %u, width: %u\n", track_info_ptr[i].u.video.i_height, track_info_ptr[i].u.video.i_width);
	}

	free(track_info_ptr);
		
   /* Free the media_player */
   libvlc_media_player_release (mp);

   libvlc_release (inst);

   return 0;
}
