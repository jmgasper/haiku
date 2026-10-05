#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <net/if.h>
#include <thread>

using status_t = int;
const int B_OK = 0;
const int B_ERROR = -1;
struct net_protocol {};
struct net_interface { unsigned flags; int index; };
struct net_interface_address { net_interface* interface; };
struct net_buffer { sockaddr* source; int link; bool consumed; };
struct net_route {
	sockaddr* destination;
	sockaddr* mask;
	sockaddr* gateway;
	unsigned flags;
	unsigned mtu;
	net_interface_address* interface_address;
};

static sockaddr_in6 sources[2];
static net_interface interfaces[] = {{IFF_UP, 0}, {IFF_UP, 1}};
static net_interface_address addresses[] = {{&interfaces[0]}, {&interfaces[1]}};
static std::atomic<int> references[2];
static std::atomic<int> acquired, released, sends;
static bool available[] = {true, true};
static status_t sendStatus = B_OK;
static net_protocol protocol;
static net_protocol* sIPv6Protocol = &protocol;

static net_interface_address*
Lookup(const sockaddr* source)
{
	assert(source->sa_family == AF_INET6);
	for (int index = 0; index < 2; index++) {
		if (available[index] && memcmp(source, &sources[index], sizeof(sockaddr_in6)) == 0) {
			references[index]++;
			acquired++;
			return &addresses[index];
		}
	}
	return nullptr;
}

static void
Release(net_interface_address* address)
{
	int index = address->interface->index;
	assert(address == &addresses[index]);
	assert(references[index].fetch_sub(1) > 0);
	released++;
}

static status_t
Send(net_protocol* current, net_route* route, net_buffer* buffer)
{
	assert(current == &protocol);
	assert(route->interface_address == &addresses[buffer->link]);
	assert(references[buffer->link] > 0);
	assert(route->interface_address->interface->flags & IFF_UP);
	// A local/host/gateway route would bypass or alter the Ethernet path.
	assert(route->flags == 0 && route->mtu == 0);
	assert(route->destination == nullptr && route->mask == nullptr && route->gateway == nullptr);
	assert(!buffer->consumed);
	buffer->consumed = sendStatus == B_OK;
	sends++;
	return sendStatus;
}

static struct DatalinkModule {
	net_interface_address* (*get_interface_address)(const sockaddr*);
	void (*put_interface_address)(net_interface_address*);
} datalink = {Lookup, Release}, *sDatalinkModule = &datalink;
static struct IPv6Module {
	status_t (*send_routed_data)(net_protocol*, net_route*, net_buffer*);
} ipv6 = {Send}, *sIPv6Module = &ipv6;

#include "ndp_send_body.inc"

static void
Check(int link, status_t expected)
{
	net_buffer buffer = {reinterpret_cast<sockaddr*>(&sources[link]), link, false};
	assert(ndp_send_data(&buffer) == expected);
	assert(buffer.consumed == (expected == B_OK));
}

int
main()
{
	for (int index = 0; index < 2; index++) {
		sources[index].sin6_family = AF_INET6;
		assert(inet_pton(AF_INET6, index == 0 ? "fd35:88:8::2" : "fd35:88:9::2",
			&sources[index].sin6_addr) == 1);
	}
	Check(1, B_OK);
	Check(0, B_OK);
	int previous = sends;
	available[0] = false; // A retry after address removal must not use a stale route.
	Check(0, EADDRNOTAVAIL);
	assert(sends == previous);
	available[0] = true;
	interfaces[0].flags = 0;
	Check(0, ENETDOWN);
	assert(sends == previous);
	interfaces[0].flags = IFF_UP;
	sendStatus = EIO;
	Check(0, EIO); // Failed sends leave ownership with the caller.
	sendStatus = B_OK;
	previous = acquired;
	sIPv6Protocol = nullptr;
	Check(1, B_ERROR);
	assert(acquired == previous);
	sIPv6Protocol = &protocol;
	std::thread workers[4];
	for (int i = 0; i < 4; i++) {
		workers[i] = std::thread([i] {
			for (int n = 0; n < 1000; n++)
				Check(i % 2, B_OK);
		});
	}
	for (auto& worker : workers)
		worker.join();
	assert(references[0] == 0 && references[1] == 0 && acquired == released);
	assert(sends == 4003);
	puts("ndp send: links, lifetimes and failure ownership passed");
}
