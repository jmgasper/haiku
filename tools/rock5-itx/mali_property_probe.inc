/* Native property checks, sharing the persistent queue probe's runtime. */
static GpuProperties sNativeProperties;
static unsigned sPropertyQueries;

static QueueProperties
Properties(int fd, uint32_t handle)
{
	QueueProperties request{}; request.version = 1; request.handle = handle;
	memset(&request.gpu, 0x95, sizeof(request.gpu));
	CHECK(Call(fd, kGetQueueProperties, request) == 0);
	CHECK(request.version == 1 && request.handle == handle && request.flags == 0
		&& request.reserved == 0 && request.reserved2[0] == 0 && request.reserved2[1] == 0);
	sPropertyQueries++;
	return request;
}

static void
CheckCachedProperties(int fd, uint32_t handle)
{
	auto request = Properties(fd, handle);
	CHECK(memcmp(&request.gpu, &sNativeProperties, sizeof(sNativeProperties)) == 0);
}

static void
RetiredProperties(int fd, uint32_t handle)
{
	QueueProperties request{}; request.version = 1; request.handle = handle;
	CHECK(Call(fd, kGetQueueProperties, request) != 0 && errno == ENOENT);
}

static void
StartPropertyChecks(int fd, uint32_t first, uint32_t second)
{
	auto buffers = Buffers(fd); auto vms = Vms(fd); auto before = Info(fd, first);
	sNativeProperties = Properties(fd, first).gpu;
	const auto& p = sNativeProperties;
	// The existing Linux GPU-query reference independently recorded this subset.
	CHECK(p.gpuId == 0xa8670005 && p.gpuRevision == 0 && p.csfId == 0x040a0412);
	CHECK(p.l2Features == 0x07120306 && p.memFeatures == 0x0301 && p.mmuFeatures == 0x2830);
	CHECK(p.addressSpaces == 0xff && p.shaderPresent == 0x50005 && p.tilerPresent == 1 && p.l2Present == 1);
	CHECK(p.firmwareVersion == 0x01050000 && p.groupCount == 8 && p.streamCount == 8
		&& p.workRegisters == 96 && p.scoreboards == 8 && p.reservedRegisters == 4);
	CHECK(p.maxQueues == 8 && p.maxQueueJobs == 64 && p.userVaLimit == (UINT64_C(1) << 47));
	CHECK(p.firmwareTimerHz > 0 && p.firmwareTimerHz <= 1000000000);
	printf("ROCK5_MALI_PROPERTIES_GPU id=%08" PRIx32 " revision=%08" PRIx32 " csf=%08" PRIx32
		" core=%08" PRIx32 " l2=%08" PRIx32 " tiler=%08" PRIx32 " memory=%08" PRIx32
		" mmu=%08" PRIx32 " coherency=%08" PRIx32 " as=%08" PRIx32 " shader_present=%016" PRIx64
		" tiler_present=%016" PRIx64 " l2_present=%016" PRIx64 " gpu_features=%016" PRIx64 "\n",
		p.gpuId, p.gpuRevision, p.csfId, p.coreFeatures, p.l2Features, p.tilerFeatures, p.memFeatures,
		p.mmuFeatures, p.coherencyFeatures, p.addressSpaces, p.shaderPresent, p.tilerPresent, p.l2Present, p.gpuFeatures);
	printf("ROCK5_MALI_PROPERTIES_THREADS features=%08" PRIx32 " max=%08" PRIx32
		" workgroup=%08" PRIx32 " barrier=%08" PRIx32 " texture0=%08" PRIx32
		" texture1=%08" PRIx32 " texture2=%08" PRIx32 " texture3=%08" PRIx32 "\n",
		p.threadFeatures, p.maxThreads, p.maxWorkgroupSize, p.maxBarrierSize,
		p.textureFeatures[0], p.textureFeatures[1], p.textureFeatures[2], p.textureFeatures[3]);
	printf("ROCK5_MALI_PROPERTIES_FIRMWARE version=%08" PRIx32 " features=%08" PRIx32
		" group_features=%08" PRIx32 " stream_features=%08" PRIx32 " groups=%u streams=%u"
		" registers=%u scoreboards=%u reserved_registers=%u timer_hz=%u queues=%u jobs=%u user_va_limit=%016" PRIx64 "\n",
		p.firmwareVersion, p.firmwareFeatures, p.groupFeatures, p.streamFeatures,
		p.groupCount, p.streamCount, p.workRegisters, p.scoreboards, p.reservedRegisters,
		p.firmwareTimerHz, p.maxQueues, p.maxQueueJobs, p.userVaLimit);
	CheckCachedProperties(fd, second);
	int duplicate = dup(fd); CHECK(duplicate >= 0);
	CheckCachedProperties(duplicate, first); CHECK(close(duplicate) == 0);
	QueueProperties clean{}; clean.version = 1; clean.handle = first;
	for (unsigned field = 0; field < 5; field++) {
		auto bad = clean;
		if (field == 0) bad.version = 2;
		if (field == 1) bad.flags = 1;
		if (field == 2) bad.reserved = 1;
		if (field == 3) bad.reserved2[0] = 1;
		if (field == 4) bad.reserved2[1] = UINT64_MAX;
		CHECK(Call(fd, kGetQueueProperties, bad) != 0 && errno == EINVAL);
	}
	for (size_t size : {size_t(0), sizeof(clean) - 1, sizeof(clean) + 1})
		CHECK(ioctl(fd, kGetQueueProperties, &clean, size) != 0 && errno == EINVAL);
	CHECK(ioctl(fd, kGetQueueProperties, NULL, sizeof(clean)) != 0 && errno == EFAULT);
	RetiredProperties(fd, 0); RetiredProperties(fd, UINT32_MAX);
	for (int mode : {O_RDWR, O_RDONLY}) {
		int foreign = open(kDevice, mode); CHECK(foreign >= 0);
		RetiredProperties(foreign, first); CHECK(close(foreign) == 0);
	}
	void* page = NULL;
	area_id area = create_area("Mali properties copyout test", &page, B_ANY_ADDRESS,
		4096, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	CHECK(area >= B_OK); memcpy(page, &clean, sizeof(clean));
	CHECK(set_area_protection(area, B_READ_AREA) == B_OK);
	CHECK(ioctl(fd, kGetQueueProperties, page, sizeof(clean)) != 0 && errno == EFAULT);
	CHECK(delete_area(area) == B_OK);
	CheckCachedProperties(fd, first);
	auto after = Info(fd, first); auto afterBuffers = Buffers(fd); auto afterVms = Vms(fd);
	CHECK(memcmp(&before, &after, sizeof(before)) == 0);
	CHECK(memcmp(&buffers, &afterBuffers, sizeof(buffers)) == 0);
	CHECK(memcmp(&vms, &afterVms, sizeof(vms)) == 0);
	puts("ROCK5_MALI_PROPERTIES_BOUNDARIES_PASS rejected=14 duplicate=1 copyout=1 counters_unchanged=1");
}

static void
CheckPendingProperties(int fd, uint32_t producer, uint32_t consumer)
{
	auto before = Info(fd, producer), dependency = Info(fd, consumer);
	CHECK(before.pending > 0 && dependency.pending == 1 && dependency.completed == 0);
	for (unsigned i = 0; i < 16; i++) {
		CheckCachedProperties(fd, producer); CheckCachedProperties(fd, consumer);
	}
	CHECK(Info(fd, producer).submitted == before.submitted
		&& Info(fd, consumer).submitted == dependency.submitted);
	printf("ROCK5_MALI_PROPERTIES_PENDING_PASS queries=32 producer_pending=%u consumer_pending=1 submissions_unchanged=1\n",
		before.pending);
}
