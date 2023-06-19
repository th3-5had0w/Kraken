#include <stdio.h>
#include <netinet/in.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <liburing.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <pthread.h>

#define DEFAULT_SERVER_PORT     8000
#define QUEUE_DEPTH             256
#define READ_SZ                 4096
#define WRITE_SZ                4096

#define MAX_CONN    1024

pthread_mutex_t mutex;

/*
 One function that prints the system call and the error details
 and then exits with error code 1. Non-zero meaning things didn't go well.
 */
void fatal_error(const char *syscall) {
    perror(syscall);
    exit(1);
}

/*
 * Helper function for cleaner looking code.
 * */

void *zh_malloc(size_t size) {
    void *buf = malloc(size);
    if (!buf) {
        fprintf(stderr, "Fatal error: unable to allocate memory.\n");
        exit(1);
    }
    return buf;
}

/*
 * This function is responsible for setting up the main listening socket used by the
 * web server.
 * */

int setup_listening_socket(int port) {
    int sock;
    struct sockaddr_in srv_addr;

    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock == -1)
        fatal_error("socket()");

    int enable = 1;
    if (setsockopt(sock,
                   SOL_SOCKET, SO_REUSEADDR,
                   &enable, sizeof(int)) < 0)
        fatal_error("setsockopt(SO_REUSEADDR)");


    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(port);
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* We bind to a port and turn this socket into a listening
     * socket.
     * */
    if (bind(sock,
             (const struct sockaddr *)&srv_addr,
             sizeof(srv_addr)) < 0)
        fatal_error("bind()");

    if (listen(sock, MAX_CONN) < 0)
        fatal_error("listen()");

    return (sock);
}

void init()
{
    if (pthread_mutex_init(&mutex, NULL) != 0) fatal_error("pthread_mutex_init()");
}

void sigint_handler(int signo)
{
    printf("^C pressed. Shutting down.\n");
    exit(0);
}

void server_loop(int server_socket)
{
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int client = accept(server_socket, &client_addr, &client_addr_len);
    if (client < 0) fatal_error("accept()");
    struct iovec readiov, writeiov;
    readiov.iov_base = zh_malloc(READ_SZ);
    readiov.iov_len = READ_SZ;
    writeiov.iov_base = zh_malloc(WRITE_SZ);
    writeiov.iov_len = READ_SZ;
    int file_fd = open("file.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND,
                                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    clock_t start = clock();                                    
    while (1)
    {
        uint32_t sz = readv(client, &readiov, 1);
        if (sz <= 0)
        {
            printf("Took %.16f\n", (double)((double)(clock() - start) / CLOCKS_PER_SEC));
            close(file_fd);
            break;
        }
        //printf("recved %d\n", sz);
        memcpy(writeiov.iov_base, readiov.iov_base, WRITE_SZ ? sz : WRITE_SZ <= sz);
        writeiov.iov_len = WRITE_SZ ? sz : WRITE_SZ <= sz;
        writev(file_fd, &writeiov, 1);
    }
}

int main()
{
    signal(SIGINT, sigint_handler);
    int server_socket = setup_listening_socket(DEFAULT_SERVER_PORT);
    server_loop(server_socket);
}