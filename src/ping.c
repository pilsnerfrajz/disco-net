#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp6.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/time.h>
#include <errno.h>

#define TIMEOUT_SECONDS 2

void exit_error(struct addrinfo *dst)
{
	if (dst != NULL)
		freeaddrinfo(dst);
	exit(EXIT_FAILURE);
}

int validate_ip(char *ip)
{
	struct in_addr ipv4_dst;
	struct in6_addr ipv6_dst;
	if (inet_pton(AF_INET, ip, &(ipv4_dst)) == 1 ||
		inet_pton(AF_INET6, ip, &(ipv6_dst)) == 1)
	{
		return 0;
	}
	return -1;
}

struct addrinfo *get_dst_addr_struct(char *dst)
{
	struct addrinfo *dst_info;
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_RAW;
	hints.ai_flags = AI_PASSIVE;

	int ret = getaddrinfo(dst, NULL, &hints, &dst_info);
	if (ret != 0)
	{
		return NULL;
	}

	struct addrinfo *temp = dst_info;
	while (temp != NULL)
	{
		if (temp->ai_family == AF_INET || temp->ai_family == AF_INET6)
		{
			break;
		}
		temp = temp->ai_next;
	}

	if (temp == NULL)
	{
		freeaddrinfo(dst_info);
		return NULL;
	}

	return temp;
}

struct protoent *get_proto(struct addrinfo *dst_info)
{
	struct protoent *protocol;
	if (dst_info->ai_family == AF_INET)
	{
		protocol = getprotobyname("icmp");
	}
	else
	{
		protocol = getprotobyname("icmp6");
	}

	if (protocol == NULL)
	{
		return NULL;
	}

	return protocol;
}

int set_socket_options(int sfd)
{
	struct timeval timeout = {
		.tv_sec = TIMEOUT_SECONDS,
		.tv_usec = 0,
	};

	int rv = setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	if (rv == -1)
	{
		perror("Setsockopt");
		return -1;
	}

	return 0;
}

uint16_t calc_checksum(void *hdr, int len)
{
	uint16_t *temp = hdr;
	uint32_t sum = 0;

	/* count 16 bits each iteration */
	for (sum = 0; len > 1; len -= 2)
	{
		sum += *temp++;
	}

	if (len == 1)
	{
		sum += *(uint8_t *)temp;
	}

	while (sum >> 16)
	{
		sum = (sum >> 16) + (sum & 0xffff);
	}

	return ~sum;
}

struct icmp create_ipv4_echo_req_hdr(int seq)
{
	struct icmp req_hdr;
	memset(&req_hdr, 0, sizeof(req_hdr));
	req_hdr.icmp_type = ICMP_ECHO;
	req_hdr.icmp_code = 0;
	req_hdr.icmp_cksum = 0;
	req_hdr.icmp_hun.ih_idseq.icd_id = htons(getpid() & 0xffff);
	req_hdr.icmp_hun.ih_idseq.icd_seq = htons(seq);
	req_hdr.icmp_cksum = calc_checksum(&req_hdr, sizeof(req_hdr));

	return req_hdr;
}

struct icmp *get_ipv4_reply_hdr(int sfd, struct addrinfo *dst)
{
	char recvbuf[sizeof(struct ip) + sizeof(struct icmp)];
	int recv_bytes = recv(sfd, &recvbuf, sizeof(recvbuf), 0);
	if (recv_bytes < 0)
	{
		if (errno == EWOULDBLOCK)
		{
			return NULL;
		}
		else
		{
			perror("Recv");
			exit_error(dst);
		}
	}
	if (recv_bytes == 0)
	{
		return NULL;
	}

	struct icmp *reply_hdr = (struct icmp *)(recvbuf + sizeof(struct ip));
	return reply_hdr;
}

int verify_ipv4_reply_hdr(struct icmp *reply_hdr, int seq)
{
	if (reply_hdr->icmp_type == ICMP_ECHOREPLY &&
		ntohs(reply_hdr->icmp_hun.ih_idseq.icd_seq) == seq - 1 &&
		ntohs(reply_hdr->icmp_hun.ih_idseq.icd_id) == (getpid() & 0xffff))
	{
		return 0;
	}
	return -1;
}

int ping(char *dst, int count)
{
	int rv = validate_ip(dst);
	if (rv == -1)
	{
		fprintf(stderr, "Invalid IP address.\n");
		exit(EXIT_FAILURE);
	}

	struct addrinfo *dst_info = get_dst_addr_struct(dst);
	if (dst_info == NULL)
	{
		fprintf(stderr, "Failed getting target address info.\n");
		exit_error(dst_info);
	}

	struct protoent *protocol = get_proto(dst_info);
	if (protocol == NULL)
	{
		fprintf(stderr, "Could not find a protocol with the given name.\n");
		exit(EXIT_FAILURE);
	}

	int sfd = socket(dst_info->ai_family, SOCK_RAW, protocol->p_proto);
	if (sfd == -1)
	{
		perror("Socket");
		exit_error(dst_info);
	}

	rv = set_socket_options(sfd);
	if (rv == -1)
	{
		exit_error(dst_info);
	}

	rv = connect(sfd, dst_info->ai_addr, dst_info->ai_addrlen);
	if (rv < 0)
	{
		perror("Connect");
		exit_error(dst_info);
	}

	int seq = 1;
	int sent_bytes;
	int host_is_up = 0;
	if (dst_info->ai_family == AF_INET)
	{
		for (int attempt = 0; attempt < count; attempt++)
		{
			struct icmp ipv4_req_hdr = create_ipv4_echo_req_hdr(seq++);

			sent_bytes = send(sfd, &ipv4_req_hdr, sizeof(ipv4_req_hdr), 0);
			if (sent_bytes == -1)
			{
				// perror("Send");
				// exit_error(dst_info);
				continue;
			}

			struct icmp *reply_hdr = get_ipv4_reply_hdr(sfd, dst_info);
			if (reply_hdr == NULL)
			{
				continue;
			}

			rv = verify_ipv4_reply_hdr(reply_hdr, seq);
			if (rv == 0)
			{
				host_is_up = 1;
				break;
			}
		}
	}

	freeaddrinfo(dst_info);
	close(sfd);

	if (host_is_up)
		return 0;

	return -1;
}