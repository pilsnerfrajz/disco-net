#ifndef ERROR_H
#define ERROR_H

#include <assert.h>

typedef enum err
{
	SUCCESS,
	NO_RESPONSE,
	UNKNOWN_HOST,
	SOCKET_ERROR,
	PROTO_NOT_FOUND,
	MEM_ALLOC_ERROR,
	IFACE_ERROR,
	BAD_BUF_SIZE,
	ARP_NOT_SUPP,
	ARP_SUPP,
	PCAP_INIT,
	PCAP_OPEN,
	PCAP_INJECT,
	PCAP_FILTER,
	PCAP_LOOP,
	COUNT, /* Not used, only for assert */
} err_t;

extern const char *const error_strings[];

#endif
