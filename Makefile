CC = gcc 
CLAGS = -Wall -g 

all: oss worker 

oss: oss.c
	$(CC) $(CFLAGS) -o oss oss.c

worker: worker.c 
	$(CC) $(CFLAGS) -o worker worker.c

clean: 
	rm -f oss worker
