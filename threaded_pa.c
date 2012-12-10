#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

#include <pulse/simple.h>
#include <pulse/error.h>

/*

The core idea here is to avoid the complex Pulse Audio API. Looks like it can not be integrated
into a poll system call anyway so why bother? We use two instances of the simple API to write to
and read from pipes. These pipes in turn are then used with poll to see if data is available.

Pulse Audio is not really meant for this. The functions are thread save but the objects are not.
Therefore each thread uses its own stuff and no Pulse Audio objects are shared.

*/

static const pa_sample_spec ss = {
	.format = PA_SAMPLE_S16LE,
	.rate = 48000,
	.channels = 2
};

int pipe_in, pipe_out;


void* recording_thread(void *data){
	printf("recording thread\n");
	
	pa_simple *pa = NULL;
	int error;
	
	if (!(pa = pa_simple_new(NULL, "recorder", PA_STREAM_RECORD, NULL, "recorder for voice chat", &ss, NULL, NULL, &error))) {
		fprintf(stderr, "pa_simple_new() failed: %s\n", pa_strerror(error));
		goto finish;
	}
	
	
	uint8_t buf[4800 * 2 * 2];
	size_t buf_size = sizeof(buf);
	while (true) {
		if (pa_simple_read(pa, buf, buf_size, &error) < 0) {
			fprintf(stderr, __FILE__": pa_simple_read() failed: %s\n", pa_strerror(error));
			goto finish;
		}
		
		size_t bytes_written = 0;
		while(bytes_written < buf_size){
			ssize_t written = write(pipe_in, buf + bytes_written, buf_size - bytes_written);
			if (written < 0){
				perror("write failed");
				break;
			}
			bytes_written += written;
		}
	}
	
	finish:
	if (pa)
		pa_simple_free(pa);
	
	return NULL;
}

void* playback_thread(void *data){
	printf("playback thread\n");
	
	pa_simple *pa = NULL;
	int error;
	
	if (!(pa = pa_simple_new(NULL, "player", PA_STREAM_PLAYBACK, NULL, "player for voice chat", &ss, NULL, NULL, &error))) {
		fprintf(stderr, "pa_simple_new() failed: %s\n", pa_strerror(error));
		goto finish;
	}
	
	
	uint8_t buf[4800 * 2 * 2];
	size_t buf_size = sizeof(buf);
	while (true) {
		ssize_t bytes_read = read(pipe_out, buf, buf_size);
		if (bytes_read == 0)
			break;
		if (bytes_read < 0){
			perror("read failed");
			break;
		}
		
		if (pa_simple_write(pa, buf, bytes_read, &error) < 0) {
			fprintf(stderr, __FILE__": pa_simple_write() failed: %s\n", pa_strerror(error));
			goto finish;
		}
	}
	
	finish:
	if (pa)
		pa_simple_free(pa);
	
	return NULL;
}

int main(int argc, char **argv){
	int fds[2];
	if (pipe(fds) == -1){
		perror("pipe failed");
		return 1;
	}
	pipe_out = fds[0];
	pipe_in = fds[1];
	
	
	pthread_t ta, tb;
	if ( pthread_create(&ta, NULL, recording_thread, NULL) != 0 )
		fprintf(stderr, "failed to create recording thread\n");
	if ( pthread_create(&tb, NULL, playback_thread, NULL) != 0 )
		fprintf(stderr, "failed to create playback thread\n");
	printf("main thread\n");
	
	if ( pthread_join(ta, NULL) != 0 )
		fprintf(stderr, "failed to wait to recording thread\n");
	if ( pthread_join(tb, NULL) != 0 )
		fprintf(stderr, "failed to wait to playback thread\n");
	
	return 0;
}