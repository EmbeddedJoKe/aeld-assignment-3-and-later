ECE5305 Assignment 5 Part 1

Build:
    make -C server
    make -C server default
    make -C server CROSS_COMPILE=aarch64-none-linux-gnu-

An explicit CC, CPPFLAGS, CFLAGS, LDFLAGS, and LDLIBS are supported.
Run make clean before changing between native and cross builds.

Run:
    ./server/aesdsocket
    ./server/aesdsocket -d

The server listens on TCP port 9000. Each newline completes a packet and
appends it to /var/tmp/aesdsocketdata. Every complete packet produces a
response containing all history, streamed in 4096-byte chunks. Incomplete
packets are discarded when a client disconnects. Clients are served
sequentially. The daemon forks after successful bind/listen, and the parent
waits for daemon setup before reporting startup success.

SIGINT and SIGTERM wake a blocked poll. Complete packets already being
processed are written and their responses sent while the peer can accept
data; a blocked or incomplete client cannot prevent shutdown. The process
closes its descriptors, frees its packet buffer, and deletes its data file.
Startup errors return -1 (shell status 255). A second instance that cannot
bind does not delete the running instance's data file.

Validation:
    make -C server check
    ./full-test.sh

The focused C tests fault-inject socket calls and exercise packet boundaries,
fragmentation, short transfers, EINTR/EAGAIN, allocation failure, peer
disconnects, bounded history reads, and actual SIGINT/SIGTERM wakeup.
They supplement the required TCP and Valgrind tests; they cannot replace them.
The instructor tests and their submodule are unchanged. Native full-test.sh
adds the course helper directory to PATH, matching the CI container.

For a manual instructor socket test, start the native server in one terminal,
run the following in another, then send SIGTERM to that server's PID:
    bash assignment-autotest/test/assignment5/sockettest.sh
Repeat with a newly started server, since the test expects empty history.

Native full-test.sh requires gcc, make, cmake, ruby, netcat, and valgrind,
plus permission to create TCP sockets and write /var/tmp. The official
Assignment 5 CI container provides these tools. CI runs the existing unit
suite before full-test and queues later runs without cancelling active jobs.

The Part 2 aesdsocket-start-stop service uses BusyBox start-stop-daemon to
start /usr/bin/aesdsocket -d and send SIGTERM on stop. It waits for the process
to exit before shutdown continues; BusyBox does not implement --retry.
Buildroot installs it as /etc/init.d/S99aesdsocket for boot and shutdown.
