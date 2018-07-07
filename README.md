# Sokeriseuranta Wixel Bridge
Use Raspberry Pi + Wixel to read data from Dexcom G4 transmitter and send values to Sokeriseuranta wep app.

## What you need

Basically any Linux box should do just fine. For pratical reasons using a mini-sized computer such as Raspberry Pi is recommended. Tested on Raspberry Pi model 3B and standard Raspbian OS.

## How to compile
```
gcc sswixelbridge.c -lusb -lconfig -lcurl -ljson-c -o sswixelbridge
```

You'll need libusb, libconfig, curl, libjson-c, and some standard development packages. Please not that the code is POSIX C, *not* ANSI C. It has been tested only with GNU gcc compiler.

## Installing binary packages

Binary distribution for Raspbian based systems will be available later.
