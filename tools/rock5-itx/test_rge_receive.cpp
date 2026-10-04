// Run the real rge_rxeof() with deterministic descriptors and checked substitutes
// for DMA and mbuf operations. Native tests must establish actual coherency.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "rge_receive_constants.h"

using bus_size_t = size_t;
#define __FreeBSD_version 1
#define NVLAN 0
#define ISSET(value, bits) ((value) & (bits))
#define CLR(value, bits) ((value) &= ~(bits))
#define M_PKTHDR 1
#define M_IPV4_CSUM_IN_OK 2
#define M_TCP_CSUM_IN_OK 4
#define M_UDP_CSUM_IN_OK 8
#define ETHER_CRC_LEN 4
#define BUS_DMASYNC_POSTREAD 1
#define BUS_DMASYNC_PREREAD 2
#define BUS_DMASYNC_POSTWRITE 4
#define MBUF_LIST_INITIALIZER() {}
static uint32_t letoh32(uint32_t value) { return value; }

struct mbuf {
	mbuf* m_next = nullptr;
	int m_len = 2046;
	int m_flags = M_PKTHDR;
	struct { int len = 2046; int csum_flags = 0; } m_pkthdr;
	std::array<uint8_t, 2048> bytes;
	uint8_t* m_data = bytes.data() + 2;
	bool freed = false;
	mbuf() { bytes.fill(0xa5); }
};
struct DmaMap {
	size_t dm_mapsize = 2046;
	bool loaded = true;
	bool ring = false;
	mbuf* packet = nullptr;
	size_t copied = 0;
	std::vector<uint8_t> device;
};
struct Sync { DmaMap* map; size_t offset; size_t size; int operations; };
static std::vector<Sync> sSyncs;

static void
bus_dmamap_sync(int, DmaMap* map, size_t offset, size_t size, int operations)
{
	assert(map->loaded && offset <= map->dm_mapsize && size <= map->dm_mapsize - offset);
	sSyncs.push_back({map, offset, size, operations});
	if (!map->ring && operations == BUS_DMASYNC_POSTREAD) {
		assert(map->packet && !map->packet->freed);
		assert(map->packet->m_len == 2046);
		assert(size > 0 && map->device.size() == map->dm_mapsize);
		memcpy(map->packet->m_data + offset, map->device.data() + offset, size);
		map->copied += size;
	}
}
static void bus_dmamap_unload(int, DmaMap* map)
{
	assert(map->loaded && map->copied > 0);
	map->loaded = false;
	map->dm_mapsize = 0;
}
static void m_freem(mbuf* packet)
{
	for (; packet; packet = packet->m_next) {
		assert(!packet->freed);
		packet->freed = true;
	}
}
static void m_adj(mbuf* packet, int amount)
{
	assert(amount == -ETHER_CRC_LEN && packet->m_pkthdr.len >= ETHER_CRC_LEN);
	int remaining = packet->m_pkthdr.len += amount;
	for (mbuf* m = packet; m; m = m->m_next) {
		m->m_len = std::min(m->m_len, remaining);
		remaining -= m->m_len;
	}
	assert(remaining == 0);
}
struct mbuf_list { std::vector<mbuf*> packets; };
static void ml_enqueue(mbuf_list* list, mbuf* m) { list->packets.push_back(m); }
struct Delivery { std::vector<uint8_t> bytes; int flags; };
struct ifnet { unsigned if_ierrors = 0; std::vector<Delivery> delivered; };
static int if_input(ifnet* interface, mbuf_list* list)
{
	for (mbuf* packet : list->packets) {
		Delivery delivery{{}, packet->m_pkthdr.csum_flags};
		for (mbuf* m = packet; m; m = m->m_next) {
			assert(!m->freed && m->m_len >= 0 && m->m_len <= 2046);
			delivery.bytes.insert(delivery.bytes.end(), m->m_data, m->m_data + m->m_len);
		}
		assert(delivery.bytes.size() == size_t(packet->m_pkthdr.len));
		interface->delivered.push_back(delivery);
		m_freem(packet);
	}
	return 0;
}
struct if_rxring { int inuse = 0; };
static int if_rxr_inuse(if_rxring* ring) { return ring->inuse; }
static void if_rxr_put(if_rxring* ring, int count)
	{ assert(count <= ring->inuse); ring->inuse -= count; }
static void if_rxr_livelocked(if_rxring*) { assert(false); }
struct rge_rx_desc {
	uint64_t unused[3] = {};
	struct { struct { uint32_t rge_extsts = 0; uint32_t rge_cmdsts = RGE_RDCMDSTS_OWN; } rx_qword4; } hi_qword1;
};
static_assert(sizeof(rge_rx_desc) == 32);
struct rge_rxq { DmaMap* rxq_dmamap = nullptr; mbuf* rxq_mbuf = nullptr; };
struct rge_softc { struct { ifnet ac_if; } sc_arpcom; int sc_dmat = 0; };
struct rge_queues {
	rge_softc* q_sc;
	struct {
		if_rxring rge_rx_ring;
		std::array<rge_rx_desc, RGE_RX_LIST_CNT> rge_rx_list;
		DmaMap* rge_rx_list_map;
		std::array<struct rge_rxq, RGE_RX_LIST_CNT> rge_rxq;
		int rge_rxq_considx = 0;
		mbuf* rge_head = nullptr;
		mbuf** rge_tail = &rge_head;
	} q_rx;
	unsigned refills = 0;
};
static void rge_fill_rx_ring(rge_queues* queue) { ++queue->refills; }

#include "rge_receive_body.inc"

struct Model {
	rge_softc sc;
	rge_queues queue{};
	DmaMap ring;
	std::array<DmaMap, RGE_RX_LIST_CNT> maps;
	std::vector<std::unique_ptr<mbuf>> packets;
	Model()
	{
		sSyncs.clear();
		queue.q_sc = &sc;
		ring.ring = true;
		ring.dm_mapsize = sizeof(queue.q_rx.rge_rx_list);
		queue.q_rx.rge_rx_list_map = &ring;
	}
	~Model()
	{
		for (auto& rx : queue.q_rx.rge_rxq)
			if (rx.rxq_mbuf) m_freem(rx.rxq_mbuf);
		if (queue.q_rx.rge_head) m_freem(queue.q_rx.rge_head);
		for (const auto& p : packets) assert(p->freed);
	}
	mbuf* post(int index, uint32_t status, uint8_t byte = 0x39)
	{
		packets.push_back(std::make_unique<mbuf>());
		mbuf* packet = packets.back().get();
		DmaMap& map = maps[index];
		map.packet = packet;
		map.device.assign(2046, byte);
		queue.q_rx.rge_rxq[index] = {&map, packet};
		queue.q_rx.rge_rx_list[index].hi_qword1.rx_qword4.rge_cmdsts = status;
		++queue.q_rx.rge_rx_ring.inuse;
		return packet;
	}
	void consume()
	{
		assert(rge_rxeof(&queue) == 1);
		assert(queue.q_rx.rge_rx_ring.inuse == 0 && queue.refills == 1);
		for (const auto& p : packets) assert(p->freed);
		assert(queue.q_rx.rge_head == nullptr);
		assert(queue.q_rx.rge_tail == &queue.q_rx.rge_head);
	}
};

int main()
{
	constexpr uint32_t complete = RGE_RDCMDSTS_SOF | RGE_RDCMDSTS_EOF;
	for (unsigned length : {64, 65, 1500, 1518, 2045, 2046}) {
		Model model;
		mbuf* packet = model.post(0, complete | length);
		model.queue.q_rx.rge_rx_list[0].hi_qword1.rx_qword4.rge_extsts
			= RGE_RDEXTSTS_IPV4 | RGE_RDEXTSTS_TCPPKT;
		model.consume();
		assert(model.maps[0].copied == length && !model.maps[0].loaded);
		assert(packet->bytes[0] == 0xa5 && packet->bytes[1] == 0xa5);
		for (unsigned i = 0; i < 2046; ++i)
			assert(packet->m_data[i] == (i < length ? 0x39 : 0xa5));
		assert(model.sc.sc_arpcom.ac_if.if_ierrors == 0);
		const auto& frames = model.sc.sc_arpcom.ac_if.delivered;
		assert(frames.size() == 1 && frames[0].bytes.size() == length - 4);
		assert(frames[0].flags == (M_IPV4_CSUM_IN_OK | M_TCP_CSUM_IN_OK | M_UDP_CSUM_IN_OK));
	}
	for (uint32_t length : {0, 2047, 16383}) {
		Model model;
		model.post(0, complete | length);
		model.post(1, complete | 64);
		model.consume();
		assert(model.sc.sc_arpcom.ac_if.if_ierrors == 1);
		assert(model.sc.sc_arpcom.ac_if.delivered.size() == 1);
		assert(model.maps[0].copied == 2046 && model.maps[1].copied == 64);
	}
	{
		Model model;
		model.queue.q_rx.rge_rxq_considx = RGE_RX_LIST_CNT - 1;
		model.post(RGE_RX_LIST_CNT - 1, RGE_RDCMDSTS_SOF | 2046, 0x31);
		model.post(0, RGE_RDCMDSTS_EOF | 34, 0x52);
		model.consume();
		const auto& frames = model.sc.sc_arpcom.ac_if.delivered;
		assert(frames.size() == 1 && frames[0].bytes.size() == 2076);
		for (unsigned i = 0; i < 2076; ++i) assert(frames[0].bytes[i] == (i < 2046 ? 0x31 : 0x52));
		assert(model.maps[0].copied == 34 && model.queue.q_rx.rge_rxq_considx == 1);
		std::vector<size_t> offsets;
		for (const auto& sync : sSyncs)
			if (sync.operations == BUS_DMASYNC_POSTWRITE) offsets.push_back(sync.offset);
		assert((offsets == std::vector<size_t>{32 * (RGE_RX_LIST_CNT - 1), 0}));
	}
	for (uint32_t bad : {uint32_t(2047), uint32_t(64 | RGE_RDCMDSTS_RXERRSUM)}) {
		Model model;
		model.post(0, RGE_RDCMDSTS_SOF | 2046);
		model.post(1, RGE_RDCMDSTS_EOF | bad);
		model.post(2, complete | 64);
		model.consume();
		assert(model.sc.sc_arpcom.ac_if.if_ierrors == 1);
		assert(model.sc.sc_arpcom.ac_if.delivered.size() == 1);
	}
	{
		Model model;
		model.post(0, RGE_RDCMDSTS_SOF | 2046);
		model.post(1, complete | 64);
		model.consume();
		assert(model.sc.sc_arpcom.ac_if.if_ierrors == 1);
		assert(model.sc.sc_arpcom.ac_if.delivered.size() == 1);
	}
	{
		Model model;
		model.post(0, RGE_RDCMDSTS_EOF | 64);
		model.consume();
		assert(model.sc.sc_arpcom.ac_if.delivered.empty());
	}
	{
		Model model;
		model.post(0, RGE_RDCMDSTS_OWN | complete | 64);
		assert(rge_rxeof(&model.queue) == 0);
		assert(model.queue.q_rx.rge_rx_ring.inuse == 1 && model.queue.refills == 0);
		assert(model.maps[0].loaded && model.maps[0].copied == 0);
		assert(sSyncs.size() == 2 && sSyncs[0].operations == BUS_DMASYNC_POSTREAD
			&& sSyncs[1].operations == BUS_DMASYNC_PREREAD);
	}
	puts("rge receive: lengths, fragments, ownership and wrap passed");
}
