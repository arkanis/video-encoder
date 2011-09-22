#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include "queue.h"

void* queue_allocator(){
	void* buffer = malloc(sizeof(int));
	printf("  thread %lx: allocating buffer %p\n", pthread_self(), buffer);
	return buffer;
}

int queue_deallocator(void *buffer){
	printf("  thread %lx: deallocating buffer %p\n", pthread_self(), buffer);
	free(buffer);
	return 0;
}

int queue_cleaner(void *buffer){
	printf("  thread %lx: cleaning buffer %p (%d)\n", pthread_self(), buffer, *((int*)buffer));
	return 0;
}


void single_thread_test()
{
	queue_t queue = queue_new(5, queue_allocator, queue_deallocator, queue_cleaner);
	
	for(int i = 0; i < 3; i++){
		printf("  pushing buffer %d\n", i);
		int* buffer = (int*) queue_push_begin(queue);
		*buffer = i;
		queue_push_end(queue);
	}
	
	for(int i = 0; i < 3; i++){
		printf("  pulling buffer %d\n", i);
		int* buffer = (int*) queue_pull_begin(queue);
		assert(*buffer == i);
		queue_pull_end(queue);
	}
	
	queue_destroy(queue);
}


void* simple_producer_func(void *data)
{
	queue_t queue = (queue_t) data;
	
	for(int i = 0; i < 7; i++){
		int* buffer = (int*) queue_push_begin(queue);
		printf("  thread %lx: producing buffer buffer %d\n", pthread_self(), i);
		*buffer = i;
		queue_push_end(queue);
	}
	
	queue_drained(queue);
}

void* simple_consumer_func(void *data)
{
	queue_t queue = (queue_t) data;
	int* buffer = NULL;
	
	while ( (buffer = (int*) queue_pull_begin(queue)) != NULL ){
		printf("  thread %lx: consuming buffer %d\n", pthread_self(), *buffer);
		queue_pull_end(queue);
	}
}

void multi_thread_test(size_t queue_size)
{
	queue_t queue = queue_new(queue_size, queue_allocator, queue_deallocator, queue_cleaner);
	
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
	
	for(int i = 0; i < 4; i++){
		int* buffer = (int*) queue_push_begin(queue);
		printf("  thread %lx: producing buffer buffer %d\n", pthread_self(), i);
		*buffer = i;
		queue_push_end(queue);
	}
	
	queue_drained(queue);
	queue_detach_producer(queue);
}

void* heandover_consumer_func(void *data)
{
	queue_t queue = (queue_t) data;
	int* buffer = NULL;
	
	for(int i = 0; i < 2; i++){
		printf("  thread %lx: number of drains: %d\n", pthread_self(), i);
		while ( (buffer = (int*) queue_pull_begin(queue)) != NULL ){
			printf("    thread %lx: consuming buffer %d\n", pthread_self(), *buffer);
			queue_pull_end(queue);
		}
	}
}

void multi_thread_handover_test()
{
	queue_t queue = queue_new(3, queue_allocator, queue_deallocator, queue_cleaner);
	
	pthread_t producer, consumer;
	pthread_create(&consumer, NULL, heandover_consumer_func, queue);
	
	pthread_create(&producer, NULL, heandover_producer_func, queue);
	pthread_join(producer, NULL);
	
	queue_attach_producer(queue, 2, queue_allocator, queue_deallocator, queue_cleaner);
	pthread_create(&producer, NULL, heandover_producer_func, queue);
	pthread_join(producer, NULL);
	
	pthread_join(consumer, NULL);
	queue_destroy(queue);
}

void main()
{
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