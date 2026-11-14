/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * main.c: Initialization, main loop
 *
 */
#include "all.h"
#include "shmlog.h"

#include <ev.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <locale.h>
#include <signal.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb_atom.h>
#include <xcb/xinerama.h>
#include <xcb/bigreq.h>

#ifdef I3_ASAN_ENABLED
#include <sanitizer/lsan_interface.h>
#endif

#include "sd-daemon.h"

#include "i3-atoms_NET_SUPPORTED.xmacro.h"
#include "i3-atoms_rest.xmacro.h"

/* The original value of RLIMIT_CORE when i3 was started. We need to restore
 * this before starting any other process, since we set RLIMIT_CORE to
 * RLIM_INFINITY for i3 debugging versions. */
struct rlimit original_rlimit_core;

/* The number of file descriptors passed via socket activation. */
int listen_fds;

/* We keep the xcb_prepare watcher around to be able to enable and disable it
 * temporarily for drag_pointer(). */
static ev_prepare *xcb_prepare;

char **start_argv;

xcb_connection_t *conn;
/* The screen (0 when you are using DISPLAY=:0) of the connection 'conn' */
int conn_screen;

/* Display handle for libstartup-notification */
SnDisplay *sndisplay;

/* The last timestamp we got from X11 (timestamps are included in some events
 * and are used for some things, like determining a unique ID in startup
 * notification). */
xcb_timestamp_t last_timestamp = XCB_CURRENT_TIME;

xcb_screen_t *root_screen;
xcb_window_t root;

xcb_window_t wm_sn_selection_owner;
xcb_atom_t wm_sn;

/* Color depth, visual id and colormap to use when creating windows and
 * pixmaps. Will use 32 bit depth and an appropriate visual, if available,
 * otherwise the root window’s default (usually 24 bit TrueColor). */
uint8_t root_depth;
xcb_visualtype_t *visual_type;
xcb_colormap_t colormap;

struct ev_loop *main_loop;

xcb_key_symbols_t *keysyms;

/* Default shmlog size if not set by user. */
const int default_shmlog_size = 25 * 1024 * 1024;

/* The list of key bindings */
struct bindings_head *bindings;
const char *current_binding_mode = NULL;

/* The list of exec-lines */
struct autostarts_head autostarts = TAILQ_HEAD_INITIALIZER(autostarts);

/* The list of exec_always lines */
struct autostarts_always_head autostarts_always = TAILQ_HEAD_INITIALIZER(autostarts_always);

/* The list of assignments */
struct assignments_head assignments = TAILQ_HEAD_INITIALIZER(assignments);

/* The list of workspace assignments (which workspace should end up on which
 * output) */
struct ws_assignments_head ws_assignments = TAILQ_HEAD_INITIALIZER(ws_assignments);

/* We hope that those are supported and set them to true */
bool xkb_supported = true;
bool shape_supported = true;

bool force_xinerama = false;

/* Define all atoms as global variables */
#define xmacro(atom) xcb_atom_t A_##atom;
I3_NET_SUPPORTED_ATOMS_XMACRO
I3_REST_ATOMS_XMACRO
#undef xmacro

/*
 * This callback is only a dummy, see xcb_prepare_cb.
 * See also man libev(3): "ev_prepare" and "ev_check" - customise your event loop
 *
 */
static void xcb_got_event(EV_P_ ev_io *w, int revents) {
    /* empty, because xcb_prepare_cb are used */
}

/*
 * Called just before the event loop sleeps. Ensures xcb’s incoming and outgoing
 * queues are empty so that any activity will trigger another event loop
 * iteration, and hence another xcb_prepare_cb invocation.
 *
 */
static void xcb_prepare_cb(EV_P_ ev_prepare *w, int revents) {
    /* Process all queued (and possibly new) events before the event loop
       sleeps. */
    xcb_generic_event_t *event;

    while ((event = xcb_poll_for_event(conn)) != NULL) {
        if (event->response_type == 0) {
            if (event_is_ignored(ENSURE_FULL_SEQUENCE(event->full_sequence), 0)) {
                DLOG("Expected X11 Error received for sequence %x\n", event->sequence);
            } else {
                xcb_generic_error_t *error = (xcb_generic_error_t *)event;
                DLOG("X11 Error received (probably harmless)! sequence 0x%x, error_code = %d\n",
                     error->sequence, error->error_code);
            }
            free(event);
            continue;
        }

        /* Strip off the highest bit (set if the event is generated) */
        const int type = (event->response_type & 0x7F);
        handle_event(type, event);

        free(event);
    }

    /* Flush all queued events to X11. */
    xcb_flush(conn);
}

/*
 * Enable or disable the main X11 event handling function.
 * This is used by drag_pointer() which has its own, modal event handler, which
 * takes precedence over the normal event handler.
 *
 */
void main_set_x11_cb(bool enable) {
    DLOG("Setting main X11 callback to enabled=%d\n", enable);
    if (enable) {
        ev_prepare_start(main_loop, xcb_prepare);
        /* Trigger the watcher explicitly to handle all remaining X11 events.
         * drag_pointer()’s event handler exits in the middle of the loop. */
        ev_feed_event(main_loop, xcb_prepare, 0);
    } else {
        ev_prepare_stop(main_loop, xcb_prepare);
    }
}

/*
 * Exit handler which destroys the main_loop. Will trigger cleanup handlers.
 *
 */
static void i3_exit(void) {
    if (*shmlogname != '\0') {
        fprintf(stderr, "Closing SHM log \"%s\"\n", shmlogname);
        fflush(stderr);
        shm_unlink(shmlogname);
    }
    ipc_shutdown(SHUTDOWN_REASON_EXIT, -1);
    unlink(config.ipc_socket_path);
    if (current_log_stream_socket_path != NULL) {
        unlink(current_log_stream_socket_path);
    }
    xcb_disconnect(conn);

    /* If a nagbar is active, kill it */
    kill_nagbar(config_error_nagbar_pid, false);
    kill_nagbar(command_error_nagbar_pid, false);
    config_watcher_kill_nagbar();

/* We need ev >= 4 for the following code. Since it is not *that* important (it
 * only makes sure that there are no i3-nagbar instances left behind) we still
 * support old systems with libev 3. */
#if EV_VERSION_MAJOR >= 4
    ev_loop_destroy(main_loop);
#endif

#ifdef I3_ASAN_ENABLED
    __lsan_do_leak_check();
#endif
}

/*
 * (One-shot) Handler for all signals with default action "Core", see signal(7)
 *
 * Unlinks the SHM log and re-raises the signal.
 *
 */
static void handle_core_signal(int sig, siginfo_t *info, void *data) {
    if (*shmlogname != '\0') {
        shm_unlink(shmlogname);
    }
    raise(sig);
}

/*
 * (One-shot) Handler for all signals with default action "Term", see signal(7)
 *
 * Exits the program gracefully.
 *
 */
static void handle_term_signal(struct ev_loop *loop, ev_signal *signal, int revents) {
    /* We exit gracefully here in the sense that cleanup handlers
     * installed via atexit are invoked. */
    exit(128 + signal->signum);
}

/*
 * Set up handlers for all signals with default action "Term", see signal(7)
 *
 */
static void setup_term_handlers(void) {
    static ev_signal signal_watchers[6];
    const size_t num_watchers = sizeof(signal_watchers) / sizeof(signal_watchers[0]);

    /* We have to rely on libev functionality here and should not use
     * sigaction handlers because we need to invoke the exit handlers
     * and cannot do so from an asynchronous signal handling context as
     * not all code triggered during exit is signal safe (and exiting
     * the main loop from said handler is not easily possible). libev's
     * signal handlers does not impose such a constraint on us. */
    ev_signal_init(&signal_watchers[0], handle_term_signal, SIGHUP);
    ev_signal_init(&signal_watchers[1], handle_term_signal, SIGINT);
    ev_signal_init(&signal_watchers[2], handle_term_signal, SIGALRM);
    ev_signal_init(&signal_watchers[3], handle_term_signal, SIGTERM);
    ev_signal_init(&signal_watchers[4], handle_term_signal, SIGUSR1);
    ev_signal_init(&signal_watchers[5], handle_term_signal, SIGUSR2);
    for (size_t i = 0; i < num_watchers; i++) {
        ev_signal_start(main_loop, &signal_watchers[i]);
        /* The signal handlers should not block ev_run from returning
         * and so none of the signal handlers should hold a reference to
         * the main loop. */
        ev_unref(main_loop);
    }
}

static int parse_restart_fd(void) {
    const char *restart_fd = getenv("_I3_RESTART_FD");
    if (restart_fd == NULL) {
        return -1;
    }

    long int fd = -1;
    if (!parse_long(restart_fd, &fd, 10)) {
        ELOG("Malformed _I3_RESTART_FD \"%s\"\n", restart_fd);
        return -1;
    }
    return fd;
}

/* Nested (test) mode: run this i3 inside a private Xephyr window so that
 * every keybinding is handled by i3 itself without interference from the
 * host window manager / compositor (e.g. testing under Hyprland). */
static int nest_display = 99;
/* Whether the display number was given explicitly with --nest=N; otherwise a
 * free display is picked automatically. */
static bool nest_display_explicit = false;

extern char **environ;

static bool nest_display_in_use(int display) {
    char socketfile[64];
    char lockfile[64];
    snprintf(socketfile, sizeof(socketfile), "/tmp/.X11-unix/X%d", display);
    snprintf(lockfile, sizeof(lockfile), "/tmp/.X%d-lock", display);
    return access(socketfile, F_OK) == 0 || access(lockfile, F_OK) == 0;
}

/*
 * Picks the first free display number, starting at 99 and counting downwards.
 *
 */
static int nest_find_free_display(void) {
    for (int display = 99; display > 0; display--) {
        if (!nest_display_in_use(display)) {
            return display;
        }
    }
    errx(EXIT_FAILURE, "nest mode: no free X display found");
}

/* Work around a broken hand-installed libxkbcommon in
 * /usr/local/lib/x86_64-linux-gnu by re-executing ourselves with the
 * distribution library directory first in the search path. */
static void nest_ensure_system_libs(void) {
    static const char *broken = "/usr/local/lib/x86_64-linux-gnu/libxkbcommon.so.0";
    static const char *good = "/usr/lib/x86_64-linux-gnu";
    struct stat sb;
    if (stat(broken, &sb) != 0) {
        return;
    }
    const char *current = getenv("LD_LIBRARY_PATH");
    if (current && strstr(current, good) != NULL) {
        return;
    }
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s:%s", good, current ? current : "");
    setenv("LD_LIBRARY_PATH", buf, 1);
    execve("/proc/self/exe", start_argv, environ);
    /* execve only returns on error */
    err(EXIT_FAILURE, "execve(/proc/self/exe)");
}

/* Private runtime directory and D-Bus socket of the nested session. */
static char nest_runtimedir[64];
static char nest_buspath[80];
static pid_t nest_bus_pid = 0;

/* Called via atexit in the nested instance: stop the private session bus
 * and remove the private runtime directory. */
static void nest_cleanup_runtime(void) {
    if (nest_runtimedir[0] == '\0') {
        return;
    }
    char pidfile[96];
    snprintf(pidfile, sizeof(pidfile), "%s/dbus.pid", nest_runtimedir);
    pid_t pid = nest_bus_pid;
    if (pid <= 0) {
        FILE *f = fopen(pidfile, "r");
        if (f != NULL) {
            if (fscanf(f, "%d", &pid) != 1 || pid <= 0) {
                pid = 0;
            }
            fclose(f);
        }
    }
    if (pid > 0) {
        kill(pid, SIGTERM);
    }
    unlink(pidfile);
    if (nest_buspath[0] != '\0') {
        unlink(nest_buspath);
    }
    rmdir(nest_runtimedir);
}

/* Isolate the nested session from the host's runtime services by pointing
 * XDG_RUNTIME_DIR at a private directory and running a private D-Bus
 * session bus. Besides hiding the host Wayland socket, this is required
 * because Firefox-based browsers (e.g. Zen) register their single-instance
 * service ("org.mozilla.zen") on the session bus: with the host bus visible,
 * launching such a browser inside the nest merely forwards the command line
 * to the host instance and the window opens on the host desktop instead of
 * inside Xephyr. */
static void nest_isolate_session(void) {
    char hostrt[32];
    snprintf(hostrt, sizeof(hostrt), "/run/user/%d", getuid());

    snprintf(nest_runtimedir, sizeof(nest_runtimedir), "/tmp/i3-nest-%d-rt", nest_display);
    snprintf(nest_buspath, sizeof(nest_buspath), "%s/bus", nest_runtimedir);

    /* Remove stale leftovers of a crashed previous run: they would make
     * bind(2) and the pidfile fail. */
    unlink(nest_buspath);
    rmdir(nest_runtimedir);

    if (mkdir(nest_runtimedir, 0700) != 0) {
        if (errno == EEXIST) {
            chmod(nest_runtimedir, 0700);
        } else {
            LOG("nest mode: cannot create runtime dir %s (%s), running without a session bus\n",
                nest_runtimedir, strerror(errno));
            unsetenv("DBUS_SESSION_BUS_ADDRESS");
            return;
        }
    }
    setenv("XDG_RUNTIME_DIR", nest_runtimedir, 1);
    atexit(nest_cleanup_runtime);

    /* Audio sockets are no longer found via XDG_RUNTIME_DIR, so point
     * PulseAudio/PipeWire clients at the host sockets directly. */
    char sock[96];
    snprintf(sock, sizeof(sock), "%s/pulse/native", hostrt);
    if (access(sock, F_OK) == 0) {
        snprintf(sock, sizeof(sock), "unix:%s/pulse/native", hostrt);
        setenv("PULSE_SERVER", sock, 0);
    }
    setenv("PIPEWIRE_RUNTIME_DIR", hostrt, 0);

    pid_t bus_pid = fork();
    if (bus_pid == -1) {
        LOG("nest mode: fork for dbus-daemon failed, running without a session bus\n");
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
        return;
    }
    if (bus_pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
        }
        char addr[128];
        snprintf(addr, sizeof(addr), "unix:path=%s", nest_buspath);
        execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork",
               "--address", addr, (char *)NULL);
        _exit(127);
    }
    nest_bus_pid = bus_pid;
    int bus_status;
    for (int i = 0; i < 50; i++) {
        if (access(nest_buspath, F_OK) == 0) {
            break;
        }
        if (waitpid(bus_pid, &bus_status, WNOHANG) == bus_pid) {
            /* dbus-daemon exited (e.g. not installed): fall back. */
            nest_bus_pid = 0;
            break;
        }
        usleep(20000);
    }
    if (nest_bus_pid == 0 || access(nest_buspath, F_OK) != 0) {
        if (nest_bus_pid > 0) {
            kill(bus_pid, SIGTERM);
            nest_bus_pid = 0;
        }
        /* Never fall back to the host bus: with it visible, browsers and
         * other single-instance apps can forward the command line to a host
         * instance and the window opens outside the nest. */
        LOG("nest mode: private session bus did not start, running without a session bus\n");
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
        return;
    }
    /* Remember the bus pid so that cleanup still works after an in-place
     * restart of i3 (atexit handlers are lost across execve). */
    char pidfile[96];
    snprintf(pidfile, sizeof(pidfile), "%s/dbus.pid", nest_runtimedir);
    FILE *f = fopen(pidfile, "w");
    if (f != NULL) {
        fprintf(f, "%d\n", (int)bus_pid);
        fclose(f);
    }
    char addr[128];
    snprintf(addr, sizeof(addr), "unix:path=%s", nest_buspath);
    setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
    LOG("nest mode: private session bus on %s\n", nest_buspath);
}

/*
 * Runs the nested i3 inside a private PID (and user) namespace.
 *
 * Process-management tools (killall, pkill, ps) enumerate processes via
 * /proc, which the nest shares with the host session; running e.g.
 * "killall st" in a nested terminal therefore kills the host's terminals
 * as well. Mounting a namespace-private /proc (unshare --mount-proc)
 * confines them to the nested session's processes. As a bonus, the nested
 * instance cannot reach host processes by raw PID.
 *
 * The nested i3 becomes PID 1 of its namespace. That is safe: libev's
 * default loop reaps arbitrary dead children (see the SIGCHLD handling
 * around ev_loop()), so orphans do not accumulate as zombies.
 *
 * Xephyr stays outside the namespace (the supervisor spawns it) and is
 * reached over the shared /tmp/.X11-unix socket; filesystem paths like
 * XDG_RUNTIME_DIR and D-Bus continue to work the same way.
 *
 * Requires unshare(1) with unprivileged user namespaces (sometimes
 * disabled via sysctl or AppArmor); falls back to running without
 * isolation otherwise.
 *
 */
static void nest_enter_pid_namespace(void) {
    if (getenv("I3_NEST_INNER") != NULL) {
        return;
    }

    const char *unshare = NULL;
    static const char *paths[] = {"/usr/bin/unshare", "/bin/unshare", "/usr/sbin/unshare", NULL};
    for (int i = 0; paths[i] != NULL; i++) {
        if (access(paths[i], X_OK) == 0) {
            unshare = paths[i];
            break;
        }
    }
    if (unshare == NULL) {
        LOG("nest mode: unshare(1) not found, running without PID namespace isolation\n");
        return;
    }

    /* Probe that user namespaces actually work on this system. The probe
     * child's stderr is silenced: unshare's diagnostics would be noise
     * when isolation is simply unavailable. */
    pid_t probe = fork();
    if (probe == -1) {
        LOG("nest mode: fork for unshare probe failed, running without PID namespace isolation\n");
        return;
    }
    if (probe == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDERR_FILENO);
        }
        execl(unshare, "unshare", "--user", "--map-current-user", "--pid",
              "--fork", "--mount-proc", "/bin/true", (char *)NULL);
        _exit(127);
    }
    int status;
    if (waitpid(probe, &status, 0) != probe || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOG("nest mode: user/PID namespaces unavailable, running without PID namespace isolation\n");
        return;
    }

    /* Re-exec ourselves inside the namespace. The --nest options are
     * dropped so that the inner instance starts as a regular i3 — this
     * also makes in-place restarts work, which would otherwise re-enter
     * nest_run() and fail because the display is already in use. */
    int argc = 0;
    while (start_argv[argc] != NULL) {
        argc++;
    }
    /* Resolve our own binary path NOW: /proc/self/exe would be resolved
     * relative to the unshare process at exec time (pointing at unshare
     * itself), not to i3. */
    char exe_path[4096];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) {
        LOG("nest mode: readlink(/proc/self/exe) failed, running without PID namespace isolation\n");
        return;
    }
    exe_path[len] = '\0';

    char **argv = scalloc(argc + 8, sizeof(char *));
    int out = 0;
    argv[out++] = (char *)"unshare";
    argv[out++] = (char *)"--user";
    argv[out++] = (char *)"--map-current-user";
    argv[out++] = (char *)"--pid";
    argv[out++] = (char *)"--fork";
    argv[out++] = (char *)"--mount-proc";
    argv[out++] = exe_path;
    for (int i = 1; i < argc; i++) {
        if (strncmp(start_argv[i], "--nest", strlen("--nest")) == 0) {
            continue;
        }
        argv[out++] = start_argv[i];
    }
    argv[out] = NULL;

    /* AppImages mount their squashfs payload through FUSE. Inside the
     * unprivileged user namespace, /usr/bin/fusermount3 belongs to an
     * unmapped uid (it shows up as nobody), so the runtime rejects it
     * ("No suitable fusermount binary found on the $PATH") and browsers
     * shipped as AppImages (Chromium, Zen, ...) silently fail to start.
     * Let the runtime extract to a temporary directory instead. */
    setenv("APPIMAGE_EXTRACT_AND_RUN", "1", 0);

    /* Marker for the inner instance (and any later in-place restarts of
     * it): never try to enter the namespace again. */
    setenv("I3_NEST_INNER", "1", 1);
    execv(unshare, argv);
    /* execv only returns on error. */
    LOG("nest mode: exec unshare failed (%s), running without PID namespace isolation\n",
        strerror(errno));
    unsetenv("I3_NEST_INNER");
    FREE(argv);
}

/*
 * Gives browsers their own profile inside the nest.
 *
 * Chromium (~/.config/chromium) and Firefox/Zen (~/.config/zen) keep a
 * single-instance lock inside the profile and refuse to start a second
 * instance on a profile that is already in use. The nest shares $HOME with the
 * host session, so launching such a browser inside Xephyr either forwards the
 * command line to the host instance (the window opens on the host desktop) or
 * shows the "profile already in use" dialog instead of a browser window.
 *
 * Both browsers honour $XDG_CONFIG_HOME, so point it at a nest-only directory
 * and symlink every host config entry into it except the browser profiles:
 * the rest of the session keeps the user's configuration while browsers start
 * with a profile of their own. Profiles are kept under
 * ~/.cache/i3-nest/<display> so that logins survive across nest sessions;
 * remove that directory to start from scratch.
 *
 */
static void nest_isolate_browser_profiles(void) {
    static const char *const profiles[] = {"chromium", "zen", NULL};
    const char *home = getenv("HOME");
    const char *display = getenv("I3_NEST_DISPLAY");
    if (home == NULL) {
        return;
    }

    /* An in-place `restart` keeps the environment: the redirection is already
     * in place and re-seeding would link the nest directory into itself. */
    if (getenv("I3_NEST_HOST_CONFIG") != NULL) {
        return;
    }

    char hostcfg[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && xdg[0] == '/') {
        snprintf(hostcfg, sizeof(hostcfg), "%s", xdg);
    } else {
        snprintf(hostcfg, sizeof(hostcfg), "%s/.config", home);
    }

    DIR *dir = opendir(hostcfg);
    if (dir == NULL) {
        LOG("nest mode: no config dir %s, browser profiles are not isolated\n", hostcfg);
        return;
    }

    char parent[PATH_MAX];
    char displaydir[PATH_MAX + 32];
    char base[PATH_MAX + 64];
    snprintf(parent, sizeof(parent), "%s/.cache/i3-nest", home);
    mkdir(parent, 0700);
    snprintf(displaydir, sizeof(displaydir), "%s/%s", parent,
             display != NULL ? display + 1 : "default");
    mkdir(displaydir, 0700);
    snprintf(base, sizeof(base), "%s/config", displaydir);
    if (mkdir(base, 0700) != 0 && errno != EEXIST) {
        LOG("nest mode: cannot create browser config dir %s (%s)\n", base, strerror(errno));
        closedir(dir);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        bool is_profile = false;
        for (int i = 0; profiles[i] != NULL; i++) {
            if (strcmp(entry->d_name, profiles[i]) == 0) {
                is_profile = true;
                break;
            }
        }
        if (is_profile) {
            continue;
        }

        char target[PATH_MAX + 256];
        char linkpath[PATH_MAX + 320];
        snprintf(target, sizeof(target), "%s/%s", hostcfg, entry->d_name);
        snprintf(linkpath, sizeof(linkpath), "%s/%s", base, entry->d_name);
        if (symlink(target, linkpath) != 0 && errno != EEXIST) {
            LOG("nest mode: cannot link %s (%s)\n", linkpath, strerror(errno));
        }
    }
    closedir(dir);

    setenv("I3_NEST_HOST_CONFIG", hostcfg, 1);
    setenv("XDG_CONFIG_HOME", base, 1);
    LOG("nest mode: browser profiles isolated in %s\n", base);
}

/* Returns true if the given window is the Xephyr instance spawned by --nest.
 * Xephyr is started with -name i3-nest-<display>, which ends up in WM_CLASS. */
static bool nest_is_xephyr_window(xcb_connection_t *host, xcb_window_t win, const char *name) {
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        host, xcb_get_property(host, 0, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 64), NULL);
    if (reply == NULL) {
        return false;
    }
    bool match = false;
    const int len = xcb_get_property_value_length(reply);
    if (len > 0) {
        const char *value = xcb_get_property_value(reply);
        match = memmem(value, (size_t)len, name, strlen(name)) != NULL;
    }
    free(reply);
    return match;
}

/* Looks for the Xephyr window on the host display. The host window manager
 * may have reparented it into a frame, so the children of the root window are
 * searched as well. */
static xcb_window_t nest_find_xephyr_window(xcb_connection_t *host, xcb_window_t root, const char *name) {
    xcb_window_t found = XCB_NONE;
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(host, xcb_query_tree(host, root), NULL);
    if (tree == NULL) {
        return XCB_NONE;
    }
    const int count = xcb_query_tree_children_length(tree);
    xcb_window_t *children = xcb_query_tree_children(tree);
    for (int i = 0; i < count && found == XCB_NONE; i++) {
        if (nest_is_xephyr_window(host, children[i], name)) {
            found = children[i];
            break;
        }
        xcb_query_tree_reply_t *sub = xcb_query_tree_reply(host, xcb_query_tree(host, children[i]), NULL);
        if (sub == NULL) {
            continue;
        }
        const int subcount = xcb_query_tree_children_length(sub);
        xcb_window_t *subchildren = xcb_query_tree_children(sub);
        for (int j = 0; j < subcount; j++) {
            if (nest_is_xephyr_window(host, subchildren[j], name)) {
                found = subchildren[j];
                break;
            }
        }
        free(sub);
    }
    free(tree);
    return found;
}

/* Give the focus to the Xephyr window (and move the pointer inside it) once it
 * has been mapped. Xephyr grabs the keyboard while its window is focused, so
 * this makes the nested session isolated from the host's keybindings as soon
 * as it starts, without requiring the user to click into the window first.
 * Best effort only: a Wayland host compositor may ignore the focus request. */
static void nest_focus_xephyr(const char *name) {
    int screen = 0;
    xcb_connection_t *host = xcb_connect(NULL, &screen);
    if (xcb_connection_has_error(host)) {
        xcb_disconnect(host);
        return;
    }

    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(host));
    for (int i = 0; i < screen && it.rem > 0; i++) {
        xcb_screen_next(&it);
    }
    if (it.rem == 0) {
        xcb_disconnect(host);
        return;
    }
    const xcb_window_t root = it.data->root;

    xcb_window_t win = XCB_NONE;
    for (int attempt = 0; attempt < 60 && win == XCB_NONE; attempt++) {
        win = nest_find_xephyr_window(host, root, name);
        if (win == XCB_NONE) {
            usleep(100000);
        }
    }

    if (win != XCB_NONE) {
        xcb_set_input_focus(host, XCB_INPUT_FOCUS_PARENT, win, XCB_CURRENT_TIME);

        xcb_translate_coordinates_reply_t *translated = xcb_translate_coordinates_reply(
            host, xcb_translate_coordinates(host, win, root, 0, 0), NULL);
        xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(
            host, xcb_get_geometry(host, win), NULL);
        if (translated != NULL && geometry != NULL) {
            xcb_warp_pointer(host, XCB_NONE, root, 0, 0, 0, 0,
                             translated->dst_x + geometry->width / 2,
                             translated->dst_y + geometry->height / 2);
        }
        free(translated);
        free(geometry);
        xcb_flush(host);
        LOG("nest mode: focused the Xephyr window 0x%08x\n", win);
    }

    /* Give the requests a moment to reach the host before it starts waiting. */
    usleep(100000);
    xcb_disconnect(host);
}

/* Runs a helper and returns true if it exited successfully. Output is
 * discarded: this is only used for best-effort host integration. */
static bool nest_run_quiet(char *const argv[]) {
    pid_t pid = fork();
    if (pid == -1) {
        return false;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Checks whether the host compositor currently focuses the Xephyr window. */
static bool nest_active_window_is(const char *name) {
    FILE *f = popen("hyprctl activewindow 2>/dev/null", "r");
    if (f == NULL) {
        return false;
    }
    char buf[4096];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    pclose(f);
    return strstr(buf, name) != NULL;
}

static bool nest_focus_hyprland_once(const char *name) {
    char title[128];
    char klass[128];
    snprintf(title, sizeof(title), "title:%s", name);
    snprintf(klass, sizeof(klass), "class:%s", name);

    /* Stock Hyprland. */
    char *by_title[] = {"hyprctl", "dispatch", "focuswindow", title, NULL};
    if (nest_run_quiet(by_title)) {
        return nest_active_window_is(name);
    }
    char *by_class[] = {"hyprctl", "dispatch", "focuswindow", klass, NULL};
    if (nest_run_quiet(by_class)) {
        return nest_active_window_is(name);
    }

    /* Hyprland builds with the Lua dispatcher API. */
    char lua[512];
    snprintf(lua, sizeof(lua),
             "local n='%s' "
             "for _,w in ipairs(hl.get_windows()) do "
             "if w.title==n or w.class==n then hl.dsp.focus(w) break end end "
             "return 'ok'",
             name);
    char *by_lua[] = {"hyprctl", "repl", lua, NULL};
    if (nest_run_quiet(by_lua)) {
        usleep(100000);
        return nest_active_window_is(name);
    }
    return false;
}

static bool nest_focus_sway_once(const char *name) {
    char query[160];
    snprintf(query, sizeof(query), "[title=\"%s\"] focus", name);
    char *by_title[] = {"swaymsg", query, NULL};
    if (nest_run_quiet(by_title)) {
        return true;
    }
    snprintf(query, sizeof(query), "[app_id=\"%s\"] focus", name);
    char *by_app_id[] = {"swaymsg", query, NULL};
    return nest_run_quiet(by_app_id);
}

/* Under a Wayland compositor, XSetInputFocus() only changes the focus inside
 * Xwayland; the compositor decides which surface receives the keyboard. Ask
 * the host to focus the Xephyr window through its own IPC instead. */
static char *nest_capture(const char *command, size_t *length);
static char *nest_niri_socket(void);
static bool nest_niri_available(void);
static bool nest_focus_niri_once(const char *title);

static void nest_focus_wayland(const char *name) {
    if (getenv("WAYLAND_DISPLAY") == NULL) {
        return;
    }

    const bool hyprland = getenv("HYPRLAND_INSTANCE_SIGNATURE") != NULL;
    const bool sway = getenv("SWAYSOCK") != NULL;
    const bool niri = !hyprland && !sway && nest_niri_available();
    if (!hyprland && !sway && !niri) {
        return;
    }

    for (int attempt = 0; attempt < 20; attempt++) {
        bool focused;
        if (hyprland) {
            focused = nest_focus_hyprland_once(name);
        } else if (sway) {
            focused = nest_focus_sway_once(name);
        } else {
            focused = nest_focus_niri_once(name);
        }
        if (focused) {
            LOG("nest mode: focused the Xephyr window via the Wayland host\n");
            return;
        }
        usleep(100000);
    }

    LOG("nest mode: could not focus the Xephyr window via the Wayland host; click it manually\n");
}

/* --- Hyprland host bind suspension ---------------------------------------
 *
 * On Wayland the compositor consumes its global shortcuts before Xephyr sees
 * them. While a nested i3 session is running we therefore temporarily remove
 * the host's SUPER bindings (Hyprland's Lua API), so $mod+Return, $mod+d, etc.
 * reach the nested instance. They are restored with `hyprctl reload` when the
 * nest exits. SUPER+Q is kept as an emergency way to close Xephyr.
 */
static bool nest_host_binds_suspended = false;

static char *nest_capture(const char *command, size_t *length) {
    FILE *f = popen(command, "r");
    if (f == NULL) {
        return NULL;
    }

    size_t cap = 8192;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        pclose(f);
        return NULL;
    }

    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                pclose(f);
                return NULL;
            }
            buf = nb;
        }
        const size_t n = fread(buf + len, 1, 4096, f);
        len += n;
        if (n == 0) {
            break;
        }
    }
    buf[len] = '\0';
    pclose(f);
    if (length != NULL) {
        *length = len;
    }
    return buf;
}

/* niri (and forks such as clarion) publish their IPC socket as
 * <name>.wayland-<n>.<pid>.sock in the runtime dir; NIRI_SOCKET may also be
 * exported already. Returns a malloc'd path, or NULL. */
static char *nest_niri_socket(void) {
    const char *env = getenv("NIRI_SOCKET");
    if (env != NULL && env[0] != '\0') {
        return sstrdup(env);
    }

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime == NULL) {
        return NULL;
    }
    DIR *dir = opendir(runtime);
    if (dir == NULL) {
        return NULL;
    }

    char *found = NULL;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const size_t len = strlen(entry->d_name);
        if (len < 6 || strcmp(entry->d_name + len - 5, ".sock") != 0) {
            continue;
        }
        /* Other socket names use "-wayland-"; niri's own use ".wayland-". */
        if (strstr(entry->d_name, ".wayland-") == NULL) {
            continue;
        }
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", runtime, entry->d_name);
        found = sstrdup(path);
        break;
    }
    closedir(dir);
    return found;
}

static bool nest_niri_available(void) {
    char *sock = nest_niri_socket();
    if (sock == NULL) {
        return false;
    }
    free(sock);
    return true;
}

/* Focuses the first niri window whose title starts with the given prefix. */
static bool nest_focus_niri_once(const char *title) {
    char *sock = nest_niri_socket();
    if (sock == NULL) {
        return false;
    }
    setenv("NIRI_SOCKET", sock, 1);
    free(sock);

    char *json = nest_capture("niri msg -j windows 2>/dev/null", NULL);
    if (json == NULL) {
        return false;
    }

    char needle[160];
    snprintf(needle, sizeof(needle), "\"title\":\"%s", title);
    char *hit = strstr(json, needle);
    if (hit == NULL) {
        free(json);
        return false;
    }

    /* niri puts the window "id" before its "title". */
    char *idfield = NULL;
    for (char *p = json; p < hit; p++) {
        if (strncmp(p, "\"id\":", 5) == 0) {
            idfield = p + 5;
        }
    }
    if (idfield == NULL) {
        free(json);
        return false;
    }
    const long id = strtol(idfield, NULL, 10);
    free(json);
    if (id <= 0) {
        return false;
    }

    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%ld", id);
    char *argv[] = {"niri", "msg", "action", "focus-window", "--id", idbuf, NULL};
    return nest_run_quiet(argv);
}

typedef struct {
    int level;
    char last_key[32];
    char key[128];
    char submap[64];
    long modmask;
    bool mouse;
    char *lua;
    size_t lua_len;
    size_t lua_cap;
    int count;
} nest_bind_state;

static bool nest_lua_reserve(nest_bind_state *state, size_t extra) {
    if (state->lua_len + extra + 1 <= state->lua_cap) {
        return true;
    }
    size_t cap = state->lua_cap ? state->lua_cap : 4096;
    while (cap < state->lua_len + extra + 1) {
        cap *= 2;
    }
    char *nb = realloc(state->lua, cap);
    if (nb == NULL) {
        return false;
    }
    state->lua = nb;
    state->lua_cap = cap;
    return true;
}

static void nest_lua_append_raw(nest_bind_state *state, const char *text) {
    const size_t len = strlen(text);
    if (!nest_lua_reserve(state, len)) {
        return;
    }
    memcpy(state->lua + state->lua_len, text, len + 1);
    state->lua_len += len;
}

static void nest_lua_unbind(nest_bind_state *state, const char *spec) {
    char escaped[256];
    size_t out = 0;
    for (size_t i = 0; spec[i] != '\0' && out + 2 < sizeof(escaped); i++) {
        if (spec[i] == '"' || spec[i] == '\\') {
            escaped[out++] = '\\';
        }
        escaped[out++] = spec[i];
    }
    escaped[out] = '\0';

    char call[320];
    snprintf(call, sizeof(call), "pcall(hl.unbind, \"%s\"); ", escaped);
    nest_lua_append_raw(state, call);
}

static int nest_bind_map_start(void *ctx) {
    nest_bind_state *state = ctx;
    if (state->level == 1) {
        state->last_key[0] = '\0';
        state->key[0] = '\0';
        state->submap[0] = '\0';
        state->modmask = 0;
        state->mouse = false;
    }
    state->level++;
    return 1;
}

static int nest_bind_map_key(void *ctx, const unsigned char *key, size_t len) {
    nest_bind_state *state = ctx;
    if (state->level == 2 && len < sizeof(state->last_key)) {
        memcpy(state->last_key, key, len);
        state->last_key[len] = '\0';
    }
    return 1;
}

static int nest_bind_integer(void *ctx, long long value) {
    nest_bind_state *state = ctx;
    if (state->level == 2 && strcmp(state->last_key, "modmask") == 0) {
        state->modmask = (long)value;
    }
    return 1;
}

static int nest_bind_string(void *ctx, const unsigned char *value, size_t len) {
    nest_bind_state *state = ctx;
    if (state->level != 2) {
        return 1;
    }
    if (strcmp(state->last_key, "key") == 0 && len < sizeof(state->key)) {
        memcpy(state->key, value, len);
        state->key[len] = '\0';
    } else if (strcmp(state->last_key, "submap") == 0 && len < sizeof(state->submap)) {
        memcpy(state->submap, value, len);
        state->submap[len] = '\0';
    }
    return 1;
}

static int nest_bind_boolean(void *ctx, int value) {
    nest_bind_state *state = ctx;
    if (state->level == 2 && strcmp(state->last_key, "mouse") == 0) {
        state->mouse = value != 0;
    }
    return 1;
}

static int nest_bind_map_end(void *ctx) {
    nest_bind_state *state = ctx;
    if (state->level == 2) {
        const bool keep_q = (state->modmask == 64 && strcmp(state->key, "Q") == 0);
        if ((state->modmask & 64) != 0 && state->submap[0] == '\0' &&
            !state->mouse && state->key[0] != '\0' && !keep_q) {
            char spec[192];
            spec[0] = '\0';
            if ((state->modmask & 64) != 0) {
                strlcat(spec, "SUPER + ", sizeof(spec));
            }
            if ((state->modmask & 8) != 0) {
                strlcat(spec, "ALT + ", sizeof(spec));
            }
            if ((state->modmask & 4) != 0) {
                strlcat(spec, "CTRL + ", sizeof(spec));
            }
            if ((state->modmask & 1) != 0) {
                strlcat(spec, "SHIFT + ", sizeof(spec));
            }
            strlcat(spec, state->key, sizeof(spec));
            nest_lua_unbind(state, spec);
            state->count++;
        }
    }
    state->level--;
    return 1;
}

static int nest_bind_array_start(void *ctx) {
    ((nest_bind_state *)ctx)->level++;
    return 1;
}

static int nest_bind_array_end(void *ctx) {
    ((nest_bind_state *)ctx)->level--;
    return 1;
}

static void nest_resume_hyprland_binds(void) {
    if (!nest_host_binds_suspended) {
        return;
    }
    nest_host_binds_suspended = false;
    char *argv[] = {"hyprctl", "reload", NULL};
    nest_run_quiet(argv);
    LOG("nest mode: restored Hyprland bindings\n");
}

/* Restore the host bindings when the nest supervisor is interrupted. Only
 * async-signal-safe calls here (fork/exec). */
static void nest_resume_binds_on_signal(int sig) {
    if (nest_host_binds_suspended) {
        pid_t pid = fork();
        if (pid == 0) {
            execlp("hyprctl", "hyprctl", "reload", (char *)NULL);
            _exit(127);
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Returns true if this Hyprland build exposes the Lua dispatcher API. */
static bool nest_hyprland_lua_available(void) {
    size_t len = 0;
    char *out = nest_capture("hyprctl repl 'return type(hl)' 2>/dev/null", &len);
    const bool available = out != NULL && strstr(out, "table") != NULL;
    free(out);
    return available;
}

static void nest_suspend_hyprland_binds(void) {
    if (getenv("HYPRLAND_INSTANCE_SIGNATURE") == NULL) {
        return;
    }
    if (!nest_hyprland_lua_available()) {
        LOG("nest mode: Hyprland Lua bind API unavailable; host binds not suspended\n");
        return;
    }

    size_t len = 0;
    char *json = nest_capture("hyprctl binds -j 2>/dev/null", &len);
    if (json == NULL) {
        return;
    }

    static const yajl_callbacks callbacks = {
        .yajl_boolean = nest_bind_boolean,
        .yajl_integer = nest_bind_integer,
        .yajl_string = nest_bind_string,
        .yajl_start_map = nest_bind_map_start,
        .yajl_map_key = nest_bind_map_key,
        .yajl_end_map = nest_bind_map_end,
        .yajl_start_array = nest_bind_array_start,
        .yajl_end_array = nest_bind_array_end,
    };

    nest_bind_state state = {0};
    yajl_handle handle = yajl_alloc(&callbacks, NULL, &state);
    const yajl_status status = yajl_parse(handle, (const unsigned char *)json, (int)len);
    const bool parsed = (status == yajl_status_ok &&
                         yajl_complete_parse(handle) == yajl_status_ok);
    if (!parsed) {
        char *error = (char *)yajl_get_error(handle, 1, (const unsigned char *)json, (int)len);
        LOG("nest mode: could not parse `hyprctl binds -j`: %s\n", error);
        yajl_free_error(handle, (unsigned char *)error);
    }
    yajl_free(handle);
    free(json);

    if (!parsed || state.lua == NULL || state.count == 0) {
        free(state.lua);
        LOG("nest mode: no Hyprland SUPER bindings to suspend\n");
        return;
    }

    nest_lua_append_raw(&state, "return 'ok'");
    char *argv[] = {"hyprctl", "repl", state.lua, NULL};
    const bool ok = nest_run_quiet(argv);
    free(state.lua);
    if (!ok) {
        LOG("nest mode: failed to suspend Hyprland bindings\n");
        return;
    }

    nest_host_binds_suspended = true;
    atexit(nest_resume_hyprland_binds);
    signal(SIGINT, nest_resume_binds_on_signal);
    signal(SIGTERM, nest_resume_binds_on_signal);
    signal(SIGHUP, nest_resume_binds_on_signal);
    LOG("nest mode: suspended %d Hyprland SUPER binding(s)\n", state.count);
}

/* Returns true when $DISPLAY refers to a reachable X server. */
static bool nest_host_display_usable(void) {
    xcb_connection_t *host = xcb_connect(NULL, NULL);
    if (host == NULL) {
        return false;
    }
    const bool ok = xcb_connection_has_error(host) == 0;
    xcb_disconnect(host);
    return ok;
}

/* Runs "Xwayland -help" and reports whether the binary exists and whether the
 * given option (e.g. "-host-grab") is supported by it. */
static bool nest_xwayland_probe(const char *option) {
    FILE *help = popen("Xwayland -help 2>&1", "r");
    if (help == NULL) {
        return false;
    }
    bool found = option == NULL;
    char line[512];
    while (fgets(line, sizeof(line), help) != NULL) {
        if (option != NULL && strstr(line, option) != NULL) {
            found = true;
        }
    }
    pclose(help);
    return found;
}

/* Spawn the nested X server, re-run ourselves on the nested display and wait
 * for it. In the child process, returns so that normal startup continues; the
 * parent process never returns.
 *
 * With a reachable host X display, Xephyr is used: the nested session lives in
 * a normal X window. Wayland hosts without an X display (niri, or compositors
 * that never start Xwayland) get a rootful Xwayland instead, which is itself a
 * Wayland client showing the whole nested screen in one window. */
static void nest_run(void) {
    char sockfile[64];
    char disparg[16];
    char nest_name[32];
    /* The child unsets WAYLAND_DISPLAY for programs it starts, so remember
     * whether the host session is Wayland before forking. */
    const bool host_wayland = getenv("WAYLAND_DISPLAY") != NULL;
    const bool use_xwayland = !nest_host_display_usable();

    if (!nest_display_explicit) {
        nest_display = nest_find_free_display();
    }

    snprintf(sockfile, sizeof(sockfile), "/tmp/.X11-unix/X%d", nest_display);
    snprintf(disparg, sizeof(disparg), ":%d", nest_display);
    /* Unique per display so that several nested sessions can run at the same
     * time without nest_focus_xephyr() picking the wrong window. */
    snprintf(nest_name, sizeof(nest_name), "i3-nest-%d", nest_display);

    if (nest_display_in_use(nest_display)) {
        errx(EXIT_FAILURE, "nest mode: display :%d is already in use (pass --nest=N to choose another display)",
             nest_display);
    }

    if (use_xwayland) {
        if (!host_wayland) {
            errx(EXIT_FAILURE,
                 "nest mode: no usable host display: DISPLAY is not set and this is not a Wayland session");
        }
        if (!nest_xwayland_probe(NULL)) {
            errx(EXIT_FAILURE,
                 "nest mode: no usable host display: DISPLAY is not set and Xwayland is not installed");
        }
    }
    const bool xwayland_host_grab = use_xwayland && nest_xwayland_probe("-host-grab");

    nest_ensure_system_libs();

    pid_t i3_pid = fork();
    if (i3_pid == -1) {
        err(EXIT_FAILURE, "fork");
    }

    if (i3_pid == 0) {
        /* child: run i3 on the nested display. Xephyr is started by the
         * parent in parallel; wait for it to accept connections. */
        setenv("DISPLAY", disparg, 1);
        /* Programs started from the nested instance must not talk to the host
         * session: when WAYLAND_DISPLAY is set (the usual case when i3 runs
         * nested inside Hyprland/Sway), GTK, Electron and Chromium pick their
         * Wayland backend and open their windows on the *host* desktop instead
         * of inside Xephyr. The nested session is X11-only, so hide the host
         * Wayland socket and advertise a plain X11 session. */
        unsetenv("WAYLAND_DISPLAY");
        unsetenv("WAYLAND_SOCKET");
        setenv("XDG_SESSION_TYPE", "x11", 1);
        /* The nested session is X11-only: pin GTK to its X11 backend. Host
         * sessions often export GDK_BACKEND=wayland; inheriting it makes GTK
         * try Wayland, fail, and give up ("Failed to open display") instead of
         * falling back to X11. */
        setenv("GDK_BACKEND", "x11", 1);
        /* Drop the host i3's IPC socket path so that this instance gets its
         * own socket, otherwise it refuses to start with "Socket is already
         * in use". */
        unsetenv("I3SOCK");
        nest_isolate_session();
        for (int i = 0; i < 100; i++) {
            if (access(sockfile, F_OK) == 0) {
                break;
            }
            usleep(100000);
        }
        if (access(sockfile, F_OK) != 0) {
            errx(EXIT_FAILURE, "nest mode: nested X server did not start on :%d", nest_display);
        }
        LOG("nest mode: running on :%d\n", nest_display);
        if (use_xwayland) {
            fprintf(stderr,
                    "nest mode: no host X display found, so the nested session runs in a rootful\n"
                    "nest mode: Xwayland window of the host compositor. Click that window if the\n"
                    "nest mode: compositor did not give it the keyboard focus.\n");
        } else {
            fprintf(stderr,
                    "nest mode: the keyboard is captured by Xephyr while its window has the focus.\n"
                    "nest mode: press Ctrl+Shift to release it to the host, press again to re-grab.\n"
                    "nest mode: a Wayland host compositor may still consume its own global shortcuts.\n");
        }
        if (host_wayland) {
            if (getenv("HYPRLAND_INSTANCE_SIGNATURE") != NULL) {
                fprintf(stderr,
                        "nest mode: Hyprland detected: SUPER bindings are suspended while the nest\n"
                        "nest mode: runs and restored on exit.\n");
            } else if (xwayland_host_grab) {
                fprintf(stderr,
                        "nest mode: the compositor's global shortcuts are inhibited while the\n"
                        "nest mode: Xwayland window has the keyboard focus.\n");
            } else {
                fprintf(stderr,
                        "nest mode: the compositor owns global shortcuts; disable the conflicting\n"
                        "nest mode: ones while testing.\n");
            }
        }
        /* Process isolation: keep killall/pkill/ps inside the nest from
         * seeing (and killing) host processes. */
        setenv("I3_NEST_DISPLAY", disparg, 1);
        nest_enter_pid_namespace();
        return;
    }

    pid_t server_pid = fork();
    if (server_pid == -1) {
        err(EXIT_FAILURE, "fork");
    }
    if (server_pid == 0) {
        if (use_xwayland) {
            /* Rootful Xwayland shows the whole nested screen in one Wayland
             * window. -host-grab additionally asks the compositor to stop
             * consuming its global shortcuts while that window is focused
             * (requires the keyboard-shortcuts-inhibit protocol). */
            if (xwayland_host_grab) {
                execlp("Xwayland", "Xwayland", disparg, "-geometry", "1280x800",
                       "-ac", "-br", "-noreset", "-host-grab", (char *)NULL);
            } else {
                execlp("Xwayland", "Xwayland", disparg, "-geometry", "1280x800",
                       "-ac", "-br", "-noreset", (char *)NULL);
            }
        } else {
            /* Windowed instead of fullscreen: otherwise it is hard to tell the
             * nested instance apart from the real session. -name identifies the
             * window for nest_focus_xephyr(). */
            execlp("Xephyr", "Xephyr", disparg, "-ac", "-br",
                   "-name", nest_name, "-title", nest_name,
                   "-screen", "1280x800", (char *)NULL);
        }
        _exit(127);
    }

    LOG("nest mode: %s on :%d (pid %d), nested i3 (pid %d)\n",
        use_xwayland ? "rootful Xwayland" : "Xephyr", nest_display, server_pid, i3_pid);

    if (use_xwayland && nest_niri_available()) {
        /* Rootful Xwayland is just another window of the host compositor: if it
         * does not have the keyboard focus, every key (including the host's own
         * global shortcuts) goes to whatever else is focused and the session
         * looks like it "leaks" onto the host desktop. Focus it through niri's
         * IPC (niri and forks such as clarion). */
        char xtitle[64];
        snprintf(xtitle, sizeof(xtitle), "Xwayland on %s", disparg);
        for (int attempt = 0; attempt < 40; attempt++) {
            if (nest_focus_niri_once(xtitle)) {
                LOG("nest mode: focused the rootful Xwayland window\n");
                break;
            }
            usleep(100000);
        }
    }

    if (!use_xwayland) {
        /* Hand the keyboard to Xephyr so that $mod bindings and Alt+Tab are
         * handled by the nested instance only, not by the host as well. */
        nest_focus_xephyr(nest_name);
        /* XSetInputFocus() does not move the compositor-level focus on Wayland. */
        nest_focus_wayland(nest_name);
    }
    /* Typos in the nested config must not be "eaten" by the host WM. */
    nest_suspend_hyprland_binds();
    if (nest_host_binds_suspended) {
        fprintf(stderr,
                "nest mode: Hyprland SUPER bindings suspended; they are restored when the nest exits.\n");
    } else if (host_wayland && !(use_xwayland && xwayland_host_grab)) {
        fprintf(stderr,
                "nest mode: could not suspend host bindings automatically; disable conflicting\n"
                "nest mode: SUPER shortcuts in your compositor while testing.\n");
    }

    int status;
    waitpid(i3_pid, &status, 0);
    kill(server_pid, SIGTERM);
    int xstatus;
    waitpid(server_pid, &xstatus, 0);
    nest_resume_hyprland_binds();

    if (WIFEXITED(status)) {
        exit(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        LOG("nested i3 terminated by signal %d\n", WTERMSIG(status));
        exit(128 + WTERMSIG(status));
    }
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    /* Keep a symbol pointing to the I3_VERSION string constant so that we have
     * it in gdb backtraces. */
    static const char *_i3_version __attribute__((used)) = I3_VERSION;
    char *override_configpath = NULL;
    bool autostart = true;
    char *layout_path = NULL;
    bool delete_layout_path = false;
    bool disable_randr15 = false;
    char *fake_outputs = NULL;
    bool disable_signalhandler = false;
    bool only_check_config = false;
    bool replace_wm = false;
    bool nest_mode = false;
    static struct option long_options[] = {
        {"no-autostart", no_argument, 0, 'a'},
        {"config", required_argument, 0, 'c'},
        {"version", no_argument, 0, 'v'},
        {"moreversion", no_argument, 0, 'm'},
        {"more-version", no_argument, 0, 'm'},
        {"more_version", no_argument, 0, 'm'},
        {"help", no_argument, 0, 'h'},
        {"layout", required_argument, 0, 'L'},
        {"restart", required_argument, 0, 0},
        {"force-xinerama", no_argument, 0, 0},
        {"force_xinerama", no_argument, 0, 0},
        {"disable-randr15", no_argument, 0, 0},
        {"disable_randr15", no_argument, 0, 0},
        {"disable-signalhandler", no_argument, 0, 0},
        {"shmlog-size", required_argument, 0, 0},
        {"shmlog_size", required_argument, 0, 0},
        {"get-socketpath", no_argument, 0, 0},
        {"get_socketpath", no_argument, 0, 0},
        {"fake_outputs", required_argument, 0, 0},
        {"fake-outputs", required_argument, 0, 0},
        {"force-old-config-parser-v4.4-only", no_argument, 0, 0},
        {"nest", optional_argument, 0, 0},
        {"replace", no_argument, 0, 'r'},
        {0, 0, 0, 0}};
    int option_index = 0, opt;

    setlocale(LC_ALL, "");

    /* Get the RLIMIT_CORE limit at startup time to restore this before
     * starting processes. */
    getrlimit(RLIMIT_CORE, &original_rlimit_core);

    /* Disable output buffering to make redirects in .xsession actually useful for debugging */
    if (!isatty(fileno(stdout))) {
        setbuf(stdout, NULL);
    }

    srand(time(NULL));

    /* Init logging *before* initializing debug_build to guarantee early
     * (file) logging. */
    init_logging();

    /* Running as the inner instance of --nest (private PID/mount namespace):
     * keep host browser profiles out of the nested session. */
    if (getenv("I3_NEST_INNER") != NULL) {
        nest_isolate_browser_profiles();
    }

    /* On release builds, disable SHM logging by default. */
    shmlog_size = (is_debug_build() || strstr(argv[0], "i3-with-shmlog") != NULL ? default_shmlog_size : 0);

    start_argv = argv;

    while ((opt = getopt_long(argc, argv, "c:CvmaL:hld:Vr", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'a':
                LOG("Autostart disabled using -a\n");
                autostart = false;
                break;
            case 'L':
                FREE(layout_path);
                layout_path = sstrdup(optarg);
                delete_layout_path = false;
                break;
            case 'c':
                FREE(override_configpath);
                override_configpath = sstrdup(optarg);
                break;
            case 'C':
                LOG("Checking configuration file only (-C)\n");
                only_check_config = true;
                break;
            case 'v':
                printf("i3 version %s © 2009 Michael Stapelberg and contributors\n", i3_version);
                exit(EXIT_SUCCESS);
                break;
            case 'm':
                printf("Binary i3 version:  %s © 2009 Michael Stapelberg and contributors\n", i3_version);
                display_running_version();
                exit(EXIT_SUCCESS);
                break;
            case 'V':
                set_verbosity(true);
                break;
            case 'd':
                LOG("Enabling debug logging\n");
                set_debug_logging(true);
                break;
            case 'l':
                /* DEPRECATED, ignored for the next 3 versions (3.e, 3.f, 3.g) */
                break;
            case 'r':
                replace_wm = true;
                break;
            case 0:
                if (strcmp(long_options[option_index].name, "force-xinerama") == 0 ||
                    strcmp(long_options[option_index].name, "force_xinerama") == 0) {
                    force_xinerama = true;
                    ELOG("Using Xinerama instead of RandR. This option should be "
                         "avoided at all cost because it does not refresh the list "
                         "of screens, so you cannot configure displays at runtime. "
                         "Please check if your driver really does not support RandR "
                         "and disable this option as soon as you can.\n");
                    break;
                } else if (strcmp(long_options[option_index].name, "disable-randr15") == 0 ||
                           strcmp(long_options[option_index].name, "disable_randr15") == 0) {
                    disable_randr15 = true;
                    break;
                } else if (strcmp(long_options[option_index].name, "disable-signalhandler") == 0) {
                    disable_signalhandler = true;
                    break;
                } else if (strcmp(long_options[option_index].name, "get-socketpath") == 0 ||
                           strcmp(long_options[option_index].name, "get_socketpath") == 0) {
                    char *socket_path = root_atom_contents("I3_SOCKET_PATH", NULL, 0);
                    if (socket_path) {
                        printf("%s\n", socket_path);
                        /* With -O2 (i.e. the buildtype=debugoptimized meson
                         * option, which we set by default), gcc 9.2.1 optimizes
                         * away socket_path at this point, resulting in a Leak
                         * Sanitizer report. An explicit free helps: */
                        free(socket_path);
                        exit(EXIT_SUCCESS);
                    }

                    exit(EXIT_FAILURE);
                } else if (strcmp(long_options[option_index].name, "shmlog-size") == 0 ||
                           strcmp(long_options[option_index].name, "shmlog_size") == 0) {
                    shmlog_size = atoi(optarg);
                    /* Re-initialize logging immediately to get as many
                     * logmessages as possible into the SHM log. */
                    init_logging();
                    LOG("Limiting SHM log size to %d bytes\n", shmlog_size);
                    break;
                } else if (strcmp(long_options[option_index].name, "restart") == 0) {
                    FREE(layout_path);
                    layout_path = sstrdup(optarg);
                    delete_layout_path = true;
                    break;
                } else if (strcmp(long_options[option_index].name, "fake-outputs") == 0 ||
                           strcmp(long_options[option_index].name, "fake_outputs") == 0) {
                    LOG("Initializing fake outputs: %s\n", optarg);
                    fake_outputs = sstrdup(optarg);
                    break;
                } else if (strcmp(long_options[option_index].name, "force-old-config-parser-v4.4-only") == 0) {
                    ELOG("You are passing --force-old-config-parser-v4.4-only, but that flag was removed by now.\n");
                    break;
                } else if (strcmp(long_options[option_index].name, "nest") == 0) {
                    nest_mode = true;
                    if (optarg) {
                        nest_display = atoi(optarg);
                        if (nest_display <= 0) {
                            errx(EXIT_FAILURE, "Invalid --nest display number: %s", optarg);
                        }
                        nest_display_explicit = true;
                    }
                    break;
                }
            /* fall-through */
            default:
                fprintf(stderr, "Usage: %s [-c configfile] [-d all] [-a] [-v] [-V] [-C]\n", argv[0]);
                fprintf(stderr, "\n");
                fprintf(stderr, "\t-a          disable autostart ('exec' lines in config)\n");
                fprintf(stderr, "\t-c <file>   use the provided configfile instead\n");
                fprintf(stderr, "\t-C          validate configuration file and exit\n");
                fprintf(stderr, "\t-d all      enable debug output\n");
                fprintf(stderr, "\t-L <file>   path to the serialized layout during restarts\n");
                fprintf(stderr, "\t-v          display version and exit\n");
                fprintf(stderr, "\t-V          enable verbose mode\n");
                fprintf(stderr, "\n");
                fprintf(stderr, "\t--force-xinerama\n"
                                "\tUse Xinerama instead of RandR.\n"
                                "\tThis option should only be used if you are stuck with the\n"
                                "\told nVidia closed source driver (older than 302.17), which does\n"
                                "\tnot support RandR.\n");
                fprintf(stderr, "\n");
                fprintf(stderr, "\t--get-socketpath\n"
                                "\tRetrieve the i3 IPC socket path from X11, print it, then exit.\n");
                fprintf(stderr, "\n");
                fprintf(stderr, "\t--shmlog-size <limit>\n"
                                "\tLimits the size of the i3 SHM log to <limit> bytes. Setting this\n"
                                "\tto 0 disables SHM logging entirely.\n"
                                "\tThe default is %d bytes.\n",
                        shmlog_size);
                fprintf(stderr, "\n");
                fprintf(stderr, "\t--replace\n"
                                "\tReplace an existing window manager.\n");
                fprintf(stderr, "\n");
                fprintf(stderr, "\t--nest[=DISPLAY]\n"
                                "\tTest mode: run i3 inside a private fullscreen\n"
                                "\tXephyr window to simulate a real session. The\n"
                                "\tconfig's $mod (Super) is additionally bound to\n"
                                "\tAlt, since the host window manager (e.g.\n"
                                "\tHyprland) usually owns Super. DISPLAY selects\n"
                                "\tthe display number (default 99).\n");
                fprintf(stderr, "\n");
                fprintf(stderr, "If you pass plain text arguments, i3 will interpret them as a command\n"
                                "to send to a currently running i3 (like i3-msg). This allows you to\n"
                                "use nice and logical commands, such as:\n"
                                "\n"
                                "\ti3 border none\n"
                                "\ti3 floating toggle\n"
                                "\ti3 kill window\n"
                                "\n");
                exit(opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE);
        }
    }

    if (nest_mode) {
        nest_run();
    }

    if (only_check_config) {
        /* load_configuration() prints a detailed report; keep the raw log lines
         * out of the terminal (they still end up in the error log file). */
        log_set_silent(true);
        exit(load_configuration(override_configpath, C_VALIDATE) ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    /* If the user passes more arguments, we act like i3-msg would: Just send
     * the arguments as an IPC message to i3. This allows for nice semantic
     * commands such as 'i3 border none'. */
    if (optind < argc) {
        /* We enable verbose mode so that the user knows what’s going on.
         * This should make it easier to find mistakes when the user passes
         * arguments by mistake. */
        set_verbosity(true);

        LOG("Additional arguments passed. Sending them as a command to i3.\n");
        char *payload = NULL;
        while (optind < argc) {
            if (!payload) {
                payload = sstrdup(argv[optind]);
            } else {
                char *both;
                sasprintf(&both, "%s %s", payload, argv[optind]);
                free(payload);
                payload = both;
            }
            optind++;
        }
        DLOG("Command is: %s (%zd bytes)\n", payload, strlen(payload));
        char *socket_path = root_atom_contents("I3_SOCKET_PATH", NULL, 0);
        if (!socket_path) {
            ELOG("Could not get i3 IPC socket path\n");
            return 1;
        }

        int sockfd = socket(AF_LOCAL, SOCK_STREAM, 0);
        if (sockfd == -1) {
            err(EXIT_FAILURE, "Could not create socket");
        }

        struct sockaddr_un addr = {0};
        addr.sun_family = AF_LOCAL;
        strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
        FREE(socket_path);
        if (connect(sockfd, (const struct sockaddr *)&addr, sizeof(struct sockaddr_un)) < 0) {
            err(EXIT_FAILURE, "Could not connect to i3");
        }

        if (ipc_send_message(sockfd, strlen(payload), I3_IPC_MESSAGE_TYPE_RUN_COMMAND,
                             (uint8_t *)payload) == -1) {
            err(EXIT_FAILURE, "IPC: write()");
        }
        FREE(payload);

        uint32_t reply_length;
        uint32_t reply_type;
        uint8_t *reply;
        int ret;
        if ((ret = ipc_recv_message(sockfd, &reply_type, &reply_length, &reply)) != 0) {
            if (ret == -1) {
                err(EXIT_FAILURE, "IPC: read()");
            }
            return 1;
        }
        if (reply_type != I3_IPC_REPLY_TYPE_COMMAND) {
            errx(EXIT_FAILURE, "IPC: received reply of type %d but expected %d (COMMAND)", reply_type, I3_IPC_REPLY_TYPE_COMMAND);
        }
        printf("%.*s\n", reply_length, (char *)reply);
        FREE(reply);
        return 0;
    }

    /* Enable logging to handle the case when the user did not specify --shmlog-size */
    init_logging();

    /* Try to enable core dumps by default when running a debug build */
    if (is_debug_build()) {
        struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};
        setrlimit(RLIMIT_CORE, &limit);
        LOG("CORE DUMPS: You are running a development version of i3, so coredumps were automatically enabled (ulimit -c unlimited).\n");

#ifdef __linux__
        /* The following code is helpful, but not required. We thus don’t pay
         * much attention to error handling, non-linux or other edge cases. */
        char *cwd = getcwd(NULL, 0);
        if (cwd != NULL) {
            LOG("CORE DUMPS: Your current working directory is \"%s\".\n", cwd);
            free(cwd);
        }
        const size_t buffer_size = 1024;
        char *buffer = scalloc(buffer_size, sizeof(char));

        int patternfd;
        if ((patternfd = open("/proc/sys/kernel/core_pattern", O_RDONLY)) >= 0) {
            if (read(patternfd, buffer, buffer_size - 1) > 0) {
                /* a trailing newline is included in buffer */
                LOG("CORE DUMPS: Your core_pattern is: %s", buffer);
            }
            close(patternfd);
        }
        free(buffer);
#endif
    }

    LOG("i3 %s starting\n", _i3_version);

    conn = xcb_connect(NULL, &conn_screen);
    if (xcb_connection_has_error(conn)) {
        errx(EXIT_FAILURE, "Cannot open display");
    }

    sndisplay = sn_xcb_display_new(conn, NULL, NULL);

    /* Initialize the libev event loop. This needs to be done before loading
     * the config file because the parser will install an ev_child watcher
     * for the nagbar when config errors are found.
     *
     * Main loop must be ev's default loop because (at the moment of writing)
     * only the default loop can handle ev_child events and reap zombies
     * (the start_application routine relies on that too). */
    main_loop = EV_DEFAULT;
    if (main_loop == NULL) {
        die("Could not initialize libev. Bad LIBEV_FLAGS?\n");
    }

    animation_init();

    root_screen = xcb_aux_get_screen(conn, conn_screen);
    root = root_screen->root;

    /* Prefetch X11 extensions that we are interested in. */
    xcb_prefetch_extension_data(conn, &xcb_xkb_id);
    xcb_prefetch_extension_data(conn, &xcb_shape_id);
    /* BIG-REQUESTS is used by libxcb internally. */
    xcb_prefetch_extension_data(conn, &xcb_big_requests_id);
    if (force_xinerama) {
        xcb_prefetch_extension_data(conn, &xcb_xinerama_id);
    } else {
        xcb_prefetch_extension_data(conn, &xcb_randr_id);
    }

    /* Prepare for us to get a current timestamp as recommended by ICCCM */
    xcb_change_window_attributes(conn, root, XCB_CW_EVENT_MASK, (uint32_t[]){XCB_EVENT_MASK_PROPERTY_CHANGE});
    xcb_change_property(conn, XCB_PROP_MODE_APPEND, root, XCB_ATOM_SUPERSCRIPT_X, XCB_ATOM_CARDINAL, 32, 0, "");

    /* Place requests for the atoms we need as soon as possible */
#define xmacro(atom) \
    xcb_intern_atom_cookie_t atom##_cookie = xcb_intern_atom(conn, 0, strlen(#atom), #atom);
    I3_NET_SUPPORTED_ATOMS_XMACRO
    I3_REST_ATOMS_XMACRO
#undef xmacro

    root_depth = root_screen->root_depth;
    colormap = root_screen->default_colormap;
    visual_type = xcb_aux_find_visual_by_attrs(root_screen, -1, 32);
    if (visual_type != NULL) {
        root_depth = xcb_aux_get_depth_of_visual(root_screen, visual_type->visual_id);
        colormap = xcb_generate_id(conn);

        xcb_void_cookie_t cm_cookie = xcb_create_colormap_checked(conn,
                                                                  XCB_COLORMAP_ALLOC_NONE,
                                                                  colormap,
                                                                  root,
                                                                  visual_type->visual_id);

        xcb_generic_error_t *error = xcb_request_check(conn, cm_cookie);
        if (error != NULL) {
            ELOG("Could not create colormap. Error code: %d\n", error->error_code);
            exit(EXIT_FAILURE);
        }
    } else {
        visual_type = get_visualtype(root_screen);
    }

    xcb_prefetch_maximum_request_length(conn);

    init_dpi();

    DLOG("root_depth = %d, visual_id = 0x%08x.\n", root_depth, visual_type->visual_id);
    DLOG("root_screen->height_in_pixels = %d, root_screen->height_in_millimeters = %d\n",
         root_screen->height_in_pixels, root_screen->height_in_millimeters);
    DLOG("One logical pixel corresponds to %d physical pixels on this display.\n", logical_px(1));

    xcb_get_geometry_cookie_t gcookie = xcb_get_geometry(conn, root);
    xcb_query_pointer_cookie_t pointercookie = xcb_query_pointer(conn, root);

    /* Get the PropertyNotify event we caused above */
    xcb_flush(conn);
    {
        xcb_generic_event_t *event;
        DLOG("waiting for PropertyNotify event\n");
        while ((event = xcb_wait_for_event(conn)) != NULL) {
            if (event->response_type == XCB_PROPERTY_NOTIFY) {
                last_timestamp = ((xcb_property_notify_event_t *)event)->time;
                free(event);
                break;
            }
            free(event);
        }
        DLOG("got timestamp %d\n", last_timestamp);
    }

    /* Setup NetWM atoms */
#define xmacro(name)                                                                       \
    do {                                                                                   \
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, name##_cookie, NULL); \
        if (!reply) {                                                                      \
            ELOG("Could not get atom " #name "\n");                                        \
            exit(-1);                                                                      \
        }                                                                                  \
        A_##name = reply->atom;                                                            \
        free(reply);                                                                       \
    } while (0);
    I3_NET_SUPPORTED_ATOMS_XMACRO
    I3_REST_ATOMS_XMACRO
#undef xmacro

    load_configuration(override_configpath, C_LOAD);

    /* Hyprland-style automatic config reload. */
    config_watcher_init();

    if (config.ipc_socket_path == NULL) {
        /* Fall back to a file name in /tmp/ based on the PID */
        if ((config.ipc_socket_path = getenv("I3SOCK")) == NULL) {
            config.ipc_socket_path = get_process_filename("ipc-socket");
        } else {
            config.ipc_socket_path = sstrdup(config.ipc_socket_path);
        }
    }
    /* Create the UNIX domain socket for IPC */
    int ipc_socket = create_socket(config.ipc_socket_path, &current_socketpath);
    if (ipc_socket == -1) {
        die("Could not create the IPC socket: %s", config.ipc_socket_path);
    }

    if (config.force_xinerama) {
        force_xinerama = true;
    }

    /* Acquire the WM_Sn selection. */
    {
        /* Get the WM_Sn atom */
        char *atom_name = xcb_atom_name_by_screen("WM", conn_screen);
        wm_sn_selection_owner = xcb_generate_id(conn);

        if (atom_name == NULL) {
            ELOG("xcb_atom_name_by_screen(\"WM\", %d) failed, exiting\n", conn_screen);
            return 1;
        }

        xcb_intern_atom_reply_t *atom_reply;
        atom_reply = xcb_intern_atom_reply(conn,
                                           xcb_intern_atom_unchecked(conn,
                                                                     0,
                                                                     strlen(atom_name),
                                                                     atom_name),
                                           NULL);
        free(atom_name);
        if (atom_reply == NULL) {
            ELOG("Failed to intern the WM_Sn atom, exiting\n");
            return 1;
        }
        wm_sn = atom_reply->atom;
        free(atom_reply);

        /* Check if the selection is already owned */
        xcb_get_selection_owner_reply_t *selection_reply =
            xcb_get_selection_owner_reply(conn,
                                          xcb_get_selection_owner(conn, wm_sn),
                                          NULL);
        if (selection_reply && selection_reply->owner != XCB_NONE && !replace_wm) {
            ELOG("Another window manager is already running (WM_Sn is owned)");
            return 1;
        }

        /* Become the selection owner */
        xcb_create_window(conn,
                          root_screen->root_depth,
                          wm_sn_selection_owner, /* window id */
                          root_screen->root,     /* parent */
                          -1, -1, 1, 1,          /* geometry */
                          0,                     /* border width */
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          root_screen->root_visual,
                          0, NULL);
        xcb_change_property(conn,
                            XCB_PROP_MODE_REPLACE,
                            wm_sn_selection_owner,
                            XCB_ATOM_WM_CLASS,
                            XCB_ATOM_STRING,
                            8,
                            (strlen("i3-WM_Sn") + 1) * 2,
                            "i3-WM_Sn\0i3-WM_Sn\0");

        xcb_set_selection_owner(conn, wm_sn_selection_owner, wm_sn, last_timestamp);

        if (selection_reply && selection_reply->owner != XCB_NONE) {
            int check_rounds = 150; /* Wait for a maximum of 15 seconds */
            xcb_get_geometry_reply_t *geom_reply = NULL;

            DLOG("waiting for old WM_Sn selection owner to exit");
            do {
                const unsigned int usleep_time = 100000;
                free(geom_reply);
                usleep(usleep_time);
                if (check_rounds-- == 0) {
                    ELOG("The old window manager is not exiting");
                    return 1;
                }
                geom_reply = xcb_get_geometry_reply(conn,
                                                    xcb_get_geometry(conn, selection_reply->owner),
                                                    NULL);
            } while (geom_reply != NULL);
        }
        free(selection_reply);

        /* Announce that we are the new owner */
        /* Every X11 event is 32 bytes long. Therefore, XCB will copy 32 bytes.
         * In order to properly initialize these bytes, we allocate 32 bytes even
         * though we only need less for an xcb_client_message_event_t */
        union {
            xcb_client_message_event_t message;
            char storage[32];
        } event = {0};
        event.message.response_type = XCB_CLIENT_MESSAGE;
        event.message.window = root_screen->root;
        event.message.format = 32;
        event.message.type = A_MANAGER;
        event.message.data.data32[0] = last_timestamp;
        event.message.data.data32[1] = wm_sn;
        event.message.data.data32[2] = wm_sn_selection_owner;

        xcb_send_event(conn, 0, root_screen->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, event.storage);
    }

    xcb_void_cookie_t cookie;
    cookie = xcb_change_window_attributes_checked(conn, root, XCB_CW_EVENT_MASK, (uint32_t[]){ROOT_EVENT_MASK});
    xcb_generic_error_t *error = xcb_request_check(conn, cookie);
    if (error != NULL) {
        ELOG("Another window manager seems to be running (X error %d)\n", error->error_code);
#ifdef I3_ASAN_ENABLED
        __lsan_do_leak_check();
#endif
        return 1;
    }

    xcb_get_geometry_reply_t *greply = xcb_get_geometry_reply(conn, gcookie, NULL);
    if (greply == NULL) {
        ELOG("Could not get geometry of the root window, exiting\n");
        return 1;
    }
    DLOG("root geometry reply: (%d, %d) %d x %d\n", greply->x, greply->y, greply->width, greply->height);

    xcursor_load_cursors();

    /* Set a cursor for the root window (otherwise the root window will show no
       cursor until the first client is launched). */
    xcursor_set_root_cursor(XCURSOR_CURSOR_POINTER);

    const xcb_query_extension_reply_t *extreply;
    extreply = xcb_get_extension_data(conn, &xcb_xkb_id);
    xkb_supported = extreply->present;
    if (!extreply->present) {
        DLOG("xkb is not present on this server\n");
    } else {
        DLOG("initializing xcb-xkb\n");
        xcb_xkb_use_extension(conn, XCB_XKB_MAJOR_VERSION, XCB_XKB_MINOR_VERSION);
        xcb_xkb_select_events(conn,
                              XCB_XKB_ID_USE_CORE_KBD,
                              XCB_XKB_EVENT_TYPE_STATE_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY,
                              0,
                              XCB_XKB_EVENT_TYPE_STATE_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY,
                              0xff,
                              0xff,
                              NULL);

        /* Setting both, XCB_XKB_PER_CLIENT_FLAG_GRABS_USE_XKB_STATE and
         * XCB_XKB_PER_CLIENT_FLAG_LOOKUP_STATE_WHEN_GRABBED, will lead to the
         * X server sending us the full XKB state in KeyPress and KeyRelease:
         * https://cgit.freedesktop.org/xorg/xserver/tree/xkb/xkbEvents.c?h=xorg-server-1.20.0#n927
         *
         * XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT enable detectable autorepeat:
         * https://www.x.org/releases/current/doc/kbproto/xkbproto.html#Detectable_Autorepeat
         * This affects bindings using the --release flag: instead of getting multiple KeyRelease
         * events we get only one event when the key is physically released by the user.
         */
        const uint32_t mask = XCB_XKB_PER_CLIENT_FLAG_GRABS_USE_XKB_STATE |
                              XCB_XKB_PER_CLIENT_FLAG_LOOKUP_STATE_WHEN_GRABBED |
                              XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT;
        xcb_xkb_per_client_flags_reply_t *pcf_reply;
        /* The last three parameters are unset because they are only relevant
         * when using a feature called “automatic reset of boolean controls”:
         * https://www.x.org/releases/X11R7.7/doc/kbproto/xkbproto.html#Automatic_Reset_of_Boolean_Controls
         * */
        pcf_reply = xcb_xkb_per_client_flags_reply(
            conn,
            xcb_xkb_per_client_flags(
                conn,
                XCB_XKB_ID_USE_CORE_KBD,
                mask,
                mask,
                0 /* uint32_t ctrlsToChange */,
                0 /* uint32_t autoCtrls */,
                0 /* uint32_t autoCtrlsValues */),
            NULL);

#define PCF_REPLY_ERROR(_value)                                    \
    do {                                                           \
        if (pcf_reply == NULL || !(pcf_reply->value & (_value))) { \
            ELOG("Could not set " #_value "\n");                   \
        }                                                          \
    } while (0)

        PCF_REPLY_ERROR(XCB_XKB_PER_CLIENT_FLAG_GRABS_USE_XKB_STATE);
        PCF_REPLY_ERROR(XCB_XKB_PER_CLIENT_FLAG_LOOKUP_STATE_WHEN_GRABBED);
        PCF_REPLY_ERROR(XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT);

        free(pcf_reply);
        xkb_base = extreply->first_event;
    }

    /* Check for Shape extension. We want to handle input shapes which is
     * introduced in 1.1. */
    extreply = xcb_get_extension_data(conn, &xcb_shape_id);
    if (extreply->present) {
        shape_base = extreply->first_event;
        xcb_shape_query_version_cookie_t cookie = xcb_shape_query_version(conn);
        xcb_shape_query_version_reply_t *version =
            xcb_shape_query_version_reply(conn, cookie, NULL);
        shape_supported = version && version->minor_version >= 1;
        free(version);
    } else {
        shape_supported = false;
    }
    if (!shape_supported) {
        DLOG("shape 1.1 is not present on this server\n");
    }

    restore_connect();

    property_handlers_init();

    ewmh_setup_hints();

    keysyms = xcb_key_symbols_alloc(conn);

    xcb_numlock_mask = aio_get_mod_mask_for(XCB_NUM_LOCK, keysyms);

    if (!load_keymap()) {
        die("Could not load keymap\n");
    }

    /* Raw key tracking for the i3bar status line (XInput2, does not consume
     * events). Must run after the xkb keymap is available. */
    key_trace_init();

    translate_keysyms();
    grab_all_keys(conn);

    bool needs_tree_init = true;
    if (layout_path != NULL) {
        LOG("Trying to restore the layout from \"%s\".\n", layout_path);
        needs_tree_init = !tree_restore(layout_path, greply);
        if (delete_layout_path) {
            unlink(layout_path);
            const char *dir = dirname(layout_path);
            /* possibly fails with ENOTEMPTY if there are files (or
             * sockets) left. */
            rmdir(dir);
        }
    }
    if (needs_tree_init) {
        tree_init(greply);
    }

    free(greply);

    /* Setup fake outputs for testing */
    if (fake_outputs == NULL && config.fake_outputs != NULL) {
        fake_outputs = config.fake_outputs;
    }

    if (fake_outputs != NULL) {
        fake_outputs_init(fake_outputs);
        FREE(fake_outputs);
        config.fake_outputs = NULL;
    } else if (force_xinerama) {
        /* Force Xinerama (for drivers which don't support RandR yet, esp. the
         * nVidia binary graphics driver), when specified either in the config
         * file or on command-line */
        xinerama_init();
    } else {
        DLOG("Checking for XRandR...\n");
        randr_init(&randr_base, disable_randr15 || config.disable_randr15);
    }

    /* We need to force disabling outputs which have been loaded from the
     * layout file but are no longer active. This can happen if the output has
     * been disabled in the short time between writing the restart layout file
     * and restarting i3. See #2326. */
    if (layout_path != NULL && randr_base > -1) {
        Con *con;
        TAILQ_FOREACH (con, &(croot->nodes_head), nodes) {
            Output *output;
            TAILQ_FOREACH (output, &outputs, outputs) {
                if (output->active || strcmp(con->name, output_primary_name(output)) != 0) {
                    continue;
                }

                /* This will correctly correlate the output with its content
                 * container. We need to make the connection to properly
                 * disable the output. */
                if (output->con == NULL) {
                    output_init_con(output);
                    output->changed = false;
                }

                output->to_be_disabled = true;
                randr_disable_output(output);
            }
        }
    }
    FREE(layout_path);

    scratchpad_fix_resolution();

    Output *output = NULL;
    xcb_query_pointer_reply_t *pointer_reply = xcb_query_pointer_reply(conn, pointercookie, NULL);
    if (!pointer_reply) {
        ELOG("Could not query pointer position, using first screen\n");
    } else {
        DLOG("Pointer at %d, %d\n", pointer_reply->root_x, pointer_reply->root_y);
        output = get_output_containing(pointer_reply->root_x, pointer_reply->root_y);
        if (!output) {
            ELOG("ERROR: No screen at (%d, %d), starting on the first screen\n",
                 pointer_reply->root_x, pointer_reply->root_y);
        }
    }
    if (!output) {
        output = get_first_output();
    }
    con_activate(con_descend_focused(output_get_content(output->con)));
    free(pointer_reply);

    tree_render();

    /* Listen to the IPC socket for clients */
    ev_io *ipc_io = scalloc(1, sizeof(struct ev_io));
    ev_io_init(ipc_io, ipc_new_client, ipc_socket, EV_READ);
    ev_io_start(main_loop, ipc_io);

    /* Chose a file name in /tmp/ based on the PID */
    char *log_stream_socket_path = get_process_filename("log-stream-socket");
    int log_socket = create_socket(log_stream_socket_path, &current_log_stream_socket_path);
    free(log_stream_socket_path);
    ev_io *log_io = NULL;
    if (log_socket == -1) {
        ELOG("Could not create the log socket, i3-dump-log -f will not work\n");
    } else {
        log_io = scalloc(1, sizeof(ev_io));
        ev_io_init(log_io, log_new_client, log_socket, EV_READ);
        ev_io_start(main_loop, log_io);
    }

    /* Also handle the UNIX domain sockets passed via socket
     * activation. The parameter 0 means "do not remove the
     * environment variables", we need to be able to reexec. */
    ev_io *socket_ipc_io = NULL;
    listen_fds = sd_listen_fds(0);
    if (listen_fds < 0) {
        ELOG("socket activation: Error in sd_listen_fds\n");
    } else if (listen_fds == 0) {
        DLOG("socket activation: no sockets passed\n");
    } else {
        int flags;
        for (int fd = SD_LISTEN_FDS_START;
             fd < (SD_LISTEN_FDS_START + listen_fds);
             fd++) {
            DLOG("socket activation: also listening on fd %d\n", fd);

            /* sd_listen_fds() enables FD_CLOEXEC by default.
             * However, we need to keep the file descriptors open for in-place
             * restarting, therefore we explicitly disable FD_CLOEXEC. */
            if ((flags = fcntl(fd, F_GETFD)) < 0 ||
                fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) < 0) {
                ELOG("Could not disable FD_CLOEXEC on fd %d\n", fd);
            }

            socket_ipc_io = scalloc(1, sizeof(ev_io));
            ev_io_init(socket_ipc_io, ipc_new_client, fd, EV_READ);
            ev_io_start(main_loop, socket_ipc_io);
        }
    }

    {
        const int restart_fd = parse_restart_fd();
        if (restart_fd != -1) {
            DLOG("serving restart fd %d", restart_fd);
            ipc_client *client = ipc_new_client_on_fd(main_loop, restart_fd);
            ipc_confirm_restart(client);
            unsetenv("_I3_RESTART_FD");
        }
    }

    /* Set up i3 specific atoms like I3_SOCKET_PATH and I3_CONFIG_PATH */
    x_set_i3_atoms();
    ewmh_update_workarea();

    /* Set the ewmh desktop properties. */
    ewmh_update_desktop_properties();

    ev_io *xcb_watcher = scalloc(1, sizeof(struct ev_io));
    xcb_prepare = scalloc(1, sizeof(ev_prepare));

    ev_io_init(xcb_watcher, xcb_got_event, xcb_get_file_descriptor(conn), EV_READ);
    ev_io_start(main_loop, xcb_watcher);

    ev_prepare_init(xcb_prepare, xcb_prepare_cb);
    ev_prepare_start(main_loop, xcb_prepare);

    xcb_flush(conn);

    /* What follows is a fugly consequence of X11 protocol race conditions like
     * the following: In an i3 in-place restart, i3 will reparent all windows
     * to the root window, then exec() itself. In the new process, it calls
     * manage_existing_windows. However, in case any application sent a
     * generated UnmapNotify message to the WM (as GIMP does), this message
     * will be handled by i3 *after* managing the window, thus i3 thinks the
     * window just closed itself. In reality, the message was sent in the time
     * period where i3 wasn’t running yet.
     *
     * To prevent this, we grab the server (disables processing of any other
     * connections), then discard all pending events (since we didn’t do
     * anything, there cannot be any meaningful responses), then ungrab the
     * server. */
    xcb_grab_server(conn);
    {
        xcb_aux_sync(conn);
        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(conn)) != NULL) {
            if (event->response_type == 0) {
                free(event);
                continue;
            }

            /* Strip off the highest bit (set if the event is generated) */
            const int type = (event->response_type & 0x7F);

            /* We still need to handle MapRequests which are sent in the
             * timespan starting from when we register as a window manager and
             * this piece of code which drops events. */
            if (type == XCB_MAP_REQUEST) {
                handle_event(type, event);
            }

            free(event);
        }
        manage_existing_windows(root);
    }
    xcb_ungrab_server(conn);

    if (autostart) {
        /* When the root's window background is set to NONE, that might mean
         * that old content stays visible when a window is closed. That has
         * unpleasant effect of "my terminal (does not seem to) close!".
         *
         * There does not seem to be an easy way to query for this problem, so
         * we test for it: Open & close a window and check if the background is
         * redrawn or the window contents stay visible.
         */
        LOG("This is not an in-place restart, checking if a wallpaper is set.\n");

        xcb_screen_t *root = xcb_aux_get_screen(conn, conn_screen);
        if (is_background_set(conn, root)) {
            LOG("A wallpaper is set, so no screenshot is necessary.\n");
        } else {
            LOG("No wallpaper set, copying root window contents to a pixmap\n");
            set_screenshot_as_wallpaper(conn, root);
        }
    }

    if (!disable_signalhandler) {
        setup_signal_handler();
    } else {
        struct sigaction action;

        action.sa_sigaction = handle_core_signal;
        action.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
        sigemptyset(&action.sa_mask);

        /* Catch all signals with default action "Core", see signal(7) */
        if (sigaction(SIGQUIT, &action, NULL) == -1 ||
            sigaction(SIGILL, &action, NULL) == -1 ||
            sigaction(SIGABRT, &action, NULL) == -1 ||
            sigaction(SIGFPE, &action, NULL) == -1 ||
            sigaction(SIGSEGV, &action, NULL) == -1) {
            ELOG("Could not setup signal handler.\n");
        }
    }

    setup_term_handlers();
    /* Ignore SIGPIPE to survive errors when an IPC client disconnects
     * while we are sending them a message */
    signal(SIGPIPE, SIG_IGN);

    /* Autostarting exec-lines */
    if (autostart) {
        while (!TAILQ_EMPTY(&autostarts)) {
            struct Autostart *exec = TAILQ_FIRST(&autostarts);

            LOG("auto-starting %s\n", exec->command);
            start_application(exec->command, exec->no_startup_id, XCB_NONE);

            FREE(exec->command);
            TAILQ_REMOVE(&autostarts, exec, autostarts);
            FREE(exec);
        }
    }

    /* Autostarting exec_always-lines */
    while (!TAILQ_EMPTY(&autostarts_always)) {
        struct Autostart *exec_always = TAILQ_FIRST(&autostarts_always);

        LOG("auto-starting (always!) %s\n", exec_always->command);
        start_application(exec_always->command, exec_always->no_startup_id, XCB_NONE);

        FREE(exec_always->command);
        TAILQ_REMOVE(&autostarts_always, exec_always, autostarts_always);
        FREE(exec_always);
    }

    /* Start i3bar processes for all configured bars */
    Barconfig *barconfig;
    TAILQ_FOREACH (barconfig, &barconfigs, configs) {
        char *command = NULL;
        sasprintf(&command, "%s %s --bar_id=%s --socket=\"%s\"",
                  barconfig->i3bar_command ? barconfig->i3bar_command : "exec i3bar",
                  barconfig->verbose ? "-V" : "",
                  barconfig->id, current_socketpath);
        LOG("Starting bar process: %s\n", command);
        start_application(command, true, XCB_NONE);
        free(command);
    }

    /* Make sure to destroy the event loop to invoke the cleanup callbacks
     * when calling exit() */
    atexit(i3_exit);

    /* There might be children who died before we initialized the event loop,
     * e.g., when restarting i3 (see #5756).
     * To not carry zombie children around, raise the signal to invite libev to
     * reap them.
     *
     * Note that there is no race condition between raising the signal below and
     * entering the event loop later: the signal is just to notify libev that
     * zombies might already be there. Actual reaping will take place in the
     * event loop anyway. */
    (void)raise(SIGCHLD);

    sd_notify(1, "READY=1");
    ev_loop(main_loop, 0);

    /* Free these heap allocations just to satisfy LeakSanitizer. */
    FREE(ipc_io);
    FREE(socket_ipc_io);
    FREE(log_io);
    FREE(xcb_watcher);
}
