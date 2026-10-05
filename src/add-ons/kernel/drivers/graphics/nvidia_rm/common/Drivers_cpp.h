#pragma once

#include <new>

#include <drivers/Drivers.h>

#include "ContainerOf.h"


class DevfsNodeHandle;

class DevfsNode {
public:
	const device_hooks* fCls;

public:
	DevfsNode(const device_hooks* cls = nullptr): fCls(cls) {}

	status_t Open(const char *path, uint32 flags, DevfsNodeHandle& handle);
};

class DevfsNodeInstance {
public:
	const device_hooks* fCls;
	void* fInst;

public:
	DevfsNodeInstance(const device_hooks* cls = nullptr, void* inst = nullptr): fCls(cls), fInst(inst) {}
};


class DevfsNodeHandle {
public:
	const device_hooks* fCls;
	void* fInst;

public:
	DevfsNodeHandle(const device_hooks* cls = nullptr, void* inst = nullptr): fCls(cls), fInst(inst) {}

	status_t Close();
	status_t Free();

	struct Read {
		operator bool();
		status_t operator()(off_t pos, void *buffer, size_t *_length);
	} Read [[no_unique_address]];

	struct Write {
		operator bool();
		status_t operator()(off_t pos, const void *buffer, size_t *_length);
	} Write [[no_unique_address]];

	struct Control {
		operator bool();
		status_t operator()(uint32 op, void *buffer, size_t length);
	} Control [[no_unique_address]];

	struct Select {
		operator bool();
		status_t operator()(uint8 event, uint32 ref, selectsync *sync);
	} Select [[no_unique_address]];

	struct Deselect {
		operator bool();
		status_t operator()(uint8 event, selectsync *sync);
	} Deselect [[no_unique_address]];

};

static_assert(sizeof(DevfsNodeHandle) == 2*sizeof(void*));


// #pragma mark -

template <typename DriverImpl, typename DevfsNodeImpl, typename DevfsNodeHandleImpl>
constexpr device_hooks GetDevfsNodeClass()
{
	device_hooks cls {};
	cls.open = [](const char *name, uint32 flags, void **cookie) {
		DevfsNodeInstance node = DriverImpl::FindDevice(name);
		if (node.fCls == nullptr) {
			return ENOENT;
		}
		DevfsNodeHandle handle;
		status_t res = static_cast<DevfsNodeImpl*>(node.fInst)->Open(flags, handle);
		*cookie = handle.fInst;
		return res;
	};
	cls.close = [](void *cookie) {
		return static_cast<DevfsNodeHandleImpl*>(cookie)->Close();
	};
	cls.free = [](void *cookie) {
		delete static_cast<DevfsNodeHandleImpl*>(cookie);
		return B_OK;
	};
	if constexpr (requires(DevfsNodeHandleImpl h, off_t pos, void *buffer, size_t *_length) {
		h.Read(pos, buffer, _length);
	}) {
		cls.read = [](void *cookie, off_t pos, void *buffer, size_t *_length) {
			return static_cast<DevfsNodeHandleImpl*>(cookie)->Read(pos, buffer, _length);
		};
	}
	if constexpr (requires(DevfsNodeHandleImpl h, off_t pos, const void *buffer, size_t *_length) {
		h.Write(pos, buffer, _length);
	}) {
		cls.write = [](void *cookie, off_t pos, const void *buffer, size_t *_length) {
			return static_cast<DevfsNodeHandleImpl*>(cookie)->Write(pos, buffer, _length);
		};
	}
	if constexpr (requires(DevfsNodeHandleImpl h, uint32 op, void *buffer, size_t length) {
		h.Control(op, buffer, length);
	}) {
		cls.control = [](void *cookie, uint32 op, void *buffer, size_t length) {
			return static_cast<DevfsNodeHandleImpl*>(cookie)->Control(op, buffer, length);
		};
	}
	if constexpr (requires(DevfsNodeHandleImpl h, uint8 event, uint32 ref, selectsync *sync) {
		h.Select(event, ref, sync);
	}) {
		cls.select = [](void *cookie, uint8 event, uint32 ref, selectsync *sync) {
			return static_cast<DevfsNodeHandleImpl*>(cookie)->Select(event, ref, sync);
		};
	}
	if constexpr (requires(DevfsNodeHandleImpl h, uint8 event, selectsync *sync) {
		h.Deselect(event, sync);
	}) {
		cls.deselect = [](void *cookie, uint8 event, selectsync *sync) {
			return static_cast<DevfsNodeHandleImpl*>(cookie)->Deselect(event, sync);
		};
	}
	return cls;
}


// #pragma mark - DevfsNode

inline status_t DevfsNode::Open(const char *path, uint32 flags, DevfsNodeHandle& handle)
{
	handle.fCls = fCls;
	return fCls->open(path, flags, &handle.fInst);
}


// #pragma mark - DevfsNodeHandle

inline status_t DevfsNodeHandle::Close()
{
	return fCls->close(fInst);
}

inline status_t DevfsNodeHandle::Free()
{
	status_t res = fCls->free(fInst);
	fInst = nullptr;
	return res;
}

inline DevfsNodeHandle::Read::operator bool()
{
	DevfsNodeHandle& base = ContainerOf(*this, &DevfsNodeHandle::Read);
	return base.fCls->read != nullptr;
}

inline status_t DevfsNodeHandle::Read::operator()(off_t pos, void *buffer, size_t *_length)
{
	DevfsNodeHandle& base = ContainerOf(*this, &DevfsNodeHandle::Read);
	return base.fCls->read(base.fInst, pos, buffer, _length);
}

inline DevfsNodeHandle::Write::operator bool()
{
	DevfsNodeHandle& base = ContainerOf(*this, &DevfsNodeHandle::Write);
	return base.fCls->write != nullptr;
}

inline status_t DevfsNodeHandle::Write::operator()(off_t pos, const void *buffer, size_t *_length)
{
	DevfsNodeHandle& base = ContainerOf(*this, &DevfsNodeHandle::Write);
	return base.fCls->write(base.fInst, pos, buffer, _length);
}

inline DevfsNodeHandle::Control::operator bool()
{
	DevfsNodeHandle& base = ContainerOf(*this, &DevfsNodeHandle::Control);
	return base.fCls->control != nullptr;
}

inline status_t DevfsNodeHandle::Control::operator()(uint32 op, void *buffer, size_t length)
{
	DevfsNodeHandle& base = ContainerOf(*this, &DevfsNodeHandle::Control);
	return base.fCls->control(base.fInst, op, buffer, length);
}

inline DevfsNodeHandle::Select::operator bool()
{
	DevfsNodeHandle& base = ContainerOf(*this, &DevfsNodeHandle::Select);
	return base.fCls->select != nullptr;
}

inline status_t DevfsNodeHandle::Select::operator()(uint8 event, uint32 ref, selectsync *sync)
{
	DevfsNodeHandle& base = ContainerOf(*this, &DevfsNodeHandle::Select);
	return base.fCls->select(base.fInst, event, ref, sync);
}

inline DevfsNodeHandle::Deselect::operator bool()
{
	DevfsNodeHandle& base = ContainerOf(*this, &DevfsNodeHandle::Deselect);
	return base.fCls->deselect != nullptr;
}

inline status_t DevfsNodeHandle::Deselect::operator()(uint8 event, selectsync *sync)
{
	DevfsNodeHandle& base = ContainerOf(*this, &DevfsNodeHandle::Deselect);
	return base.fCls->deselect(base.fInst, event, sync);
}


#define DRIVER_ENTRY_POINTS(DriverImpl) \
\
_EXPORT int32 api_version = B_CUR_DRIVER_API_VERSION; \
\
_EXPORT status_t init_hardware() \
{ \
	return B_OK; \
} \
\
_EXPORT status_t init_driver() \
{ \
	return DriverImpl::InitDriver(); \
} \
\
_EXPORT void uninit_driver() \
{ \
	DriverImpl::UninitDriver(); \
} \
\
_EXPORT const char **publish_devices() \
{ \
	return DriverImpl::PublishDevices(); \
} \
\
_EXPORT device_hooks *find_device(const char *name) \
{ \
	return const_cast<device_hooks*>(DriverImpl::FindDevice(name).fCls); \
}
