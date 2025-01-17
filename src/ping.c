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

// return codes
#define PING_SUCCESS 0
#define NO_RESPONSE 1
#define INVALID_IP 2
#define STRUCT_ERROR 3
#define SOCKET_ERROR 4

#define TIMEOUT_SECONDS 2

/**
 * @brief ICMP6 pseudo header used when calculating the checksum
 * for an ICMP6 packet.
 *
 */
typedef struct icmp6_pseudo_hdr
{
	struct in6_addr source;
	struct in6_addr dest;
	u_int32_t length;
	u_int32_t zero[3];
	u_int8_t next;
} icmp6_pseudo_hdr;

/**
 * @brief Validates IP address strings. Supports IPv4 and IPv6.
 *
 * @param ip The IP address to validate.
 * @return `int` 0 on valid address. -1 if the address is invalid
 * or if an error occurs.
 */
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

/**
 * @brief Gets a pointer to an addrinfo structure for the destination
 * IP address. Allows socket setup to be IP-address family agnostic.
 * The returned struct should be freed with `freeaddrinfo()`.
 *
 * @param dst IP string.
 * @return `struct addrinfo*` on success. `NULL` if an error occurs.
 */
struct addrinfo *get_dst_addr_struct(char *dst)
{
	struct addrinfo *dst_info;
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
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
		return NULL;
	}

	return temp;
}

/**
 * @brief Gets a proto object for the ICMP or ICMP6 protocols.
 *
 * @param dst_info `addrinfo*` struct of the target address.
 * @return `struct protoent*` on success or `NULL` if an error occurs.
 */
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

/**
 * @brief Sets a timeout on the socket. The socket blocks for `TIMEOUT_SECONDS`
 * if no data is received, before proceeding.
 *
 * @param sfd The socket file descriptor.
 * @return `int` 0 if options are set correctly. Otherwise -1.
 */
int set_socket_options(int sfd)
{
	struct timeval timeout = {
		.tv_sec = TIMEOUT_SECONDS,
		.tv_usec = 0,
	};

	int rv = setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	if (rv == -1)
	{
		return -1;
	}

	return 0;
}

/**
 * @brief Calculates the internet checksum of an ICMP header or ICMP6 pseudo header.
 *
 * @param hdr The address of the header struct.
 * @param len The size of the header struct.
 * @return `uint16_t` Internet checksum of header struct.
 */
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

/**
 * @brief Creates an ICMP echo request header.
 *
 * @param seq The sequence number of the packet.
 * @return `struct icmp` ICMP header struct.
 */
struct icmp create_icmp4_echo_req_hdr(int seq)
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

/**
 * @brief Receives the ICMP echo reply and parses it to get the ICMP header.
 *
 * @param sfd The socket file descriptor.
 * @param dst The `addrinfo*` struct of the target address.
 * @return `struct icmp*` on success. `NULL` if no bytes are received or if a timeout
 * occurs.
 */
struct icmp *get_icmp4_reply_hdr(int sfd)
{
	char recvbuf[sizeof(struct ip) + sizeof(struct icmp)];
	int recv_bytes = recv(sfd, &recvbuf, sizeof(recvbuf), 0);
	if (recv_bytes <= 0)
	{
		return NULL;
	}

	struct icmp *reply_hdr = (struct icmp *)(recvbuf + sizeof(struct ip));
	return reply_hdr;
}

/**
 * @brief Verifies the echo reply.
 *
 * @param reply_hdr The ICMP header of the reply.
 * @param seq The sequence number of the echo request.
 * @return `int` 0 if reply matches request. Otherwise -1.
 */
int verify_icmp4_reply_hdr(struct icmp *reply_hdr, int seq)
{
	if (reply_hdr->icmp_type == ICMP_ECHOREPLY &&
		ntohs(reply_hdr->icmp_hun.ih_idseq.icd_seq) == seq &&
		ntohs(reply_hdr->icmp_hun.ih_idseq.icd_id) == (getpid() & 0xffff))
	{
		return 0;
	}
	return -1;
}

/**
 * @brief Receives the ICMP6 echo reply and parses it to get the ICMP6 header.
 *
 * @param sfd The socket file descriptor.
 * @param dst The `addrinfo*` struct of the target address.
 * @return `struct icmp6_hdr*` on success. `NULL` if no bytes are received or if a timeout
 * occurs.
 */
struct icmp6_hdr *get_icmp6_reply_hdr(int sfd)
{
	char recvbuf[sizeof(struct icmp6_hdr)];
	int recv_bytes = recv(sfd, &recvbuf, sizeof(recvbuf), 0);
	if (recv_bytes <= 0)
	{
		return NULL;
	}

	struct icmp6_hdr *reply_hdr = (struct icmp6_hdr *)(recvbuf);
	return reply_hdr;
}

/**
 * @brief Creates an ICMP6 echo request header. The checksum has to be set
 * manually after declaration using the ICMP6 pseudo header.
 *
 * @param seq The sequence number of the packet.
 * @return `struct icmp6_hdr` ICMP6 header struct.
 */
struct icmp6_hdr create_icmp6_echo_req_hdr(int seq)
{
	struct icmp6_hdr icmp6_hdr;

	memset(&icmp6_hdr, 0, sizeof(icmp6_hdr));
	icmp6_hdr.icmp6_type = ICMP6_ECHO_REQUEST;
	icmp6_hdr.icmp6_code = 0;
	icmp6_hdr.icmp6_id = htons(getpid() & 0xffff);
	icmp6_hdr.icmp6_seq = htons(seq);
	icmp6_hdr.icmp6_cksum = 0;

	return icmp6_hdr;
}

int ping(char *address, int tries)
{
	int rv = validate_ip(address);
	if (rv == -1)
	{
		return INVALID_IP;
	}

	struct addrinfo *dst_info = get_dst_addr_struct(address);
	if (dst_info == NULL)
	{
		freeaddrinfo(dst_info);
		return STRUCT_ERROR;
	}

	struct protoent *protocol = get_proto(dst_info);
	if (protocol == NULL)
	{
		freeaddrinfo(dst_info);
		return STRUCT_ERROR;
	}

	int sfd = socket(dst_info->ai_family, SOCK_DGRAM, protocol->p_proto);
	if (sfd == -1)
	{
		freeaddrinfo(dst_info);
		return SOCKET_ERROR;
	}

	rv = set_socket_options(sfd);
	if (rv == -1)
	{
		freeaddrinfo(dst_info);
		return SOCKET_ERROR;
	}

	rv = connect(sfd, dst_info->ai_addr, dst_info->ai_addrlen);
	if (rv < 0)
	{
		freeaddrinfo(dst_info);
		return SOCKET_ERROR;
	}

	int seq = 0;
	int sent_bytes;
	int host_is_up = 0;
	if (dst_info->ai_family == AF_INET)
	{
		for (int attempt = 0; attempt < tries; attempt++)
		{
			struct icmp icmp4_req_hdr = create_icmp4_echo_req_hdr(++seq);

			sent_bytes = send(sfd, &icmp4_req_hdr, sizeof(icmp4_req_hdr), 0);
			if (sent_bytes == -1)
			{
				continue;
			}

			struct icmp *reply_hdr = get_icmp4_reply_hdr(sfd);
			if (reply_hdr == NULL)
			{
				continue;
			}

			if (reply_hdr->icmp_type == ICMP_ECHO)
			{
				reply_hdr = get_icmp4_reply_hdr(sfd);
			}

			if (reply_hdr == NULL)
			{
				continue;
			}

			rv = verify_icmp4_reply_hdr(reply_hdr, seq);
			if (rv == 0)
			{
				host_is_up = 1;
				break;
			}
		}
	}
	else
	{
		for (int attempt = 0; attempt < tries; attempt++)
		{
			struct icmp6_hdr icmp6_req_hdr = create_icmp6_echo_req_hdr(++seq);

			struct sockaddr_in6 src;
			socklen_t sock_len = sizeof(src);
			int rv = getsockname(sfd, (struct sockaddr *)&src, &sock_len);
			if (rv == -1)
			{
				freeaddrinfo(dst_info);
				return SOCKET_ERROR;
			}

			// parse the dst_info struct to get a suitable structure to use in pseudo.
			struct sockaddr_in6 *temp_sockaddr = (struct sockaddr_in6 *)dst_info->ai_addr;
			struct in6_addr dest_addr = temp_sockaddr->sin6_addr;
			icmp6_pseudo_hdr pseudo_hdr = {
				.source = src.sin6_addr,
				.dest = dest_addr,
				.zero = {0, 0, 0},
				.length = htonl(sizeof(icmp6_req_hdr)),
				.next = IPPROTO_ICMPV6,
			};

			icmp6_req_hdr.icmp6_cksum = calc_checksum(&pseudo_hdr, sizeof(pseudo_hdr) + sizeof(icmp6_req_hdr));

			int sent_bytes = send(sfd, &icmp6_req_hdr, sizeof(icmp6_req_hdr), 0);
			if (sent_bytes == -1)
			{
				continue;
			}

			struct icmp6_hdr *reply_hdr = reply_hdr = get_icmp6_reply_hdr(sfd);
			if (reply_hdr == NULL)
			{
				continue;
			}

			// when pinging loopback, the request is sometimes captured by recv
			if (reply_hdr->icmp6_type == ICMP6_ECHO_REQUEST)
			{
				reply_hdr = get_icmp6_reply_hdr(sfd);
			}

			if (reply_hdr != NULL &&
				reply_hdr->icmp6_type == ICMP6_ECHO_REPLY &&
				ntohs(reply_hdr->icmp6_seq) == seq &&
				ntohs(reply_hdr->icmp6_id) == (getpid() & 0xffff))
			{
				host_is_up = 1;
				break;
			}
		}
	}

	freeaddrinfo(dst_info);
	close(sfd);

	if (host_is_up)
		return PING_SUCCESS;

	return NO_RESPONSE;
}
