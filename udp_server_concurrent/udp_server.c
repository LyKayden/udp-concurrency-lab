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
// 全局会话路由表
struct session {
    // 存储客户端发送的各个 Datagram 对应的真实源 socket 地址信息（端口加上IP地址）
    struct sockaddr_in src_addr;
    // 1 活跃会话, 0 空闲/已关闭
    int valid;
    // 该 Datagram 来源客户端 socket 对应已经 connect 的专属服务器 socket fd
    int dedicated_fd;
} session_table[MAX_SESSIONS];

// 当前活跃会话数量
int session_conunt = 0;

// 按源地址查找或创建会话（这里使用的简易的O(N)线性查表）
session_ptr find_or_create_session(struct sockaddr_in* src_addr)
{
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (session_table[i].valid &&
        session_table[i].src_addr.sin_addr.s_addr == src_addr->sin_addr.s_addr &&
        session_table[i].src_addr.sin_port == src_addr->sin_port) {
            return &session_table[i];
        }
    }
    if (session_conunt >= MAX_SESSIONS) return NULL;
    session_ptr s = &session_table[session_conunt++];
    s->src_addr = *src_addr;
    s->valid = 1;
    s->dedicated_fd = -1;
    return s;
}

/* 统一的业务入口 */
// I/O路径允许分散，业务逻辑必须绝对集中。所有包最终汇聚于此。
void handle_packet(session_ptr sess, char* buf, int len)
{
    printf("[BUSINESS] session %s:%d | data:%s | length:%d | dedicated_fd:%d\n",
        inet_ntoa(sess->src_addr.sin_addr), ntohs(sess->src_addr.sin_port),
        buf, len, sess->dedicated_fd);
    // fflush(stdout);
    // 你的真实业务逻辑：协议解析、状态机、回复客户端等
}

/* I/O 读取路径 */
int read_data(int sockfd)
{
    char buf[MAXBUF];
    // 强制清零栈内存，以免后续打印时出现上次栈遗留数据干扰调试
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
    // I/O已成功读取，因会话层容量限制主动丢弃不处理该 Datagram
    if (!sess) return ret;

    handle_packet(sess, buf, ret);
    // 业务层处理完毕，返回原 Datagram 长度
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
        // Datagram 被消费读空是正常逻辑现象，这个打印相当于无效打印，且会干扰调试，所以注释掉了
        // printf("[ERROR] recvfrom failed in function read_data: %s\n", strerror(errno));
        return -1;
    }

    session_ptr sess = find_or_create_session(&peer_Addr);
    // 因超出会话层容量限制主动丢弃不处理该 Datagram
    if (!sess) return ret;

    int new_fd_to_register = 0;

    // 只有当该客户端尚无专属fd时，才创建新socket
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
        // 已有专属fd，说明是 listenfd 队列积压的重复首Datagram
        printf("[ACCEPT-EXIST] src:%s:%d routed to existing fd:%d\n",
            inet_ntoa(peer_Addr.sin_addr), ntohs(peer_Addr.sin_port), sess->dedicated_fd);
    }

    // Datagram 已从 listenfd 消费，统一交业务处理
    handle_packet(sess, buf, ret);
    // fflush(stdout);

    // 返回语义：>0 需注册epoll；0 包已处理无需注册；-1 由上方 recvfrom 处理
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
                        continue; // 重复的首Datagram已消费，无需新建fd，继续排空
                    } else {
                        break; // new_fd < 0, EAGAIN 队列空
                    }
                }
            } else {
                /*
                    第一种情况：
                    server 这边在 udp_accept 里通过 connect 客户端对应发送首个 Datagram 的各个端口，建立了专属通信规则后，
                    第二次及以后这些已对应的客户端 socket 发 Datagram 都直接到达了专属对应的服务器 socket。
                    （内核首先会对已进行 connect 的双端 socket 四元组精确匹配，会直接发送被内核分配到对应的对端 Socket 缓冲区内）


                    第二种情况：
                    客户端某些端口由于发过来的首 Datagram 在窗口期直接到达了服务器网卡，被服务器内核分配到了该时刻新创建绑定但
                    还未执行到 connect 这一行的 new_fd 代表的 new socket 缓冲区内了，因为内核分配是异步软中断的，我们无法
                    将其控制和我们的用户层程序同步，所以这种用 udp 根据到达的每个Datagram来为其创建对应的 socket
                    并将其连接到客户端对应的发送该 Datagram 的 socket 的方法会不可避免地出现窗口期这一个情况。
                */
                read_data(events[i].data.fd);
            }
        }
    }
    close(listenfd);
    return 0;
}
