/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	The Linux API of linux_compat.h that needs no Haiku C++: logging,
	errors, drm_mm, the minimal dma_fence, drmm allocations, debugfs,
	seq_file and module parameters. linux_compat_haiku.cpp has the rest. */


#include "linux_compat.h"


const volatile void* volatile lx_mmio_trace_base;
u64 lx_dma_mask = DMA_BIT_MASK(32);


status_t
lx_status(int error)
{
	switch (error) {
		case 0:
			return B_OK;
		case -ENOMEM:
			return B_NO_MEMORY;
		case -ETIMEDOUT:
			return B_TIMED_OUT;
		case -EINVAL:
			return B_BAD_VALUE;
		case -EIO:
			return B_IO_ERROR;
		case -EBUSY:
			return B_BUSY;
		case -ENODEV:
			return B_DEV_NOT_READY;
		case -ENOENT:
			return B_ENTRY_NOT_FOUND;
		case -ENOSPC:
			return B_DEVICE_FULL;
		case -EFAULT:
			return B_BAD_ADDRESS;
		case -EPERM:
			return B_NOT_ALLOWED;
		case -E2BIG:
		case -ERANGE:
		case -EOVERFLOW:
			return B_RESULT_NOT_REPRESENTABLE;
		default:
			return error < 0 ? B_ERROR : B_OK;
	}
}


/* #pragma mark - printing */


void
lx_log(int level, const char* format, ...)
{
	char buffer[256];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (length < 0)
		return;

	// Linux messages end in a newline, mostly; dprintf() lines must.
	size_t end = strnlen(buffer, sizeof(buffer));
	while (end > 0 && buffer[end - 1] == '\n')
		buffer[--end] = '\0';

	const char* prefix = "";
	if (level == LX_LOG_ERROR)
		prefix = "error: ";
	else if (level == LX_LOG_WARNING)
		prefix = "warning: ";
	dprintf("powervr: %s%s\n", prefix, buffer);
}


void
lx_warn_on(const char* file, int line, const char* condition)
{
	const char* name = strrchr(file, '/');
	dprintf("powervr: warning: %s at %s:%d\n", condition,
		name != NULL ? name + 1 : file, line);
}


/* #pragma mark - DRM device */


struct lx_managed {
	struct list_head	link;
	u64					data[];
};


void
lx_drm_dev_init(struct drm_device* drm, struct device* device)
{
	drm->dev = device;
	INIT_LIST_HEAD(&drm->managed);
}


void
lx_drm_dev_release(struct drm_device* drm)
{
	while (!list_empty(&drm->managed)) {
		struct lx_managed* allocation = list_first_entry(&drm->managed,
			struct lx_managed, link);
		list_del(&allocation->link);
		free(allocation);
	}
}


void*
drmm_kzalloc(struct drm_device* drm, size_t size, gfp_t flags)
{
	(void)flags;
	struct lx_managed* allocation = (struct lx_managed*)calloc(1,
		sizeof(struct lx_managed) + size);
	if (allocation == NULL)
		return NULL;
	list_add_tail(&allocation->link, &drm->managed);
	return allocation->data;
}


/* #pragma mark - drm_mm */


void
drm_mm_init(struct drm_mm* mm, u64 start, u64 size)
{
	mm->start = start;
	mm->size = size;
	INIT_LIST_HEAD(&mm->nodes);
}


void
drm_mm_takedown(struct drm_mm* mm)
{
	if (!list_empty(&mm->nodes))
		lx_log(LX_LOG_WARNING, "drm_mm_takedown: nodes left in the heap");
	INIT_LIST_HEAD(&mm->nodes);
}


/*!	Links \a node into the sorted node list, in front of \a next (or at
	the end, if \a next is the list head).
*/
static void
drm_mm_link(struct drm_mm* mm, struct drm_mm_node* node,
	struct list_head* next)
{
	(void)mm;
	list_add_tail(&node->link, next);
	node->allocated = true;
}


int
drm_mm_insert_node_in_range(struct drm_mm* mm, struct drm_mm_node* node,
	u64 size, u64 alignment, unsigned long color, u64 rangeStart,
	u64 rangeEnd, enum drm_mm_insert_mode mode)
{
	(void)color;
	(void)mode;
	if (size == 0 || node->allocated)
		return -EINVAL;
	if (alignment == 0)
		alignment = 1;

	u64 start = max(rangeStart, mm->start);
	u64 end = min(rangeEnd, mm->start + mm->size);

	// first fit: walk the holes in front of each node, then the last one
	u64 holeStart = mm->start;
	struct list_head* position;
	for (position = mm->nodes.next;; position = position->next) {
		u64 holeEnd = mm->start + mm->size;
		if (position != &mm->nodes) {
			holeEnd = list_entry(position, struct drm_mm_node, link)->start;
		}

		u64 candidate = max(holeStart, start);
		candidate = (candidate + alignment - 1) / alignment * alignment;
		if (candidate + size > candidate
			&& candidate + size <= min(holeEnd, end)) {
			node->start = candidate;
			node->size = size;
			drm_mm_link(mm, node, position);
			return 0;
		}

		if (position == &mm->nodes)
			break;
		struct drm_mm_node* current = list_entry(position, struct drm_mm_node,
			link);
		holeStart = current->start + current->size;
	}
	return -ENOSPC;
}


int
drm_mm_reserve_node(struct drm_mm* mm, struct drm_mm_node* node)
{
	u64 end = node->start + node->size;
	if (node->size == 0 || node->allocated || end < node->start
		|| node->start < mm->start || end > mm->start + mm->size) {
		return -ENOSPC;
	}

	struct list_head* position;
	list_for_each(position, &mm->nodes) {
		struct drm_mm_node* current = list_entry(position, struct drm_mm_node,
			link);
		if (current->start >= end)
			break;
		if (current->start + current->size > node->start)
			return -ENOSPC;
	}
	drm_mm_link(mm, node, position);
	return 0;
}


void
drm_mm_remove_node(struct drm_mm_node* node)
{
	if (!node->allocated)
		return;
	list_del(&node->link);
	node->allocated = false;
}


/* #pragma mark - dma_fence */


u64
dma_fence_context_alloc(unsigned int count)
{
	static int64 sNextContext = 1;
	return (u64)atomic_add64(&sNextContext, count);
}


void
dma_fence_init(struct dma_fence* fence, const struct dma_fence_ops* ops,
	spinlock_t* lock, u64 context, u64 seqno)
{
	fence->ops = ops;
	fence->lock = lock;
	fence->context = context;
	fence->seqno = seqno;
	fence->flags = 0;
	kref_init(&fence->refcount);
}


void
dma_fence_free(struct dma_fence* fence)
{
	free(fence);
}


static void
dma_fence_release(struct kref* kref)
{
	struct dma_fence* fence = container_of(kref, struct dma_fence, refcount);
	if (fence->ops != NULL && fence->ops->release != NULL)
		fence->ops->release(fence);
	else
		dma_fence_free(fence);
}


void
dma_fence_put(struct dma_fence* fence)
{
	if (fence != NULL)
		kref_put(&fence->refcount, dma_fence_release);
}


int
dma_fence_signal(struct dma_fence* fence)
{
	if (fence == NULL)
		return -EINVAL;
	if (__atomic_fetch_or(&fence->flags, 1UL << DMA_FENCE_FLAG_SIGNALED_BIT,
			__ATOMIC_SEQ_CST) & (1UL << DMA_FENCE_FLAG_SIGNALED_BIT)) {
		return -EINVAL;
	}
	return 0;
}


/* #pragma mark - seq_file */


#define LX_SEQ_BUFFER_SIZE	1024


int
seq_open(struct file* file, const struct seq_operations* operations)
{
	struct seq_file* seq = (struct seq_file*)calloc(1, sizeof(*seq));
	if (seq == NULL)
		return -ENOMEM;
	seq->buffer = (char*)malloc(LX_SEQ_BUFFER_SIZE);
	if (seq->buffer == NULL) {
		free(seq);
		return -ENOMEM;
	}
	seq->size = LX_SEQ_BUFFER_SIZE;
	seq->op = operations;
	file->private_data = seq;
	return 0;
}


int
seq_release(struct inode* inode, struct file* file)
{
	(void)inode;
	struct seq_file* seq = (struct seq_file*)file->private_data;
	if (seq != NULL) {
		free(seq->buffer);
		free(seq);
	}
	file->private_data = NULL;
	return 0;
}


ssize_t
seq_read(struct file* file, char __user* buffer, size_t size,
	loff_t* position)
{
	// lx_debugfs_dump() drives the seq_file operations directly
	(void)file;
	(void)buffer;
	(void)size;
	(void)position;
	return -EINVAL;
}


loff_t
seq_lseek(struct file* file, loff_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return -EINVAL;
}


void
seq_printf(struct seq_file* file, const char* format, ...)
{
	if (file->count >= file->size)
		return;
	va_list args;
	va_start(args, format);
	int length = vsnprintf(file->buffer + file->count,
		file->size - file->count, format, args);
	va_end(args);
	if (length > 0)
		file->count = min(file->count + (size_t)length, file->size - 1);
}


void
seq_puts(struct seq_file* file, const char* text)
{
	seq_printf(file, "%s", text);
}


/* #pragma mark - debugfs */


#define LX_DEBUGFS_FILES	8

static struct {
	char							name[32];
	void*							data;
	const struct file_operations*	fops;
} sDebugFiles[LX_DEBUGFS_FILES];


struct dentry*
debugfs_create_file(const char* name, umode_t mode, struct dentry* parent,
	void* data, const struct file_operations* fops)
{
	(void)mode;
	(void)parent;
	for (int i = 0; i < LX_DEBUGFS_FILES; i++) {
		if (sDebugFiles[i].fops == NULL
			|| strcmp(sDebugFiles[i].name, name) == 0) {
			strlcpy(sDebugFiles[i].name, name, sizeof(sDebugFiles[i].name));
			sDebugFiles[i].data = data;
			sDebugFiles[i].fops = fops;
			return NULL;
		}
	}
	lx_log(LX_LOG_WARNING, "no room for debugfs file %s", name);
	return NULL;
}


void
lx_debugfs_remove_all(void)
{
	memset(sDebugFiles, 0, sizeof(sDebugFiles));
}


static int
lx_debugfs_find(const char* name)
{
	for (int i = 0; i < LX_DEBUGFS_FILES; i++) {
		if (sDebugFiles[i].fops != NULL
			&& strcmp(sDebugFiles[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}


/*!	Reads a seq_file based debugfs file the way seq_read() would, and
	hands each line of its output to \a line.
*/
int
lx_debugfs_dump(const char* name, void (*line)(void* cookie, const char* text),
	void* cookie, unsigned int maxLines)
{
	int index = lx_debugfs_find(name);
	if (index < 0)
		return -ENOENT;
	const struct file_operations* fops = sDebugFiles[index].fops;
	if (fops->open == NULL)
		return -EINVAL;

	struct inode inode = { sDebugFiles[index].data };
	struct file file = { NULL };
	int error = fops->open(&inode, &file);
	if (error != 0)
		return error;

	struct seq_file* seq = (struct seq_file*)file.private_data;
	unsigned int lines = 0;
	loff_t position = 0;
	void* entry = seq->op->start(seq, &position);
	while (entry != NULL && lines < maxLines) {
		seq->count = 0;
		if (seq->op->show(seq, entry) == 0) {
			seq->buffer[min(seq->count, seq->size - 1)] = '\0';
			// one show() may hold several lines
			char* text = seq->buffer;
			while (*text != '\0' && lines < maxLines) {
				char* end = strchr(text, '\n');
				if (end != NULL)
					*end = '\0';
				line(cookie, text);
				lines++;
				if (end == NULL)
					break;
				text = end + 1;
			}
		}
		entry = seq->op->next(seq, entry, &position);
	}
	seq->op->stop(seq, entry);

	if (fops->release != NULL)
		fops->release(&inode, &file);
	return (int)lines;
}


int
lx_debugfs_attr_set(const char* name, u64 value)
{
	int index = lx_debugfs_find(name);
	if (index < 0)
		return -ENOENT;
	if (sDebugFiles[index].fops->attr_set == NULL)
		return -EINVAL;
	return sDebugFiles[index].fops->attr_set(sDebugFiles[index].data, value);
}


/* #pragma mark - module parameters */


int
kstrtouint(const char* text, unsigned int base, unsigned int* _value)
{
	if (text == NULL || *text == '\0')
		return -EINVAL;
	char* end;
	unsigned long value = strtoul(text, &end, base);
	if (*end == '\n')
		end++;
	if (end == text || *end != '\0')
		return -EINVAL;
	if (value > UINT32_MAX)
		return -ERANGE;
	*_value = (unsigned int)value;
	return 0;
}


int
param_get_hexint(char* buffer, const struct kernel_param* parameter)
{
	return sprintf(buffer, "%#08x\n", *(unsigned int*)parameter->arg);
}
