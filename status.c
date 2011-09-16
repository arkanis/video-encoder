#include <stdbool.h>
#include <termios.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

bool is_terminal = false;
bool show_progress = true;
bool show_details = true;
struct termios original_settings;
char *line_ptr = NULL;
size_t line_size = 0;
int message_size = 0;
// Is set to true if a progress message was last outputted. This is important because other
// messages have to output a newline first because progress messages overwrite the same
// line repeatedly.
bool progress_last_shown = false;

/**
 * If `stdin` is a terminal canonical input mode (line editing, etc.) and input echoing is switched of.
 * Also configures the terminal in a way that it can be polled for single character user input. This is used
 * by the `status_poll_command()` function.
 */
int status_init(bool show_progress_messages, bool show_detail_messages){
	if ( isatty(STDIN_FILENO) ){
		is_terminal = true;
		
		struct termios current_settings;
		if ( tcgetattr(STDIN_FILENO, &current_settings) == -1 ){
			perror("tcgetattr");
			return -1;
		}
			
		original_settings = current_settings;
		
		current_settings.c_lflag &= ~(ICANON | ECHO);
		current_settings.c_cc[VMIN] = 0;
		current_settings.c_cc[VTIME] = 0;
		
		if ( tcsetattr(STDIN_FILENO, TCSADRAIN, &current_settings) == -1 ){
			perror("tcsetattr");
			return -2;
		}
	}
	
	show_progress = show_progress_messages;
	show_details = show_detail_messages;
	
	line_size = 80;
	line_ptr = malloc(line_size * sizeof(char));
	
	return 1;
}

/**
 * Just a normal output function. Outputs the message to `stderr`. Should be used for normal output.
 */
int status_info(const char *format, ...){
	va_list args;
	
	va_start(args, format);
	
	if (progress_last_shown){
		printf("\n");
		progress_last_shown = false;
	}
	
	return vfprintf(stderr, format, args);
}


int status_detail(const char *format, ...){
	va_list args;
	
	if (!show_details)
		return 0;
	
	va_start(args, format);
	
	if (progress_last_shown){
		printf("\n");
		progress_last_shown = false;
	}
	
	return vfprintf(stderr, format, args);
}

/**
 * Output function for progress messages. The message is only outputted if `stderr` is a terminal. Otherwise
 * the progress information would flood the log file.
 */
int status_progress(uint16_t offset, const char *format, ...){
	va_list args;
	va_start(args, format);
	
	if (!show_progress)
		return 0;
	if (offset >= line_size)
		return -1;
	
	// Set the flag so other functions will print a newline after the progress line
	progress_last_shown = true;
	
	// Fill the empty space up to the offset with spaces
	if (offset > message_size && offset < line_size)
		memset(line_ptr + message_size, ' ', offset - message_size);
	
	int bytes_written = vsnprintf(line_ptr + offset, line_size - offset, format, args);
	// Calculate the number of valid chars in the message. Also check for returned size values that
	// indicate the line buffer was to small. In that case just output the whole line.
	message_size = (bytes_written > line_size - offset) ? line_size : offset + bytes_written;
	
	return fprintf(stderr, "\r%*s\n", message_size, line_ptr);
}

/**
 * Checks if the user issued any command by pressing a key. The return value carries the command
 * if one is pending:
 * 
 * 0: no command pending
 * 1: user requested to abort encoding
 */
int status_poll_command(){
	char input = 0;
	
	read(STDIN_FILENO, &input, 1);
	if (input == 'q')
		return 1;
	
	return 0;
}

/**
 * Restores the original terminal state.
 */
int status_close(){
	free(line_ptr);
	
	if (is_terminal){
		if ( tcsetattr(STDIN_FILENO, TCSADRAIN, &original_settings) == -1 ){
			perror("tcsetattr");
			return -2;
		}
	}
	
	return 1;
}