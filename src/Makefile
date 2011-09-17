CFLAGS = --std=c99 -g

# Some support stuff and test programms
queue.o: queue.c queue.h
	gcc $(CFLAGS) queue.c -c -o queue.o

queue_test: queue_test.c queue.o
	gcc $(CFLAGS) queue_test.c queue.o -o queue_test -pthread

clean:
	rm -f *.o