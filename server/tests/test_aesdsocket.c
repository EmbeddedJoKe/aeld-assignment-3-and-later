/* Focused unit checks with fault-injected network calls. These complement,
 * rather than replace, the instructor's real TCP and Valgrind tests. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *incoming;
static size_t incoming_size, incoming_offset, fragment_size;
static int output_fd, file_fd;
static int fail_allocation, interrupt_send, block_send, interrupt_write;
static int disconnect_send, fake_poll = 1;

static void *injected_realloc(void *pointer, size_t length)
{
    if (fail_allocation) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(pointer, length);
}

static ssize_t injected_write(int fd, const void *buffer, size_t length)
{
    if (fd == file_fd) {
        if (interrupt_write) {
            interrupt_write = 0;
            errno = EINTR;
            return -1;
        }
        if (length > 37)
            length = 37;
    }
    return write(fd, buffer, length);
}

static ssize_t injected_send(int fd, const void *buffer, size_t length, int flags)
{
    (void)fd;
    assert(flags == MSG_NOSIGNAL);
    assert(length <= 4096);  /* History must be streamed, not read all at once. */
    if (interrupt_send || block_send || disconnect_send) {
        errno = interrupt_send ? EINTR : block_send ? EAGAIN : EPIPE;
        interrupt_send = 0;
        block_send = 0;
        return -1;
    }
    return write(output_fd, buffer, length > 17 ? 17 : length);
}

static ssize_t injected_recv(int fd, void *buffer, size_t length, int flags)
{
    (void)fd;
    (void)flags;
    size_t remaining = incoming_size - incoming_offset;
    if (length > fragment_size)
        length = fragment_size;
    if (length > remaining)
        length = remaining;
    memcpy(buffer, incoming + incoming_offset, length);
    incoming_offset += length;
    return (ssize_t)length;
}

static int injected_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (!fake_poll)
        return poll(fds, count, timeout);
    assert(count == 2);
    fds[0].revents = fds[0].events;
    fds[1].revents = 0;
    return 1;
}

#define main aesdsocket_entrypoint
#define realloc injected_realloc
#define write injected_write
#define send injected_send
#define recv injected_recv
#define poll injected_poll
#include "../aesdsocket.c"
#undef main
#undef realloc
#undef write
#undef send
#undef recv
#undef poll

static int temporary_file(void)
{
    char path[] = "/tmp/aesdsocket-unit-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(unlink(path) == 0);
    assert(fcntl(fd, F_SETFL, O_APPEND) == 0);
    return fd;
}

static void expect_contents(int fd, const char *expected, size_t length)
{
    char buffer[4096];
    size_t offset = 0;
    assert(lseek(fd, 0, SEEK_SET) == 0);
    while (offset < length) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        assert(count > 0 && (size_t)count <= length - offset);
        assert(memcmp(buffer, expected + offset, (size_t)count) == 0);
        offset += (size_t)count;
    }
    assert(read(fd, buffer, sizeof(buffer)) == 0);
}

static int run_client(const char *data, size_t length, size_t fragment)
{
    incoming = data;
    incoming_size = length;
    incoming_offset = 0;
    fragment_size = fragment;
    return serve_client(123, file_fd);
}

static void test_packets(void)
{
    file_fd = temporary_file();
    output_fd = temporary_file();
    interrupt_send = block_send = interrupt_write = 1;
    assert(run_client("first\nsecond\nincomplete", 23, 2) == 0);
    expect_contents(file_fd, "first\nsecond\n", 13);
    expect_contents(output_fd, "first\nfirst\nsecond\n", 19);
    assert(ftruncate(output_fd, 0) == 0);
    assert(run_client("third\nfourth\n", 13, 4096) == 0);
    expect_contents(file_fd, "first\nsecond\nthird\nfourth\n", 26);
    expect_contents(output_fd,
                    "first\nsecond\nthird\nfirst\nsecond\nthird\nfourth\n", 45);
    disconnect_send = 1;
    assert(run_client("peer-reset\n", 11, 4096) == 0);
    disconnect_send = 0;
    close(file_fd);
    close(output_fd);
}

static void test_large_packet(void)
{
    size_t length = 1024 * 1024 + 123;
    char *packet = malloc(length);
    assert(packet);
    memset(packet, 'x', length);
    packet[length - 1] = '\n';
    file_fd = temporary_file();
    output_fd = temporary_file();
    assert(run_client(packet, length, 4093) == 0);
    expect_contents(file_fd, packet, length);
    expect_contents(output_fd, packet, length);
    free(packet);
    close(file_fd);
    close(output_fd);
}

static void test_allocation_failure(void)
{
    char *packet = NULL;
    size_t used = 0, capacity = 0;
    assert(append_packet(&packet, &used, &capacity, "partial", 7) == 0);
    char *original = packet;
    fail_allocation = 1;
    assert(append_packet(&packet, &used, &capacity, "x", capacity) == -1);
    assert(packet == original && used == 7);
    assert(memcmp(packet, "partial", 7) == 0);
    fail_allocation = 0;
    used = SIZE_MAX;
    assert(append_packet(&packet, &used, &capacity, "x", 1) == -1);
    free(packet);
    file_fd = temporary_file();
    output_fd = temporary_file();
    fail_allocation = 1;
    assert(run_client("discard\n", 8, 4096) == -1);
    expect_contents(file_fd, "", 0);
    fail_allocation = 0;
    close(file_fd);
    close(output_fd);
}

static void test_signal_wakeup(int signo)
{
    int blocked_pipe[2];
    unsigned char drain[32];
    stopping = 0;
    while (read(signal_pipe[0], drain, sizeof(drain)) > 0) { }
    assert(pipe(blocked_pipe) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&delay, NULL);
        _exit(kill(getppid(), signo) == 0 ? 0 : 1);
    }
    fake_poll = 0;
    assert(wait_ready(blocked_pipe[0], POLLIN) == 0);
    assert(stopping);
    int status = 0;
    while (waitpid(child, &status, 0) < 0)
        assert(errno == EINTR);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    fake_poll = 1;
    close(blocked_pipe[0]);
    close(blocked_pipe[1]);
}

int main(void)
{
    assert(setup_signals() == 0);
    test_packets();
    test_large_packet();
    test_allocation_failure();
    test_signal_wakeup(SIGINT);
    test_signal_wakeup(SIGTERM);
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    puts("PASS: packet boundaries, fragmentation, partial I/O, streamed history,");
    puts("allocation failures, peer disconnects, and SIGINT/SIGTERM wakeup.");
    return 0;
}
