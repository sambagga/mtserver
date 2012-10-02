/*
 * server.c
 *
 *  Created on: Sep 30, 2012
 *      Author: sameer
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>

#define PORT "8080"

#define PCONNECT 10     // pending connections

#define MAXDATASIZE 1024
void *clientchat(void *clientfd)
{
	unsigned int cfd;
	int nbytes;
	char buf[MAXDATASIZE],req[20];
	cfd= *(unsigned int *) clientfd;
	if ((nbytes = recv(cfd, buf, MAXDATASIZE-1, 0)) == -1) {
		perror("recv");
		exit(1);
	}
	buf[nbytes]='\0';

	printf("server: received '%s'\n",buf);
	sscanf(buf,"%s",req);
	if (send(cfd, req, strlen(req), 0) == -1)
		perror("send");
	close(cfd);
	pthread_exit(NULL);
}
void sigchld_handler(int s)
{
    while(waitpid(-1, NULL, WNOHANG) > 0);
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(void)
{
    int sockfd, clientfd;
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage clientadr; // connector's address information
    socklen_t sin_size;
    struct sigaction sa;
    int yes=1;
    char s[INET6_ADDRSTRLEN];
    int rv;
    pthread_t clthread;
    pthread_attr_t attr;
    int clret,numbytes;
    unsigned int ids;
    char buf[MAXDATASIZE];

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;//support IPv4 & 6
    hints.ai_socktype = SOCK_STREAM; //TCP
    hints.ai_flags = AI_PASSIVE; //localhost

    if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // loop and bind to address
    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                p->ai_protocol)) == -1) {
            perror("server: socket");
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                sizeof(int)) == -1) {
            perror("setsockopt");
            exit(1);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("server: bind");
            continue;
        }

        break;
    }

    if (p == NULL)  {
        fprintf(stderr, "server: failed to bind\n");
        return 2;
    }

    freeaddrinfo(servinfo);

    if (listen(sockfd, PCONNECT) == -1) {
        perror("listen");
        exit(1);
    }

    sa.sa_handler = sigchld_handler; // reap all dead processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    printf("server: waiting for connections...\n");
    pthread_attr_init(&attr);
    while(1) {  // main accept() loop
        sin_size = sizeof clientadr;
        clientfd = accept(sockfd, (struct sockaddr *)&clientadr, &sin_size);
        if (clientfd == -1) {
            perror("accept");
            continue;
        }

        inet_ntop(clientadr.ss_family,
            get_in_addr((struct sockaddr *)&clientadr),
            s, sizeof s);
        printf("server: got connection from %s\n", s);

        ids=clientfd;
        clret = pthread_create(&clthread, &attr, clientchat, &ids);
    }
    close(sockfd);

    return 0;
}
