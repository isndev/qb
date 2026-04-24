/*
 * qb-io libev backend: Linux io_uring (POLL_ADD / POLL_REMOVE) + timerfd sleep.
 *
 * Goals vs. a minimal upstream-style uring glue:
 * - One libev loop / one OS thread: IORING_SETUP_SINGLE_ISSUER (+ COOP_TASKRUN when supported),
 *   with automatic fallback if io_uring_setup rejects those flags.
 * - No silent loss of SQEs when the submission ring is full: flush to the kernel, then retry.
 * - Correct iouring_to_submit accounting across enter / fd_event (no blanket zero at poll end).
 * - Blocking: poll(2) on the uring fd (aligned with timerfd absolute deadline when armed), EINTR-safe.
 * - mmap: MAP_POPULATE first, fallback without; EINTR retry; total ring size bounds + overflow checks.
 * - Memory fences on SQ tail, CQ head/tail visibility; POLL_ADD one-shot re-arm via fd_change like linuxaio.
 * - CQEs: ignore -ECANCELED/-EINTR/-ENOENT from poll races; res==0 forces re-arm; mmap offsets sanity-checked.
 * - timerfd: drain expirations with EINTR retry; EAGAIN ignored; other read errors -> ev_syserr.
 * - loop_destroy skips close(backend_fd) before iouring_destroy (see ev.c) to avoid double-close.
 *
 * Requires a kernel new enough for IORING_OP_POLL_ADD (libev gates the backend in ev.c).
 */

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
#include <limits.h>

#ifndef POLLRDHUP
# define POLLRDHUP 0
#endif

#define IOURING_QUEUE_DEPTH 64
#define USERDATA_REMOVE ((uint64_t)-2)

#define IOURING_SQ_FLUSH_MAX_SPIN 64
/* Refuse absurd ring maps (bad kernel params / fuzz); normal depth is tiny. */
#define IOURING_MAX_RING_BYTES ((uint64_t)32 << 20)
#define IOURING_CQ_DRAIN_BUDGET 4096

/* UAPI flags not present in every distro linux/io_uring.h (kernel may still support them). */
#ifndef IORING_SETUP_COOP_TASKRUN
# define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif
#ifndef IORING_SETUP_SINGLE_ISSUER
# define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif

#ifndef ECANCELED
# define ECANCELED 125 /* Linux asm-generic/errno.h */
#endif

/* Map a uring region; retry without MAP_POPULATE when populate fails (memory pressure, LXC, old kernels). */
static void *iouring_mmap_region(int uring_fd, off_t off, size_t len) {
  void *addr;

  do
    addr = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, uring_fd, off);
  while (addr == MAP_FAILED && errno == EINTR);

  if (addr != MAP_FAILED)
    return addr;
  if (errno == ENOMEM || errno == EINVAL || errno == EPERM || errno == EAGAIN) {
    do
      addr = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, uring_fd, off);
    while (addr == MAP_FAILED && errno == EINTR);
  }
  return addr;
}

/* Returns 0 on success; fills *sq_sz, *cq_sz, *sqe_sz as uint32_t for loop storage. */
static int iouring_validate_ring_layout(struct io_uring_params *p,
                                        uint32_t *sq_sz, uint32_t *cq_sz, uint32_t *sqe_sz) {
  uint64_t sq, cq, sqe;
  uint64_t max_u32 = (uint64_t)UINT32_MAX;

  if (ecb_expect_false(!p->sq_entries || !p->cq_entries))
    return -1;

  sq = (uint64_t)p->sq_off.array + (uint64_t)p->sq_entries * sizeof(unsigned);
  if (sq < (uint64_t)p->sq_off.array || sq > IOURING_MAX_RING_BYTES || sq > max_u32)
    return -1;

  cq = (uint64_t)p->cq_off.cqes + (uint64_t)p->cq_entries * sizeof(struct io_uring_cqe);
  if (cq < (uint64_t)p->cq_off.cqes || cq > IOURING_MAX_RING_BYTES || cq > max_u32)
    return -1;

  sqe = (uint64_t)p->sq_entries * sizeof(struct io_uring_sqe);
  if (sqe > IOURING_MAX_RING_BYTES || sqe > max_u32)
    return -1;

  *sq_sz = (uint32_t)sq;
  *cq_sz = (uint32_t)cq;
  *sqe_sz = (uint32_t)sqe;
  return 0;
}

/* Kernel must place head/tail/mask/array/cqes inside the mmap windows we computed. */
static int iouring_validate_ptr_offsets(const struct io_uring_params *p,
                                        uint32_t sq_sz, uint32_t cq_sz) {
  const uint64_t u = sizeof(unsigned);
  const uint64_t cqe_sz = sizeof(struct io_uring_cqe);

  if ((uint64_t)p->sq_off.head + u > (uint64_t)sq_sz
      || (uint64_t)p->sq_off.tail + u > (uint64_t)sq_sz
      || (uint64_t)p->sq_off.ring_mask + u > (uint64_t)sq_sz
      || (uint64_t)p->sq_off.array + (uint64_t)p->sq_entries * u > (uint64_t)sq_sz)
    return -1;

  if ((uint64_t)p->cq_off.head + u > (uint64_t)cq_sz
      || (uint64_t)p->cq_off.tail + u > (uint64_t)cq_sz
      || (uint64_t)p->cq_off.ring_mask + u > (uint64_t)cq_sz
      || (uint64_t)p->cq_off.cqes + (uint64_t)p->cq_entries * cqe_sz > (uint64_t)cq_sz)
    return -1;

  return 0;
}

static inline int sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
  return syscall(__NR_io_uring_setup, entries, p);
}

static inline int sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                                     unsigned flags, void *sig, size_t sigsz) {
  return syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig, sigsz);
}

/* Peek one SQE slot; does not flush. Caller must ensure kernel has consumed SQEs if full. */
static struct io_uring_sqe *iouring_sqe_try(EV_P) {
  unsigned tail = *iouring_sq_tail;
  ECB_MEMORY_FENCE_ACQUIRE;
  if ((tail - *iouring_sq_head) >= iouring_sq_ring_entries) return 0;
  return &((struct io_uring_sqe *)iouring_sqes)[tail & *iouring_sq_ring_mask];
}

/* Push pending SQEs to the kernel so the SQ ring frees slots (single-threaded libev loop). */
static void iouring_sq_flush(EV_P) {
  for (;;) {
    unsigned n = iouring_to_submit;
    if (!n)
      return;
    EV_RELEASE_CB;
    int r = sys_io_uring_enter(iouring_fd, n, 0, 0, 0, 0);
    EV_ACQUIRE_CB;
    if (r < 0 && errno == EINTR)
      continue;
    if (r < 0)
      ev_syserr("(libev) io_uring_enter (flush sq)");
    if (iouring_to_submit >= n)
      iouring_to_submit -= n;
    else
      iouring_to_submit = 0;
    ECB_MEMORY_FENCE_ACQUIRE;
    return;
  }
}

static struct io_uring_sqe *iouring_get_sqe(EV_P) {
  int spin;
  for (spin = 0; spin < IOURING_SQ_FLUSH_MAX_SPIN; ++spin) {
    struct io_uring_sqe *sqe = iouring_sqe_try(EV_A);
    if (ecb_expect_true(sqe))
      return sqe;
    iouring_sq_flush(EV_A);
  }
  ev_syserr("(libev) io_uring submission queue saturated");
  return 0;
}

static void submit_sqe(EV_P_ struct io_uring_sqe *sqe) {
  (void)sqe;
  unsigned index = *iouring_sq_tail & *iouring_sq_ring_mask;
  iouring_sq_array[index] = index;
  /* Publish SQE before advancing the submission queue tail (kernel may read immediately). */
  ECB_MEMORY_FENCE_RELEASE;
  (*iouring_sq_tail)++;
  ++iouring_to_submit;
}

static void iouring_tfd_cb(EV_P_ ev_io *w, int revents) {
  uint64_t val;
  (void)revents;
  for (;;)
    {
      ssize_t n = read(iouring_tfd, &val, sizeof(val));
      if (n == (ssize_t)sizeof(val))
        break;
      if (n < 0 && errno == EINTR)
        continue;
      /* EAGAIN: no expirations to drain; other errors are exceptional for timerfd. */
      if (n < 0 && errno != EAGAIN)
        ev_syserr("(libev) io_uring timerfd read");
      break;
    }
  iouring_tfd_to = EV_TSTAMP_HUGE;
}

static void iouring_modify(EV_P_ int fd, int oev, int nev)
{
  if (oev) {
    struct io_uring_sqe *sqe = iouring_get_sqe(EV_A);

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_POLL_REMOVE;
    sqe->fd = fd;
    sqe->addr = (uint64_t)((uint32_t)fd | ((uint64_t)(uint32_t)anfds[fd].egen << 32));
    sqe->user_data = USERDATA_REMOVE;
    submit_sqe(EV_A_ sqe);
    ++anfds[fd].egen;
  }

  if (nev) {
    struct io_uring_sqe *sqe = iouring_get_sqe(EV_A);

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->poll_events = (nev & EV_READ ? POLLIN | POLLRDHUP : 0)
                     | (nev & EV_WRITE ? POLLOUT : 0);
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
      if (ecb_expect_false(timerfd_settime(iouring_tfd, TFD_TIMER_ABSTIME, &its, 0) < 0))
        ev_syserr("(libev) io_uring timerfd_settime");
    }
  }

  unsigned to_sub = iouring_to_submit;
  /* Must sleep until a CQ event (POLL_ADD one-shots, timerfd, other fds). */
  int blocking = (timeout < 0.) || (timeout > 0.);

  EV_RELEASE_CB;
  int ret = 0;

  if (to_sub > 0) {
    ret = sys_io_uring_enter(iouring_fd, to_sub, 0, 0, 0, 0);
    if (ret < 0 && errno == EINTR) {
      EV_ACQUIRE_CB;
      return;
    }
    if (ret < 0) {
      EV_ACQUIRE_CB;
      ev_syserr("(libev) io_uring_enter (submit)");
    }
    if (iouring_to_submit >= to_sub)
      iouring_to_submit -= to_sub;
    else
      iouring_to_submit = 0;
    ECB_MEMORY_FENCE_ACQUIRE;
  }

  /* Wait for completion traffic on the ring fd (avoids GETEVENTS with min_complete
   * when nothing is in flight, which can misbehave). Skip sleep if CQ already has work. */
  if (blocking) {
    ECB_MEMORY_FENCE_ACQUIRE;
    if (*iouring_cq_head == *iouring_cq_tail) {
      struct pollfd pfd;
      int pms;
      double msd;

      pfd.fd = iouring_fd;
      pfd.events = (short)(POLLIN | POLLERR | POLLHUP);
      pfd.revents = 0;
      if (timeout < 0.)
        pms = -1;
      else {
        /* Prefer deadline from timerfd arm (absolute) so poll matches sub-ms timer wakeups. */
        if (iouring_tfd_to < EV_TSTAMP_HUGE) {
          if (iouring_tfd_to > mn_now) {
            msd = (iouring_tfd_to - mn_now) * 1000. + 0.9999;
            if (msd >= (double)INT_MAX)
              pms = INT_MAX;
            else if (msd <= 0.)
              pms = 0;
            else
              pms = (int)msd;
          } else
            pms = 0; /* timer already elapsed; poll once without sleeping */
        } else {
          msd = EV_TS_TO_MSEC(timeout);
          if (msd >= (double)INT_MAX)
            pms = INT_MAX;
          else if (msd <= 0.)
            pms = 0;
          else
            pms = (int)msd;
        }
      }
      for (;;) {
        ret = poll(&pfd, 1, pms);
        if (ret < 0 && errno == EINTR) {
          EV_ACQUIRE_CB;
          return;
        }
        if (ret < 0) {
          EV_ACQUIRE_CB;
          ev_syserr("(libev) poll (io_uring fd)");
        }
        if (ecb_expect_false(pfd.revents & (POLLERR | POLLNVAL | POLLHUP))) {
          EV_ACQUIRE_CB;
          ev_syserr("(libev) io_uring fd poll error");
        }
        break;
      }
    }
  }

  EV_ACQUIRE_CB;

  ECB_MEMORY_FENCE_ACQUIRE;
  unsigned head = *iouring_cq_head;
  unsigned mask = *iouring_cq_ring_mask;
  unsigned drained = 0;

  for (;;) {
    unsigned tail;

    ECB_MEMORY_FENCE_ACQUIRE;
    tail = *iouring_cq_tail;
    if (head == tail)
      break;
    if (ecb_expect_false(++drained > IOURING_CQ_DRAIN_BUDGET))
      break;

    struct io_uring_cqe *cqe = &((struct io_uring_cqe *)((char *)iouring_cq_ring + iouring_cq_cqes))[head & mask];
    uint64_t user_data = cqe->user_data;

    if (user_data == USERDATA_REMOVE || user_data == 0) goto skip;

    int fd = (int)(uint32_t)(user_data & 0xffffffffU);
    if (fd < 0 || fd >= anfdmax) goto skip;

    int gen = (int)(user_data >> 32);
    if ((uint32_t)anfds[fd].egen != (uint32_t)gen) goto skip;

    int res = cqe->res;
    if (res < 0) {
      /* POLL_REMOVE races, cancellation, or stale poll after fd_kill. */
      if (res == -EBADF)
        fd_kill(EV_A_ fd);
      else if (res == -ECANCELED || res == -EINTR || res == -ENOENT)
        ;
      else
        {
          errno = -res;
          ev_syserr("(libev) io_uring poll error");
        }
      goto skip;
    }

    /* POLL_ADD one-shot with empty mask: re-arm so fd_reify re-submits (matches edge kernels). */
    if (ecb_expect_false(res == 0))
      {
        anfds[fd].events = 0;
        fd_change(EV_A_ fd, EV_ANFD_REIFY);
        goto skip;
      }

    int ev = 0;
    if (res & (POLLIN | POLLRDHUP)) ev |= EV_READ;
    if (res & POLLOUT) ev |= EV_WRITE;
    if (res & (POLLERR | POLLHUP)) ev |= (EV_READ | EV_WRITE);

    if (ev) {
      /* POLL_ADD is one-shot; mirror linuxaio_fd_rearm so fd_reify re-submits. */
      fd_event(EV_A_ fd, ev);
      anfds[fd].events = 0;
      fd_change(EV_A_ fd, EV_ANFD_REIFY);
    }

  skip:
    head++;
  }

  ECB_MEMORY_FENCE_RELEASE;
  *iouring_cq_head = head;
}

inline_size int iouring_init(EV_P_ int flags)
{
  struct io_uring_params p;
  (void)flags;

  memset(&p, 0, sizeof(p));
  /* libev runs one thread per loop; tell the kernel when supported (fails with EINVAL on old kernels). */
  p.flags = (unsigned)IORING_SETUP_SINGLE_ISSUER | (unsigned)IORING_SETUP_COOP_TASKRUN;

  iouring_fd = sys_io_uring_setup(IOURING_QUEUE_DEPTH, &p);
  if (iouring_fd < 0 && (errno == EINVAL || errno == EPERM)) {
    memset(&p, 0, sizeof(p));
    iouring_fd = sys_io_uring_setup(IOURING_QUEUE_DEPTH, &p);
  }
  if (iouring_fd < 0)
    return 0;

  if (ecb_expect_false(iouring_validate_ring_layout(&p, &iouring_sq_ring_size,
                                                    &iouring_cq_ring_size, &iouring_sqes_size) < 0))
    {
      close(iouring_fd);
      iouring_fd = -1;
      return 0;
    }

  if (ecb_expect_false(iouring_validate_ptr_offsets(&p, iouring_sq_ring_size,
                                                    iouring_cq_ring_size) < 0))
    {
      close(iouring_fd);
      iouring_fd = -1;
      return 0;
    }

  iouring_sq_ring_entries = p.sq_entries;
  iouring_cq_ring_entries = p.cq_entries;

  iouring_sq_ring = iouring_mmap_region(iouring_fd, (off_t)IORING_OFF_SQ_RING,
                                        (size_t)iouring_sq_ring_size);
  iouring_cq_ring = iouring_mmap_region(iouring_fd, (off_t)IORING_OFF_CQ_RING,
                                        (size_t)iouring_cq_ring_size);
  iouring_sqes    = iouring_mmap_region(iouring_fd, (off_t)IORING_OFF_SQES,
                                        (size_t)iouring_sqes_size);

  if (iouring_sq_ring == MAP_FAILED || iouring_cq_ring == MAP_FAILED || iouring_sqes == MAP_FAILED) {
    if (iouring_sq_ring != MAP_FAILED) munmap(iouring_sq_ring, iouring_sq_ring_size);
    if (iouring_cq_ring != MAP_FAILED) munmap(iouring_cq_ring, iouring_cq_ring_size);
    if (iouring_sqes != MAP_FAILED) munmap(iouring_sqes, iouring_sqes_size);
    iouring_sq_ring = iouring_cq_ring = iouring_sqes = MAP_FAILED;
    close(iouring_fd);
    iouring_fd = -1;
    return 0;
  }

  iouring_sq_head         = (unsigned *)((char *)iouring_sq_ring + p.sq_off.head);
  iouring_sq_tail         = (unsigned *)((char *)iouring_sq_ring + p.sq_off.tail);
  iouring_sq_ring_mask    = (unsigned *)((char *)iouring_sq_ring + p.sq_off.ring_mask);
  iouring_sq_array        = (unsigned *)((char *)iouring_sq_ring + p.sq_off.array);

  iouring_cq_head         = (unsigned *)((char *)iouring_cq_ring + p.cq_off.head);
  iouring_cq_tail         = (unsigned *)((char *)iouring_cq_ring + p.cq_off.tail);
  iouring_cq_ring_mask    = (unsigned *)((char *)iouring_cq_ring + p.cq_off.ring_mask);
  iouring_cq_cqes         = p.cq_off.cqes;

  iouring_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (iouring_tfd < 0) {
    munmap(iouring_sq_ring, iouring_sq_ring_size);
    munmap(iouring_cq_ring, iouring_cq_ring_size);
    munmap(iouring_sqes, iouring_sqes_size);
    iouring_sq_ring = iouring_cq_ring = iouring_sqes = MAP_FAILED;
    close(iouring_fd);
    iouring_fd = -1;
    return 0;
  }

  iouring_tfd_to = EV_TSTAMP_HUGE;

  ev_io_init(&iouring_tfd_w, iouring_tfd_cb, iouring_tfd, EV_READ);
  ev_set_priority(&iouring_tfd_w, EV_MINPRI);
  ev_io_start(EV_A_ &iouring_tfd_w);
  ev_unref(EV_A);

  iouring_to_submit = 0;

  backend_fd     = (uintptr_t)(unsigned)iouring_fd;
  backend_modify = iouring_modify;
  backend_poll   = iouring_poll;
  backend_mintime = EV_TS_CONST(1e-3);

  return EVBACKEND_IOURING;
}

inline_size void iouring_destroy(EV_P)
{
  ev_io_stop(EV_A_ &iouring_tfd_w);
  if (iouring_sq_ring != MAP_FAILED) munmap(iouring_sq_ring, iouring_sq_ring_size);
  if (iouring_cq_ring != MAP_FAILED) munmap(iouring_cq_ring, iouring_cq_ring_size);
  if (iouring_sqes != MAP_FAILED) munmap(iouring_sqes, iouring_sqes_size);
  iouring_sq_ring = iouring_cq_ring = iouring_sqes = MAP_FAILED;
  if (iouring_fd >= 0) close(iouring_fd);
  iouring_fd = -1;
  if (iouring_tfd >= 0) close(iouring_tfd);
  iouring_tfd = -1;
}

ecb_cold static void iouring_fork(EV_P)
{
  iouring_destroy(EV_A);
  while (!iouring_init(EV_A_ 0))
    ev_syserr("io_uring_setup (fork recovery)");
  fd_rearm_all(EV_A);
}
