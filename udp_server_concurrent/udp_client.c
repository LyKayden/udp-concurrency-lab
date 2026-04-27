#include <unistd.h>
#include <string.h>

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>


#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/resource.h>

#define MAXBUF 1024

void createClient(int id, int myPort, int peerPort)
{
    struct sockaddr_in peer_Addr;
    bzero(&peer_Addr, sizeof(peer_Addr));
    peer_Addr.sin_family = PF_INET;
    peer_Addr.sin_port = htons(peerPort);
    peer_Addr.sin_addr.s_addr = inet_addr("192.168.64.128"); // server ipv4 address

    struct sockaddr_in self_Addr;
    bzero(&self_Addr, sizeof(self_Addr));
    self_Addr.sin_family = PF_INET;
    self_Addr.sin_port = htons(myPort);
    self_Addr.sin_addr.s_addr = inet_addr("0.0.0.0"); // bind to all local ipv4 address

    int socketFd = socket(PF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socketFd == -1) {
        printf("[ERROR] socket creation failed: %s\n", strerror(errno));
        exit(1);
    }


    int reuse = 1;
    if(setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &reuse,sizeof(reuse)) < 0 ||
        setsockopt(socketFd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        printf("[ERROR] setsockopt failed: %s\n", strerror(errno));
        exit(1);
    }

    int opt = fcntl(socketFd,F_GETFL);
    fcntl(socketFd, F_SETFL, opt | O_NONBLOCK);

    if (bind(socketFd, (struct sockaddr *) &self_Addr, sizeof(struct sockaddr)) < 0) {
        printf("[ERROR] bind failed: %s\n", strerror(errno));
        exit(1);
    } else {

    }

    if (connect(socketFd, (struct sockaddr *) &peer_Addr, sizeof(struct sockaddr)) < 0) {
        printf("[ERROR] connect failed: %s\n", strerror(errno));
        exit(1);
    }


    usleep(1); // --> key

    char buffer[MAXBUF] = {0};
    memset(buffer, 0, MAXBUF);
    sprintf(buffer, "hello %d", id);
    sendto(socketFd, buffer, strlen(buffer), 0, (struct sockaddr*)&peer_Addr, sizeof(struct sockaddr_in));
}

void serial(int clinetNum) {
    for(int i = 0; i < clinetNum; ++i) {
        createClient(i, 2025 + i, 1234);
        // usleep(10);
    }
}

int main(int argc, char* argv[])
{

	serial(1024);

    printf("serial success\n");
    return 0;
}
