// Native allocation observer copied unchanged from the qualified offscreen probe.
struct Snapshot {
    ClientInfo buffers;
    VmInfo vms;
    HeapInfo heaps;
    SyncInfo sync;
    unsigned kernel_areas, user_maps;
};
static int snapshot(int fd, Snapshot *s)
{
    memset(s, 0, sizeof(*s));
    s->buffers.version = s->vms.version = s->heaps.version = s->sync.version = 1;
    CHECK(ioctl(fd, kGetClientInfo, &s->buffers, sizeof(s->buffers)) == 0);
    CHECK(ioctl(fd, kGetVmInfo, &s->vms, sizeof(s->vms)) == 0);
    CHECK(ioctl(fd, kGetHeapInfo, &s->heaps, sizeof(s->heaps)) == 0);
    CHECK(ioctl(fd, kGetSyncInfo, &s->sync, sizeof(s->sync)) == 0);
    for (team_id team : {team_id(B_SYSTEM_TEAM), team_id(B_CURRENT_TEAM)}) {
        ssize_t cookie = 0;
        area_info area;
        unsigned visited = 0;
        while (get_next_area_info(team, &cookie, &area) == B_OK) {
            visited++;
            if (team == B_SYSTEM_TEAM && !strncmp(area.name, "Mali CSF ", 9))
                s->kernel_areas++;
            if (team == B_CURRENT_TEAM && !strcmp(area.name, "Mali CSF buffer mapping"))
                s->user_maps++;
        }
        CHECK(visited > 0);
    }
    return 1;
}
static int same_snapshot(const Snapshot& before, const Snapshot& after, unsigned cycle)
{
    printf("ROCK5_MESA_NATIVE_LIFETIME cycle=%u clients=%u buffers=%u bytes=%llu vms=%u generations=%u vm_pages=%u heaps=%u chunks=%u heap_bytes=%llu heap_pages=%u heap_generations=%u sync_clients=%u sync_objects=%u sync_points=%u sync_events=%u sync_exports=%u sync_waits=%u kernel_areas=%u user_maps=%u\n",
        cycle, after.buffers.globalClients, after.buffers.globalBuffers,
        (unsigned long long)after.buffers.globalBufferBytes,
        after.vms.globalVms, after.vms.globalGenerations, after.vms.globalTablePages,
        after.heaps.globalHeaps, after.heaps.globalChunks,
        (unsigned long long)after.heaps.globalBytes, after.heaps.globalTablePages,
        after.heaps.globalHeapGenerations, after.sync.globalClients, after.sync.globalObjects,
        after.sync.globalPoints, after.sync.globalEvents, after.sync.globalExports,
        after.sync.globalWaits, after.kernel_areas, after.user_maps);
    CHECK(!memcmp(&before, &after, sizeof(before)));
    return 1;
}
