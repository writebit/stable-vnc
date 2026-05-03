#include "network_info.h"
#include <string.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>

void network_get_addresses(NetworkAddresses *addrs) {
    memset(addrs, 0, sizeof(*addrs));

    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        if (ifa->ifa_addr->sa_family == AF_INET && addrs->ipv4_count < MAX_ADDRS) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &sa->sin_addr,
                       addrs->ipv4_addrs[addrs->ipv4_count], ADDR_STRLEN);
            addrs->ipv4_count++;
        } else if (ifa->ifa_addr->sa_family == AF_INET6 && addrs->ipv6_count < MAX_ADDRS) {
            struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)ifa->ifa_addr;
            // Skip link-local addresses (fe80::)
            if (IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr)) continue;
            inet_ntop(AF_INET6, &sa6->sin6_addr,
                       addrs->ipv6_addrs[addrs->ipv6_count], ADDR_STRLEN);
            addrs->ipv6_count++;
        }
    }

    freeifaddrs(ifaddr);
}
