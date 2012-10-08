#define	BUF_LEN	8192

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<ctype.h>
#include	<assert.h>
#include	<sys/types.h>
#include	<sys/socket.h>
#include	<netdb.h>
#include	<netinet/in.h>
#include	<inttypes.h>
#include	<pthread.h>
char *progname;
char buf[BUF_LEN];
pthread_mutex_t get_mutex=PTHREAD_MUTEX_INITIALIZER;
static char* not_found_response_template = "HTTP/1.0 404 Not Found\n" "Content-type: text/html\n" "\n" "<html>\n" " <body>\n" " <h1>Not Found</h1>\n" " <p>The requested URL %s was not found on this server.</p>\n" " </body>\n" "</html>\n";
static char* bad_method_response_template = "HTTP/1.0 501 Method Not Implemented\n" "Content-type: text/html\n" "\n" "<html>\n" " <body>\n" " <h1>Method Not Implemented</h1>\n" " <p>The method %s is not implemented by this server.</p>\n" " </body>\n" "</html>\n";
static char* ok_response = "HTTP/1.0 200 OK\n" "Content-type: text/html\n" "\n";
static char* bad_request_response = "HTTP/1.0 400 Bad Request\n" "Content-type: text/html\n" "\n" "<html>\n" " <body>\n" " <h1>Bad Request</h1>\n" " <p>This server did not understand your request.</p>\n" " </body>\n" "</html>\n";
void usage();
int setup_client();
int setup_server();

int s, sock, ch, server, done, bytes, aflg,nthreads=4;
int soctype = SOCK_STREAM;
char *host = NULL;
char *port = NULL;
extern char *optarg;
extern int optind;


static void handle_get (int connection_fd, const char* page)
{
	if (*page == '/' && strchr (page + 1, '/') == NULL)
	{
		char module_file_name[64];
		snprintf (module_file_name, sizeof (module_file_name), "%s.so", page + 1);
		if (module_file_name == NULL)
		{
			char response[1024];
			snprintf (response, sizeof (response), not_found_response_template, page);
			write (connection_fd, response, strlen (response));
		}
		else
		{
			write (connection_fd, ok_response, strlen (ok_response));
		}
	}
}
static void * handle_connection (int fdSock)
{
	int connection_fd = fdSock;
	char buffer[256];
	ssize_t bytes_read;
	bytes_read = read (connection_fd, buffer, sizeof (buffer) - 1);
	if (bytes_read > 0)
	{
		char method[sizeof (buffer)];
		char url[sizeof (buffer)];
		char protocol[sizeof (buffer)];
		buffer[bytes_read] = '\0';
		sscanf (buffer, "%s %s %s", method, url, protocol);
		while (strstr (buffer, "\r\n\r\n") == NULL)
			bytes_read = read (connection_fd, buffer, sizeof (buffer));
		if (bytes_read == -1)
		{
			close (connection_fd);
		}
		if (strcmp (protocol, "HTTP/1.0") && strcmp (protocol, "HTTP/1.1"))
		{
			write (connection_fd, bad_request_response, sizeof (bad_request_response));
		}
		else if (strcmp (method, "GET"))
		{
			char response[1024];
			snprintf (response, sizeof (response), bad_method_response_template, method);
			write (connection_fd, response, strlen (response));
		}
		else
			handle_get (connection_fd, url);
	 }
	 else if (bytes_read == 0)
		 ;
	 else
	 {
		perror ("read");
	 }
	 close(connection_fd);
}
void * serv_request(void *insock)
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
			 alen=sizeof(fsin); /* Serving new incoming client's connection */
			 pthread_mutex_lock(&get_mutex);
			 ssock=accept(msock,(struct sockaddr *)&fsin,&alen);
			 pthread_mutex_unlock(&get_mutex);
			 if (ssock < 0)
				 perror("accept()");
			 socklen_t address_length;
			 address_length = sizeof (socket_address);
			 rval = getpeername (ssock, &socket_address, &address_length);
			 assert (rval == 0);
			 printf ("Thread '%d' accepted connection from %s\n", pthread_self(),inet_ntoa (socket_address.sin_addr));
			 FD_SET(ssock,&afds);
		 }
		 for (fd=0;fd < nfds; ++fd)
			 if (fd != msock && FD_ISSET(fd,&rfds))
			 {
				 printf("Thread '%d' responding request...\n",pthread_self()); /* Serving client had been already connected to server */
				 handle_connection(fd);
				 FD_CLR(fd,&afds);
			 }
		 }
}

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
		setup_server();
		size_t stacksize;
		pthread_t *thread_pool;
		pthread_attr_t attr;
		thread_pool=malloc((nthreads+2)*sizeof(pthread_t));
		pthread_attr_init(&attr);
		stacksize = 500000;
		pthread_attr_setstacksize (&attr, stacksize);
		pthread_attr_getstacksize (&attr, &stacksize);
		for(i=0; i<nthreads+2; i++)
		{
			 pthread_create(&thread_pool[i],&attr,serv_request,(void*)s);
		}
		pthread_attr_destroy(&attr);
		for(i=0;i<nthreads+2;i++)
		{
			pthread_join(thread_pool[i], NULL);
			printf("Completed join with thread %d\n",i);
		}
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

/*
 * setup_server() - set up socket for mode of soc running as a server.
 */

void setup_server() {
	struct sockaddr_in serv, remote;
	struct servent *se;
	int newsock, len;

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
	listen(s, 1);
	//newsock = s;
	if (soctype == SOCK_STREAM) {
		fprintf(stderr, "Entering accept() waiting for connection.\n");
	//	newsock = accept(s, (struct sockaddr *) &remote, &len);
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
