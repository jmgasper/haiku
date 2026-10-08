/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <AuxDisplay.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <Bitmap.h>
#include <String.h>
#include <View.h>

#include <auxdisplay.h>


struct BAuxDisplay::Buffer {
	area_id	area;
	void*	address;
};


/*!	Walks AUX_DISPLAY_DIRECTORY depth-first; devices in order of their
	paths. */
static void
collect_devices(const char* directory, BString* paths, int32& count,
	int32 capacity, int depth)
{
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return;

	while (struct dirent* entry = readdir(dir)) {
		if (entry->d_name[0] == '.')
			continue;
		BString path(directory);
		path << "/" << entry->d_name;
		struct stat st;
		if (stat(path.String(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 3)
				collect_devices(path.String(), paths, count, capacity, depth + 1);
		} else if (count < capacity)
			paths[count++] = path;
	}
	closedir(dir);
}


static const int32 kMaxDisplays = 16;


//	#pragma mark -


BAuxDisplay::BAuxDisplay()
	:
	fDevice(-1),
	fStatus(B_NO_INIT),
	fFlags(0),
	fWidth(0),
	fHeight(0),
	fBytesPerRow(0),
	fColorSpace(B_RGB32),
	fRefreshRate(0),
	fBufferCount(0),
	fBufferSize(0),
	fBuffers(NULL),
	fNext(0),
	fBitmap(NULL)
{
	fPath[0] = '\0';
	fName[0] = '\0';
}


BAuxDisplay::~BAuxDisplay()
{
	Unset();
}


/*static*/ int32
BAuxDisplay::CountDisplays()
{
	BString paths[kMaxDisplays];
	int32 count = 0;
	collect_devices(AUX_DISPLAY_DIRECTORY, paths, count, kMaxDisplays, 0);
	return count;
}


/*static*/ status_t
BAuxDisplay::GetDisplayPath(int32 index, BString& path)
{
	BString paths[kMaxDisplays];
	int32 count = 0;
	collect_devices(AUX_DISPLAY_DIRECTORY, paths, count, kMaxDisplays, 0);
	if (index < 0 || index >= count)
		return B_BAD_INDEX;
	path = paths[index];
	return B_OK;
}


status_t
BAuxDisplay::SetTo(int32 index)
{
	BString path;
	status_t status = GetDisplayPath(index, path);
	if (status != B_OK) {
		Unset();
		fStatus = status;
		return status;
	}
	return SetTo(path.String());
}


status_t
BAuxDisplay::SetTo(const char* devicePath)
{
	Unset();
	fStatus = _Open(devicePath);
	if (fStatus != B_OK)
		Unset();
	return fStatus;
}


void
BAuxDisplay::Unset()
{
	delete fBitmap;
	fBitmap = NULL;
	_UnmapBuffers();
	if (fDevice >= 0)
		close(fDevice);
	fDevice = -1;
	fStatus = B_NO_INIT;
	fPath[0] = '\0';
	fName[0] = '\0';
	fFlags = 0;
	fWidth = fHeight = fBytesPerRow = 0;
	fBufferCount = fBufferSize = 0;
	fNext = 0;
}


status_t
BAuxDisplay::InitCheck() const
{
	return fStatus;
}


const char*
BAuxDisplay::Name() const
{
	return fName;
}


const char*
BAuxDisplay::DevicePath() const
{
	return fPath;
}


int32
BAuxDisplay::Width() const
{
	return fWidth;
}


int32
BAuxDisplay::Height() const
{
	return fHeight;
}


BRect
BAuxDisplay::Bounds() const
{
	return BRect(0, 0, fWidth - 1, fHeight - 1);
}


color_space
BAuxDisplay::ColorSpace() const
{
	return fColorSpace;
}


float
BAuxDisplay::RefreshRate() const
{
	return fRefreshRate / 1000.0f;
}


bool
BAuxDisplay::HasTouch() const
{
	return (fFlags & AUX_DISPLAY_FLAG_TOUCH) != 0;
}


bool
BAuxDisplay::HasBacklight() const
{
	return (fFlags & AUX_DISPLAY_FLAG_BACKLIGHT) != 0;
}


bool
BAuxDisplay::IsOn() const
{
	if (fDevice < 0)
		return false;
	aux_display_info info;
	if (ioctl(fDevice, AUX_DISPLAY_GET_INFO, &info, sizeof(info)) != 0)
		return false;
	return (info.flags & AUX_DISPLAY_FLAG_ON) != 0;
}


status_t
BAuxDisplay::SetPower(bool on)
{
	if (fDevice < 0)
		return fStatus;
	uint32 value = on ? 1 : 0;
	if (ioctl(fDevice, AUX_DISPLAY_SET_POWER, &value, sizeof(value)) != 0)
		return errno;
	return B_OK;
}


status_t
BAuxDisplay::SetBacklight(float brightness)
{
	if (fDevice < 0)
		return fStatus;
	if ((fFlags & AUX_DISPLAY_FLAG_BACKLIGHT) == 0)
		return B_NOT_SUPPORTED;
	if (brightness < 0)
		brightness = 0;
	if (brightness > 1)
		brightness = 1;
	uint32 percent = (uint32)(brightness * 100 + 0.5f);
	if (ioctl(fDevice, AUX_DISPLAY_SET_BACKLIGHT, &percent, sizeof(percent))
			!= 0) {
		return errno;
	}
	return B_OK;
}


BBitmap*
BAuxDisplay::Bitmap()
{
	if (fDevice < 0)
		return NULL;
	if (fBitmap == NULL) {
		fBitmap = new(std::nothrow) BBitmap(Bounds(), B_RGB32, true);
		if (fBitmap != NULL && fBitmap->InitCheck() != B_OK) {
			delete fBitmap;
			fBitmap = NULL;
		}
	}
	return fBitmap;
}


status_t
BAuxDisplay::Present()
{
	if (fBitmap == NULL)
		return B_NO_INIT;
	return Present(fBitmap);
}


status_t
BAuxDisplay::Present(const BBitmap* bitmap)
{
	if (fDevice < 0)
		return fStatus;
	if (bitmap == NULL || bitmap->InitCheck() != B_OK)
		return B_BAD_VALUE;
	if ((fFlags & AUX_DISPLAY_FLAG_BUFFERS) == 0)
		return B_NOT_SUPPORTED;

	const BBitmap* source = bitmap;
	BBitmap* converted = NULL;
	BRect bounds = bitmap->Bounds();
	if (bitmap->ColorSpace() != B_RGB32 && bitmap->ColorSpace() != B_RGBA32) {
		converted = new(std::nothrow) BBitmap(bounds, B_RGB32);
		if (converted == NULL || converted->InitCheck() != B_OK
			|| converted->ImportBits(bitmap) != B_OK) {
			delete converted;
			return B_NO_MEMORY;
		}
		source = converted;
	}
	if ((int32)bounds.IntegerWidth() + 1 != fWidth
		|| (int32)bounds.IntegerHeight() + 1 != fHeight) {
		// scale through a view
		BBitmap* scaled = new(std::nothrow) BBitmap(Bounds(), B_RGB32, true);
		if (scaled == NULL || scaled->InitCheck() != B_OK) {
			delete scaled;
			delete converted;
			return B_NO_MEMORY;
		}
		BView* view = new BView(Bounds(), "scale", B_FOLLOW_NONE, 0);
		scaled->AddChild(view);
		scaled->Lock();
		view->DrawBitmap(source, source->Bounds(), Bounds());
		view->Sync();
		scaled->Unlock();
		delete converted;
		converted = scaled;
		source = scaled;
	}

	if (fBuffers == NULL) {
		status_t status = _MapBuffers();
		if (status != B_OK) {
			delete converted;
			return status;
		}
	}

	// the buffer not being shown
	uint32 index = fNext;
	uint8* target = (uint8*)fBuffers[index].address;
	const uint8* bits = (const uint8*)source->Bits();
	int32 sourceRow = source->BytesPerRow();
	int32 rowBytes = fWidth * 4;
	// a bitmap that accepts views is locked while its bits are read
	BBitmap* lockable = const_cast<BBitmap*>(source);
	bool locked = lockable->Lock();
	for (int32 y = 0; y < fHeight; y++)
		memcpy(target + y * fBytesPerRow, bits + y * sourceRow, rowBytes);
	if (locked)
		lockable->Unlock();
	delete converted;

	if (ioctl(fDevice, AUX_DISPLAY_PRESENT, &index, sizeof(index)) != 0)
		return errno;
	fNext = (index + 1) % fBufferCount;
	return B_OK;
}


status_t
BAuxDisplay::WaitForTouch(aux_display_touch_event* events, int32* _count,
	bigtime_t timeout)
{
	if (fDevice < 0)
		return fStatus;
	if ((fFlags & AUX_DISPLAY_FLAG_TOUCH) == 0)
		return B_NOT_SUPPORTED;
	if (events == NULL || _count == NULL || *_count < 1)
		return B_BAD_VALUE;

	aux_display_touch_events request;
	request.timeout = timeout;
	request.count = *_count > AUX_DISPLAY_MAX_TOUCH_EVENTS
		? AUX_DISPLAY_MAX_TOUCH_EVENTS : *_count;
	if (ioctl(fDevice, AUX_DISPLAY_WAIT_TOUCH, &request, sizeof(request))
			!= 0) {
		return errno;
	}
	memcpy(events, request.events, request.count * sizeof(events[0]));
	*_count = request.count;
	return B_OK;
}


//	#pragma mark - private


status_t
BAuxDisplay::_Open(const char* path)
{
	if (path == NULL)
		return B_BAD_VALUE;
	int device = open(path, O_RDWR | O_CLOEXEC);
	if (device < 0)
		return errno;

	aux_display_info info;
	if (ioctl(device, AUX_DISPLAY_GET_INFO, &info, sizeof(info)) != 0) {
		status_t status = errno;
		close(device);
		return status == B_OK ? B_ERROR : status;
	}
	if (info.version != AUX_DISPLAY_API_VERSION || info.width == 0
		|| info.height == 0) {
		close(device);
		return B_MISMATCHED_VALUES;
	}

	fDevice = device;
	strlcpy(fPath, path, sizeof(fPath));
	strlcpy(fName, info.name, sizeof(fName));
	fFlags = info.flags;
	fWidth = info.width;
	fHeight = info.height;
	fBytesPerRow = info.bytes_per_row;
	fColorSpace = (color_space)info.color_space;
	fRefreshRate = info.refresh_rate;
	fBufferCount = info.buffer_count;
	fBufferSize = info.buffer_size;
	fNext = 0;
	return B_OK;
}


status_t
BAuxDisplay::_MapBuffers()
{
	if (fBufferCount == 0 || fBufferCount > 8)
		return B_NOT_SUPPORTED;
	fBuffers = new(std::nothrow) Buffer[fBufferCount];
	if (fBuffers == NULL)
		return B_NO_MEMORY;
	for (uint32 i = 0; i < fBufferCount; i++) {
		aux_display_buffer request = {};
		request.index = i;
		if (ioctl(fDevice, AUX_DISPLAY_CLONE_BUFFER, &request, sizeof(request))
				!= 0) {
			status_t status = errno;
			for (uint32 j = 0; j < i; j++)
				delete_area(fBuffers[j].area);
			delete[] fBuffers;
			fBuffers = NULL;
			return status;
		}
		fBuffers[i].area = request.area;
		fBuffers[i].address = request.address;
	}
	return B_OK;
}


void
BAuxDisplay::_UnmapBuffers()
{
	if (fBuffers == NULL)
		return;
	for (uint32 i = 0; i < fBufferCount; i++)
		delete_area(fBuffers[i].area);
	delete[] fBuffers;
	fBuffers = NULL;
}
