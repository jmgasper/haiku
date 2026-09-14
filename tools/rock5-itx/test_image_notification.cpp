// Exercise the production notification with the real KMessage container.
#include <util/KMessage.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using BPrivate::KMessage;

extern "C" void debugger(const char*) { std::abort(); }

struct image {
	struct {
		struct { int32 id; } basic_info;
	} info;
};

static constexpr uint32 IMAGE_MONITOR = 0x494d4f4e;
static image* sExpectedImage;
static uint32 sExpectedEvent;
static int sDelivered;

class DefaultNotificationService {
public:
	explicit DefaultNotificationService(const char* name)
	{
		assert(strcmp(name, "images") == 0);
	}

	void Notify(const KMessage& event, uint32 eventCode)
	{
		assert(event.What() == IMAGE_MONITOR && eventCode == sExpectedEvent);
		assert(event.GetInt32("event", -1) == int32(sExpectedEvent));
		assert(event.GetInt32("image", -1) == sExpectedImage->info.basic_info.id);
		// This is the pointer consumed by SystemProfiler::EventOccurred().
		assert(event.GetPointer("imageStruct", nullptr) == sExpectedImage);
		assert(event.ContentSize() == 124 + int32(sizeof(void*)));
		++sDelivered;
	}
};

#include "image_notification.inc"

int main()
{
	static_assert(sizeof(void*) == 8, "Exercise the 64-bit notification layout");
	image first = {{{17}}}, second = {{{INT32_MAX}}};
	assert(uintptr_t(&first) > UINT32_MAX);
	ImageNotificationService service;
	for (image* target : {&first, &second}) {
		sExpectedImage = target;
		for (uint32 event : {uint32(1), uint32(2)}) {
			sExpectedEvent = event;
			service.Notify(event, target);
		}
	}
	assert(sDelivered == 4);

	// Retain an independent negative control for the old buffer capacity.
	alignas(8) char oldBuffer[128] = {};
	KMessage old;
	assert(old.SetTo(oldBuffer, sizeof(oldBuffer), IMAGE_MONITOR) == B_OK);
	assert(old.AddInt32("event", 1) == B_OK);
	assert(old.AddInt32("image", first.info.basic_info.id) == B_OK);
	assert(old.AddPointer("imageStruct", &first) == B_BUFFER_OVERFLOW);
	assert(old.ContentSize() == 124);
	assert(old.GetPointer("imageStruct", nullptr) == nullptr);

	// A four-byte pointer payload fits the original 32-bit message layout.
	KMessage narrow;
	assert(narrow.SetTo(oldBuffer, sizeof(oldBuffer), IMAGE_MONITOR) == B_OK);
	assert(narrow.AddInt32("event", 1) == B_OK);
	assert(narrow.AddInt32("image", first.info.basic_info.id) == B_OK);
	uint32 pointerValue = 0xfedcba98;
	assert(narrow.AddData("imageStruct", B_POINTER_TYPE, &pointerValue,
		sizeof(pointerValue), true) == B_OK);
	const void* bytes;
	int32 size;
	assert(narrow.FindData("imageStruct", B_POINTER_TYPE, &bytes, &size) == B_OK);
	assert(size == 4 && memcmp(bytes, &pointerValue, size) == 0);
	assert(narrow.ContentSize() == 128);

	// Serialized fields and the external buffer may be only four-byte aligned.
	for (size_t displacement : {size_t(0), size_t(4)}) {
		alignas(8) char serialized[260] = {};
		KMessage scalars;
		assert(scalars.SetTo(serialized + displacement, 256, IMAGE_MONITOR) == B_OK);
		const int64 values[] = {INT64_MIN + 19, INT64_MAX - 7};
		assert(scalars.AddArray("x", B_INT64_TYPE, values, sizeof(int64), 2) == B_OK);
		for (int32 i = 0; i < 2; ++i) {
			int64 value = 0;
			assert(scalars.FindInt64("x", i, &value) == B_OK && value == values[i]);
		}
		int64 unchanged = 23;
		assert(scalars.FindInt64("x", 2, &unchanged) != B_OK && unchanged == 23);
		int32 wrongSize = 29;
		assert(scalars.FindInt32("x", &wrongSize) != B_OK && wrongSize == 29);
	}
	puts("Image notification: 64-bit add/remove delivery and old-capacity rejection passed");
}
