#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#ifdef __APPLE__
#include <net/if_dl.h>	  /* sockaddr_dl */
#include <net/if_types.h> /* IFT_ETHER */
#endif

#ifdef __linux__
#include <net/if_arp.h>		 /* ARPHRD_ETHER */
#include <linux/if_packet.h> /* AF_PACKET and sockaddr_ll */
#endif

/* Send/read packets */
#include <pcap/pcap.h>

#include "../include/arp.h"
#include "../include/sock_utils.h"

#define ETH_TYPE_IP4 0x0800
#define ETH_TYPE_ARP 0x0806

int arp(char *address)
{
	/*char errbuf[PCAP_ERRBUF_SIZE];
	int rv = pcap_init(PCAP_CHAR_ENC_LOCAL, errbuf);
	if (rv != 0)
	{
		fprintf(stderr, "Pcap_init error: %s\n", errbuf);
		return -1;
	}*/

	// pcap_inject();

	struct addrinfo *dst_info = get_dst_addr_struct(address, SOCK_DGRAM);
	if (dst_info == NULL)
	{
		freeaddrinfo(dst_info);
		return STRUCT_ERROR;
	}

	u_int8_t sender_ip[4];
	u_int8_t sender_mac[6];
	int ret = get_arp_details((struct sockaddr_in *)dst_info->ai_addr,
							  sender_ip, sender_mac);
	if (ret != SUCCESS)
	{
		freeaddrinfo(dst_info);
		return -1; // TODO
	}

	struct ethernet_header ethernet_header;
	struct arp_packet arp_packet;
	memset(&ethernet_header, 0, sizeof(ethernet_header));
	memset(&arp_packet, 0, sizeof(arp_packet));

	/* Populate Ethernet header
	 * https://en.wikipedia.org/wiki/EtherType
	 */
	memset(&ethernet_header.dst, 0xff, 6);
	memcpy(&ethernet_header.src, sender_mac, 6);
	ethernet_header.ptype = htons(ETH_TYPE_ARP);

	/* Populate ARP packet
	 * https://en.wikipedia.org/wiki/Address_Resolution_Protocol
	 */
	arp_packet.hrd = htons(1);
	arp_packet.pro = htons(ETH_TYPE_IP4);
	arp_packet.hln = 6;
	arp_packet.pln = 4;
	arp_packet.op = htons(1); /* Request */
	memcpy(&arp_packet.sha, sender_mac, sizeof(arp_packet.sha));
	memcpy(&arp_packet.spa, sender_ip, sizeof(arp_packet.spa));
	memcpy(&arp_packet.tpa,
		   &((struct sockaddr_in *)dst_info->ai_addr)->sin_addr.s_addr,
		   sizeof(arp_packet.tpa));
	/* arp_packet.tha already zero from memset on struct */

	/* Prepare frame */
	struct arp_frame_t arp_frame = {
		.eth_hdr = ethernet_header,
		.arp_pkt = arp_packet,
	};

	freeaddrinfo(dst_info);
	return SUCCESS;
}

int compare_subnets(in_addr_t src, in_addr_t dst, in_addr_t mask)
{
	int src_net = ntohl(src) & ntohl(mask);
	int dst_net = ntohl(dst) & ntohl(mask);

	if (src_net == dst_net)
	{
		return 0;
	}
	return -1; // TODO ??
}

int get_arp_details(struct sockaddr_in *dst, u_int8_t *sender_ip,
					u_int8_t *sender_mac)
{
	struct ifaddrs *ifap;
	struct ifaddrs *ifa;
	int rv = getifaddrs(&ifap);
	if (rv != 0)
	{
		perror("getifaddrs");
		return STRUCT_ERROR;
	}

	/* Bools for checking if we have the wanted info */
	int net_mask = 0;
	int ip_addr = 0;
	int mac_addr = 0;

	char *iface = "";
	unsigned char *mac;
	struct sockaddr_in *ip;
	struct sockaddr_in *mask;

	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next)
	{
		/* Check if interface is loopback. Continue if it is */
		if (ip_addr && (((ntohl(ip->sin_addr.s_addr) & 0xFF000000) == 0x7F000000)))
		{
			net_mask = 0;
			ip_addr = 0;
			mac_addr = 0;
			continue;
		}

		if (ip_addr && mac_addr && net_mask)
		{
			/* If target and host are on the same subnet, ARP is possible */
			if (compare_subnets(ip->sin_addr.s_addr,
								dst->sin_addr.s_addr,
								mask->sin_addr.s_addr) != 0)
			{
				net_mask = 0;
				ip_addr = 0;
				mac_addr = 0;
				continue;
			}

			/*printf("ARP is possible for:\nInterface: %s\n", iface);

			char print_ip[INET_ADDRSTRLEN];
			if (inet_ntop(AF_INET, &ip->sin_addr.s_addr, print_ip, INET_ADDRSTRLEN) == NULL)
			{
				perror("inet_ntop");
			}
			printf("IP: %s\n", print_ip);

			char print_mask[INET_ADDRSTRLEN];
			if (inet_ntop(AF_INET, &mask->sin_addr, print_mask, INET_ADDRSTRLEN) == NULL)
			{
				perror("inet_ntop");
			}
			printf("MASK: %s\n", print_mask);

			printf("MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
				   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);*/

			/* Buffers must be 4 and 6 bytes */
			if (sizeof(sender_ip) >= 4 && sizeof(sender_mac) >= 6)
			{
				memcpy(sender_ip, &ip->sin_addr.s_addr, 4);
				memcpy(sender_mac, mac, 6);
			}
			else
			{
				return -1; // TODO
			}

			freeifaddrs(ifap);
			return SUCCESS;
		}

		if (ifa->ifa_name != NULL)
		{
			/* If a new interface is found, reset */
			if (strcmp(iface, ifa->ifa_name) != 0)
			{
				iface = ifa->ifa_name;
				net_mask = 0;
				ip_addr = 0;
				mac_addr = 0;
			}
		}

/* AF_LINK = macOS */
#ifdef __APPLE__
		if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_LINK)
		{
			/* https://www.illumos.org/man/3SOCKET/sockaddr_dl */
			struct sockaddr_dl *s = (struct sockaddr_dl *)ifa->ifa_addr;

			if (s->sdl_type == IFT_ETHER)
			{
				/* Jump to address in sdl_data with macro */
				mac = (unsigned char *)LLADDR(s);
				mac_addr = 1;
			}
		}
#endif

/* UNTESTED */
#ifdef __LINUX__
		if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_PACKET)
		{
			/* https://www.illumos.org/man/3SOCKET/sockaddr_dl */
			struct sockaddr_ll *s = (struct sockaddr_ll *)ifa->ifa_addr;

			if (s->sll_hatype == ARPHRD_ETHER)
			{
				mac = (unsigned char *)s->sll_addr;
				mac_addr = 1;
			}
		}
#endif

		if (ifa->ifa_netmask != NULL && ifa->ifa_netmask->sa_family == AF_INET)
		{
			mask = (struct sockaddr_in *)(ifa->ifa_netmask);
			net_mask = 1;
		}

		if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_INET)
		{
			ip = (struct sockaddr_in *)ifa->ifa_addr;
			ip_addr = 1;
		}
	}

	freeifaddrs(ifap);
	return -1; // TODO
}
