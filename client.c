#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <poll.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <pulse/simple.h>
#include <pulse/error.h>

#include <opus.h>
#include "proto.h"


typedef struct {
	char *host;
	char *port;
	
	uint32_t sample_rate;  // in Hz
	uint8_t channel_count;  // 1 or 2
	uint16_t frame_duration;  // in 0.1 ms units, 25 (2.5ms), 50, 100, 200, 400, 600
	
	int input_fd, output_fd;
	
	size_t frame_samples_per_channel;
	size_t frame_size;  // in bytes
} options_t, *options_p;

options_t opts;

void parse_options(int argc, char **argv, options_p opts);
void show_usage_and_exit(char *program_name);
void notice(const char *format, ...);
void error(const char *format, ...);
void die(int status, const char *format, ...);
void pdie(int status, const char *message);


//
// Output functions
//

void notice(const char *format, ...){
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

void error(const char *format, ...){
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

void die(int status, const char *format, ...){
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	exit(status);
}

void pdie(int status, const char *message){
	perror(message);
	exit(status);
}

//
// Argument parsing stuff
//

void parse_options(int argc, char **argv, options_p opts){
	// Set default options
	*opts = (options_t){
		.host = NULL, .port = "61234",
		.sample_rate = 48000,
		.channel_count = 2,
		.frame_duration = 100,
		.input_fd = -1, .output_fd = -1
	};
	
	// Parse the arguments
	int opt_char;
	struct option longopts[] = {
		{"input", required_argument, NULL, 'i'},
		{"output", required_argument, NULL, 'o'},
		{"sample-rate", required_argument, NULL, 'r'},
		{"channels", required_argument, NULL, 'c'},
		{"frame-duration", required_argument, NULL, 'd'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0}
	};
	while( (opt_char = getopt_long(argc, argv, "i:o:r:c:d:h", longopts, NULL)) != -1 ){
		switch(opt_char){
			case 'i':
				if (strcmp(optarg, "-") == 0) {
					opts->input_fd = STDIN_FILENO;
				} else {
					opts->input_fd = open(optarg, O_RDONLY);
					if (opts->input_fd == -1)
						pdie(1, "Could not open input file");
				}
				break;
			case 'o':
				if (strcmp(optarg, "-") == 0) {
					opts->output_fd = STDOUT_FILENO;
				} else {
					opts->output_fd = open(optarg, O_RDONLY);
					if (opts->output_fd == -1)
						pdie(1, "Could not open input file");
				}
				break;
			case 'r':
				opts->sample_rate = strtol(optarg, NULL, 10);
				switch(opts->sample_rate){
					case 8000: case 12000: case 16000: case 24000: case 48000:
						// These values are ok
						break;
					default:
						die(1, "The sample rate %u is not supported, only 8000, 12000, 16000, 24000 or 48000 work\n", opts->sample_rate);
						break;
				}
				break;
			case 'c':
				opts->channel_count = strtol(optarg, NULL, 10);
				switch(opts->channel_count){
					case 1: case 2:
						// These values are ok
						break;
					default:
						die(1, "Only mono (channel count of 1) and stereo (2) are supported\n");
						break;
				}
				break;
			case 'd':
				if ( strcmp(optarg, "2.5") == 0 )
					opts->frame_duration = 25;
				else
					opts->frame_duration = strtol(optarg, NULL, 10) * 10;
				switch(opts->frame_duration){
					case 25: case 50: case 100: case 200: case 400: case 600:
						// These values are ok
						break;
					default:
						die(1, "Only the following frame durations are supported: 2.5, 5, 10, 20, 40 or 60 ms\n");
						break;
				}
				break;
			case '?': case 'h':
				show_usage_and_exit(argv[0]);
				break;
		}
	}
	
	// After option parsing we're at the host:port argument
	if (optind >= argc)
		show_usage_and_exit(argv[0]);
	
	char *colon = strchr(argv[optind], ':');
	if (colon != NULL){
		opts->host = strndup(argv[optind], colon - argv[optind]);
		opts->port = strdup(colon + 1);
	} else {
		opts->host = argv[optind];
	}
	
	// Calculate derived values
	opts->frame_samples_per_channel = (opts->sample_rate * opts->frame_duration) / 10000LL;
	opts->frame_size = opts->channel_count * opts->frame_samples_per_channel * sizeof(int16_t);
	
	// Print options
	notice("Options:\n"
		"  host: %s, port: %s\n"
		"  sample_rate: %u, channel_count: %hhu, frame_duration: %.1f\n"
		"  input_fd: %d, output_fd %d\n"
		"  frame_samples_per_channel: %zu, frame_size: %zu\n",
		opts->host, opts->port,
		opts->sample_rate, opts->channel_count, opts->frame_duration / 10.0,
		opts->input_fd, opts->output_fd,
		opts->frame_samples_per_channel, opts->frame_size
	);
}

void show_usage_and_exit(char *program_name){
	die(1,
		"%s [-i file] [-o file]\n"
		"    [-r sampe-rate] [-c channels] [-d frame-duration]\n"
		"    [-h help]\n"
		"    host[:port]\n",
		program_name
	);
}


//
// Recording functions
//

void* recording_thread(void *pipe_out_fd_p){
	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	pthread_sigmask(SIG_BLOCK, &sigs, NULL);
	
	int recording_pipe_in = *((int*)pipe_out_fd_p);
	
	const pa_sample_spec ss = {
		.format = PA_SAMPLE_S16LE,
		.rate = opts.sample_rate,
		.channels = opts.channel_count
	};
	pa_simple *pa = NULL;
	int pa_error;
	
	if ( !(pa = pa_simple_new(NULL, "arkanis voice chat", PA_STREAM_RECORD, NULL, "arkanis voice chat", &ss, NULL, NULL, &pa_error)) )
		die(2, "pa_simple_new() failed: %s\n", pa_strerror(pa_error));
	
	notice("Recording thread started...\n");
	
	uint8_t *buffer = malloc(opts.frame_size);
	while (true) {
		if ( pa_simple_read(pa, buffer, opts.frame_size, &pa_error) < 0 ){
			error("pa_simple_read() failed: %s\n", pa_strerror(pa_error));
			break;
		}
		
		size_t bytes_written = 0;
		while(bytes_written < opts.frame_size){
			ssize_t written = write(recording_pipe_in, buffer + bytes_written, opts.frame_size - bytes_written);
			if (written < 0){
				perror("write() failed");
				break;
			}
			bytes_written += written;
		}
	}
	
	free(buffer);
	free(pipe_out_fd_p);
	pa_simple_free(pa);
	
	return NULL;
}

int startup_recording_thread(){
	int fds[2];
	pthread_t thread;
	
	if ( pipe(fds) == -1 )
		pdie(2, "pipe() failed");
	
	int *pipe_in_fd_p = malloc(sizeof(int));
	*pipe_in_fd_p = fds[1];
	if ( pthread_create(&thread, NULL, recording_thread, pipe_in_fd_p) != 0 )
		die(2, "Failed to create recording thread\n");
	
	return fds[0];
}


//
// Playback functions
//

void* playback_thread(void *pipe_out_fd_p){
	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	pthread_sigmask(SIG_BLOCK, &sigs, NULL);
	
	int playback_pipe_out = *((int*)pipe_out_fd_p);
	
	const pa_sample_spec ss = {
		.format = PA_SAMPLE_S16LE,
		.rate = opts.sample_rate,
		.channels = opts.channel_count
	};
	pa_simple *pa = NULL;
	int pa_error;
	
	if ( !(pa = pa_simple_new(NULL, "arkanis voice chat", PA_STREAM_PLAYBACK, NULL, "arkanis voice chat", &ss, NULL, NULL, &pa_error)) )
		die(2, "pa_simple_new() failed: %s\n", pa_strerror(pa_error));
	
	notice("Playback thread started...\n");
	
	uint8_t *buffer = malloc(opts.frame_size);
	while (true) {
		ssize_t bytes_read = read(playback_pipe_out, buffer, opts.frame_size);
		if (bytes_read == 0)
			break;
		if (bytes_read < 0){
			perror("read() failed");
			break;
		}
		
		if (pa_simple_write(pa, buffer, bytes_read, &pa_error) < 0){
			error("pa_simple_write() failed: %s\n", pa_strerror(pa_error));
			break;
		}
	}
	
	free(buffer);
	free(pipe_out_fd_p);
	pa_simple_free(pa);
	
	return NULL;
}

int startup_playback_thread(){
	int fds[2];
	pthread_t thread;
	
	if ( pipe(fds) == -1 )
		pdie(2, "pipe() failed");
	
	int *pipe_out_fd_p = malloc(sizeof(int));
	*pipe_out_fd_p = fds[0];
	if ( pthread_create(&thread, NULL, playback_thread, pipe_out_fd_p) != 0 )
		die(2, "Failed to create playback thread\n");
	
	return fds[1];
}


//
// Signal handling stuff
//
bool quit = false;

void sigint_handler(int signum){
	quit = true;
}

void establish_signal_handlers(){
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = sigint_handler;
	action.sa_flags = SA_RESTART;
	if ( sigaction(SIGINT, &action, NULL) == -1 )
		perror("sigaction() failed");
}

void log_print(const char *format, ...){
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}


int main(int argc, char **argv){
	parse_options(argc, argv, &opts);
	establish_signal_handlers();
	
	if (opts.input_fd == -1)
		opts.input_fd = startup_recording_thread();
	if (opts.output_fd == -1)
		opts.output_fd = startup_playback_thread();
	
	/*
	// read audio data in 48 kHz and s16ne
	const size_t freq = 48000; // in Hz
	const size_t frame_dur = 10; // in ms, 2.5 (won't work), 5, 10, 20, 40, 60
	const size_t frame_samples = (freq * frame_dur) / 1000;
	const size_t channel_count = 2;
	log_print("%zu samples per frame, %zu channels\n", frame_samples, channel_count);
	*/
	
	// Allocate frame buffers
	int16_t *in_frame = malloc(opts.frame_size);
	int16_t *out_frame = malloc(opts.frame_size);
	
	// Init Opus encoder and decoder
	int error_code = 0;
	OpusEncoder *enc;
	enc = opus_encoder_create(opts.sample_rate, opts.channel_count, OPUS_APPLICATION_VOIP, &error_code);
	assert(error_code == OPUS_OK);
	
	OpusDecoder *dec;
 	dec = opus_decoder_create(opts.sample_rate, opts.channel_count, &error_code);
 	assert(error_code == OPUS_OK);
	
	// Search for the server
	struct addrinfo hints = {0};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	struct addrinfo *addr_info;
	error_code = getaddrinfo(opts.host, opts.port, &hints, &addr_info);
	if (error_code != 0)
		die(3, "getaddrinfo failed: %s\n", gai_strerror(error_code));
	
	// Remember the first matching IP address
	struct sockaddr_in server_addr;
	memcpy(&server_addr, addr_info->ai_addr, sizeof(server_addr));
	
	// Show all found addresses... just some fun for debugging
	size_t host_len = 1024, service_len = 1024;
	char host[host_len], service[service_len];
	for (struct addrinfo * cur_addr_info = addr_info; cur_addr_info != NULL; cur_addr_info = cur_addr_info->ai_next){
		getnameinfo(cur_addr_info->ai_addr, cur_addr_info->ai_addrlen, host, host_len, service, service_len, NI_DGRAM | NI_NUMERICHOST);
		notice("%s:%s\n", host, service);
	}
	
	// Free the entire address list
	freeaddrinfo(addr_info);
	
	
	// Create a local socket and bind to some free port
	int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (client_fd == -1)
		pdie(3, "socket");
	
	struct sockaddr_in addr = (struct sockaddr_in){ AF_INET, 0, .sin_addr = { INADDR_ANY } };
	if (bind(client_fd, (const struct sockaddr *)&addr, sizeof(addr)) == -1)
		pdie(3, "bind");
	
	
	packet_t packet;
	ssize_t bytes_send, bytes_received;
	
	// Do connection setup
	packet = (packet_t){ PACKET_HELLO };
	bytes_send = sendto(client_fd, &packet, offsetof(packet_t, user), 0, (const struct sockaddr *)&server_addr, sizeof(server_addr));
	if (bytes_send == -1)
		perror("sendto");
	
	uint8_t user_id = 0;
	do {
		bytes_received = recvfrom(client_fd, &packet, sizeof(packet), 0, NULL, NULL);
	} while(packet.type != PACKET_WELCOME);
	user_id = packet.user;
	notice("Welcome from server, you're client %hhu\n", packet.user);
	
	
	void conceal_loss(){
		int decoded_samples = opus_decode(dec, NULL, 0, out_frame, opts.frame_samples_per_channel, 0);
		if (decoded_samples < 0)
			error("opus_decode error: %d\n", opus_decode);
		else
			write(opts.output_fd, out_frame, decoded_samples * opts.channel_count * sizeof(int16_t));
	}
	
	size_t frame_filled = 0;
	uint16_t send_seq = 0;
	uint16_t recv_seq = 0;
	while(!quit){
		// Read and receive stuff
		struct pollfd pollfds[2] = {
			(struct pollfd){ client_fd, POLLIN },
			(struct pollfd){ opts.input_fd, POLLIN }
		};
		error_code = poll(pollfds, 2, -1);
		if (error_code == -1){
			perror("poll");
			continue;
		}
		
		if (pollfds[0].revents & POLLIN){
			// Ready to receive packet from the server
			bytes_received = recvfrom(client_fd, &packet, sizeof(packet), 0, NULL, NULL);
			size_t data_len = bytes_received - offsetof(packet_t, data);
			//log_print("received packet type %hhu, %zu data bytes\n", packet.type, data_len);
			
			if (packet.type == PACKET_DATA) {
				if (data_len != packet.len){
					log_print("incomplete packet, expected %hu, got %zu\n", packet.len, data_len);
					conceal_loss();
				}
				
				log_print("packet seq: %hu, cur seq: %hu\n", packet.seq, recv_seq);
				/*
				//if (packet.seq == 0)
				//	recv_seq == 0;
				if (recv_seq == UINT16_MAX) {
					// Init seq number
					//recv_seq = packet.seq;
				}
				
				uint16_t lost = packet.seq - recv_seq;
				if (lost == 0) {
					// received expected seq, everthing is fine, expect next packet
					recv_seq++;
				} else if (lost < UINT16_MAX / 2) {
					// We use the range [1, UINT16_MAX / 2] for normal packet loss
					log_print("packet loss, last known seq: %hu, packet seq: %hu, lost: %hu\n",
						recv_seq, packet.seq, lost);
					for(size_t i = 0; i < lost; i++)
						conceal_loss();
					recv_seq = packet.seq;
				} else {
					// lost was actually negative and warped around. We use the range
					// [UINT16_MAX / 2, UINT16_MAX] to capture this.
					log_print("old (out of order) packet from seq %hu, curren seq: %hu, age: %hu\n",
						packet.seq, recv_seq, UINT16_MAX - lost);
					continue;
				}
				*/
				int decoded_samples = opus_decode(dec, packet.data, data_len, out_frame, opts.frame_samples_per_channel, 0);
				if (decoded_samples < 0)
					log_print("opus_decode error: %d\n", opus_decode);
				else
					write(opts.output_fd, out_frame, decoded_samples * opts.channel_count * sizeof(int16_t));
			} else if (packet.type == PACKET_JOIN) {
				recv_seq = 0;
				log_print("user %hhu joined\n", packet.user);
			} else if (packet.type == PACKET_BYE) {
				log_print("user %hhu disconnected\n", packet.user);
			} else {
				log_print("unknown packet, type %hhu, %zu bytes data\n", packet.type, data_len);
			}
		}
		
		if (pollfds[1].revents & POLLIN){
			// Audio data from input fd ready to read
			ssize_t bytes_read = read(opts.input_fd, in_frame + frame_filled, opts.frame_size - frame_filled);
			if (bytes_read == -1){
				perror("read");
				continue;
			}
			
			frame_filled += bytes_read;
			if (frame_filled >= opts.frame_size){
				packet = (packet_t){ PACKET_DATA, user_id, send_seq };
				int32_t len = opus_encode(enc, in_frame, opts.frame_samples_per_channel, packet.data, sizeof(packet) - offsetof(packet_t, data));
				frame_filled -= opts.frame_size;
				
				if (len < 0)
					log_print("opus_encode error!\n");
				else if (len == 1)
					continue;
				
				packet.len = len;
				bytes_send = sendto(client_fd, &packet, offsetof(packet_t, data) + len, 0, (const struct sockaddr *)&server_addr, sizeof(server_addr));
				if (bytes_send < 0)
					perror("sendto");
				
				log_print("send %zd bytes\n", bytes_send);
				send_seq++;
			}
			
		}
	}
	
	log_print("exiting...\n");
	packet = (packet_t){ PACKET_BYE, user_id };
	bytes_send = sendto(client_fd, &packet, offsetof(packet_t, seq), 0, (const struct sockaddr *)&server_addr, sizeof(server_addr));
	if (bytes_send == -1)
		perror("sendto");
	
	opus_encoder_destroy(enc);
	
	free(in_frame);
	free(out_frame);
	close(client_fd);
}