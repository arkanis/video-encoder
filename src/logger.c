/**
 * This si a small output logger that prints everything to `stderr`. If you need it in a file use output
 * redirection in bash. This "logger" is just meant to be nothing more than a `printf` with a bit more
 * semantics and configuration (log levels).
 * 
 * The logger is not thread safe. However multiple threads logging a normal message should not make
 * problems. Logging progress messages however includes some global state and therefore is not thread
 * safe.
 */

#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"

unsigned int log_level = 0;
bool is_terminal = false;

size_t line_size = 0;
int line_filled = 0;
char *line_ptr = NULL;
// if `true` other log functions first have to send a new line
// to not overwrite a progress message
bool in_progress_line = false;


int log_init(unsigned int level)
{
	log_level = level;
	is_terminal = isatty(STDERR_FILENO);
	
	line_size = 81;
	line_ptr = malloc(line_size * sizeof(char));
	
	return 0;
}

int log_close()
{
	free(line_ptr);
	return 0;
}


static int log_message(unsigned int msg_level, const char *prefix, const char *format, va_list args)
{
	if ( msg_level > log_level )
		return 0;
	
	if (in_progress_line){
		fprintf(stderr, "\n");
		in_progress_line = false;
	}
	
	if (prefix != NULL)
		fprintf(stderr, "%s", prefix);
	
	return vfprintf(stderr, format, args);
}

int log_debug(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	int bytes_written = log_message(LOG_LEVEL_DEBUG, NULL, format, args);
	va_end(args);
	
	return bytes_written;
}

int log_info(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	int bytes_written = log_message(LOG_LEVEL_INFO, NULL, format, args);
	va_end(args);
	
	return bytes_written;
}

int log_warn(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	int bytes_written = log_message(LOG_LEVEL_WARN, "warning: ", format, args);
	va_end(args);
	
	return bytes_written;
}

int log_error(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	int bytes_written = log_message(LOG_LEVEL_ERROR, "error: ", format, args);
	va_end(args);
	
	return bytes_written;
}

int log_progress(unsigned short offset, const char *format, ...)
{
	if (log_level < LOG_LEVEL_PROGRESS || !is_terminal)
		return 0;
	if (offset >= line_size)
		return -1;
	
	// Fill the empty space from the end of the current
	// content up to the offset with spaces
	if (offset > line_filled && offset < line_size)
		memset(line_ptr + line_filled, ' ', offset - line_filled);
	
	// First let `vsnprintf` calculate the length of the output message and backup the character
	// that will be overwritten by the NUL terminator.
	va_list args;
	va_start(args, format);
	int message_size = vsnprintf(NULL, 0, format, args);
	va_end(args);
	
	int terminator_idx = (offset + message_size > line_size - 1) ? line_size - 1 : offset + message_size;
	char backup = line_ptr[terminator_idx];
	
	// Put the new stuff in the progress line buffer and restore the character overwritten by
	// the NUL terminator.
	int max_message_size = line_size - offset;
	va_start(args, format);
	int bytes_written = vsnprintf(line_ptr + offset, max_message_size, format, args);
	va_end(args);
	line_ptr[terminator_idx] = backup;
	
	// Calculate the new length of the content in the line buffer.
	int end_of_message = offset + ( (bytes_written > max_message_size) ? max_message_size : bytes_written );
	line_filled = (end_of_message > line_filled) ? end_of_message : line_filled;
	
	// Advice the `log` function to output a new line to get out of the progress line when it needs
	// to print a message.
	in_progress_line = true;
	
	// The negative line width makes sure the line is left aligned
	return fprintf(stderr, "\r%*s", -line_filled, line_ptr);
}