#define	BUF_LEN	8192

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<ctype.h>
#include	<fcntl.h>
#include	<assert.h>
#include	<sys/types.h>
#include	<sys/socket.h>
#include	<netdb.h>
#include	<netinet/in.h>
#include	<inttypes.h>
#include	<pthread.h>
#include 	<sys/stat.h>
#define OK_IMAGE    "HTTP/1.0 200 OK\nContent-Type:image/gif\n\n"
#define OK_HTML     "HTTP/1.0 200 OK\nContent-Type:text/html\n\n"
#define NOTOK_404   "HTTP/1.0 404 Not Found\nContent-Type:text/html\n\n"
#define FNF_404    "<html><body><h1>FILE NOT FOUND</h1></body></html>"
#define BUFSIZE 1024
char *progname;
char buf[BUF_LEN];
pthread_mutex_t get_mutex=PTHREAD_MUTEX_INITIALIZER;
void usage();
int setup_client();
void *setup_server();
void *queue();

int s, sock, ch, server, done, bytes, aflg,nthreads=4;
int soctype = SOCK_STREAM;
char *host = NULL;
char *port = NULL;
extern char *optarg;
extern int optind;
int newsock=0,clsock;
void *handle_requests(void * arg)
{
    int client_s=(int) arg;         //copy socket

    char ibuf[BUFSIZE];          // for GET request
    char obuf[BUFSIZE];          // for HTML response
    char *fname;
    int fd;
    int buffile;
    int retcode;



      retcode = recv(client_s, ibuf, BUFSIZE, 0);	//HTTP request

      if (retcode < 0)
    	  printf("recv error detected ...\n");
      else
      {
        //get the file name
        strtok(ibuf, " ");
        fname = strtok(NULL, " ");
        fd = open(&fname[0], O_RDONLY, S_IREAD | S_IWRITE);
        if (fd == -1)
        {
          printf("File %s not found\n", &fname[1]);
          strcpy(obuf, NOTOK_404);
          send(client_s, obuf, strlen(obuf), 0);
          strcpy(obuf, FNF_404);
          send(client_s, obuf, strlen(obuf), 0);
        }
        else
        {
          printf("File %s is being sent \n", &fname[1]);
          if ((strstr(fname, ".jpg") != NULL) ||(strstr(fname, ".gif") != NULL) || (strstr(fname, ".png") != NULL))
        	  strcpy(obuf, OK_IMAGE);
          else
        	  strcpy(obuf, OK_HTML);
          send(client_s, obuf, strlen(obuf), 0);

          buffile = 1;
          while (buffile > 0)
          {
            buffile = read(fd, obuf, BUFSIZE);
            if (buffile > 0)
            {
              send(client_s, obuf, buffile, 0);
            }
          }
        }
      }
      close(fd);
      close(client_s);
      pthread_exit(NULL);
}

/*void * serv_request(void *insock)
{
	 int msock = (int)insock;
	 struct sockaddr_in fsin;
	 fd_set rfds;
	 fd_set afds;
	 unsigned int alen;
	 int fd,nfds;
	 int rval;
	 socklen_t address_length;
	 struct sockaddr_in socket_address;
	 nfds=getdtablesize();
	 FD_ZERO(&afds);
	 FD_SET(msock,&afds);
	 while(1)
	 {
		 memcpy(&rfds,&afds,sizeof(rfds));
		 if (select(nfds,&rfds,(fd_set *)0,(fd_set *)0,(struct timeval *)0) < 0)
			 perror("select()");
		 if (FD_ISSET(msock,&rfds))
		 {
			 int ssock;
			 alen=sizeof(fsin); // Serving new incoming client's connection
			 pthread_mutex_lock(&get_mutex);
			 ssock=accept(msock,(struct sockaddr *)&fsin,&alen);
			 pthread_mutex_unlock(&get_mutex);
			 if (ssock < 0)
				 perror("accept()");
			 socklen_t address_length;
			 address_length = sizeof (socket_address);
			 rval = getpeername (ssock, &socket_address, &address_length);
			 assert (rval == 0);
			 printf ("\nThread '%d' accepted connection from %s\n", pthread_self(),inet_ntoa (socket_address.sin_addr));
			 FD_SET(ssock,&afds);
		 }
		 for (fd=0;fd < nfds; ++fd)
			 if (fd != msock && FD_ISSET(fd,&rfds))
			 {
				 printf("\nThread '%d' responding request...\n",pthread_self()); //Serving client had been already connected to server
				 handle_connection(fd);
				 FD_CLR(fd,&afds);
			 }
		 }
}*/

int main(int argc,char *argv[])
{
	fd_set ready;
	struct sockaddr_in msgfrom;
	int msgsize,i;
	union {
		uint32_t addr;
		char bytes[4];
	} fromaddr;

	if ((progname = rindex(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		progname++;
	while ((ch = getopt(argc, argv, "adsn:p:h:")) != -1)
		switch(ch) {
			case 'a':
				aflg++;		/* print address in output */
				break;
			case 'd':
				soctype = SOCK_DGRAM;
				break;
			case 's':
				server = 1;
				break;
			case 'n':
				nthreads = atoi(optarg);
				break;
			case 'p':
				port = optarg;
				break;
			case 'h':
				host = optarg;
				break;
			case '?':
			default:
				usage();
		}
	argc -= optind;
	if (argc != 0)
		usage();
	if (!server && (host == NULL || port == NULL))
		usage();
	if (server && host != NULL)
		usage();
/*
 * Create socket on local host.
 */
	if ((s = socket(AF_INET, soctype, 0)) < 0) {
		perror("socket");
		exit(1);
	}
	if (!server)
		sock = setup_client();
	else
	{
		//setup_server();
		size_t stacksize;
		pthread_t *listhread,*schque;
		pthread_attr_t attr;
		listhread=malloc(sizeof(pthread_t));
		schque=malloc(sizeof(pthread_t));
		pthread_attr_init(&attr);
		stacksize = 500000;
		pthread_attr_setstacksize (&attr, stacksize);
		pthread_attr_getstacksize (&attr, &stacksize);
		pthread_create(&listhread,&attr,setup_server,NULL);
		pthread_create(&schque,&attr,queue,NULL);
		/*for(i=0; i<nthreads+1; i++)
		{
			 pthread_create(&thread_pool[i],&attr,serv_request,(void*)s);
		}*/
		pthread_attr_destroy(&attr);
		//for(i=0;i<nthreads+2;i++)
		//{
			pthread_join(listhread, NULL);
			printf("Completed join with thread %d\n",i);
		//}
	}
/*
 * Set up select(2) on both socket and terminal, anything that comes
 * in on socket goes to terminal, anything that gets typed on terminal
 * goes out socket...
 */
	while (!done) {
		FD_ZERO(&ready);
		FD_SET(sock, &ready);
		FD_SET(fileno(stdin), &ready);
		if (select((sock + 1), &ready, 0, 0, 0) < 0) {
			perror("select");
			exit(1);
		}
		if (FD_ISSET(fileno(stdin), &ready)) {
			if ((bytes = read(fileno(stdin), buf, BUF_LEN)) <= 0)
				done++;
			send(sock, buf, bytes, 0);
		}
		msgsize = sizeof(msgfrom);
		if (FD_ISSET(sock, &ready)) {
			if ((bytes = recvfrom(sock, buf, BUF_LEN, 0, (struct sockaddr *)&msgfrom, &msgsize)) <= 0) {
				done++;
			} else if (aflg) {
				fromaddr.addr = ntohl(msgfrom.sin_addr.s_addr);
				fprintf(stderr, "%d.%d.%d.%d: ", 0xff & (unsigned int)fromaddr.bytes[0],
			    	0xff & (unsigned int)fromaddr.bytes[1],
			    	0xff & (unsigned int)fromaddr.bytes[2],
			    	0xff & (unsigned int)fromaddr.bytes[3]);
			}
			write(fileno(stdout), buf, bytes);
		}
	}
	return(0);
}

/*
 * setup_client() - set up socket for the mode of soc running as a
 *		client connecting to a port on a remote machine.
 */

int
setup_client() {

	struct hostent *hp, *gethostbyname();
	struct sockaddr_in serv;
	struct servent *se;

/*
 * Look up name of remote machine, getting its address.
 */
	if ((hp = gethostbyname(host)) == NULL) {
		fprintf(stderr, "%s: %s unknown host\n", progname, host);
		exit(1);
	}
/*
 * Set up the information needed for the socket to be bound to a socket on
 * a remote host.  Needs address family to use, the address of the remote
 * host (obtained above), and the port on the remote host to connect to.
 */
	serv.sin_family = AF_INET;
	memcpy(&serv.sin_addr, hp->h_addr, hp->h_length);
	if (isdigit(*port))
		serv.sin_port = htons(atoi(port));
	else {
		if ((se = getservbyname(port, (char *)NULL)) < (struct servent *) 0) {
			perror(port);
			exit(1);
		}
		serv.sin_port = se->s_port;
	}
/*
 * Try to connect the sockets...
 */
	if (connect(s, (struct sockaddr *) &serv, sizeof(serv)) < 0) {
		perror("connect");
		exit(1);
	} else
		fprintf(stderr, "Connected...\n");
	return(s);
}
void *queue()
{
	static int top=0;
	size_t stacksize;
	pthread_t *thread_pool;
	pthread_attr_t attr;
	thread_pool=malloc((nthreads)*sizeof(pthread_t));
	pthread_attr_init(&attr);
	stacksize = 500000;
	pthread_attr_setstacksize (&attr, stacksize);
	pthread_attr_getstacksize (&attr, &stacksize);
	while(1)
	{
		if(newsock && top<nthreads)
		{
			newsock=0;
			printf("\nThread%d created\n",top);
			pthread_create(&thread_pool[top],&attr,handle_requests,(void*)clsock);
			pthread_join(thread_pool[top], NULL);
			top++;
		}
	}
	pthread_attr_destroy(&attr);
}

/*
 * setup_server() - set up socket for mode of soc running as a server.
 */

void *setup_server() {
	struct sockaddr_in serv, remote;
	struct servent *se;
	int len;

	len = sizeof(remote);
	memset((void *)&serv, 0, sizeof(serv));
	serv.sin_family = AF_INET;
	if (port == NULL)
		serv.sin_port = htons(0);
	else if (isdigit(*port))
		serv.sin_port = htons(atoi(port));
	else {
		if ((se = getservbyname(port, (char *)NULL)) < (struct servent *) 0) {
			perror(port);
			exit(1);
		}
		serv.sin_port = se->s_port;
	}
	if (bind(s, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
		perror("bind");
		exit(1);
	}
	if (getsockname(s, (struct sockaddr *) &remote, &len) < 0) {
		perror("getsockname");
		exit(1);
	}
	fprintf(stderr, "Port number is %d\n", ntohs(remote.sin_port));
	while(1){
		listen(s, nthreads);
		//newsock = s;
		if(newsock==0)
		{
			if (soctype == SOCK_STREAM) {
				fprintf(stderr, "Entering accept() waiting for connection.\n");
				clsock = accept(s, (struct sockaddr *) &remote, &len);
				if(clsock!=-1)
				{
					newsock=1;
					//queue();
				}
			}
		}
	}

	//return(newsock);
}

/*
 * usage - print usage string and exit
 */

void
usage()
{
	fprintf(stderr, "usage: %s -h host -p port\n", progname);
	fprintf(stderr, "usage: %s -s [-p port -n noofthreads]\n", progname);
	exit(1);
}
