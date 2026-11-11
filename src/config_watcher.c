/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * config_watcher.c: inotify-based automatic reload of the configuration
 * file, Hyprland-style. All configuration files (the main file and
 * everything pulled in via "include") are watched; on change the new
 * configuration is validated first so that a broken edit never destroys
 * the running session.
 *
 */
#include "all.h"
#include "config_watcher.h"

#include <libgen.h>
#include <sys/inotify.h>
#include <sys/wait.h>

/*
 * Time between the last observed change and the actual reload. Editors
 * often write a file in several steps (write, rename into place, …), so
 * the events are coalesced.
 *
 */
#define DEBOUNCE_SECONDS 0.2

struct watched_file {
    int wd;
    char *filename; /* basename of the watched file */
    TAILQ_ENTRY(watched_file) files;
};

static TAILQ_HEAD(watched_files_head, watched_file) watched_files =
    TAILQ_HEAD_INITIALIZER(watched_files);

static int inotify_fd = -1;
static ev_io inotify_watcher;
static ev_timer debounce_timer;
static bool debounce_started = false;
static pid_t error_nagbar_pid = -1;

static void schedule_reload(void);

/*
 * Clears all watches so they can be rebuilt after the set of included
 * configuration files changed.
 *
 */
static void clear_watches(void) {
    struct watched_file *file;
    while (!TAILQ_EMPTY(&watched_files)) {
        file = TAILQ_FIRST(&watched_files);
        if (inotify_fd != -1) {
            inotify_rm_watch(inotify_fd, file->wd);
        }
        TAILQ_REMOVE(&watched_files, file, files);
        FREE(file->filename);
        FREE(file);
    }
}

/*
 * Adds a watch for the directory containing "path" and remembers the file
 * name to filter directory events. Watches on the directory instead of the
 * file itself because editors commonly replace files via rename(2), which
 * does not trigger IN_MODIFY on the old inode.
 *
 */
static void watch_file(const char *path) {
    if (inotify_fd == -1) {
        return;
    }

    char *copy = sstrdup(path);
    char *dir = dirname(copy);
    const uint32_t mask = IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE;
    const int wd = inotify_add_watch(inotify_fd, dir, mask);
    FREE(copy);
    if (wd == -1) {
        ELOG("inotify_add_watch(%s) failed: %s\n", path, strerror(errno));
        return;
    }

    copy = sstrdup(path);
    char *filename = basename(copy);

    struct watched_file *file;
    TAILQ_FOREACH (file, &watched_files, files) {
        if (file->wd == wd && strcmp(file->filename, filename) == 0) {
            FREE(copy);
            return;
        }
    }

    file = scalloc(1, sizeof(struct watched_file));
    file->wd = wd;
    file->filename = sstrdup(filename);
    TAILQ_INSERT_TAIL(&watched_files, file, files);
    DLOG("watching %s (wd %d)\n", path, wd);
    FREE(copy);
}

/*
 * Rebuilds the watch list from the currently loaded configuration, i.e.
 * the main config file plus every file pulled in via "include".
 *
 */
void config_watcher_rearm(void) {
    if (inotify_fd == -1) {
        return;
    }

    clear_watches();

    if (current_configpath != NULL) {
        watch_file(current_configpath);
    }
    IncludedFile *file;
    TAILQ_FOREACH (file, &included_files, files) {
        watch_file(file->path);
    }
}

/*
 * Validates the current configuration by running a fresh i3 instance with
 * -C. This must happen out of process: a failed parse would otherwise
 * leave the live Config struct partially overwritten.
 *
 */
static bool validate_current_config(void) {
    pid_t pid = fork();
    if (pid == -1) {
        ELOG("fork() failed: %s, skipping validation\n", strerror(errno));
        return true;
    }
    if (pid == 0) {
        execl("/proc/self/exe", "i3", "-C", "-c", current_configpath, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/*
 * Applies the reload. Mirrors the relevant parts of cmd_reload().
 *
 */
static void apply_reload(void) {
    kill_nagbar(config_error_nagbar_pid, false);
    kill_nagbar(command_error_nagbar_pid, false);
    /* start_nagbar() will refuse to start a new process if the passed pid
     * is set (see #4104). */
    config_error_nagbar_pid = command_error_nagbar_pid = -1;

    load_configuration(NULL, C_RELOAD);
    x_set_i3_atoms();
    ipc_send_workspace_event("reload", NULL, false);
    Barconfig *current;
    TAILQ_FOREACH (current, &barconfigs, configs) {
        ipc_send_barconfig_update_event(current);
    }
    config_watcher_rearm();
}

static void reload_cb(struct ev_loop *loop, ev_timer *w, int revents) {
    ev_timer_stop(loop, w);
    debounce_started = false;

    LOG("config file changed on disk, reloading\n");
    if (!validate_current_config()) {
        ELOG("configuration validation failed, keeping the previous configuration\n");
        kill_nagbar(error_nagbar_pid, false);
        error_nagbar_pid = -1;
        char *argv[] = {
            "i3-nagbar",
            "-t",
            "error",
            "-m",
            "i3: configuration file has errors, auto-reload skipped (previous configuration kept)",
            NULL};
        start_nagbar(&error_nagbar_pid, argv);
        return;
    }
    kill_nagbar(error_nagbar_pid, false);
    error_nagbar_pid = -1;

    apply_reload();
}

static void schedule_reload(void) {
    if (debounce_started) {
        ev_timer_stop(main_loop, &debounce_timer);
    }
    ev_timer_init(&debounce_timer, reload_cb, DEBOUNCE_SECONDS, 0.0);
    ev_timer_start(main_loop, &debounce_timer);
    debounce_started = true;
}

static void inotify_cb(struct ev_loop *loop, ev_io *w, int revents) {
    char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool changed = false;

    for (;;) {
        const ssize_t n = read(inotify_fd, buffer, sizeof(buffer));
        if (n <= 0) {
            if (n < 0 && errno == EAGAIN) {
                break;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            /* The watch became unusable (e.g. fd error); disable hot reload. */
            ELOG("inotify read failed (%s), disabling config hot reload\n",
                 n < 0 ? strerror(errno) : "EOF");
            ev_io_stop(loop, w);
            return;
        }

        for (char *p = buffer; p < buffer + n;
             p += sizeof(struct inotify_event) + ((struct inotify_event *)p)->len) {
            const struct inotify_event *event = (struct inotify_event *)p;
            if (event->mask & IN_IGNORED) {
                /* A watched directory vanished (e.g. config in /tmp was
                 * deleted); drop the stale watch entries. */
                struct watched_file *file = TAILQ_FIRST(&watched_files);
                while (file != NULL) {
                    struct watched_file *next = TAILQ_NEXT(file, files);
                    if (file->wd == event->wd) {
                        TAILQ_REMOVE(&watched_files, file, files);
                        FREE(file->filename);
                        FREE(file);
                    }
                    file = next;
                }
                continue;
            }
            if (event->len == 0 || !(event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE))) {
                continue;
            }
            struct watched_file *file;
            TAILQ_FOREACH (file, &watched_files, files) {
                if (file->wd == event->wd && strcmp(file->filename, event->name) == 0) {
                    DLOG("config change detected: %s (mask 0x%x)\n", event->name, event->mask);
                    changed = true;
                    break;
                }
            }
        }
    }

    if (changed) {
        schedule_reload();
    }
}

/*
 * Sets up the inotify watch on the configuration files. Silently does
 * nothing when inotify is unavailable; manual reload (SIGHUP-style
 * $mod+Shift+c) keeps working as before.
 *
 */
void config_watcher_init(void) {
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd == -1) {
        ELOG("inotify_init1() failed: %s, config hot reload disabled\n", strerror(errno));
        return;
    }

    config_watcher_rearm();

    ev_io_init(&inotify_watcher, inotify_cb, inotify_fd, EV_READ);
    ev_io_start(main_loop, &inotify_watcher);
    LOG("config hot reload active\n");
}
