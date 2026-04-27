#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>
#include <assert.h>


#define MAXBUF 1024
#define MAXEPOLLSIZE 1024
#define MAX_SESSIONS 10000

typedef struct session session_t;
typedef struct session* session_ptr;

/* Global session routing table */
struct session {
    // Stores the real client source address (IP + port) for each incoming datagram
    struct sockaddr_in src_addr;
    // 1 = active session, 0 = idle/closed
    int valid;
    // Dedicated server socket fd bound to this client session
    int dedicated_fd;
} session_table[MAX_SESSIONS];

// Current number of active sessions
int session_count = 0;

// Find or create a session by source address (uses simple O(N) linear search)
session_ptr find_or_create_session(struct sockaddr_in* src_addr)
{
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (session_table[i].valid &&
        session_table[i].src_addr.sin_addr.s_addr == src_addr->sin_addr.s_addr &&
        session_table[i].src_addr.sin_port == src_addr->sin_port) {
            return &session_table[i];
        }
    }
    if (session_count >= MAX_SESSIONS) return NULL;
    session_ptr s = &session_table[session_count++];
    s->src_addr = *src_addr;
    s->valid = 1;
    s->dedicated_fd = -1;
    return s;
}

/* Unified business logic entry point */
// I/O paths can be distributed, but business logic must be centrally processed. 
// All received datagrams will ultimately converge here for processing.
void handle_packet(session_ptr sess, char* buf, int len)
{
    printf("[BUSINESS] session %s:%d | data:%s | length:%d | dedicated_fd:%d\n",
        inet_ntoa(sess->src_addr.sin_addr), ntohs(sess->src_addr.sin_port),
        buf, len, sess->dedicated_fd);
    // fflush(stdout);
    
    /*
     * Your core business logic: protocol parsing, state machine, database access , 
     * specific business logic calculations, replying to clients, etc.
     */
}

/* The main datagram I/O read path after the initial datagram from each client socket has been received */
int read_data(int sockfd)
{
    char buf[MAXBUF];
    // Explicitly zero out stack memory to prevent stale data from interfering with debug prints
    memset(buf, 0, sizeof(buf));

    struct sockaddr_in pkt_Src;
    bzero(&pkt_Src, sizeof(pkt_Src));
    socklen_t src_len = sizeof(pkt_Src);

    int ret = recvfrom(sockfd, buf, MAXBUF, 0, (struct sockaddr*)&pkt_Src, &src_len);
    if (ret < 0) {
        printf("[ERROR] recvfrom failed in function read_data: %s\n", strerror(errno));
        return -1;
    }

    session_ptr sess = find_or_create_session(&pkt_Src);
    // I/O read succeeded, but packet is proactively dropped due to session table capacity limits
    if (!sess) return ret;

    handle_packet(sess, buf, ret);
    // Business processing complete, return original datagram length
    return ret;
}



int udp_accept(int listenfd, struct sockaddr_in my_Addr)
{
    char buf[MAXBUF];
    memset(buf, 0, sizeof(buf));

    struct sockaddr_in peer_Addr;
    bzero(&peer_Addr, sizeof(peer_Addr));
    socklen_t cli_len = sizeof(peer_Addr);

    int ret = recvfrom(listenfd, buf, MAXBUF, 0, (struct sockaddr*)&peer_Addr, &cli_len);
    if (ret < 0) {
        // EAGAIN is expected. Error logging is suppressed to avoid debug noise.
        // printf("[ERROR] recvfrom failed in function read_data: %s\n", strerror(errno));
        return -1;
    }

    session_ptr sess = find_or_create_session(&peer_Addr);
    // Proactively drop packet if session table capacity is exceeded
    if (!sess) return ret;

    int new_fd_to_register = 0;

    // Only create a new socket if this client lacks a dedicated fd
    if (sess->dedicated_fd == -1) {
        int new_sd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (new_sd < 0) {
            printf("[ERROR] socket creation failed in function udp_accept: %s\n", strerror(errno));
            exit(1);
        }

        int reuse = 1;
        setsockopt(new_sd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        int flag = fcntl(new_sd, F_GETFL, 0);
        fcntl(new_sd, F_SETFL, flag | O_NONBLOCK);

        if (bind(new_sd, (struct sockaddr*)&my_Addr, sizeof(my_Addr)) < 0) {
            printf("[ERROR] bind failed in function udp_accept: %s\n", strerror(errno));
            close(new_sd);
            exit(1);
        }

        if (connect(new_sd, (struct sockaddr*)&peer_Addr, sizeof(peer_Addr)) < 0) {
            printf("[ERROR] connect failed in function udp_accept: %s\n", strerror(errno));
            close(new_sd);
            exit(1);
        }

        sess->dedicated_fd = new_sd;
        new_fd_to_register = new_sd;
        printf("[ACCEPT-NEW] src:%s:%d created dedicated_fd:%d\n",
            inet_ntoa(peer_Addr.sin_addr), ntohs(peer_Addr.sin_port), new_sd);
    } else {
        // Dedicated fd already exists. This indicates a duplicate initial datagram backlogged in the listenfd queue.
        printf("[ACCEPT-EXIST] src:%s:%d routed to existing fd:%d\n",
            inet_ntoa(peer_Addr.sin_addr), ntohs(peer_Addr.sin_port), sess->dedicated_fd);
    }

    // Datagram consumed from listenfd, routed to the unified business handler
    handle_packet(sess, buf, ret);
    // fflush(stdout);

    // Return semantics: >0 = new fd to register with epoll; 0 = packet handled, no registration needed; -1 = queue empty (EAGAIN)
    return new_fd_to_register;
}



int main(int argc, char* argv[])
{
    struct sockaddr_in my_addr;
    int port = 1234;
    bzero(&my_addr, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(port);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    int listenfd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (listenfd < 0) {
        printf("[ERROR] socket creation failed: %s\n", strerror(errno));
        exit(1);
    } else {
        printf("listen socket OK, new socket:%d\n", listenfd);
    }

    int reuse = 1;
    if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0 ||
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        printf("[ERROR] setsockopt failed: %s\n", strerror(errno));
        close(listenfd);
        exit(1);
    }

    int flag = fcntl(listenfd, F_GETFL, 0);
    fcntl(listenfd, F_SETFL, flag | O_NONBLOCK);

    if (bind(listenfd, (struct sockaddr*)&my_addr, sizeof(my_addr)) < 0) {
        printf("[ERROR] bind failed: %s\n", strerror(errno));
        close(listenfd);
        exit(1);
    }

    int epfd = epoll_create(MAXEPOLLSIZE);
    struct epoll_event ev, events[MAXEPOLLSIZE];
    
    /* 
     * To set it as edge-triggered, a while loop needs to be added. 
     * Within one event loop, the socket buffer corresponding to this fd should be emptied.
     */
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listenfd;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) < 0) {
        printf("[ERROR] epoll_ctl add listenfd failed: %s\n", strerror(errno));
        close(listenfd);
        close(epfd);
        exit(1);
    } else {
        printf("epoll add listenfd:%d OK\n", listenfd);
    }

    while (1) {
        int nfds = epoll_wait(epfd, events, MAXEPOLLSIZE, -1);
        if (nfds < 0) {
            printf("[ERROR] epoll_wait failed: %s\n", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == listenfd) {
                while (1) {
                    int new_fd = udp_accept(listenfd, my_addr);
                    if (new_fd > 0) {
                        ev.events = EPOLLIN;
                        ev.data.fd = new_fd;
                        if (epoll_ctl(epfd, EPOLL_CTL_ADD, new_fd, &ev) < 0) {
                            printf("[ERROR] epoll_ctl add new_fd failed in function main: %s\n", strerror(errno));
                            close(new_fd);
                        } else {
                            printf("epoll add new_fd:%d OK\n", new_fd);
                        }
                    } else if (new_fd == 0) {
                        /* 
                         * The duplicate first Datagram has been consumed. 
                         * There is no need to create a new socketfd. Continue to empty the queue.
                         */
                        continue; 
                    } else {
                        break; // new_fd < 0 (EAGAIN), queue is empty
                    }
                }
            } else {
                /*
                 * Case 1: Normal Path
                 * After udp_accept() establishes a dedicated connected socket for a client's initial datagram,
                 * all subsequent datagrams from that client are routed directly to this dedicated socket by the kernel.
                 * (The kernel prioritizes exact 4-tuple matching for connected UDP sockets, delivering packets
                 * straight to the corresponding socket's receive queue.)
                 *
                 * Case 2: Race Condition Path (Bind-Connect Window)
                 * A critical race window exists between bind() and connect(). During this interval, the socket
                 * resides in the kernel's lookup structure for unconnected UDP sockets (the wildcard hash table).
                 * If an initial datagram arrives at the NIC within this microsecond window, the kernel's asynchronous
                 * softirq/NAPI processing triggers a socket lookup. Since connect() has not yet locked the exact
                 * 4-tuple (source/destination IP and port), the kernel cannot route the packet to a dedicated
                 * connected socket. Consequently, the datagram is hash-dispatched to the receive buffer of an
                 * unconnected server socket bound to the destination port. This represents a temporary, non-fixed
                 * packet distribution mechanism based solely on bound addresses rather than precise connections.
                 *
                 * Kernel packet dispatch is inherently asynchronous and cannot be synchronized with the exact
                 * moment user-space executes connect(). Therefore, this dynamic per-datagram socket creation model
                 * deterministically encounters this race window. The session routing layer resolves it by strictly
                 * decoupling I/O file descriptors from business logic.
                 */
                read_data(events[i].data.fd);
            }
        }
    }
    close(listenfd);
    return 0;
}
