CFLAGS = --std=c99 -g

# Some support stuff and test programms
queue.o: queue.c queue.h
	gcc $(CFLAGS) queue.c -c -o queue.o

queue_test: queue_test.c queue.o
	gcc $(CFLAGS) queue_test.c queue.o -o queue_test -pthread

logger.o: logger.c logger.h
	gcc $(CFLAGS) logger.c -c -o logger.o

logger_test: logger_test.c logger.o
	gcc $(CFLAGS) logger_test.c logger.o -o logger_test

clean:
	rm -f *.o