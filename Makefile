CC=gcc
LIBS=-lusb -lconfig -lcurl -ljson-c

sswixelbridge: sswixelbridge.o
	$(CC) -o sswixelbridge sswixelbridge.c $(LIBS)

clean:
	rm -f *.o sswixelbridge
