#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>

#define QUEUE_MINIQUEUE
#include "queue.h"

void* queue_allocator(int type){
	void* buffer = malloc(sizeof(int));
	printf("  thread %lx: allocating buffer %p, type: %x\n", pthread_self(), buffer, type);
	return buffer;
}

void queue_cleaner(int type, void *buffer){
	printf("  thread %lx: cleaning buffer %p, type: %x, content: %d\n", pthread_self(), buffer, type, *((int*)buffer));
}

void queue_deallocator(int type, void *buffer){
	printf("  thread %lx: deallocating buffer %p, type: %x, content: %d\n", pthread_self(), buffer, type, *((int*) buffer));
	free(buffer);
}


void miniqueue_test()
{
	queue_buffer_t *head = NULL, *tail = NULL;
	queue_buffer_t buffers[3];
	
	miniqueue_push(&head, &tail, &buffers[0]);
	assert(head == &buffers[0]);
	assert(tail == &buffers[0]);
	assert(head->next == NULL);
	
	miniqueue_push(&head, &tail, &buffers[1]);
	assert(head == &buffers[0]);
	assert(tail == &buffers[1]);
	assert(head->next == &buffers[1]);
	assert(tail->next == NULL);
	
	miniqueue_push(&head, &tail, &buffers[2]);
	assert(head == &buffers[0]);
	assert(tail == &buffers[2]);
	assert(head->next == &buffers[1]);
	assert(tail->next == NULL);
	
	queue_buffer_t* pulled = miniqueue_pull(&head, &tail);
	assert(pulled == &buffers[0]);
	assert(head == &buffers[1]);
	assert(tail == &buffers[2]);
	
	pulled = miniqueue_pull(&head, &tail);
	assert(pulled == &buffers[1]);
	assert(head == &buffers[2]);
	assert(tail == &buffers[2]);
	
	pulled = miniqueue_pull(&head, &tail);
	assert(pulled == &buffers[2]);
	assert(head == NULL);
	assert(tail == NULL);
}


void single_thread_test()
{
	queue_t queue = queue_new();
	queue_attach_producer(queue, queue_allocator, queue_cleaner, queue_deallocator);
	queue_preallocate_buffers(queue, 5, 'test');
	
	for(int i = 0; i < 3; i++){
		printf("  pushing buffer %d\n", i);
		int* buffer = (int*) queue_push_begin(queue, 'test');
		*buffer = i;
		queue_push_end(queue);
	}
	
	for(int i = 0; i < 3; i++){
		printf("  pulling buffer %d\n", i);
		int* buffer = (int*) queue_pull_begin(queue, NULL);
		assert(*buffer == i);
		queue_pull_end(queue);
	}
	
	queue_destroy(queue);
}


// A dirty global variable to give the producer thread more data in an easy way
size_t queue_size;

void* simple_producer_func(void *data)
{
	queue_t queue = (queue_t) data;
	
	queue_attach_producer(queue, queue_allocator, queue_cleaner, queue_deallocator);
	queue_preallocate_buffers(queue, queue_size, 'test');
	
	for(int i = 0; i < 7; i++){
		int* buffer = (int*) queue_push_begin(queue, 'test');
		printf("  thread %lx: producing buffer %d\n", pthread_self(), i);
		*buffer = i;
		queue_push_end(queue);
	}
	
	queue_push_begin(queue, 'stop');
	printf("  thread %lx: producing stop buffer\n", pthread_self());
	queue_push_end(queue);
}

void* simple_consumer_func(void *data)
{
	queue_t queue = (queue_t) data;
	int* buffer = NULL;
	int type = 0;
	
	
	do {
		buffer = (int*) queue_pull_begin(queue, &type);
		if (type == 'test')
			printf("  thread %lx: consuming buffer %d\n", pthread_self(), *buffer);
		else
			printf("  thread %lx: consuming stop buffer\n", pthread_self());
		queue_pull_end(queue);
	} while(type != 'stop');
}

void multi_thread_test(size_t size)
{
	queue_t queue = queue_new();
	queue_size = size;
	
	pthread_t producer, consumer;
	pthread_create(&producer, NULL, simple_producer_func, queue);
	pthread_create(&consumer, NULL, simple_consumer_func, queue);
	
	pthread_join(producer, NULL);
	pthread_join(consumer, NULL);
	
	queue_destroy(queue);
}


void* heandover_producer_func(void *data)
{
	queue_t queue = (queue_t) data;
	
	printf("  thread %lx: attaching as producer\n", pthread_self());
	queue_attach_producer(queue, queue_allocator, queue_cleaner, queue_deallocator);
	queue_preallocate_buffers(queue, 3, 'test');
	
	for(int i = 0; i < 4; i++){
		int* buffer = (int*) queue_push_begin(queue, 'test');
		printf("  thread %lx: producing buffer %d\n", pthread_self(), i);
		*buffer = i;
		queue_push_end(queue);
	}
	
	queue_push_begin(queue, 'stop');
	printf("  thread %lx: producing stop buffer\n", pthread_self());
	queue_push_end(queue);
	
	queue_detach_producer(queue);
}

void* heandover_consumer_func(void *data)
{
	queue_t queue = (queue_t) data;
	int* buffer = NULL;
	int type;
	
	for(int i = 0; i < 2; i++){
		printf("    thread %lx: received stop buffers: %d\n", pthread_self(), i);
		do {
			buffer = (int*) queue_pull_begin(queue, &type);
			if (type == 'test')
				printf("    thread %lx: consuming buffer %d\n", pthread_self(), *buffer);
			else
				printf("    thread %lx: consuming stop buffer\n", pthread_self());
			queue_pull_end(queue);
		} while(type != 'stop');
	}
}

void multi_thread_handover_test()
{
	queue_t queue = queue_new();
	
	pthread_t producer, consumer;
	pthread_create(&consumer, NULL, heandover_consumer_func, queue);
	
	pthread_create(&producer, NULL, heandover_producer_func, queue);
	pthread_join(producer, NULL);
	
	pthread_create(&producer, NULL, heandover_producer_func, queue);
	pthread_join(producer, NULL);
	
	pthread_join(consumer, NULL);
	queue_destroy(queue);
}


void main()
{
	printf("running miniqueue_test...\n");
	miniqueue_test();
	
	printf("running single_thread_test...\n");
	single_thread_test();
	
	printf("running multi_thread_test with 5 buffers...\n");
	multi_thread_test(5);
	
	// This simulates the usage of `av_read_frame` in FFMPEG where the filled
	// packet can only be used until the next call of `av_read_frame`.
	printf("running multi_thread_test with 1 buffer...\n");
	multi_thread_test(1);
	
	printf("running multi_thread_handover_test...\n");
	multi_thread_handover_test();
}