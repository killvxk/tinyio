#include "io.h"

#if IO_PLATFORM_LINUX

#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/syscall.h>

static int 
io_uring_setup(unsigned entries,
               struct io_uring_params *p)
{
    return (int) syscall(SYS_io_uring_setup, entries, p);
}

static int
io_uring_enter(int ring_fd, unsigned int to_submit,
               unsigned int min_complete, unsigned int flags)
{
    return (int) syscall(SYS_io_uring_enter, ring_fd, to_submit, 
                         min_complete, flags, NULL, 0);
}

bool io_context_init(struct io_context *ioc,
                     struct io_operation *ops,
                     uint32_t max_ops)
{
    ioc->ops = ops;
    ioc->max_ops = max_ops;

    for (uint32_t i = 0; i < max_ops; i++)
        ioc->ops[i].type = IO_VOID;

    struct io_uring_params p;
    void *sq_ptr, *cq_ptr;
    /* See io_uring_setup(2) for io_uring_params.flags you can set */
    memset(&p, 0, sizeof(p));
    int fd = io_uring_setup(32, &p);
    if (fd < 0)
        return false;

    ioc->handle = fd;

    /*
     * io_uring communication happens via 2 shared kernel-user space ring
     * buffers, which can be jointly mapped with a single mmap() call in
     * kernels >= 5.4.
     */
    int sring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    int cring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    /* Rather than check for kernel version, the recommended way is to
     * check the features field of the io_uring_params structure, which is a 
     * bitmask. If IORING_FEAT_SINGLE_MMAP is set, we can do away with the
     * second mmap() call to map in the completion ring separately.
     */
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        if (cring_sz > sring_sz)
            sring_sz = cring_sz;
        cring_sz = sring_sz;
    }
    /* Map in the submission and completion queue ring buffers.
     *  Kernels < 5.4 only map in the submission queue, though.
     */
    sq_ptr = mmap(0, sring_sz, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) {
        // TODO: Cleanup
        return false;
    }
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        cq_ptr = sq_ptr;
    } else {
        /* Map in the completion queue ring buffer in older kernels separately */
        cq_ptr = mmap(0, cring_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
        if (cq_ptr == MAP_FAILED) {
            // TODO: Cleanup
            return false;
        }
    }

    /* Save useful fields for later easy reference */
    ioc->submissions.head = (_Atomic unsigned*) (sq_ptr + p.sq_off.head);
    ioc->submissions.tail = (_Atomic unsigned*) (sq_ptr + p.sq_off.tail);
    ioc->submissions.mask = (unsigned*) (sq_ptr + p.sq_off.ring_mask);
    ioc->submissions.array = sq_ptr + p.sq_off.array;
    ioc->submissions.limit = p.sq_entries;

    /* Map in the submission queue entries array */
    ioc->submissions.entries = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe),
                                    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                                    fd, IORING_OFF_SQES);
    if (ioc->submissions.entries == MAP_FAILED) {
        // TODO: Cleanup
        return false;
    }

    /* Save useful fields for later easy reference */
    ioc->completions.head = cq_ptr + p.cq_off.head;
    ioc->completions.tail = cq_ptr + p.cq_off.tail;
    ioc->completions.mask = cq_ptr + p.cq_off.ring_mask;
    ioc->completions.entries = cq_ptr + p.cq_off.cqes;
    ioc->completions.limit = p.cq_entries;
    return true;
}

void io_context_free(struct io_context *ioc)
{
    close(ioc->handle);
}

static bool start_oper(struct io_context *ioc,
                       struct io_uring_sqe sqe)
{
    unsigned int mask = *ioc->submissions.mask;
    unsigned int tail = atomic_load(ioc->submissions.tail);
    unsigned int head = atomic_load(ioc->submissions.head);

    if (tail >= head + ioc->submissions.limit)
        return false;

    unsigned int index = tail & mask;
    ioc->submissions.entries[index] = sqe;
    ioc->submissions.array[index] = index;

    atomic_store(ioc->submissions.tail, tail+1);

    int ret = io_uring_enter(ioc->handle, 1, 0, 0);
    if (ret < 0)
        return false;
    
    return true;
}

static struct io_operation *alloc_op(struct io_context *ioc)
{
    for (uint32_t i = 0; i < ioc->max_ops; i++)
        if (ioc->ops[i].type == IO_VOID)
            return &ioc->ops[i];
    return NULL;
}

bool io_start_recv(struct io_context *ioc, io_handle handle,
                   void *dst, uint32_t max, void *user)
{
    struct io_operation *op;
    struct io_uring_sqe sqe;

    op = alloc_op(ioc);
    if (op == NULL)
        return false;

    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = IORING_OP_READ;
    sqe.fd   = (int) handle;
    sqe.addr = (uint64_t) dst;
    sqe.len  = max;
    sqe.user_data = (uint64_t) op;
    
    if (!start_oper(ioc, sqe))
        return false;
    
    op->user = user;
    op->type = IO_RECV; // Commit operation structure
    return true;
}

bool io_start_send(struct io_context *ioc, io_handle handle,
                   void *src, uint32_t num, void *user)
{
    struct io_operation *op;
    struct io_uring_sqe sqe;

    op = alloc_op(ioc);
    if (op == NULL)
        return false;

    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = IORING_OP_WRITE;
    sqe.fd   = (int) handle;
    sqe.addr = (uint64_t) src;
    sqe.len  = num;
    sqe.user_data = (uint64_t) op;
   
    if (!start_oper(ioc, sqe))
        return false;
    
    op->user = user;
    op->type = IO_SEND; // Commit operation structure
    return true;
}

bool io_start_accept(struct io_context *ioc, io_handle handle, void *user)
{
    struct io_operation *op;
    struct io_uring_sqe sqe;

    op = alloc_op(ioc);
    if (op == NULL)
        return false;

    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = IORING_OP_ACCEPT;
    sqe.fd = (int) handle;
    sqe.user_data = (uint64_t) op;

    if (!start_oper(ioc, sqe))
        return false;

    op->user = user;
    op->type = IO_ACCEPT; // Commit operation structure
    return true;
}

void io_wait(struct io_context *ioc, struct io_event *ev)
{
    /* --- Read barrier --- */
    unsigned int head = atomic_load(ioc->completions.head);
    unsigned int tail = atomic_load(ioc->completions.tail);

    if (head == tail) {
        
        /*
         * Completion queue is empty. Wait for some operations to complete.
         */
        int ret = io_uring_enter(ioc->handle, 0, 1, IORING_ENTER_GETEVENTS);
        if (ret < 0) {
            ev->error = true;
            return;
        }
    }

    struct io_uring_cqe *cqe;
    struct io_operation *op;

    cqe = &ioc->completions.entries[head & (*ioc->completions.mask)];

    op = (void*) cqe->user_data;
    ev->user = op->user;
    ev->type = op->type;
    ev->error = cqe->res < 0;

    if (ev->error == false) {
        switch (op->type) {
            case IO_VOID: /* UNREACHABLE */ break;
            case IO_RECV: ev->num = cqe->res; break;
            case IO_SEND: ev->num = cqe->res; break;
            case IO_ACCEPT: ev->handle = cqe->res; break;
        }
    }

    op->type = IO_VOID; // Mark unused

    /* --- write barrier --- */
    atomic_store(ioc->completions.head, head+1);
}

#endif
