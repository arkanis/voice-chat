GCC_FLAGS = -g -std=gnu99 -Wall -Iopus/include
LINKER_ARGS = opus/.libs/libopus.a -lm -lpulse-simple -lpulse

all: server client

server: server.c proto.h opus
	gcc $(GCC_FLAGS) server.c -o server $(LINKER_ARGS)

client: client.c proto.h opus
	gcc -pthread $(GCC_FLAGS) client.c -o client $(LINKER_ARGS)

threaded_pa: threaded_pa.c
	gcc -pthread $(GCC_FLAGS) threaded_pa.c -o threaded_pa -lpulse-simple

clean:
	rm -f server client threaded_pa


deps: opus

ubuntu_packages:
	sudo apt-get install autoconf libtool

opus:
	git clone --depth 0 git://git.opus-codec.org/opus.git
	cd opus; ./autogen.sh
	cd opus; ./configure
	cd opus; make -j 4

clean_deps:
	rm -rf opus


test_listener:
	./client | pacat --latency-msec 5 --rate 48000

test_mic:
	parec --latency-msec 500 --rate 48000 | MALLOC_CHECK_=3 PULSE_PROP=filter.want=echo-cancel ./client

test_client:
	parec --latency-msec 5 --rate 48000 | ./client | pacat --latency-msec 5 --rate 48000