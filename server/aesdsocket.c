#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#define SERVER_PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define IO_SIZE 4096

static volatile sig_atomic_t stopping;
static int signal_pipe[2] = {-1, -1};

/* Only async-signal-safe operations belong in this handler. The pipe also
 * wakes poll if a signal arrives between checking stopping and blocking. */
static void handle_signal(int signo)
{
    int saved_errno = errno;
    unsigned char wakeup = (unsigned char)signo;
    stopping = 1;
    if (signal_pipe[1] >= 0) {
        ssize_t ignored = write(signal_pipe[1], &wakeup, sizeof(wakeup));
        (void)ignored;
    }
    errno = saved_errno;
}

static void log_error(const char *operation)
{
    syslog(LOG_ERR, "%s: %s", operation, strerror(errno));
}

static int configure_fd(int fd, int nonblocking)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return -1;
    if (nonblocking) {
        flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            return -1;
    }
    return 0;
}

static int setup_signals(void)
{
    struct sigaction action;
    if (pipe(signal_pipe) < 0 || configure_fd(signal_pipe[0], 1) < 0 ||
        configure_fd(signal_pipe[1], 1) < 0)
        return -1;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0)
        return -1;
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}

/* Returns 1 when ready, 0 on shutdown, and -1 on an error. */
static int wait_ready(int fd, short events)
{
    struct pollfd fds[2] = {
        {.fd = fd, .events = events},
        {.fd = signal_pipe[0], .events = POLLIN}
    };
    while (!stopping) {
        int rc = poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            log_error("poll");
            return -1;
        }
        if (stopping || (fds[1].revents & POLLIN))
            return 0;
        if (fds[0].revents & POLLNVAL) {
            errno = EBADF;
            log_error("poll descriptor");
            return -1;
        }
        if (fds[0].revents & (events | POLLERR | POLLHUP))
            return 1;
    }
    return 0;
}

static int write_all(int fd, const char *buffer, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = write(fd, buffer + offset, length - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (count == 0)
                errno = EIO;
            log_error("write data file");
            return -1;
        }
        offset += (size_t)count;
    }
    return 0;
}

static int send_all(int client, const char *buffer, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = send(client, buffer + offset, length - offset,
                             MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (wait_ready(client, POLLOUT) == 1)
                    continue;
                return -1;
            }
            log_error("send");
            return -1;
        }
        if (count == 0) {
            errno = EPIPE;
            log_error("send");
            return -1;
        }
        offset += (size_t)count;
    }
    return 0;
}

/* Stream the history in bounded chunks; its total size need not fit in RAM. */
static int send_history(int client, int data_fd)
{
    char buffer[IO_SIZE];
    if (lseek(data_fd, 0, SEEK_SET) < 0) {
        log_error("seek data file");
        return -1;
    }
    for (;;) {
        ssize_t count = read(data_fd, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            log_error("read data file");
            return -1;
        }
        if (count == 0)
            return 0;
        if (send_all(client, buffer, (size_t)count) < 0)
            return -2; /* A disconnected peer does not invalidate the file. */
    }
}

static int append_packet(char **packet, size_t *used, size_t *capacity,
                         const char *input, size_t length)
{
    if (length > SIZE_MAX - *used) {
        errno = ENOMEM;
        log_error("packet size overflow");
        return -1;
    }
    size_t needed = *used + length;
    if (needed > *capacity) {
        size_t next = *capacity ? *capacity : IO_SIZE;
        while (next < needed) {
            if (next > SIZE_MAX / 2) {
                next = needed;
                break;
            }
            next *= 2;
        }
        char *resized = realloc(*packet, next);
        if (!resized) {
            log_error("allocate packet");
            return -1;
        }
        *packet = resized;
        *capacity = next;
    }
    memcpy(*packet + *used, input, length);
    *used = needed;
    return 0;
}

static int serve_client(int client, int data_fd)
{
    char input[IO_SIZE];
    char *packet = NULL;
    size_t used = 0, capacity = 0;
    int result = 0;
    while (!stopping) {
        int ready = wait_ready(client, POLLIN);
        if (ready != 1) {
            result = ready < 0 ? -1 : 0;
            break;
        }
        ssize_t count = recv(client, input, sizeof(input), 0);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            log_error("recv");
            break;
        }
        if (count == 0)
            break;
        size_t offset = 0;
        while (offset < (size_t)count) {
            char *newline = memchr(input + offset, '\n', (size_t)count - offset);
            size_t length = newline ? (size_t)(newline - (input + offset)) + 1
                                    : (size_t)count - offset;
            if (append_packet(&packet, &used, &capacity, input + offset,
                              length) < 0) {
                result = -1;
                goto done;
            }
            offset += length;
            if (newline) {
                /* Finish the complete packet before acting on a shutdown.
                 * A blocked peer cannot hold shutdown indefinitely. */
                if (write_all(data_fd, packet, used) < 0) {
                    result = -1;
                    goto done;
                }
                used = 0;
                int rc = send_history(client, data_fd);
                if (rc < 0) {
                    result = rc == -1 ? -1 : 0;
                    goto done;
                }
            }
        }
    }
done:
    /* Incomplete packets are discarded, never mixed with another client. */
    free(packet);
    return result;
}

/* The parent waits for daemon setup, so -d reports startup failures too.
 * Return 1 in the parent, 0 in the child, or -1 on failure. */
static int daemonize(void)
{
    int startup[2];
    if (pipe(startup) < 0) {
        log_error("daemon startup pipe");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        log_error("fork");
        close(startup[0]);
        close(startup[1]);
        return -1;
    }
    if (pid > 0) {
        unsigned char status = 1;
        ssize_t count;
        close(startup[1]);
        do {
            count = read(startup[0], &status, sizeof(status));
        } while (count < 0 && errno == EINTR);
        close(startup[0]);
        if (count == sizeof(status) && status == 0)
            return 1;
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) { }
        return -1;
    }
    close(startup[0]);
    unsigned char status = 1;
    if (setsid() < 0 || chdir("/") < 0) {
        log_error("daemon session setup");
    } else {
        umask(022);
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd < 0) {
            log_error("open /dev/null");
        } else {
            if (dup2(null_fd, STDIN_FILENO) < 0 ||
                dup2(null_fd, STDOUT_FILENO) < 0 ||
                dup2(null_fd, STDERR_FILENO) < 0) {
                log_error("redirect daemon descriptors");
            } else {
                status = 0;
                closelog();
                openlog("aesdsocket", LOG_PID, LOG_USER);
            }
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
    }
    ssize_t count;
    do {
        count = write(startup[1], &status, sizeof(status));
    } while (count < 0 && errno == EINTR);
    close(startup[1]);
    return status == 0 && count == sizeof(status) ? 0 : -1;
}

int main(int argc, char *argv[])
{
    int listener = -1, client = -1, data_fd = -1;
    int result = -1;
    int owns_data = 0;
    int daemon_mode = argc == 2 && strcmp(argv[1], "-d") == 0;
    char address[INET_ADDRSTRLEN] = "unknown";
    if (argc > 2 || (argc == 2 && !daemon_mode)) {
        fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
        return -1;
    }
    openlog("aesdsocket", LOG_PID | LOG_PERROR, LOG_USER);
    if (setup_signals() < 0) {
        log_error("setup signals");
        goto cleanup;
    }
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        log_error("socket");
        goto cleanup;
    }
    int reuse = 1;
    struct sockaddr_in server_address = {
        .sin_family = AF_INET,
        .sin_port = htons(SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (configure_fd(listener, 1) < 0 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        bind(listener, (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0 || listen(listener, SOMAXCONN) < 0) {
        log_error("setup listening socket");
        goto cleanup;
    }
    data_fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (data_fd < 0) {
        log_error("open data file");
        goto cleanup;
    }
    owns_data = 1;
    if (daemon_mode) {
        int rc = daemonize();
        if (rc < 0)
            goto cleanup;
        if (rc > 0) {
            owns_data = 0;
            result = 0;
            goto cleanup;
        }
    }
    result = 0;
    while (!stopping) {
        int ready = wait_ready(listener, POLLIN);
        if (ready != 1) {
            if (ready < 0)
                result = -1;
            break;
        }
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        client = accept(listener, (struct sockaddr *)&peer, &peer_length);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            log_error("accept");
            result = -1;
            break;
        }
        if (configure_fd(client, 1) < 0) {
            log_error("configure client socket");
            result = -1;
            break;
        }
        if (!inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address)))
            strcpy(address, "unknown");
        syslog(LOG_DEBUG, "Accepted connection from %s", address);
        int rc = serve_client(client, data_fd);
        close(client);
        client = -1;
        syslog(LOG_DEBUG, "Closed connection from %s", address);
        if (rc < 0) {
            result = -1;
            break;
        }
    }
cleanup:
    if (stopping)
        syslog(LOG_DEBUG, "Caught signal, exiting");
    if (client >= 0)
        close(client);
    if (listener >= 0)
        close(listener);
    if (data_fd >= 0 && close(data_fd) < 0) {
        log_error("close data file");
        result = -1;
    }
    if (owns_data && unlink(DATA_FILE) < 0 && errno != ENOENT) {
        log_error("remove data file");
        result = -1;
    }
    if (signal_pipe[0] >= 0)
        close(signal_pipe[0]);
    if (signal_pipe[1] >= 0)
        close(signal_pipe[1]);
    closelog();
    return result;
}
