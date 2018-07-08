CC=gcc
LIBS=-lusb -lconfig -lcurl -ljson-c

sswixelbridge: sswixelbridge.o
	$(CC) -o sswixelbridge sswixelbridge.c $(LIBS)

clean:
	rm -f *.o sswixelbridge

.PHONY: install
install: sswixelbridge
	cp sswixelbridge /usr/sbin/sswixelbridge
	mkdir -p /etc/sswixelbridge
	cp sswixelbridge.cfg /etc/sswixelbridge/sswixelbridge.conf
	mkdir -p /etc/systemd/system
	cp sswixelbridge.service /etc/systemd/system/sswixelbridge.service

.PHONY: uninstall
uninstall:
	rm -f /usr/sbin/sswixelbridge
	rm -f /etc/systemd/system/sswixelbridge
	rm -rf /etc/sswixelbridge
