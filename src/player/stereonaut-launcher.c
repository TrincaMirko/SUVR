/* Starts the persistent OpenXR host (browser and video renderer in one
 * process) and one playback session per selected title. Keeping a single XR
 * session alive avoids the SteamVR dashboard popping up between screens. */
#define _POSIX_C_SOURCE 200809L
#include "player-ui-ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Exit codes of stereonaut-session. */
#define EXIT_UNSUPPORTED_FORMAT 3
#define EXIT_SOURCE_FAILED 4
#define EXIT_BROKEN_SOURCE 5

static int ui_fd = -1;
static pid_t host_pid = -1;

static bool exit_after_playback; /* the host ended during playback */

static double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int child_status(int status, const char *name, int *exit_code)
{
    *exit_code = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s exited abnormally\n", name);
        if (WIFEXITED(status))
            *exit_code = WEXITSTATUS(status);
        return -1;
    }
    *exit_code = 0;
    return 0;
}

/* Duplicate a CLOEXEC descriptor to one a child inherits across exec. */
static int inheritable(int fd)
{
    return fcntl(fd, F_DUPFD, 3);
}

static int start_host(void)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) < 0) {
        perror("socketpair host");
        return -1;
    }
    int child_fd = inheritable(sockets[1]);
    if (child_fd < 0) {
        perror("duplicate host fd");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    char fd_text[32];
    snprintf(fd_text, sizeof(fd_text), "%d", child_fd);
    host_pid = fork();
    if (host_pid < 0) {
        perror("fork host");
        close(child_fd);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    if (host_pid == 0) {
        close(sockets[0]);
        close(sockets[1]);
        execl("./stereonaut-renderer", "stereonaut-renderer", fd_text,
              (char *)NULL);
        perror("exec stereonaut-renderer");
        _exit(127);
    }
    close(child_fd);
    close(sockets[1]);
    ui_fd = sockets[0];
    return 0;
}

static int send_command(uint32_t type, const char *path, const char *message,
                        int fd)
{
    struct player_ui_command command = {.type = type};
    if (path)
        snprintf(command.path, sizeof(command.path), "%s", path);
    if (message)
        snprintf(command.message, sizeof(command.message), "%s", message);
    struct iovec iov = {.iov_base = &command, .iov_len = sizeof(command)};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr header = {.msg_iov = &iov, .msg_iovlen = 1};
    if (fd >= 0) {
        header.msg_control = control;
        header.msg_controllen = sizeof(control);
        struct cmsghdr *item = CMSG_FIRSTHDR(&header);
        item->cmsg_level = SOL_SOCKET;
        item->cmsg_type = SCM_RIGHTS;
        item->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(item), &fd, sizeof(fd));
    }
    ssize_t written;
    do {
        written = sendmsg(ui_fd, &header, MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written != (ssize_t)sizeof(command)) {
        fprintf(stderr, "XR host is not reachable\n");
        return -1;
    }
    return 0;
}

/* Runs one session attached to the host. `extra` holds additional session
 * arguments (used by --play for bounded test runs). */
static int run_playback(const char *path, char *const *extra, int *exit_code)
{
    *exit_code = -1;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) < 0) {
        perror("socketpair playback");
        return -1;
    }
    int session_fd = inheritable(sockets[0]);
    if (session_fd < 0) {
        perror("duplicate session fd");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    char renderer_text[48];
    snprintf(renderer_text, sizeof(renderer_text), "%d,0", session_fd);
    pid_t child = fork();
    if (child < 0) {
        perror("fork playback");
        close(session_fd);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    if (child == 0) {
        close(sockets[0]);
        close(sockets[1]);
        char *argv[32];
        size_t count = 0;
        argv[count++] = "stereonaut-session";
        argv[count++] = (char *)path;
        argv[count++] = "--renderer";
        argv[count++] = renderer_text;
        for (; extra && *extra && count + 1 < sizeof(argv) / sizeof(argv[0]);
             ++extra)
            argv[count++] = *extra;
        argv[count] = NULL;
        execv("./stereonaut-session", argv);
        perror("exec stereonaut-session");
        _exit(127);
    }
    close(session_fd);
    close(sockets[0]);
    int sent = send_command(PLAYER_UI_PLAY, path, NULL, sockets[1]);
    close(sockets[1]);
    if (sent < 0)
        kill(child, SIGTERM);
    /* While the session runs, the host may ask to stop it (back pressed
     * during loading) or report that the XR session ended. */
    double stop_sent = 0.0;
    int watch_fd = ui_fd; /* -1 once the host has closed its end */
    for (;;) {
        int status = 0;
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child)
            return child_status(status, "stereonaut-session", exit_code);
        if (done < 0 && errno != EINTR) {
            perror("waitpid");
            return -1;
        }
        struct pollfd pollfd = {.fd = watch_fd, .events = POLLIN};
        if (poll(&pollfd, 1, 100) > 0) {
            struct player_ui_result result = {0};
            ssize_t received = recv(watch_fd, &result, sizeof(result),
                                    MSG_DONTWAIT);
            if (received == (ssize_t)sizeof(result) &&
                result.type == PLAYER_UI_EXIT_REQUESTED)
                exit_after_playback = true;
            if (received == 0 ||
                (received == (ssize_t)sizeof(result) &&
                 (result.type == PLAYER_UI_STOP_REQUESTED ||
                  result.type == PLAYER_UI_EXIT_REQUESTED))) {
                if (!stop_sent) {
                    printf("playback_stop_requested\n");
                    kill(child, SIGTERM);
                    stop_sent = monotonic_seconds();
                }
            }
            if (received == 0) {
                exit_after_playback = true;
                watch_fd = -1;
            }
        }
        if (stop_sent && monotonic_seconds() - stop_sent > 5.0) {
            fprintf(stderr, "stereonaut-session ignored SIGTERM\n");
            kill(child, SIGKILL);
            stop_sent = monotonic_seconds() + 3600.0;
        }
    }
}

static void selected_parent(const char *path, char *directory, size_t size)
{
    if (strncmp(path, "jf:item:", 8) == 0) {
        /* jf:item:<id>:<folder> returns to the folder it was chosen in;
         * titles from the start page ("continue watching") have none. */
        const char *folder = strchr(path + 8, ':');
        if (folder && folder[1] == '?') /* search results */
            snprintf(directory, size, "jf:search:%s", folder + 2);
        else if (folder && folder[1])
            snprintf(directory, size, "jf:view:%s", folder + 1);
        else
            snprintf(directory, size, "root:");
        return;
    }
    if (strncmp(path, "webdav:play:", 12) == 0) {
        /* webdav:play:<idx>:<file> returns to the folder it lives in. */
        int index = atoi(path + 12);
        const char *file = strchr(path + 12, ':');
        file = file ? file + 1 : "/";
        char dir[640];
        snprintf(dir, sizeof(dir), "%s", file);
        char *slash = strrchr(dir, '/');
        if (slash)
            slash[1] = '\0'; /* keep the trailing slash */
        else
            snprintf(dir, sizeof(dir), "/");
        snprintf(directory, size, "webdav:srv:%d:%s", index, dir);
        return;
    }
    snprintf(directory, size, "%s", path);
    char *slash = strrchr(directory, '/');
    if (!slash || slash == directory)
        snprintf(directory, size, "/");
    else
        *slash = '\0';
}

static void shutdown_host(void)
{
    if (ui_fd >= 0) {
        close(ui_fd);
        ui_fd = -1;
    }
    if (host_pid > 0) {
        int status;
        waitpid(host_pid, &status, 0);
        host_pid = -1;
    }
}

/* Redirect stdout/stderr into <exe_dir>/log/stereonaut.1.log, keeping the 3
 * most recent runs (oldest dropped, rest shifted down). Children inherit the
 * descriptors, so this single redirect also captures the host and sessions. */
static void setup_logging(void)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0)
        return;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash)
        return;
    *slash = '\0';

    char dir[PATH_MAX + 8];
    snprintf(dir, sizeof(dir), "%s/log", exe);
    mkdir(dir, 0755);

    char a[PATH_MAX + 64], b[PATH_MAX + 64];
    snprintf(a, sizeof(a), "%s/stereonaut.3.log", dir);
    unlink(a);
    for (int i = 2; i >= 1; i--) {
        snprintf(a, sizeof(a), "%s/stereonaut.%d.log", dir, i);
        snprintf(b, sizeof(b), "%s/stereonaut.%d.log", dir, i + 1);
        rename(a, b);
    }
    snprintf(a, sizeof(a), "%s/stereonaut.1.log", dir);
    int fd = open(a, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);
}

int main(int argc, char **argv)
{
    setup_logging();
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    bool play_only = argc >= 3 && strcmp(argv[1], "--play") == 0;
    if ((!play_only && argc > 2) || (argc >= 2 && !play_only &&
                                      strncmp(argv[1], "--", 2) == 0)) {
        fprintf(stderr,
                "usage: %s [INITIAL_LOCATION]\n"
                "       %s --play PATH [SESSION_ARGS...]\n",
                argv[0], argv[0]);
        return 2;
    }
    if (start_host() < 0)
        return 1;
    if (play_only) {
        /* Bounded test run: attach one session, then leave. */
        if (send_command(PLAYER_UI_BROWSE, "root:", NULL, -1) < 0) {
            shutdown_host();
            return 1;
        }
        int exit_code = 0;
        int result = run_playback(argv[2], &argv[3], &exit_code);
        shutdown_host();
        return result < 0 ? (exit_code > 0 ? exit_code : 1) : 0;
    }
    const char *initial = argc == 2 ? argv[1] : "root:";
    if (!initial[0])
        initial = "root:";
    char directory[PLAYER_UI_PATH_MAX];
    char browser_message[PLAYER_UI_MESSAGE_MAX] = {0};
    snprintf(directory, sizeof(directory), "%s", initial);
    if (send_command(PLAYER_UI_BROWSE, directory, NULL, -1) < 0) {
        shutdown_host();
        return 1;
    }
    for (;;) {
        struct player_ui_result result = {0};
        ssize_t received;
        do {
            received = recv(ui_fd, &result, sizeof(result), 0);
        } while (received < 0 && errno == EINTR);
        if (received != (ssize_t)sizeof(result)) {
            fprintf(stderr, "XR host ended\n");
            shutdown_host();
            return 0;
        }
        if (result.type == PLAYER_UI_EXIT_REQUESTED) {
            shutdown_host();
            return 0;
        }
        if (result.type != PLAYER_UI_FILE_SELECTED || !result.path[0]) {
            fprintf(stderr, "XR host returned unknown action\n");
            shutdown_host();
            return 1;
        }
        selected_parent(result.path, directory, sizeof(directory));
        printf("selected_file=%s\n", result.path);
        int exit_code = 0;
        browser_message[0] = '\0';
        char start_text[32];
        char *start_arguments[] = {"--start-us", start_text, NULL};
        snprintf(start_text, sizeof(start_text), "%lld",
                 (long long)result.start_us);
        if (result.start_us > 0)
            printf("selected_start_us=%s\n", start_text);
        int played = run_playback(result.path,
                                  result.start_us > 0 ? start_arguments
                                                      : NULL,
                                  &exit_code);
        if (exit_after_playback) {
            shutdown_host();
            return 0;
        }
        if (played < 0) {
            fprintf(stderr, "playback failed; returning to browser\n");
            if (exit_code == EXIT_BROKEN_SOURCE)
                snprintf(browser_message, sizeof(browser_message),
                         "Video file on the server is corrupt");
            else if (exit_code == EXIT_SOURCE_FAILED)
                snprintf(browser_message, sizeof(browser_message),
                         "Jellyfin or network not reachable");
            else
                snprintf(browser_message, sizeof(browser_message),
                         "Video or audio format is not currently supported");
        }
        if (send_command(PLAYER_UI_BROWSE, directory, browser_message,
                         -1) < 0) {
            shutdown_host();
            return 1;
        }
    }
}
