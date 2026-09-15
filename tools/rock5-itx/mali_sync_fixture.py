"""Compile real sync/FD code with only OS scheduling, copying and storage replaced."""

def prepare_sync_fixture(root, repository):
    source = repository / 'src/add-ons/kernel/drivers/graphics/mali_csf'
    sync = (source / 'CsfSync.cpp').read_text()
    (root / 'sync.inc').write_text(sync[sync.index('using namespace MaliCSF;'):])
    fd = (repository / 'headers/private/kernel/fs/fd.h').read_text()
    (root / 'fd_structures.inc').write_text(
        fd[fd.index('struct fd_ops {'):fd.index('/* Prototypes */')])
    fd = (repository / 'src/system/kernel/fs/fd.cpp').read_text()
    start = fd.index('struct file_descriptor*\nalloc_fd(void)')
    end = fd.index('/*!\tDecrements the open counter', start)
    (root / 'fd_core.inc').write_text(fd[start:end])
