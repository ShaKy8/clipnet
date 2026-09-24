/*
 * clipnetd — the CLIP//NET clipboard daemon.
 *
 * Watches the Wayland clipboard, stores every copy with all of its formats,
 * and serves the UI and CLI over a Unix socket. It owns the clipboard while
 * a pasted clip is on it, so it runs on its own (a systemd user service),
 * independent of the UI.
 *
 *   clipnetd                     run
 *   clipnetd --ctl '<json>'      send one request to the running daemon
 *
 * Options: --data DIR (default $XDG_DATA_HOME/clipnet), --socket PATH,
 * --no-wayland (IPC and storage only, for tests), --no-hyprland, -v.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "buffers.h"
#include "capture.h"
#include "ctl.h"
#include "db.h"
#include "hotkeys.h"
#include "hypr.h"
#include "ipc.h"
#include "loop.h"
#include "proto.h"
#include "serve.h"
#include "settings.h"
#include "transform.h"
#include "util.h"
#include "wl.h"

#define RETENTION_FIRST_MS (10 * 1000)
#define RETENTION_EVERY_MS (60 * 60 * 1000)

static void usage(FILE *f)
{
  fprintf(f,
          "usage: clipnetd [--data DIR] [--socket PATH] [--no-wayland] [--no-hyprland] [-v]\n"
          "       clipnetd [--socket PATH] --ctl '<json request>'\n");
}

static void on_signal(void *ctx, int fd, uint32_t events)
{
  struct app *app = ctx;
  struct signalfd_siginfo si;
  while (read(fd, &si, sizeof si) == sizeof si) {
    if (si.ssi_signo == SIGHUP) {
      log_info("SIGHUP: nothing to reload yet");
      continue;
    }
    log_info("signal %u: shutting down", si.ssi_signo);
    loop_quit(app->loop);
  }
}

static void on_retention(void *ctx)
{
  struct app *app = ctx;
  app->retention_timer = NULL;
  int n = db_retention(app->db, now_ms());
  if (n > 0) {
    hotkeys_prune(app->hotkeys);
    app_emit(app, "clips.reset", NULL);
  }
  app->retention_timer = loop_timer(app->loop, RETENTION_EVERY_MS, on_retention, app);
}

static void on_hypr_reload(void *ctx)
{
  /* A config reload drops every runtime bind; put ours back. */
  hotkeys_sync(ctx);
}

static void on_window_change(void *ctx)
{
  capture_owner_may_have_left(ctx);
}

static char *default_socket(void)
{
  const char *rt = getenv("XDG_RUNTIME_DIR");
  if (!rt || !*rt) return NULL;
  return xasprintf("%s/clipnet/clipnetd.sock", rt);
}

int main(int argc, char **argv)
{
  static const struct option opts[] = {
    { "data", required_argument, NULL, 'd' },
    { "socket", required_argument, NULL, 's' },
    { "ctl", required_argument, NULL, 'c' },
    { "no-wayland", no_argument, NULL, 'W' },
    { "no-hyprland", no_argument, NULL, 'H' },
    { "verbose", no_argument, NULL, 'v' },
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, 'V' },
    { 0 },
  };
  char *data_dir = NULL, *socket_path = NULL;
  const char *ctl = NULL;
  bool no_wayland = false, no_hypr = false;
  int o;
  while ((o = getopt_long(argc, argv, "vh", opts, NULL)) != -1) {
    switch (o) {
    case 'd': data_dir = xstrdup(optarg); break;
    case 's': socket_path = xstrdup(optarg); break;
    case 'c': ctl = optarg; break;
    case 'W': no_wayland = true; break;
    case 'H': no_hypr = true; break;
    case 'v': log_threshold = LOG_DEBUG; break;
    case 'V': puts("clipnetd " CLIPNET_VERSION); return 0;
    case 'h': usage(stdout); return 0;
    default: usage(stderr); return 2;
    }
  }
  if (!socket_path) socket_path = default_socket();
  if (!socket_path) {
    fprintf(stderr, "clipnetd: XDG_RUNTIME_DIR is not set; pass --socket\n");
    return 2;
  }
  if (ctl) {
    int rc = ctl_main(socket_path, ctl);
    free(socket_path);
    free(data_dir);
    return rc;
  }

  /* Everything we create (database, blobs, socket) is private. */
  umask(077);
  transform_init_locale();
  signal(SIGPIPE, SIG_IGN);

  struct app app = { 0 };
  app.socket_path = socket_path;
  char *sock_dir = xstrdup(socket_path);
  char *slash = strrchr(sock_dir, '/');
  if (slash) *slash = 0;
  app.runtime_dir = sock_dir;
  if (mkdir_p(app.runtime_dir, 0700) < 0) {
    log_err("create %s: %m", app.runtime_dir);
    return 1;
  }

  /* One daemon per socket. The lock dies with the process, so a crash never
   * leaves a stale lock behind. */
  char *lock_path = xasprintf("%s.lock", socket_path);
  int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  free(lock_path);
  if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
    log_err("another clipnetd is already running on %s", socket_path);
    return 1;
  }

  if (!data_dir) data_dir = xdg_path("XDG_DATA_HOME", ".local/share", "clipnet");
  app.loop = loop_new();
  app.db = db_open(data_dir);
  if (!app.loop || !app.db) return 1;
  log_info("clipnetd %s: data in %s", CLIPNET_VERSION, data_dir);

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGHUP);
  sigprocmask(SIG_BLOCK, &mask, NULL);
  int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  struct loop_watch *sig_watch = loop_add_fd(app.loop, sfd, EPOLLIN, on_signal, &app);

  app.capture = capture_new(&app);
  app.serve = serve_new(&app);
  if (!no_wayland) {
    app.wl = wl_connect(&app);
    if (!app.wl) return 1;
    if (!no_hypr) app.hypr = hypr_connect(&app);
    if (app.hypr) hypr_on_window_change(app.hypr, on_window_change, app.capture);
    if (!app.hypr && !no_hypr) log_warn("not running under Hyprland: paste keystrokes and app detection are off");
  }
  app.ipc = ipc_listen(&app, socket_path);
  if (!app.ipc) return 1;
  app.buffers = buffers_new(&app);
  app.hotkeys = hotkeys_new(&app);
  if (app.hypr) hypr_on_reload(app.hypr, on_hypr_reload, app.hotkeys);
  hotkeys_prune(app.hotkeys);
  hotkeys_sync(app.hotkeys);

  /* A timed pause that ran out while we were stopped is over. */
  if (setting_bool(app.db, "paused")) {
    int64_t until = setting_int(app.db, "paused_until");
    if (until && until <= now_ms()) app_resume(&app);
    else if (until) app_pause(&app, (int)((until - now_ms() + 59999) / 60000));
  }
  app.retention_timer = loop_timer(app.loop, RETENTION_FIRST_MS, on_retention, &app);

  log_info("ready on %s", socket_path);
  int rc = loop_run(app.loop);

  loop_timer_cancel(app.loop, app.retention_timer);
  loop_timer_cancel(app.loop, app.pause_timer);
  ipc_close(app.ipc);
  app.ipc = NULL;
  hotkeys_free(app.hotkeys);
  buffers_free(app.buffers);
  capture_free(app.capture);
  serve_free(app.serve);
  hypr_disconnect(app.hypr);
  wl_disconnect(app.wl);
  db_close(app.db);
  loop_del_fd(app.loop, sig_watch);
  loop_free(app.loop);
  close(sfd);
  close(lock_fd);
  free(data_dir);
  free(app.socket_path);
  free(app.runtime_dir);
  return rc < 0 ? 1 : 0;
}
