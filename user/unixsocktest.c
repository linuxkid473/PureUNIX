/*
 * user/unixsocktest.c — real on-target regression test for the new
 * kernel AF_UNIX domain socket subsystem (kernel/unix_socket.c, added
 * for the PCManFM-Qt port's MenuCache dependency — see
 * docs/pcmanfm-port.md phase 6 and include/pureunix/syscall.h's own
 * SYS_SOCKET/.../SYS_CONNECT comment). Exercises the real thing
 * end-to-end across a real fork(): a server process bind()s+listen()s,
 * a client process (a real, separate, unrelated task — not just two fds
 * in the same process, unlike a bare pipe()) connect()s to it by path,
 * and real bytes flow both directions through the actual kernel ring
 * buffers, same class of on-target check every other vendored/kernel
 * dependency in this repo gets (see user/libctest.c/user/glibtest.c).
 *
 * Same harness convention as systest.c/libctest.c: every check is
 * independent and numbered, a failure never stops the run, a summary
 * prints at the end. Server/client synchronize via a second, ordinary
 * pipe (not the socket under test) so the parent's checks only run
 * after real data has genuinely round-tripped.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_num = 0;
static int g_pass = 0;
static int g_fail = 0;

static void t_begin(const char *desc)
{
    g_num++;
    printf("[%03d] %s\n", g_num, desc);
}

static void t_check(int cond, const char *what)
{
    if (cond) {
        g_pass++;
        printf("      PASS: %s\n", what);
    } else {
        g_fail++;
        printf("      FAIL: %s\n", what);
    }
}

#define SOCK_PATH "/tmp/unixsocktest.sock"

int main(void)
{
    t_begin("socket(AF_UNIX, SOCK_STREAM, 0): real socket creation");
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    t_check(listen_fd >= 0, "socket() returns a real fd");

    t_begin("bind(): real path registration");
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    int bind_rc = bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    t_check(bind_rc == 0, "bind() succeeds on a fresh path");

    t_begin("listen(): real backlog activation");
    int listen_rc = listen(listen_fd, 4);
    t_check(listen_rc == 0, "listen() succeeds on a bound socket");

    int sync_pipe[2];
    pipe(sync_pipe);

    pid_t child = fork();
    t_check(child >= 0, "fork() succeeds (separate client process, not just another fd)");

    if (child == 0) {
        /* Client process: connect(), write a real message, read a real
         * reply, report success/failure over the sync pipe (never
         * printf's its own PASS/FAIL — only the parent's harness does,
         * to keep one linear, single-writer numbered report). */
        close(sync_pipe[0]);
        int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un caddr;
        memset(&caddr, 0, sizeof(caddr));
        caddr.sun_family = AF_UNIX;
        strncpy(caddr.sun_path, SOCK_PATH, sizeof(caddr.sun_path) - 1);
        int crc = connect(cfd, (struct sockaddr *)&caddr, sizeof(caddr));
        char result = (crc == 0) ? '1' : '0';

        if (crc == 0) {
            const char *msg = "hello from client";
            write(cfd, msg, strlen(msg) + 1);
            char reply[64];
            memset(reply, 0, sizeof(reply));
            int n = read(cfd, reply, sizeof(reply) - 1);
            result = (n > 0 && strcmp(reply, "hello from server") == 0) ? '1' : '0';
        }
        write(sync_pipe[1], &result, 1);
        close(cfd);
        _exit(0);
    }

    close(sync_pipe[1]);

    t_begin("accept(): real blocking accept of the client's connect()");
    int server_fd = accept(listen_fd, NULL, NULL);
    t_check(server_fd >= 0, "accept() returns a fresh connected fd");

    t_begin("read()/write() over a connected AF_UNIX socket: real bytes both directions");
    char buf[64];
    memset(buf, 0, sizeof(buf));
    int n = read(server_fd, buf, sizeof(buf) - 1);
    t_check(n > 0 && strcmp(buf, "hello from client") == 0,
            "server reads the client's real message");

    const char *reply = "hello from server";
    int wn = write(server_fd, reply, strlen(reply) + 1);
    t_check(wn == (int)strlen(reply) + 1, "server writes a real reply");

    char sync_byte = '0';
    read(sync_pipe[0], &sync_byte, 1);
    t_check(sync_byte == '1', "client process saw a real, correct round trip");

    int status = 0;
    waitpid(child, &status, 0);

    t_begin("connect() to a nonexistent path: honest ECONNREFUSED");
    int bad_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un bad_addr;
    memset(&bad_addr, 0, sizeof(bad_addr));
    bad_addr.sun_family = AF_UNIX;
    strncpy(bad_addr.sun_path, "/tmp/no-such-socket-real", sizeof(bad_addr.sun_path) - 1);
    int bad_rc = connect(bad_fd, (struct sockaddr *)&bad_addr, sizeof(bad_addr));
    t_check(bad_rc == -1 && errno == ECONNREFUSED,
            "connect() to an unbound path fails with a real ECONNREFUSED");
    close(bad_fd);

    close(server_fd);
    close(listen_fd);

    printf("\nunixsocktest: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
