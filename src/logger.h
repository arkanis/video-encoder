#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN 2
#define LOG_LEVEL_INFO 3
#define LOG_LEVEL_PROGRESS 4
#define LOG_LEVEL_DEBUG 5

int log_init(unsigned int level);
int log_close();

int log_debug(const char *format, ...);
int log_progress(unsigned short offset, const char *format, ...);
int log_info(const char *format, ...);
int log_warn(const char *format, ...);
int log_error(const char *format, ...);