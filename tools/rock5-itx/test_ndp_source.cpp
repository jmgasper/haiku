#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static bool operator!=(const in6_addr& a, const in6_addr& b)
{
	return memcmp(&a, &b, sizeof(a)) != 0;
}

struct net_interface {};
struct net_interface_address { sockaddr* local; };
struct ipv6_datalink_protocol { net_interface* interface; in6_addr local_address; };
static net_interface interface;
static std::vector<net_interface_address*> listed;
static int held, visits;

static bool
Next(net_interface* current, net_interface_address** address)
{
	assert(current == &interface);
	visits++;
	size_t next = 0;
	if (*address != nullptr) {
		assert(held == 1);
		held--;
		while (next < listed.size() && listed[next] != *address)
			next++;
		assert(next < listed.size());
		next++;
	}
	if (next == listed.size()) {
		*address = nullptr;
		return false;
	}
	held++;
	*address = listed[next];
	return true;
}

static struct DatalinkModule {
	bool (*get_next_interface_address)(net_interface*, net_interface_address**);
} datalink = {Next}, *sDatalinkModule = &datalink;

#include "ndp_source_body.inc"

int
main()
{
	sockaddr_in6 oldAddress = {}, newAddress = {}, remaining = {};
	for (sockaddr_in6* value : {&oldAddress, &newAddress, &remaining})
		value->sin6_family = AF_INET6;
	assert(inet_pton(AF_INET6, "fd35:88:8:1::2", &oldAddress.sin6_addr) == 1);
	assert(inet_pton(AF_INET6, "fd35:88:8:2::2", &newAddress.sin6_addr) == 1);
	assert(inet_pton(AF_INET6, "fd35:88:8:3::2", &remaining.sin6_addr) == 1);
	sockaddr ipv4 = {};
	ipv4.sa_family = AF_INET;
	net_interface_address old = {reinterpret_cast<sockaddr*>(&oldAddress)};
	net_interface_address other = {reinterpret_cast<sockaddr*>(&remaining)};
	net_interface_address v4 = {&ipv4}, empty = {nullptr};
	ipv6_datalink_protocol protocol = {&interface, oldAddress.sin6_addr};
	listed = {&old};
	ndp_replace_local_source(&protocol, oldAddress.sin6_addr,
		reinterpret_cast<sockaddr*>(&newAddress));
	assert(!(protocol.local_address != newAddress.sin6_addr) && visits == 0);
	// Removing an unrelated alias must retain the selected source.
	ndp_replace_local_source(&protocol, oldAddress.sin6_addr, nullptr);
	assert(!(protocol.local_address != newAddress.sin6_addr) && visits == 0);
	for (auto entries : {std::vector<net_interface_address*>{&old, &v4, &empty, &other},
			std::vector<net_interface_address*>{&other, &old}}) {
		listed = entries;
		protocol.local_address = oldAddress.sin6_addr;
		ndp_replace_local_source(&protocol, oldAddress.sin6_addr, nullptr);
		assert(!(protocol.local_address != remaining.sin6_addr) && held == 0);
	}
	for (sockaddr* replacement : {static_cast<sockaddr*>(nullptr), &ipv4}) {
		listed = {&old, &v4, &empty};
		protocol.local_address = oldAddress.sin6_addr;
		ndp_replace_local_source(&protocol, oldAddress.sin6_addr, replacement);
		assert(IN6_IS_ADDR_UNSPECIFIED(&protocol.local_address) && held == 0);
	}
	puts("ndp source: replacement and deletion passed");
}
