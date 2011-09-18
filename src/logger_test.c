// _XOPEN_SOURCE is necessary to get `usleep`
#define _XOPEN_SOURCE 500
#include <unistd.h>
#include "logger.h"

void main()
{
	log_init(LOG_LEVEL_DEBUG);
	
	log_debug("debug message, test values: %d\n", 1);
	log_info("info message, test values: %d, %f\n", 2, 3.141);
	log_warn("something is strange, test values: %s\n", "hello");
	log_error("something is wrong, test values: %#04zx\n", sizeof(8.2));
	
	log_info("starting progress…\n");
	
	usleep(500000);
	log_progress(0, "first: %1d |", 1);
	usleep(500000);
	log_progress(25, "third");
	usleep(500000);
	log_progress(10, "second: %1d |", 2);
	
	for(int i = 0; i < 5; i++){
		usleep(500000);
		log_progress(0, "first: %1d |", i);
	}
	
	usleep(500000);
	log_progress(25, "third");
	
	log_info("ending progress…\n");
	
	log_close();
}