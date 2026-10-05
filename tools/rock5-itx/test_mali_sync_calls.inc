/* Shared API-only calls for the synchronization and runtime fixtures. */
template<typename T>
static status_t SyncCall(void* client, uint32 op, T& value)
{
	return ControlSync(client, op, &value, sizeof(value));
}
static inline void* SyncOpen(bool writable = true)
{
	void* client = NULL; assert(OpenSyncClient(writable, &client) == B_OK); return client;
}
static inline uint32 SyncMake(void* client, bool signaled = false)
{
	SyncCreate value = {1, signaled ? 1u : 0u, 0, 0};
	assert(SyncCall(client, kCreateSync, value) == B_OK); return value.handle;
}
static inline void SyncDestroy(void* client, uint32 handle)
{
	SyncHandle value = {1, handle, 0}; assert(SyncCall(client, kDestroySync, value) == B_OK);
}
static inline SyncInfo SyncQuery(void* client, uint32 handle = 0)
{
	SyncInfo value = {}; value.version = 1; value.handle = handle;
	assert(SyncCall(client, kGetSyncInfo, value) == B_OK); return value;
}
static inline status_t SyncUpdate(void* client, uint32 op, std::initializer_list<SyncPoint> points)
{
	std::vector<uint8> bytes(sizeof(SyncBatch) + points.size() * sizeof(SyncPoint));
	auto* value = (SyncBatch*)bytes.data(); *value = {1, uint32(points.size()), 0, 0};
	memcpy(value + 1, points.begin(), points.size() * sizeof(SyncPoint));
	return ControlSync(client, op, value, bytes.size());
}
static inline status_t SyncWaitFor(void* client, std::initializer_list<SyncPoint> points,
	uint32 flags = kSyncWaitAll, int64 timeout = 0, uint32* first = NULL, status_t* result = NULL)
{
	std::vector<uint8> bytes(sizeof(SyncWait) + points.size() * sizeof(SyncPoint));
	auto* value = (SyncWait*)bytes.data(); *value = {1, flags, uint32(points.size()), 0, timeout, 0, 0};
	memcpy(value + 1, points.begin(), points.size() * sizeof(SyncPoint));
	status_t status = ControlSync(client, kWaitSync, value, bytes.size());
	if (first != NULL) *first = value->first;
	if (result != NULL) *result = value->result;
	return status;
}
static inline int SyncExport(void* client, uint32 handle, bool snapshot = false, uint64 point = 0)
{
	SyncFd value = {1, handle, -1, snapshot ? 1u : 0u, point, 0};
	assert(SyncCall(client, kExportSync, value) == B_OK); assert(value.fd >= 0); return value.fd;
}
static inline uint32 SyncImport(void* client, int fd)
{
	SyncFd value = {1, 0, fd, 0, 0, 0}; assert(SyncCall(client, kImportSync, value) == B_OK);
	return value.handle;
}
static inline void SyncImportSnapshot(void* client, uint32 handle, int fd, uint64 point = 0)
{
	SyncFd value = {1, handle, fd, 1, point, 0}; assert(SyncCall(client, kImportSync, value) == B_OK);
}
static inline SyncSubmission SyncEnqueue(void* client, std::initializer_list<SyncPoint> waits,
	std::initializer_list<SyncPoint> signals)
{
	std::vector<SyncPoint> points(waits); points.insert(points.end(), signals.begin(), signals.end());
	SyncSubmission submission = {};
	assert(PrepareSyncSubmission(client, points.data(), waits.size(), signals.size(), submission) == B_OK);
	CommitSyncSubmission(submission); return submission;
}
static inline status_t SyncFdWait(int fd, status_t* result = NULL, int64 timeout = 0)
{
	file_descriptor* descriptor = get_fd(get_current_io_context(false), fd); assert(descriptor != NULL);
	SyncFileWait request = {1, 0, 0, timeout, 0, 0};
	status_t status = descriptor->ops->fd_ioctl(descriptor, kWaitSyncFile, &request, sizeof(request));
	if (result != NULL) *result = request.result;
	put_fd(descriptor); return status;
}
static inline void SyncBalanced()
{
	assert(sSyncClients == 0 && sSyncObjects == 0 && sSyncNodes == 0 && sSyncEvents == 0
		&& sSyncExports == 0 && sSyncAllocations.empty() && sDescriptorCount == 0
		&& sModuleReferences == 0 && sSyncPendingCaptures == NULL && sConditionWaiters == 0);
	assert(sSyncWaiters == 0);
	for (auto& context : sFdContexts) assert(context.second->num_used_fds == 0);
}
