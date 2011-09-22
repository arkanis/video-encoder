#ifndef _QUEUE_H
#define _QUEUE_H

/**
 * Queue for buffer flow between threads.
 * 
 * Features:
 * - Buffers are handled in the producer thread by the callbacks: allocate, cleanup, deallocate
 * - Buffers have a defined life cycle:
 *   - allocation: before the producer pushes a buffer it is allocated
 *   - cleanup: after the consumer consumed it this callback can clean the buffer for the next usage
 *   - deallocation: called when the queue is destroyed or cleanup requested that the buffer should be freed
 * - A queue can be handed of from one producer to another without waking up the consumer.
 * - Buffers can be preallocated. These buffers are then used as a ring buffers.
 */

#include <pthread.h>
#include <stdbool.h>

#ifdef QUEUE_MINIQUEUE
struct queue_buffer_s {
	int type;
	void *data;
	struct queue_buffer_s *next;
};

typedef struct queue_buffer_s queue_buffer_t;

void miniqueue_push(queue_buffer_t **head, queue_buffer_t **tail, queue_buffer_t *buffer);
queue_buffer_t* miniqueue_pull(queue_buffer_t **head, queue_buffer_t **tail);
#endif

typedef void* (*queue_buffer_allocator_t)(int type);
typedef void (*queue_buffer_cleaner_t)(int type, void *buffer);
typedef void (*queue_buffer_deallocator_t)(int type, void *buffer);

struct queue_s {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	
	queue_buffer_allocator_t allocator;
	queue_buffer_deallocator_t deallocator;
	queue_buffer_cleaner_t cleaner;
	
	queue_buffer_t *free_head, *free_tail, *filled_head, *filled_tail, *dirty_head, *dirty_tail;
	
	int preallocated_type;
	queue_buffer_t *pushed, *pulled;
};

typedef struct queue_s *queue_t;

queue_t queue_new();
void queue_destroy(queue_t queue);

void queue_attach_producer(queue_t queue, queue_buffer_allocator_t allocator, queue_buffer_cleaner_t cleaner, queue_buffer_deallocator_t deallocator);
void queue_preallocate_buffers(queue_t queue, size_t count, int type);
void queue_detach_producer(queue_t queue);

void* queue_push_begin(queue_t queue, int type);
int queue_push_end(queue_t queue);

void* queue_pull_begin(queue_t queue, int *type);
int queue_pull_end(queue_t queue);

#endif