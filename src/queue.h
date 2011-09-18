#include <pthread.h>
#include <stdbool.h>

typedef void* (*queue_buffer_allocator_t)();
typedef int (*queue_buffer_deallocator_t)(void *buffer);
typedef int (*queue_buffer_cleaner_t)(void *buffer);

struct queue_s {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	
	queue_buffer_allocator_t allocator;
	queue_buffer_deallocator_t deallocator;
	queue_buffer_cleaner_t cleaner;
	
	size_t size;
	size_t dirty_idx, dirty_buffers, filled_buffers;
	void **buffers;
	
	bool pushing, pulling;
	bool drained, drained_ack;
};

typedef struct queue_s *queue_t;

queue_t queue_new(unsigned int size, queue_buffer_allocator_t allocator, queue_buffer_deallocator_t deallocator, queue_buffer_cleaner_t cleaner);
void queue_destroy(queue_t queue);
void* queue_push_begin(queue_t queue);
int queue_push_end(queue_t queue);
void* queue_pull_begin(queue_t queue);
int queue_pull_end(queue_t queue);

void queue_drained(queue_t queue);
void queue_refilled(queue_t queue);

void queue_detach_producer(queue_t queue);
void queue_attach_producer(queue_t queue, unsigned int size, queue_buffer_allocator_t allocator, queue_buffer_deallocator_t deallocator, queue_buffer_cleaner_t cleaner);