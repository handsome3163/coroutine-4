#include <stdlib.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/mman.h>

#include "list.h"
#include "utils.h"
#include "sched.h"
#include "context.h"


coroutine_t g_exit_coroutine;
fdstat_t *g_fds;

static int g_pollfd;
static void* coroutine_exit_sched(void *arg);

int
coroutine_sched_init() {
  struct rlimit rlim;

  if (coroutine_create(&g_exit_coroutine, NULL,
      coroutine_exit_sched, NULL) == -1)
  {
    return -1;
  }

  if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
    return -1;
  }

  g_fds = (fdstat_t*)malloc(sizeof(fdstat_t*) * rlim.rlim_max);
  if (g_fds == NULL) {
    return -1;
  }

  if ((g_pollfd = epoll_create(1024)) == -1) {
    return -1;
  }

  return 0;
}


int
coroutine_sched_regfd(int fd)
{
  int fl;

  if ((fl = getfl(fd)) == -1) {
    return -1;
  }

  if (setfl(fd, fl | O_NONBLOCK) == -1) {
    return -1;
  }

  g_fds[fd].open = 1;
  g_fds[fd].fl = fl;
  return 0;
}


int
coroutine_sched_unregfd(int fd)
{
  g_fds[fd].open = 0;
  return 0;
}


void
coroutine_sched_block(coroutine_ctx_t *ctx, int fd, int type)
{
  struct epoll_event ev;

  ctx->flag = BLOCKING;

  ev.events = type;
  ev.data.ptr = ctx;

  epoll_ctl(g_pollfd, EPOLL_CTL_ADD, fd, &ev);

  coroutine_sched();
}


void
coroutine_sched()
{
  int i, nfds;
  coroutine_t cid;
  coroutine_ctx_t *ctx;
  struct epoll_event event[1024];

  cid = -1;
  nfds = epoll_wait(g_pollfd, event, 1024, 0);
  for (i = 0; i < nfds; ++i) {
    ctx = (coroutine_ctx_t*)event[i].data.ptr;
    ctx->flag = READY;
  }

  list_for_each_entry(ctx, &g_coroutine_list, list) {
    if (ctx->flag == READY && ctx->cid != 1 && ctx->cid != coroutine_self()) {
      cid = ctx->cid;
      break;
    }
  }

  if (cid != -1) {
    coroutine_resume(cid);
  }

  nfds = epoll_wait(g_pollfd, event, 1024, -1);
  for (i = 0; i < nfds; ++i) {
    ctx = (coroutine_ctx_t*)event[i].data.ptr;
    ctx->flag = READY;
    cid = ctx->cid;
  }

  coroutine_resume(cid);

  return;
}


void*
coroutine_exit_sched(void *arg)
{
  coroutine_t cid;
  coroutine_ctx_t *ctx;
  for (;;) {
    /*
     * Next line will get the last running coroutine id,
     * NOT the coroutine id of exit_sched.
     */
    cid = coroutine_self();
    ctx = coroutine_get_ctx(cid);

    list_del(&ctx->list);
    hlist_del(&ctx->hash);
    munmap(ctx->ctx.uc_stack.ss_sp, ctx->ctx.uc_stack.ss_size);
    free(ctx);

    coroutine_yield();
  }

  return NULL;
}

