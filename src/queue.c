#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include "queue.h"

/**
 * Creates a new queue with `size` buffers in it. The `allocator` function is called `size` times to
  * allocate all buffers.
 */
queue_t queue_new(unsigned int size, queue_buffer_allocator_t allocator, queue_buffer_deallocator_t deallocator, queue_buffer_cleaner_t cleaner)
{
	assert(size > 0);
	assert(allocator != NULL);
	assert(deallocator != NULL);
	assert(cleaner != NULL);
	
	queue_t queue = malloc(sizeof(struct queue_s));
	assert(queue != NULL);

	// Set defaults
	*queue = (struct queue_s){
		.dirty_idx = 0, .dirty_buffers = 0, .filled_buffers = 0,
		.pushing = false, .pulling = false, .drained = false
	};
	
	// Setup threading primitives
	int err = pthread_mutex_init(&queue->mutex, NULL);
	assert(err == 0);

	err = pthread_cond_init(&queue->cond, NULL);
	assert(err == 0);
	
	// Setup the queue configuration
	queue->size = size;
	queue->allocator = allocator;
	queue->deallocator = deallocator;
	queue->cleaner = cleaner;
	
	// Setup the queue buffers
	queue->buffers = malloc(size * sizeof(void*));
	assert(queue->buffers != NULL);
	
	for(int i = 0; i < size; i++)
		queue->buffers[i] = queue->allocator();
	
	return queue;
}

//
// Helper functions to calculate states of the queue that are not directly stored
// as variables.
//

static inline size_t free_buffers(queue_t queue) {
	return queue->size - (queue->dirty_buffers + queue->filled_buffers);
}

static inline size_t out_idx(queue_t queue) {
	return (queue->dirty_idx + queue->dirty_buffers) % queue->size;
}

static inline size_t in_idx(queue_t queue) {
	return (queue->dirty_idx + queue->dirty_buffers + queue->filled_buffers) % queue->size;
}

/**
 * Cleans all dirty buffers currently in the queue. This function requires that the queue
 * mutex is locked. Otherwise race conditions can easily happen.
 */
static void clean_dirty_buffers(queue_t queue)
{
	while( queue->dirty_buffers > 0 ){
		queue->cleaner(queue->buffers[queue->dirty_idx]);
		queue->dirty_idx = (queue->dirty_idx + 1) % queue->size;
		queue->dirty_buffers--;
	}
}


/**
 * First cleans all dirty buffers and then deallocates all buffers. After that the
 * queue and all bound resources are destroyed.
 */
void queue_destroy(queue_t queue)
{
	clean_dirty_buffers(queue);
	
	for(int i = 0; i < queue->size; i++)
		queue->deallocator(queue->buffers[i]);
	
	int err = pthread_cond_destroy(&queue->cond);
	assert(err == 0);
	err = pthread_mutex_destroy(&queue->mutex);
	assert(err == 0);
	
	free(queue->buffers);
	free(queue);
}


/**
 * Returns an unused buffer from the queue. Note that this buffer might contain old data from previous usage.
 * If no unused buffer is available right now the function will block until a buffer is freed by another thread using
 * `queue_pull_end`.
 */
void* queue_push_begin(queue_t queue)
{
	pthread_mutex_lock(&queue->mutex);
		assert(queue->pushing == false);
		queue->pushing = true;
		
		clean_dirty_buffers(queue);
		while( free_buffers(queue) < 1 ){
			pthread_cond_wait(&queue->cond, &queue->mutex);
			clean_dirty_buffers(queue);
		}
		
		void* buffer = queue->buffers[in_idx(queue)];
	pthread_mutex_unlock(&queue->mutex);

	return buffer;
}

int queue_push_end(queue_t queue)
{
	pthread_mutex_lock(&queue->mutex);

	assert(queue->pushing == true);
	queue->pushing = false;

	queue->filled_buffers++;
	
	pthread_mutex_unlock(&queue->mutex);
	
	pthread_cond_signal(&queue->cond);
}

void* queue_pull_begin(queue_t queue)
{
	pthread_mutex_lock(&queue->mutex);

	assert(queue->pulling == false);
	queue->pulling = true;

	while ( queue->filled_buffers < 1 ){
		if (queue->drained){
			pthread_mutex_unlock(&queue->mutex);
			return NULL;
		}

		pthread_cond_wait(&queue->cond, &queue->mutex);
	}

	void* buffer = queue->buffers[out_idx(queue)];
	
	pthread_mutex_unlock(&queue->mutex);

	return buffer;
}

int queue_pull_end(queue_t queue)
{
	pthread_mutex_lock(&queue->mutex);

	assert(queue->pulling == true);
	queue->pulling = false;
	
	queue->filled_buffers--;
	queue->dirty_buffers++;
	
	pthread_mutex_unlock(&queue->mutex);
	
	pthread_cond_signal(&queue->cond);
}

/**
  * Marks the queue as "drained". That is `queue_pull_begin` does no longer wait if the queue is
  * empty but returns `NULL` immediately.
  */
void queue_drained(queue_t queue)
{
	pthread_mutex_lock(&queue->mutex);
	queue->drained = true;
	pthread_mutex_unlock(&queue->mutex);
}

/**
  * Removes the "drained" marking from the queue. After that `queue_pull_begin` blocks again
  * if the queue is empty.
  */
void queue_refilled(queue_t queue)
{
	pthread_mutex_lock(&queue->mutex);
	queue->drained = false;
	pthread_mutex_unlock(&queue->mutex);
}
