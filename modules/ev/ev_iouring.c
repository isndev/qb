#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <poll.h>
#include <assert.h>
#include <stdio.h>

#define IOURING_QUEUE_DEPTH 64
#define USERDATA_REMOVE ((uint64_t)-2)

static inline int sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
  return syscall(__NR_io_uring_setup, entries, p);
}

static inline int sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                                     unsigned flags, void *sig, size_t sigsz) {
  return syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig, sigsz);
}

static struct io_uring_sqe *get_sqe(EV_P) {
  unsigned tail = *iouring_sq_tail;
  if ((tail - *iouring_sq_head) >= iouring_sq_ring_entries) return 0;
  return &((struct io_uring_sqe *)iouring_sqes)[tail & *iouring_sq_ring_mask];
}

static void submit_sqe(EV_P_ struct io_uring_sqe *sqe) {
  unsigned index = *iouring_sq_tail & *iouring_sq_ring_mask;
  iouring_sq_array[index] = index;
  (*iouring_sq_tail)++;
  ++iouring_to_submit;
}

static void iouring_tfd_cb(EV_P_ ev_io *w, int revents) {
  uint64_t val;
  read(iouring_tfd, &val, sizeof(val));
  iouring_tfd_to = EV_TSTAMP_HUGE;
}

static void iouring_modify(EV_P_ int fd, int oev, int nev)
{
  if (oev) {
    struct io_uring_sqe *sqe = get_sqe(EV_A);
    if (!sqe) return;

    sqe->opcode = IORING_OP_POLL_REMOVE;
    sqe->fd = fd;
    sqe->addr = (uint64_t)((uint32_t)fd | ((uint64_t)(uint32_t)anfds[fd].egen << 32));
    sqe->user_data = USERDATA_REMOVE;
    submit_sqe(EV_A_ sqe);
    ++anfds[fd].egen;
  }

  if (nev) {
    struct io_uring_sqe *sqe = get_sqe(EV_A);
    if (!sqe) return;

    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->poll_events = (nev & EV_READ ? POLLIN : 0) | (nev & EV_WRITE ? POLLOUT : 0);
    sqe->user_data = (uint32_t)fd | ((uint64_t)(uint32_t)anfds[fd].egen << 32);
    submit_sqe(EV_A_ sqe);
  }
}

static void iouring_poll(EV_P_ ev_tstamp timeout)
{
  if (timeout >= 0.) {
    ev_tstamp tfd_to = mn_now + timeout;

    if (tfd_to < iouring_tfd_to) {
      struct itimerspec its;
      iouring_tfd_to = tfd_to;
      EV_TS_SET(its.it_interval, 0.);
      EV_TS_SET(its.it_value, tfd_to);
      timerfd_settime(iouring_tfd, TFD_TIMER_ABSTIME, &its, 0);
    }
  }

  EV_RELEASE_CB;
  int ret = -1;
  if (iouring_to_submit > 0 || timeout >= 0.) {
    ret = sys_io_uring_enter(iouring_fd, iouring_to_submit, 0, 0, 0, 0);
  }
  EV_ACQUIRE_CB;

  if (ret < 0 && errno != EINTR)
    ev_syserr("io_uring_enter");

  iouring_to_submit = 0;

  unsigned head = *iouring_cq_head;
  unsigned tail = *iouring_cq_tail;
  unsigned mask = *iouring_cq_ring_mask;

  while (head != tail) {
    struct io_uring_cqe *cqe = &((struct io_uring_cqe *)((char *)iouring_cq_ring + iouring_cq_cqes))[head & mask];
    uint64_t user_data = cqe->user_data;

    if (user_data == USERDATA_REMOVE || user_data == 0) goto skip;

    int fd = (int)(user_data & 0xffffffffU);
    if (fd < 0 || fd >= anfdmax) goto skip;

    int gen = (int)(user_data >> 32);
    if ((uint32_t)anfds[fd].egen != (uint32_t)gen) goto skip;

    int res = cqe->res;
    if (res < 0) {
      if (res == -EBADF) {
        fprintf(stderr, "[iouring] EBADF on fd %d\n", fd);
        fd_kill(EV_A_ fd);
      } else {
        errno = -res;
        ev_syserr("(libev) io_uring poll error");
      }
      goto skip;
    }

    int ev = 0;
    if (res & POLLIN) ev |= EV_READ;
    if (res & POLLOUT) ev |= EV_WRITE;
    if (res & (POLLERR | POLLHUP)) ev |= (EV_READ | EV_WRITE);

    if (ev) {
      fprintf(stderr, "[iouring] fd_event: fd=%d ev=0x%x\n", fd, ev);
      anfds[fd].events = 0;
      fd_event(EV_A_ fd, ev);
    }

  skip:
    head++;
  }

  *iouring_cq_head = head;
}

inline_size int iouring_init(EV_P_ int flags)
{
  struct io_uring_params p;
  memset(&p, 0, sizeof(p));

  iouring_fd = sys_io_uring_setup(IOURING_QUEUE_DEPTH, &p);
  if (iouring_fd < 0)
    return 0;

  iouring_sq_ring_size = p.sq_off.array + p.sq_entries * sizeof(unsigned);
  iouring_cq_ring_size = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
  iouring_sqes_size    = p.sq_entries * sizeof(struct io_uring_sqe);
  iouring_sq_ring_entries = p.sq_entries;
  iouring_cq_ring_entries = p.cq_entries;

  iouring_sq_ring = mmap(0, iouring_sq_ring_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, iouring_fd, IORING_OFF_SQ_RING);
  iouring_cq_ring = mmap(0, iouring_cq_ring_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, iouring_fd, IORING_OFF_CQ_RING);
  iouring_sqes    = mmap(0, iouring_sqes_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, iouring_fd, IORING_OFF_SQES);

  if (iouring_sq_ring == MAP_FAILED || iouring_cq_ring == MAP_FAILED || iouring_sqes == MAP_FAILED)
    return 0;

  iouring_sq_head         = (unsigned *)((char *)iouring_sq_ring + p.sq_off.head);
  iouring_sq_tail         = (unsigned *)((char *)iouring_sq_ring + p.sq_off.tail);
  iouring_sq_ring_mask    = (unsigned *)((char *)iouring_sq_ring + p.sq_off.ring_mask);
  iouring_sq_array        = (unsigned *)((char *)iouring_sq_ring + p.sq_off.array);

  iouring_cq_head         = (unsigned *)((char *)iouring_cq_ring + p.cq_off.head);
  iouring_cq_tail         = (unsigned *)((char *)iouring_cq_ring + p.cq_off.tail);
  iouring_cq_ring_mask    = (unsigned *)((char *)iouring_cq_ring + p.cq_off.ring_mask);
  iouring_cq_cqes         = p.cq_off.cqes;

  iouring_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (iouring_tfd < 0)
    return 0;

  iouring_tfd_to = EV_TSTAMP_HUGE;

  ev_io_init(&iouring_tfd_w, iouring_tfd_cb, iouring_tfd, EV_READ);
  ev_set_priority(&iouring_tfd_w, EV_MINPRI);
  ev_io_start(EV_A_ &iouring_tfd_w);
  ev_unref(EV_A);

  iouring_to_submit = 0;

  backend_fd     = iouring_fd;
  backend_modify = iouring_modify;
  backend_poll   = iouring_poll;
  backend_mintime = EV_TS_CONST(1e-3);

  return EVBACKEND_IOURING;
}

inline_size void iouring_destroy(EV_P)
{
  if (iouring_sq_ring != MAP_FAILED) munmap(iouring_sq_ring, iouring_sq_ring_size);
  if (iouring_cq_ring != MAP_FAILED) munmap(iouring_cq_ring, iouring_cq_ring_size);
  if (iouring_sqes != MAP_FAILED) munmap(iouring_sqes, iouring_sqes_size);
  if (iouring_fd >= 0) close(iouring_fd);
  if (iouring_tfd >= 0) close(iouring_tfd);
  ev_io_stop(EV_A_ &iouring_tfd_w);
}

ecb_cold static void iouring_fork(EV_P)
{
  iouring_destroy(EV_A);
  while (!iouring_init(EV_A_ 0))
    ev_syserr("io_uring_setup (fork recovery)");
  fd_rearm_all(EV_A);
}
