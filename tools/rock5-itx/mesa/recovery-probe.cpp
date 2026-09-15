// Reuse the qualified native queue, VM, buffer and synchronization helpers.
#define main queue_standalone_main
#include "../mali_queue_probe.cpp"
#undef main
#include "native-observer.h"

static void FailedQueue(Context& context, uint64_t completed, uint64_t failed)
{
	QueueWait wait{1, context.queue, context.sequence, 10000000, 0, 0, 0};
	CHECK(Call(context.fd, kWaitQueue, wait) == 0 && wait.result == B_IO_ERROR);
	auto info = Info(context.fd, context.queue);
	printf("ROCK5_RECOVERY_QUEUE handle=%u submitted=%" B_PRIu64 " completed=%" B_PRIu64
		" failed=%" B_PRIu64 " pending=%u state=%u error=%" B_PRId32 " wait=%" B_PRId32 "\n",
		context.queue, info.submitted, info.completed, info.failedSequence, info.pending,
		info.state, info.error, wait.result);
	CHECK(info.state == kQueueFailed && info.error == B_IO_ERROR);
	CHECK(info.submitted == context.sequence && info.completed == completed && info.failedSequence == failed);
	QueueSubmit rejected{1, context.queue, context.generation, 0, 0, 0, 0, 0};
	CHECK(Call(context.fd, kSubmitQueue, rejected) != 0 && errno == EIO);
}

int main(int argc, char** argv)
{
	setbuf(stdout, NULL);
	CHECK(argc == 2);
	int observer = open(kDevice, O_RDWR | O_CLOEXEC);
	if (observer < 0 && errno == ENOENT) { puts("ROCK5_RECOVERY_NO_DEVICE"); return 77; }
	CHECK(observer >= 0);
	Snapshot baseline{}, after{};
	CHECK(snapshot(observer, &baseline));
	CHECK(baseline.kernel_areas == 0 && baseline.user_maps == 0);
	int first = open(kDevice, O_RDWR | O_CLOEXEC), second = open(kDevice, O_RDWR | O_CLOEXEC);
	CHECK(first >= 0 && second >= 0);
	Context a(first, 0x62a731d9, argv[1]), b(second, 0x937541ab, NULL);
	a.Store(0, true); b.Store(0, true);
	Buffer nops; nops.Allocate(first, 1024 * 1024);
	memset(nops.Data(), 0, 1024 * 1024);
	const uint64_t nopsAddress = UINT64_C(0x130000000);
	a.generation = Bind(first, a.vm, {{kVmMapOperation, kVmNoExecute, nopsAddress,
		1024 * 1024, nops.handle, 0, 0}});
	// Mesa 25.3.6 genxml/v10.xml CS Opcode occupies bits 56..63; 0xff is
	// unassigned. One deliberately invalid instruction in mapped command RAM.
	// This exercises a command exception, not arbitrary addresses or a shader
	// that runs indefinitely. The preceding valid NOP work is already qualified.
	const uint64_t instruction = UINT64_C(0xff00000000000000);
	memcpy(a.stream.Data(), &instruction, sizeof(instruction));
	uint32_t fault = MakeSync(first), queued = MakeSync(first), affected = MakeSync(second);
	int sharedFault = ExportSync(first, fault);
	uint32_t importedFault = ImportSync(second, sharedFault);
	for (unsigned i = 0; i < 32; i++) a.Submit(nopsAddress, 1024 * 1024);
	uint64_t faultSequence = SubmitSync(a, kStreamAddress, sizeof(instruction), {}, {{fault, 0, 1}});
	SubmitSync(a, 0, 0, {{fault, 0, 1}}, {{queued, 0, 1}});
	SubmitSync(b, 0, 0, {{importedFault, 0, 1}}, {{affected, 0, 1}});
	int outputs[] = {ExportSync(first, fault, true, 1), ExportSync(first, queued, true, 1),
		ExportSync(second, affected, true, 1)};
	auto pendingA = Info(first, a.queue), pendingB = Info(second, b.queue);
	CHECK(pendingA.error == B_OK && pendingB.error == B_OK && pendingA.pending >= 2 && pendingB.pending == 1);
	printf("ROCK5_RECOVERY_ARMED a=%u b=%u instruction=%016" B_PRIx64
		" fault_sequence=%" B_PRIu64 " a_submitted=%" B_PRIu64 " b_submitted=%" B_PRIu64
		" a_pending=%u b_pending=%u\n", a.queue, b.queue, instruction, faultSequence,
		pendingA.submitted, pendingB.submitted, pendingA.pending, pendingB.pending);
	bigtime_t start = system_time();
	for (unsigned i = 0; i < 3; i++) {
		status_t result = B_ERROR;
		CHECK(WaitSyncFd(outputs[i], 10000000, &result) == 0 && result == B_IO_ERROR);
		printf("ROCK5_RECOVERY_FENCE index=%u result=%" B_PRId32 "\n", i, result);
		CHECK(close(outputs[i]) == 0);
	}
	FailedQueue(a, faultSequence - 1, faultSequence);
	FailedQueue(b, 1, 2);
	CHECK(close(sharedFault) == 0);
	DropSync(first, fault); DropSync(first, queued);
	DropSync(second, affected); DropSync(second, importedFault);
	nops.Drop(first); nops.Unmap();
	a.Finish();
	CHECK(Info(second, b.queue).state == kQueueFailed);
	b.Finish();
	CHECK(close(first) == 0 && close(second) == 0);
	CHECK(snapshot(observer, &after) && same_snapshot(baseline, after, 0));
	printf("ROCK5_RECOVERY_CLOSED elapsed_us=%" B_PRId64 " contexts=2 fences=3\n", system_time() - start);
	// A fresh native queue must load new firmware and execute real stores in
	// the same Haiku boot. The launcher follows with the full GLES pixel probe.
	int fresh = open(kDevice, O_RDWR | O_CLOEXEC); CHECK(fresh >= 0);
	Context c(fresh, 0x543279b1, argv[1]);
	CHECK(c.queue > a.queue && c.queue > b.queue);
	c.Store(0, true); c.Store(1);
	printf("ROCK5_RECOVERY_FRESH handle=%u completed=%" B_PRIu64 " words=4096\n", c.queue, c.sequence);
	c.Finish(); CHECK(close(fresh) == 0);
	CHECK(snapshot(observer, &after) && same_snapshot(baseline, after, 1));
	CHECK(close(observer) == 0);
	puts("ROCK5_RECOVERY_NATIVE_PASS contexts=2 failed_jobs=3 fences=3 fresh_stores=2");
	return 0;
}
