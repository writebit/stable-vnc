#ifndef NETWORK_INFO_H
#define NETWORK_INFO_H

#define MAX_ADDRS 16
#define ADDR_STRLEN 64

typedef struct {
    char ipv4_addrs[MAX_ADDRS][ADDR_STRLEN];
    int ipv4_count;
    char ipv6_addrs[MAX_ADDRS][ADDR_STRLEN];
    int ipv6_count;
} NetworkAddresses;

void network_get_addresses(NetworkAddresses *addrs);

#endif
