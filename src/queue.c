#include <assert.h>
#include <stdlib.h>

// Also include the mini queue functions
#define QUEUE_MINIQUEUE
#include "queue.h"

// Prototypes for heper functions
static void queue_deallocate_buffers(queue_t queue);
static void clean_dirty_buffers(queue_t queue);

/**
 * Creates a new detached queue. A consumer can pull from the queue immediately
 * and will wait until a producer pushed a buffer. Producers however first need to attach
 * themselfs to the queue before they can push buffers to it.
 */
queue_t queue_new()
{
	queue_t queue = malloc(sizeof(struct queue_s));
	assert(queue != NULL);
	
	// Set defaults
	*queue = (struct queue_s){
		.allocator = NULL, .deallocator = NULL, .cleaner = NULL,
		.free_head = NULL, .free_tail = NULL,
		.filled_head = NULL, .filled_tail = NULL,
		.dirty_head = NULL, .dirty_tail = NULL,
		.pushed = NULL, .pulled = NULL
	};
	
	// Setup threading primitives
	int err = pthread_mutex_init(&queue->mutex, NULL);
	assert(err == 0);
	
	err = pthread_cond_init(&queue->cond, NULL);
	assert(err == 0);
	
	return queue;
}

/**
 * If the queue is still attached to a producer it is first detached (this includes waiting for the
 * consumer to pull all remaining buffers). After that the queue and all bound resources are
 * destroyed.
 */
void queue_destroy(queue_t queue)
{
	if (queue->allocator != NULL)
		queue_detach_producer(queue);
	
	int err = pthread_cond_destroy(&queue->cond);
	assert(err == 0);
	err = pthread_mutex_destroy(&queue->mutex);
	assert(err == 0);
	
	free(queue);
}


/**
 * The producer takes possession of the queue by defining callbacks to handle buffers. After that
 * the producer can start to push buffers into the queue.
 */
void queue_attach_producer(queue_t queue, queue_buffer_allocator_t allocator, queue_buffer_cleaner_t cleaner, queue_buffer_deallocator_t deallocator)
{
	assert(allocator != NULL);
	assert(deallocator != NULL);
	assert(cleaner != NULL);
	
	pthread_mutex_lock(&queue->mutex);
	
	// Setup the queue callbacks
	queue->allocator = allocator;
	queue->deallocator = deallocator;
	queue->cleaner = cleaner;
	
	pthread_mutex_unlock(&queue->mutex);
}

/**
 * Waits until all buffers have been consumed and then destroys the queue but leaves the mutex
 * and conditional variable intact. The consumer waiting on the cond variable will continue to
 * wait while the queue can be handed over to a new producer.
 * 
 * To do this we clean up all the buffers (so they are disposed of in the original thread where
 * they were created) and reset the configuration.
 * 
 * `queue_detach_producer` is ment to be called by the dying thread that wants to heand
 * over its queue. The receiving thread then needs to call `queue_attach_producer` to
 * reinitialize the queue and can preallocate new buffers.
 */
void queue_detach_producer(queue_t queue)
{
	pthread_mutex_lock(&queue->mutex);
	
	// Wait until all buffers have been consumed
	while ( queue->filled_head != NULL )
		pthread_cond_wait(&queue->cond, &queue->mutex);
	
	// Cleanup and deallocate buffers
	queue_deallocate_buffers(queue);

	// Kill callbacks
	queue->allocator = NULL;
	queue->deallocator = NULL;
	queue->cleaner = NULL;
	
	pthread_mutex_unlock(&queue->mutex);
}


/**
 * Allocates `count` buffers of a specific `type`. This is only possible once after
 * attaching to the queue. These buffers are then reused until the producer detatches
 * itself or the queue is destroyed.
 */
void queue_preallocate_buffers(queue_t queue, size_t count, int type)
{
	assert(count > 0);
	
	pthread_mutex_lock(&queue->mutex);
	assert(queue->free_head == NULL);
	
	queue->preallocated_type = type;
	
	for(int i = 0; i < count; i++){
		queue_buffer_t *buffer = malloc(sizeof(queue_buffer_t));
		buffer->type = type;
		buffer->data = queue->allocator(type);
		miniqueue_push(&queue->free_head, &queue->free_tail, buffer);
	}
	
	pthread_mutex_unlock(&queue->mutex);
}

/**
 * Cleans up all dirty buffers and then frees all preallocated buffers.
 */
static void queue_deallocate_buffers(queue_t queue)
{
	clean_dirty_buffers(queue);
	
	while(queue->free_head != NULL){
		queue_buffer_t *buffer = miniqueue_pull(&queue->free_head, &queue->free_tail);
		queue->deallocator(buffer->type, buffer->data);
		free(buffer);
	}
}





/**
 * Returns an unused buffer from the queue. Note that this buffer might contain old data from previous usage.
 * If no unused buffer is available right now the function will block until a buffer is freed by another thread using
 * `queue_pull_end`.
 */
void* queue_push_begin(queue_t queue, int type)
{
	pthread_mutex_lock(&queue->mutex);
	
	assert(queue->pushed == NULL);
	
	clean_dirty_buffers(queue);
	
	queue_buffer_t *buffer = NULL;
	if (queue->preallocated_type == type) {
		// If we preallocated buffer of that type wait until a preallocated buffer is free and pull it
		// of the free queue
		while( queue->free_head == NULL ){
			pthread_cond_wait(&queue->cond, &queue->mutex);
			clean_dirty_buffers(queue);
		}
		buffer = miniqueue_pull(&queue->free_head, &queue->free_tail);
	} else {
		// No preallocated buffer available, allocate one
		buffer = malloc(sizeof(queue_buffer_t));
		buffer->type = type;
		buffer->data = queue->allocator(type);
	}
	
	// We need to remember the pushed buffer because we only return the `data` part of the buffer.
	// Without the remembered buffer we don't know what we should put on the filled list on `queue_push_end`.
	// Remember the pushed buffer so we know 
	queue->pushed = buffer;
	void *data = buffer->data;
	
	pthread_mutex_unlock(&queue->mutex);
	
	return data;
}

int queue_push_end(queue_t queue)
{
	pthread_mutex_lock(&queue->mutex);
	
	assert(queue->pushed != NULL);
	miniqueue_push(&queue->filled_head, &queue->filled_tail, queue->pushed);
	queue->pushed = NULL;
	
	pthread_mutex_unlock(&queue->mutex);
	pthread_cond_signal(&queue->cond);
}


/**
 * Pulls a buffer from the queue. This buffer can then be used until `queue_pull_end` is called which gives
 * it back to the producer thread for cleanup and deallocation. The type of the pulled buffer is written into
 * the variable pointed to by the `type` argument. `type` can be `NULL` if the type of the buffer is of no
 * interest.
 */
void* queue_pull_begin(queue_t queue, int *type)
{
	pthread_mutex_lock(&queue->mutex);
	
	assert(queue->pulled == NULL);
	
	while ( queue->filled_head == NULL )
		pthread_cond_wait(&queue->cond, &queue->mutex);
	
	queue->pulled = miniqueue_pull(&queue->filled_head, &queue->filled_tail);
	assert(queue->pulled != NULL);
	void* data = queue->pulled->data;
	if (type != NULL)
		*type = queue->pulled->type;
	
	pthread_mutex_unlock(&queue->mutex);
	
	return data;
}

int queue_pull_end(queue_t queue)
{
	pthread_mutex_lock(&queue->mutex);
	
	assert(queue->pulled != NULL);
	miniqueue_push(&queue->dirty_head, &queue->dirty_tail, queue->pulled);
	queue->pulled = NULL;
	
	pthread_mutex_unlock(&queue->mutex);
	pthread_cond_signal(&queue->cond);
}














/**
 * Cleans all dirty buffers currently in the queue. This function requires that the queue
 * mutex is locked. Otherwise race conditions can easily happen.
 * 
 * Preallocated buffers are put into the free queue after they have been cleaned. Other
 * buffers are given right to the deallocator.
 */
static void clean_dirty_buffers(queue_t queue)
{
	while( queue->dirty_head != NULL ){
		queue_buffer_t *buffer = miniqueue_pull(&queue->dirty_head, &queue->dirty_tail);
		queue->cleaner(buffer->type, buffer->data);
		if (buffer->type != queue->preallocated_type) {
			queue->deallocator(buffer->type, buffer->data);
			free(buffer);
		} else {
			miniqueue_push(&queue->free_head, &queue->free_tail, buffer);
		}
	}
}


/**
 * Pushes (appends) a new `buffer` to the mini queue represented by `head` and `tail`.
 * The tail (and maybe head) of the mini queue are changed when pushing a buffer.
 */
void miniqueue_push(queue_buffer_t **head, queue_buffer_t **tail, queue_buffer_t *buffer)
{
	assert(buffer != NULL);
	
	if (*tail != NULL)
		(*tail)->next = buffer;
	else
		*head = buffer;
	*tail = buffer;
	buffer->next = NULL;
}

/**
 * Pulls a buffer out of the mini queue represented by `head` and `tail` and returns
 * the pointer to a buffer struct. The head (and maybe the tail) are changed when
 * pulling a buffer.
 */
queue_buffer_t* miniqueue_pull(queue_buffer_t **head, queue_buffer_t **tail)
{
	assert(*head != NULL);
	
	queue_buffer_t *buffer = *head;
	*head = buffer->next;
	if (buffer->next == NULL)
		*tail = NULL;
	return buffer;
}