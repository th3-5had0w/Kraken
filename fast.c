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
#define EVENT_TYPE_ACCEPT       0
#define EVENT_TYPE_READ         1
#define EVENT_TYPE_WRITE        2

#define MAX_CONN    1024


pthread_mutex_t mutex;

typedef struct request {
    int event_type;
    int iovec_count;
    int client_socket;
    struct iovec iov[];
} request;

struct io_uring ring;
struct io_uring_params params;

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

int add_accept_request(int server_socket, struct sockaddr_in *client_addr,
                       socklen_t *client_addr_len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_accept(sqe, server_socket, (struct sockaddr *) client_addr,
                         client_addr_len, 0);
    struct request *req = malloc(sizeof(*req));
    req->event_type = EVENT_TYPE_ACCEPT;
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);

    return 0;
}

int add_read_request(uint32_t client_sock) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    struct request *req = zh_malloc(sizeof(*req) + sizeof(struct iovec));
    req->iov[0].iov_base = zh_malloc(READ_SZ);
    req->iov[0].iov_len = READ_SZ;
    req->event_type = EVENT_TYPE_READ;
    memset(req->iov[0].iov_base, 0, READ_SZ);
    /* Linux kernel 5.5 has support for readv, but not for recv() or read() */
    io_uring_prep_readv(sqe, client_sock, &req->iov[0], 1, 0);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);
    return 0;
}

int add_write_request(struct request *req) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    req->event_type = EVENT_TYPE_WRITE;
    io_uring_prep_writev(sqe, req->client_socket, req->iov, req->iovec_count, 0);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);
    return 0;
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
    struct io_uring_cqe *cqe;
    struct sockaddr_in client_addr;
    int file_fd;
    uint32_t peek;
    socklen_t client_addr_len = sizeof(client_addr);
    uint32_t client_sock;
    clock_t start = clock();

    add_accept_request(server_socket, &client_addr, &client_addr_len);
    while (1) {
        peek = io_uring_peek_cqe(&ring, &cqe);
        if (peek) continue;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0)
            fatal_error("io_uring_wait_cqe");
        struct request *req = (struct request *) cqe->user_data;
        if (cqe->res < 0) {
            fprintf(stderr, "Async request failed: %s for event: %d\n",
                    strerror(-cqe->res), req->event_type);
            exit(1);
        }

        switch (req->event_type) {
            case EVENT_TYPE_ACCEPT:
                client_sock = cqe->res;
                free(req);
                file_fd = open("file.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND,
                                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
                add_read_request(client_sock);
                break;

            case EVENT_TYPE_READ:
                if (!cqe->res) {
                    printf("Took %.16f\n", (double)((double)(clock() - start) / CLOCKS_PER_SEC));
                    close(client_sock);
                    exit(0);
                }
                else
                {
                    uint32_t sz = cqe->res;
                    struct request *write_req = zh_malloc(sizeof(*req) + sizeof(struct iovec));
                    write_req->iov[0].iov_base = zh_malloc(WRITE_SZ);
                    write_req->iov[0].iov_len = WRITE_SZ ? sz : WRITE_SZ <= sz;
                    write_req->client_socket = file_fd;
                    write_req->iovec_count = 1;
                    memcpy(write_req->iov[0].iov_base, req->iov[0].iov_base, WRITE_SZ ? sz : WRITE_SZ <= sz);
                    add_write_request(write_req);
                    add_read_request(client_sock);
                    break;
                }
                free(req->iov[0].iov_base);
                req->iov[0].iov_base = 0;
                free(req);
                break;


            case EVENT_TYPE_WRITE:
                for (int i = 0; i < req->iovec_count; i++) {
                    free(req->iov[i].iov_base);
                    req->iov[i].iov_base = 0;
                }
                free(req);
                break;


        }
        /* Mark this request as processed */
        io_uring_cqe_seen(&ring, cqe);
    }
}

int main()
{
    signal(SIGINT, sigint_handler);
    int server_socket = setup_listening_socket(DEFAULT_SERVER_PORT);
    params.flags |= IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 120000; // 2 minutes in ms
    io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params);
    //io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
    server_loop(server_socket);
}
