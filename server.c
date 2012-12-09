#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>


#include "proto.h"


int main(int argc, char **argv){
	if (argc != 2){
		fprintf(stderr, "usage: %s port\n", argv[0]);
		return -1;
	}
	
	const in_port_t port = strtoul(argv[1], NULL, 10);
	
	
	int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (server_fd == -1){
		perror("socket");
		return -1;
	}
	
	struct sockaddr_in addr = (struct sockaddr_in){ AF_INET, htons(port), .sin_addr = { INADDR_ANY } };
	if (bind(server_fd, (const struct sockaddr *)&addr, sizeof(addr)) == -1){
		perror("bind");
		return -1;
	}
	
	
	printf("starting server on port %hu\n", port);
	
	size_t client_count = 0;
	struct sockaddr_in *clients = NULL;
	packet_t packet;
	while(true){
		struct sockaddr_in client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		ssize_t bytes_received = recvfrom(server_fd, &packet, sizeof(packet), 0, (struct sockaddr *)&client_addr, &client_addr_len);
		if (bytes_received == -1){
			perror("recvfrom");
			continue;
		}
		
		size_t data_len = bytes_received - offsetof(packet_t, data);
		switch(packet.type){
			case PACKET_HELLO: {
				size_t client_idx = client_count;
				printf("client from %s:%hu connected as %zu\n",
					inet_ntoa(client_addr.sin_addr), client_addr.sin_port, client_idx);
				
				// Add client to the client list
				client_count++;
				clients = realloc(clients, client_count * sizeof(struct sockaddr_in));
				clients[client_idx] = client_addr;
				
				// Send a welcome packet with its client number
				packet = (packet_t){PACKET_WELCOME, client_idx, 0, 0};
				ssize_t bytes_send = sendto(server_fd, &packet, offsetof(packet_t, seq), 0, (const struct sockaddr *)&client_addr, sizeof(client_addr));
				if (bytes_send == -1)
					perror("sendto");
				
				// Send a join packet to all other clients
				packet = (packet_t){PACKET_JOIN, client_idx, 0, 0};
				for(size_t i = 0; i < client_count; i++){
					if (clients[i].sin_addr.s_addr == client_addr.sin_addr.s_addr && clients[i].sin_port == client_addr.sin_port)
						continue;
					if (clients[i].sin_addr.s_addr == 0)
						continue;
					
					bytes_send = sendto(server_fd, &packet, offsetof(packet_t, seq), 0, (const struct sockaddr *)&clients[i], sizeof(clients[i]));
					if (bytes_send == -1)
						perror("sendto");
				}
				
				} break;
			case PACKET_DATA: case PACKET_BYE: {
				// Broadcast packet to all clients but the one sending it
				//printf("broadcasting packet from %s:%hu to:\n",
				//	inet_ntoa(client_addr.sin_addr), client_addr.sin_port);
				ssize_t bytes_send;
				for(size_t i = 0; i < client_count; i++){
					if (clients[i].sin_addr.s_addr == client_addr.sin_addr.s_addr && clients[i].sin_port == client_addr.sin_port)
						continue;
					if (clients[i].sin_addr.s_addr == 0)
						continue;
					
					//printf("- %s:%hu\n", inet_ntoa(clients[i].sin_addr), clients[i].sin_port);
					bytes_send = sendto(server_fd, &packet, bytes_received, 0, (const struct sockaddr *)&clients[i], sizeof(clients[i]));
					if (bytes_send == -1)
						perror("sendto");
				}
				
				// If we got a BYE packet mark the client as dead (set its IP to 0)
				if (packet.type == PACKET_BYE && packet.user < client_count){
					clients[packet.user].sin_addr.s_addr = 0;
					printf("client %s:%hu (%hhu) disconnected\n",
						inet_ntoa(client_addr.sin_addr), client_addr.sin_port, packet.user);
				}
				
				} break;
			default:
				printf("unknown packet, type %hhu, %zu bytes data\n", packet.type, data_len);
				break;
		}
	}
	
	close(server_fd);
}