#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>


#include <opus.h>

#include "proto.h"


bool quit = false;

void sig_handler(int signum){
	quit = true;
}

void log_print(const char *format, ...){
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}


int main(int argc, char **argv){
	int error = 0;
	
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = sig_handler;
	action.sa_flags = SA_RESTART;
	if ( sigaction(SIGINT, &action, NULL) == -1 )
		perror("sigaction");
	
	// read audio data in 48 kHz and s16ne
	const size_t freq = 48000; // in Hz
	const size_t frame_dur = 10; // in ms, 2.5 (won't work), 5, 10, 20, 40, 60
	const size_t frame_samples = (freq * frame_dur) / 1000;
	const size_t channel_count = 2;
	log_print("%zu samples per frame, %zu channels\n", frame_samples, channel_count);
	
	// Allocate frame buffer
	const size_t frame_size = channel_count * frame_samples * sizeof(int16_t);
	int16_t *frame = malloc(frame_size);
	int16_t *out_frame = malloc(frame_size);
	
	
	// Init encoder and decoder
	OpusEncoder *enc;
	enc = opus_encoder_create(freq, channel_count, OPUS_APPLICATION_VOIP, &error);
	assert(error == OPUS_OK);
	
	OpusDecoder *dec;
 	dec = opus_decoder_create(freq, channel_count, &error);
 	assert(error == OPUS_OK);
	
	
	// Search for the server
	struct addrinfo hints = {0};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	struct addrinfo *addr_info;
	error = getaddrinfo("localhost", "61234", &hints, &addr_info);
	if (error != 0){
		log_print("getaddrinfo failed: %s\n", gai_strerror(error));
		return -1;
	}
	
	// Remember the first matching IP address
	struct sockaddr_in server_addr;
	memcpy(&server_addr, addr_info->ai_addr, sizeof(server_addr));
	
	// Show all found addresses... just some fun for debugging
	size_t host_len = 1024, service_len = 1024;
	char host[host_len], service[service_len];
	for (struct addrinfo * cur_addr_info = addr_info; cur_addr_info != NULL; cur_addr_info = cur_addr_info->ai_next){
		getnameinfo(cur_addr_info->ai_addr, cur_addr_info->ai_addrlen, host, host_len, service, service_len, NI_DGRAM | NI_NUMERICHOST);
		log_print("%s:%s\n", host, service);
	}
	
	// Free the entire address list
	freeaddrinfo(addr_info);
	
	
	// Create a local socket and bind to some free port
	int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (client_fd == -1){
		perror("socket");
		return -1;
	}
	
	struct sockaddr_in addr = (struct sockaddr_in){ AF_INET, 0, .sin_addr = { INADDR_ANY } };
	if (bind(client_fd, (const struct sockaddr *)&addr, sizeof(addr)) == -1){
		perror("bind");
		return -1;
	}
	
	
	
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
	log_print("Welcome from server, you're client %hhu\n", packet.user);
	
	
	void conceal_loss(){
		int decoded_samples = opus_decode(dec, NULL, 0, out_frame, frame_samples, 0);
		if (decoded_samples < 0)
			log_print("opus_decode error: %d\n", opus_decode);
		else
			write(STDOUT_FILENO, out_frame, decoded_samples * channel_count * sizeof(int16_t));
	}
	
	size_t frame_filled = 0;
	uint16_t send_seq = 0;
	uint16_t recv_seq = 0;
	while(!quit){
		// Read and receive stuff
		struct pollfd pollfds[2] = {
			(struct pollfd){ client_fd, POLLIN },
			(struct pollfd){ STDIN_FILENO, POLLIN }
		};
		error = poll(pollfds, 2, -1);
		if (error == -1){
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
				int decoded_samples = opus_decode(dec, packet.data, data_len, out_frame, frame_samples, 0);
				if (decoded_samples < 0)
					log_print("opus_decode error: %d\n", opus_decode);
				else
					write(STDOUT_FILENO, out_frame, decoded_samples * channel_count * sizeof(int16_t));
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
			// Audio data from stdin ready to read
			ssize_t bytes_read = read(STDIN_FILENO, frame + frame_filled, frame_size - frame_filled);
			if (bytes_read == -1){
				perror("read");
				continue;
			}
			
			frame_filled += bytes_read;
			if (frame_filled >= frame_size){
				packet = (packet_t){ PACKET_DATA, user_id, send_seq };
				int32_t len = opus_encode(enc, frame, frame_samples, packet.data, sizeof(packet) - offsetof(packet_t, data));
				frame_filled -= frame_size;
				
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
	
	free(frame);
	free(out_frame);
	close(client_fd);
}