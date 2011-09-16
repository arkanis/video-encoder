int status_init(bool show_progress_messages, bool show_detail_messages);
int status_info(const char *format, ...);
int status_progress(uint16_t offset, const char *format, ...);
int status_detail(const char *format, ...);
int status_poll_command();
int status_close();