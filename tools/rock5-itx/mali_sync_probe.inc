/* Native synchronization checks, sharing the persistent queue probe's setup. */
static SyncInfo
SyncState(int fd, uint32_t handle = 0)
{
	SyncInfo info{}; info.version = 1; info.handle = handle;
	CHECK(Call(fd, kGetSyncInfo, info) == 0); return info;
}
static uint32_t
MakeSync(int fd, bool signaled = false)
{
	SyncCreate request{1, signaled ? 1u : 0u, 0, 0};
	CHECK(Call(fd, kCreateSync, request) == 0); return request.handle;
}
static void
DropSync(int fd, uint32_t handle)
{
	SyncHandle request{1, handle, 0}; CHECK(Call(fd, kDestroySync, request) == 0);
}
static int
UpdateSync(int fd, uint32_t op, uint32_t handle, uint64_t point = 0)
{
	struct { SyncBatch header; SyncPoint point; } request{{1, 1, 0, 0}, {handle, 0, point}};
	return ioctl(fd, op, &request, sizeof(request));
}
static int
WaitSync(int fd, uint32_t handle, uint64_t point, uint32_t flags, int64_t timeout,
	status_t* result = NULL)
{
	struct { SyncWait header; SyncPoint point; } request{{1, flags, 1, 0, timeout, 0, 0}, {handle, 0, point}};
	int status = ioctl(fd, kWaitSync, &request, sizeof(request));
	if (result != NULL) *result = request.header.result;
	return status;
}
static int
ExportSync(int fd, uint32_t handle, bool snapshot = false, uint64_t point = 0)
{
	SyncFd request{1, handle, -1, snapshot ? 1u : 0u, point, 0};
	CHECK(Call(fd, kExportSync, request) == 0 && request.fd >= 0);
	CHECK((fcntl(request.fd, F_GETFD) & FD_CLOEXEC) != 0); return request.fd;
}
static uint32_t
ImportSync(int fd, int shared)
{
	SyncFd request{1, 0, shared, 0, 0, 0}; CHECK(Call(fd, kImportSync, request) == 0); return request.handle;
}
static int
WaitSyncFd(int fd, int64_t timeout = 0, status_t* result = NULL)
{
	SyncFileWait request{1, 0, 0, timeout, 0, 0};
	int status = Call(fd, kWaitSyncFile, request);
	if (result != NULL) *result = request.result;
	return status;
}
static uint64_t
SubmitSync(Context& context, uint64_t address, uint32_t bytes,
	std::initializer_list<SyncPoint> waits, std::initializer_list<SyncPoint> signals)
{
	struct { QueueSubmitSync header; SyncPoint points[8]; } request{};
	CHECK(waits.size() + signals.size() <= 8);
	request.header.queue = {1, context.queue, context.generation, address, bytes, 0, 0, 0};
	request.header.waitCount = waits.size(); request.header.signalCount = signals.size();
	unsigned count = 0;
	for (const auto& point : waits) request.points[count++] = point;
	for (const auto& point : signals) request.points[count++] = point;
	__sync_synchronize();
	CHECK(ioctl(context.fd, kSubmitQueueSync, &request, sizeof(QueueSubmitSync) + count * sizeof(SyncPoint)) == 0);
	CHECK(request.header.queue.sequence == ++context.sequence
		&& request.header.queue.generation == context.generation);
	return context.sequence;
}
static void
SyncSameCounts(const SyncInfo& before, const SyncInfo& after)
{
	CHECK(before.globalClients == after.globalClients && before.globalObjects == after.globalObjects
		&& before.globalPoints == after.globalPoints && before.globalEvents == after.globalEvents
		&& before.globalExports == after.globalExports && before.clientHandles == after.clientHandles
		&& before.globalWaits == after.globalWaits);
}
struct SyncCpuWait { int fd; uint32_t handle, flags; int status; status_t result; };
static int32
SyncCpuThread(void* cookie)
{
	auto& wait = *(SyncCpuWait*)cookie;
	wait.status = WaitSync(wait.fd, wait.handle, 0, wait.flags, 5000000, &wait.result);
	return B_OK;
}
static void
SyncCpuChecks(int fd)
{
	auto baseline = SyncState(fd);
	uint32_t handle = MakeSync(fd), other = MakeSync(fd, true);
	CHECK(WaitSync(fd, handle, 0, 0, 0) != 0 && errno == EINVAL);
	CHECK(WaitSync(fd, handle, 0, kSyncWaitAvailable, 0) != 0 && errno == ETIMEDOUT);
	SyncCpuWait waits[2] = {{fd, handle, kSyncWaitForSubmit, -1, B_ERROR},
		{fd, handle, kSyncWaitAvailable, -1, B_ERROR}};
	thread_id threads[2];
	for (unsigned i = 0; i < 2; i++) {
		threads[i] = spawn_thread(SyncCpuThread, "Mali CPU sync wait", B_NORMAL_PRIORITY, &waits[i]);
		CHECK(threads[i] >= B_OK && resume_thread(threads[i]) == B_OK);
	}
	bigtime_t deadline = system_time() + 3000000;
	while (SyncState(fd).globalWaits != baseline.globalWaits + 2) {
		CHECK(system_time() < deadline); snooze(1000);
	}
	CHECK(UpdateSync(fd, kSignalSync, handle) == 0);
	CHECK(UpdateSync(fd, kResetSync, handle) == 0); // publication must already be captured
	for (unsigned i = 0; i < 2; i++) {
		status_t result; CHECK(wait_for_thread(threads[i], &result) == B_OK && result == B_OK);
		CHECK(waits[i].status == 0 && waits[i].result == B_OK);
	}
	CHECK(SyncState(fd, handle).flags == 0);
	for (uint64_t point = 10; point <= 1280; point += 10) {
		CHECK(UpdateSync(fd, kSignalSync, handle, point) == 0);
		CHECK(WaitSync(fd, handle, point - 5, 0, 0) == 0);
	}
	auto info = SyncState(fd, handle); CHECK(info.submitted == 1280 && info.completed == 1280);
	CHECK(UpdateSync(fd, kSignalSync, handle, 1200) != 0 && errno == EINVAL);
	SyncTransfer transfer{1, 0, {handle, 0, 1275}, {other, 0, 5}, 0};
	CHECK(Call(fd, kTransferSync, transfer) == 0 && WaitSync(fd, other, 5, 0, 0) == 0);
	DropSync(fd, handle); DropSync(fd, other); SyncSameCounts(baseline, SyncState(fd));
	puts("ROCK5_MALI_SYNC_CPU_PASS wait_for_submit=1 wait_available=1 replacement=1 timeline_points=128 transfer=1");
}
static void
FillSyncData(Context& context, unsigned round)
{
	for (unsigned i = 0; i < 2048; i++) context.Data()[i] = CommandGuard(round, i);
}
static void
CheckSyncData(Context& context, unsigned round, bool producer, bool copied, uint32_t value)
{
	__sync_synchronize();
	for (unsigned i = 0; i < 2048; i++) {
		uint32_t expected = CommandGuard(round, i);
		if (producer && i == 1) expected = context.seed;
		if ((producer || copied) && i == 31) expected = value;
		CHECK(context.Data()[i] == expected);
	}
}
static void
RunNativeSynchronization(int fd)
{
	SyncCpuChecks(fd);
	auto baseline = SyncState(fd); auto bufferBaseline = Buffers(fd); auto vmBaseline = Vms(fd);
	unsigned areaBaseline = Areas("Mali CSF queue DMA");
	Context producer(fd, 0x74381a62, NULL), consumer(fd, 0x354691af, NULL), independent(fd, 0x692154fb, NULL);
	Buffer longStream; longStream.Allocate(fd, 1024 * 1024); memset(longStream.Data(), 0, 1024 * 1024);
	const uint64_t longAddress = UINT64_C(0x130000000), sharedAddress = UINT64_C(0x140000000);
	producer.generation = Bind(fd, producer.vm, {{kVmMapOperation, kVmNoExecute, longAddress,
		1024 * 1024, longStream.handle, 0, 0}});
	consumer.generation = Bind(fd, consumer.vm, {{kVmMapOperation, kVmNoExecute | kVmReadOnly,
		sharedAddress, 8192, producer.data.handle, 0, 0}});
	const unsigned round = 907; const uint32_t value = producer.seed ^ (round * 0x174123u);
	FillSyncData(producer, round); FillSyncData(consumer, round);
	unsigned programBytes = producer.Program((uint64_t*)producer.stream.Data(), round, true);
	// CS LOAD_MULTIPLE/STORE_MULTIPLE: Mesa25.3.6 v10.xml, source address d0,
	// scalar r2, mask1, offset124. This consumes the producer's actual GPU write.
	uint64_t copy[] = {UINT64_C(0x0300000000ff0000), UINT64_C(0x1700000000000002),
		UINT64_C(0x0100000000000000) | sharedAddress, UINT64_C(0x140200000001007c),
		UINT64_C(0x0300000000010000), UINT64_C(0x0100000000000000) | kDataAddress,
		UINT64_C(0x150200000001007c), UINT64_C(0x0300000000010000)};
	memcpy(consumer.stream.Data(), copy, sizeof(copy));
	uint32_t timeline = MakeSync(fd), importedSnapshot = MakeSync(fd);
	for (unsigned n = 0; n < 32; n++) producer.Submit(longAddress, 1024 * 1024);
	SubmitSync(producer, kStreamAddress, programBytes, {}, {{timeline, 0, 10}});
	SubmitSync(independent, 0, 0, {}, {{timeline, 0, 20}});
	SubmitSync(consumer, kStreamAddress, sizeof(copy), {{timeline, 0, 10}}, {{timeline, 0, 30}});
	independent.Wait(independent.sequence);
	auto checkpoint = SyncState(fd, timeline);
	unsigned pending = Info(fd, producer.queue).pending;
	CHECK(pending > 0 && checkpoint.submitted == 30 && checkpoint.completed == 0);
	CHECK(Info(fd, consumer.queue).completed == 0 && Info(fd, consumer.queue).pending == 1);
	CheckPendingProperties(fd, producer.queue, consumer.queue);
	CHECK(WaitSync(fd, timeline, 20, kSyncWaitAvailable, 0) == 0);
	CHECK(WaitSync(fd, timeline, 20, 0, 0) != 0 && errno == ETIMEDOUT);
	int twenty = ExportSync(fd, timeline, true, 15), thirty = ExportSync(fd, timeline, true, 30);
	SyncFd import{1, importedSnapshot, thirty, kSyncSnapshotFd, 0, 0};
	CHECK(Call(fd, kImportSync, import) == 0);
	CHECK(UpdateSync(fd, kResetSync, timeline) == 0 && UpdateSync(fd, kSignalSync, timeline) == 0);
	CHECK(WaitSync(fd, timeline, 0, 0, 0) == 0);
	CHECK(WaitSyncFd(twenty) != 0 && errno == ETIMEDOUT);
	status_t result;
	CHECK(WaitSyncFd(thirty, 10000000, &result) == 0 && result == B_OK);
	CHECK(WaitSync(fd, importedSnapshot, 0, 0, 0, &result) == 0 && result == B_OK);
	producer.Wait(producer.sequence); consumer.Wait(consumer.sequence);
	CheckSyncData(producer, round, true, false, value); CheckSyncData(consumer, round, false, true, value);
	CHECK(close(twenty) == 0 && close(thirty) == 0);
	DropSync(fd, timeline); DropSync(fd, importedSnapshot);
	consumer.Finish(); independent.Finish();
	printf("ROCK5_MALI_SYNC_GPU_PASS producer_pending=%u submitted=35 completed_checked=35 shared_words=1 guards=4093 timeline_out_of_order=1 snapshots=1\n", pending);
	for (unsigned trial = 0; trial < 2; trial++) {
		Context dependent(fd, 0x72583ca1 + trial, NULL); FillSyncData(dependent, trial);
		programBytes = dependent.Program((uint64_t*)dependent.stream.Data(), trial, true);
		uint32_t gate = MakeSync(fd), childOutput = MakeSync(fd), propagated = MakeSync(fd);
		int gateFd = ExportSync(fd, gate), outputFd = ExportSync(fd, childOutput);
		for (unsigned n = 0; n < 32; n++) producer.Submit(longAddress, 1024 * 1024);
		SubmitSync(producer, 0, 0, {}, {{gate, 0, 10}});
		int ready[2], release[2]; CHECK(pipe(ready) == 0 && pipe(release) == 0);
		pid_t child = fork(); CHECK(child >= 0);
		if (child == 0) {
			close(ready[0]); close(release[1]);
			int childFd = open(kDevice, O_RDWR); CHECK(childFd >= 0);
			uint32_t childGate = ImportSync(childFd, gateFd), childSignal = ImportSync(childFd, outputFd);
			Context context(childFd, 0x824631ad + trial, NULL);
			unsigned bytes = context.Program((uint64_t*)context.stream.Data(), trial, true);
			SubmitSync(context, kStreamAddress, bytes, {{childGate, 0, 10}}, {{childSignal, 0, 1}});
			context.DropHandles();
			for (unsigned i = 0; i < 16; i++) MakeSync(childFd, true);
			ExportSync(childFd, childSignal, true, 1); // intentionally retained until process exit
			unsigned waiting = Info(childFd, context.queue).pending;
			CHECK(write(ready[1], &waiting, sizeof(waiting)) == sizeof(waiting));
			char value; CHECK(read(release[0], &value, 1) == 1); _exit(0);
		}
		close(ready[1]); close(release[0]);
		unsigned childPending; CHECK(read(ready[0], &childPending, sizeof(childPending)) == sizeof(childPending));
		SubmitSync(dependent, kStreamAddress, programBytes, {{childOutput, 0, 1}}, {{propagated, 0, 1}});
		pending = Info(fd, producer.queue).pending;
		CHECK(childPending == 1 && pending > 0);
		if (trial == 0) CHECK(write(release[1], "x", 1) == 1);
		else CHECK(kill(child, SIGKILL) == 0);
		int status; CHECK(waitpid(child, &status, 0) == child);
		CHECK(trial == 0 ? WIFEXITED(status) && WEXITSTATUS(status) == 0
			: WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
		CHECK(close(ready[0]) == 0 && close(release[1]) == 0);
		CHECK(WaitSync(fd, childOutput, 1, 0, 10000000, &result) == 0 && result == B_CANCELED);
		CHECK(WaitSync(fd, propagated, 1, 0, 10000000, &result) == 0 && result == B_CANCELED);
		QueueWait canceled{1, dependent.queue, dependent.sequence, 10000000, 0, 0, 0};
		CHECK(Call(fd, kWaitQueue, canceled) == 0 && canceled.result == B_CANCELED);
		auto failed = Info(fd, dependent.queue);
		CHECK(failed.state == kQueueFailed && failed.pending == 0 && failed.failedSequence == 1
			&& failed.completed == 0 && failed.error == B_CANCELED);
		CheckSyncData(dependent, trial, false, false, 0);
		producer.Wait(producer.sequence); dependent.Finish();
		CHECK(close(gateFd) == 0 && close(outputFd) == 0);
		DropSync(fd, gate); DropSync(fd, childOutput); DropSync(fd, propagated);
		printf("ROCK5_MALI_SYNC_EXIT_PASS trial=%u normal=%u killed=%u child_pending=%u producer_pending=%u canceled=2 guards=2048\n",
			trial, trial == 0, trial == 1, childPending, pending);
	}
	producer.Finish(); longStream.Drop(fd); longStream.Unmap();
	SyncSameCounts(baseline, SyncState(fd));
	auto after = Buffers(fd); auto vms = Vms(fd);
	CHECK(after.globalClients == bufferBaseline.globalClients && after.globalBuffers == bufferBaseline.globalBuffers
		&& after.globalBufferBytes == bufferBaseline.globalBufferBytes);
	CHECK(vms.globalVms == vmBaseline.globalVms && vms.globalGenerations == vmBaseline.globalGenerations
		&& vms.globalTablePages == vmBaseline.globalTablePages && Areas("Mali CSF queue DMA") == areaBaseline);
	puts("ROCK5_MALI_SYNC_PASS submitted=105 completed_checked=101 contexts=7 canceled=4 producer_consumer=1 rendered=0");
}
struct SyncKeepalive { int shared, snapshot; };
static SyncKeepalive
KeepSyncAcrossClose(int fd)
{
	uint32_t handle = MakeSync(fd, true);
	SyncKeepalive keep{ExportSync(fd, handle), ExportSync(fd, handle, true)};
	DropSync(fd, handle); return keep;
}
static void
CheckSyncAcrossClose(SyncKeepalive keep)
{
	CHECK(WaitSyncFd(keep.snapshot) == 0);
	int duplicate = dup(keep.shared); CHECK(duplicate >= 0 && (fcntl(duplicate, F_GETFD) & FD_CLOEXEC) == 0);
	CHECK(close(keep.shared) == 0);
	int fd = open(kDevice, O_RDWR); CHECK(fd >= 0);
	auto retained = SyncState(fd);
	CHECK(retained.globalObjects == 1 && retained.globalExports == 2 && retained.globalEvents == 0
		&& retained.globalClients == 1 && retained.globalWaits == 0);
	uint32_t imported = ImportSync(fd, duplicate);
	CHECK(UpdateSync(fd, kResetSync, imported) == 0 && SyncState(fd, imported).flags == 0);
	CHECK(WaitSyncFd(keep.snapshot) == 0);
	CHECK(UpdateSync(fd, kSignalSync, imported) == 0 && WaitSyncFd(duplicate) == 0);
	CHECK(close(duplicate) == 0 && close(keep.snapshot) == 0); DropSync(fd, imported);
	auto after = SyncState(fd);
	CHECK(after.globalObjects == 0 && after.globalPoints == 0 && after.globalEvents == 0
		&& after.globalExports == 0 && after.clientHandles == 0 && after.globalWaits == 0);
	CHECK(close(fd) == 0);
	puts("ROCK5_MALI_SYNC_FD_LIFETIME_PASS last_device_close=1 reopen_import=1 dup=1 cloexec=1 snapshot=1 leaked=0");
}
