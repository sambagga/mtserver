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
#include	<semaphore.h>
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
void *queue(void *);

int s, sock, ch, server, done, bytes, aflg,nthreads=4;
int soctype = SOCK_STREAM;
char *host = NULL;
char *port = NULL;
extern char *optarg;
extern int optind;
int newsock=0,clsock;
static int tpool_running=1;

struct treq{
	void* (*function)(void* arg);
	void* arg;
	struct treq* next;
	struct treq* prev;
};

struct trque{
	struct treq *start;
	struct treq *end;
	int njob;
	sem_t *queueSem;
};

//threadpool
struct tpool{
	pthread_t* tid;
	int nthreads;
	struct trque* reqqueue;
};

struct thread_data{
	pthread_mutex_t *mutex_p;
	struct tpool *tp_p;
};

void tpool_handlereq(struct tpool* t);
//Queue
int queinit(struct tpool* t)
{
	t->reqqueue=(struct trque*)malloc(sizeof(struct trque));
	if (t->reqqueue==NULL) return -1;
	t->reqqueue->start=NULL;
	t->reqqueue->end=NULL;
	t->reqqueue->njob=0;
	return 0;
}

void addque(struct tpool* t, struct treq* r)
{
	r->next=NULL;
	r->prev=NULL;
	struct treq *oldfront;
	oldfront = t->reqqueue->start;

	switch(t->reqqueue->njob)
	{
		case 0:     // empty queue
			t->reqqueue->end=r;
			t->reqqueue->start=r;
			break;
		default: 	// >0 jobs
			oldfront->prev=r;
			r->next=oldfront;
			t->reqqueue->start=r;
	}
	(t->reqqueue->njob)++;
	sem_post(t->reqqueue->queueSem);

	int sval;
	sem_getvalue(t->reqqueue->queueSem, &sval);
}

int remqueue(struct tpool* t)
{
	struct treq *oldback;
	oldback = t->reqqueue->end;
	switch(t->reqqueue->njob) //delete from back
	{
		case 0:     //empty queue
			return -1;
			break;
		case 1:     //only one request
			t->reqqueue->end=NULL;
			t->reqqueue->start=NULL;
			break;
		default: 	//>1 requests in queue
			oldback->prev->next=NULL;
			t->reqqueue->end=oldback->prev;
	}

	(t->reqqueue->njob)--;
	int sval;
	sem_getvalue(t->reqqueue->queueSem, &sval);
	return 0;
}

struct treq* getlast(struct tpool* t)
{
	return t->reqqueue->end;
}

void delqueue(struct tpool* t)
{
	struct treq* curreq;
	curreq=t->reqqueue->end;
	while(t->reqqueue->njob)
	{
		t->reqqueue->end=curreq->prev;
		free(curreq);
		curreq=t->reqqueue->end;
		t->reqqueue->njob--;
	}
	t->reqqueue->start=NULL;
	t->reqqueue->end=NULL;
}

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

struct tpool* poolinit()
{
	struct tpool* t;
	t=(struct tpool*)malloc(sizeof(struct tpool));
	if (t==NULL)
	{
		printf("poolinit(): Memory allocation for thread pool error\n");
		return NULL;
	}
	t->tid=(pthread_t*)malloc(nthreads*sizeof(pthread_t));
	if (t->tid==NULL)
	{
		fprintf(stderr, "poolinit(): Memory allocation for thread IDs error\n");
		return NULL;
	}
	t->nthreads=nthreads;

	// Initialise the request queue
	if (queinit(t)==-1){
		fprintf(stderr, "thpool_init(): Memory allocation for request queue error\n");
		return NULL;
	}

	// Initialise semaphore
	t->reqqueue->queueSem=(sem_t*)malloc(sizeof(sem_t));
	sem_init(t->reqqueue->queueSem, 0, 0);
	/* Make threads in pool */
	int i;
	for (i=0;i<nthreads;i++){
		printf("Created thread %d in pool \n", i);
		pthread_create(&(t->tid[i]), NULL, (void *)tpool_handlereq, (void *)t);
	}
	return t;
}

void tpool_handlereq(struct tpool* t)
{
	while(tpool_running)
	{
		if (sem_wait(t->reqqueue->queueSem)) 		//waiting for a request
		{
			perror("tpool_handlereq(): Error in semaphore");
			exit(1);
		}
		if (tpool_running)		// Read and handle request from queue
		{
			void*(*func_buff)(void* arg);
			void*  arg_buff;
			struct treq* req;
			pthread_mutex_lock(&get_mutex);                 // get mutex lock
			req = getlast(t);
			func_buff=req->function;
			arg_buff =req->arg;
			remqueue(t);
			pthread_mutex_unlock(&get_mutex);               //release mutex
			func_buff(arg_buff);               			 	//execute function
			free(req);
		}
		else
		{
			return;
		}
	}
	return;
}

int tpool_quereq(struct tpool* t, void *(*func)(void*), void* iarg)
{
	struct treq* nreq;
	nreq=(struct treq*)malloc(sizeof(struct treq));
	if (nreq==NULL)
	{
		fprintf(stderr, "tpool_quereq(): Memory allocation for new request failed\n");
		exit(1);
	}

	nreq->function=func;
	nreq->arg=iarg;

	// add request to queue
	pthread_mutex_lock(&get_mutex);
	addque(t, nreq);
	pthread_mutex_unlock(&get_mutex);

	return 0;
}

void delpool(struct tpool* t)
{
	int i;
	tpool_running=0; //end thread's infinite loop
	for (i=0; i<(t->nthreads); i++)	//idle threads waiting at semaphore
	{
		if (sem_post(t->reqqueue->queueSem))
		{
			fprintf(stderr, "delpool(): error in sem_wait()\n");
		}
	}

	if (sem_destroy(t->reqqueue->queueSem)!=0)
	{
		fprintf(stderr, "delpool(): error in destroying semaphore\n");
	}

	for (i=0;i<(t->nthreads); i++)//join so all threads finish before exit
	{
		pthread_join(t->tid[i], NULL);
	}

	delqueue(t);

	free(t->tid);
	free(t->reqqueue->queueSem);
	free(t->reqqueue);
	free(t);
}



int main(int argc,char *argv[])
{
	fd_set ready;
	struct sockaddr_in msgfrom;
	int msgsize,i;
	struct tpool *threadpool;
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
		size_t stacksize;
		pthread_t listhread,schque;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		stacksize = 500000;
		pthread_attr_setstacksize (&attr, stacksize);
		pthread_attr_getstacksize (&attr, &stacksize);
		pthread_create(&listhread,&attr,setup_server,NULL);
		threadpool=poolinit();
		pthread_create(&schque,&attr,queue,(void *)threadpool);
		pthread_attr_destroy(&attr);
		pthread_join(listhread, NULL);
		printf("Completed join with thread %d\n",i);
	}
/*
 * Set up select(2) on both socket and terminal, anything that comes
 * in on socket goes to terminal, anything that gets typed on terminal
 * goes out socket...
 */
	/*while (!done) {
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
	}*/
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
void *queue(void *tp)
{
	static int top=0;
	int suc=0;
	size_t stacksize;
	struct tpool *thread_pool=(struct tpool *)tp;
	while(1)
	{
		if(newsock)
		{
			newsock=0;
			tpool_quereq(thread_pool,handle_requests,(void*)clsock);
			if(suc==0)
				printf("\n%d Request\n",top);
			top++;
		}
	}
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
