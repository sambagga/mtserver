#define	BUF_LEN	8192
#include	<iostream>
#include	<vector>
#include	<map>
#include	<stdio.h>
#include	<unistd.h>
#include	<dirent.h>
#include	<stdlib.h>
#include	<string.h>
#include	<ctype.h>
#include 	<termios.h>
#include	<fcntl.h>
#include	<assert.h>
#include	<sys/types.h>
#include	<sys/socket.h>
#include	<netdb.h>
#include	<netinet/in.h>
#include	<inttypes.h>
#include	<pthread.h>
#include	<semaphore.h>
#include	<time.h>
#include 	<arpa/inet.h>
#include 	<sys/stat.h>

using namespace std;
#define OK_IMAGE    "HTTP/1.0 200 OK\nContent-Type:image/gif\n\n"
#define OK_HTML     "HTTP/1.0 200 OK\nContent-Type:text/html\n\n"
#define NOTOK_404   "HTTP/1.0 404 Not Found\nContent-Type:text/html\n\n"
#define FNF_404    "<html><body><h1>FILE NOT FOUND</h1></body></html>"
#define BUFSIZE 1024
char *progname;
char buf[BUF_LEN];
char log_file[256];
char rootdir[256];
pthread_mutex_t get_mutex=PTHREAD_MUTEX_INITIALIZER;

void usage();
void *setup_server(void *);
void *queue(void *);

int s, sock, ch, server, done, bytes, aflg,nthreads=4,debug=0;;
int soctype = SOCK_STREAM;
char *port = NULL,sched[5];
extern char *optarg;
extern int optind;
int newsock=0,clsock,sch=1,quetime=60;
static int execution=1;
struct request{
	void * arg;
	char fname[BUFSIZE];
};

struct trque{
	vector<struct request> fcfs;
	map<int,struct request> sjf;
	int njob;
	sem_t *queueSem;
};

//threadpool
struct tpool{
	pthread_t* tid;
	int nthreads;
	struct trque reqqueue;
};

typedef struct logging {
    int sock_fd;
    char receipt_time[BUFSIZE];
    char sched_time[BUFSIZE];
    int status;
    size_t size;
    char method[256];
    struct in_addr client_address;
}log;

FILE *log_file_fd = NULL;
map<int,log>log_data;
string exec(char* cmd) {
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "ERROR";
    char buffer[128];
    std::string result = "";
    while(!feof(pipe)) {
        if(fgets(buffer, 128, pipe) != NULL)
                result += buffer;
    }
    pclose(pipe);
    return result;
}
void log_status(int index)
{
	char logs[1024],err;
	log_file_fd = fopen(log_file, "a+");
	if(!log_file_fd)
		perror("couldn't open log file");

		sprintf(logs,"%s %s %s %s %d %zu\n",
		inet_ntoa (log_data[index].client_address),
		log_data[index].receipt_time,
		log_data[index].sched_time,
		log_data[index].method,
		log_data[index].status,
		log_data[index].size);
		//printf("\nLOG:%s\n",logs);
		err=fprintf(log_file_fd,"%s",logs);//fwrite(logs,sizeof(logs),1,log_file_fd);
		/*if(err!=sizeof(logs))
		{
			printf("Writing error!");
		}*/

	fclose(log_file_fd);
}

void* handlereq(void *th);
//Queue
struct request getlast(struct tpool* t)
{
	request r;
	int index=t->reqqueue.njob-1;
	if(sch==1)
	{
		r=t->reqqueue.fcfs[index];
		//strcpy(r.fname,t->reqqueue.fcfs[index].fname);
		t->reqqueue.fcfs.erase(t->reqqueue.fcfs.begin()+index);
	}
	else
	{
		r=(*t->reqqueue.sjf.begin()).second;
		t->reqqueue.sjf.erase(t->reqqueue.sjf.begin());
	}
	t->reqqueue.njob--;
	return r;
}

void delqueue(struct tpool* t)
{
	t->reqqueue.fcfs.clear();
	t->reqqueue.sjf.clear();
}

void *protocol(struct request r)
{
    int client_s=(int)r.arg;         //copy socket
    char ibuf[BUFSIZE];          // for GET request
    char obuf[BUFSIZE];          // for HTML response
    char *fname=r.fname;
    int fd;
    int buffile;
    char line[256];
    char *ptr,lsbuf[BUF_LEN];
    int status;
    struct stat st_buf;
    time_t curt=time(NULL);
    strcpy(log_data[(int)r.arg].sched_time,asctime(gmtime(&curt)));
    /*string ls=exec("ls");
    cout<<"Using ls"<<ls;*/
    status = stat (r.fname, &st_buf);
    if (status != 0)
    {
    	printf ("Error, not an existing file or directory");
    }
    else
    {
    	if (S_ISREG (st_buf.st_mode))
    	{
    		fd = open(&r.fname[0], O_RDONLY, S_IREAD | S_IWRITE);
    		if (fd == -1)
    		{
    			if ((strstr(r.fname, ".jpg") != NULL) ||(strstr(r.fname, ".gif") != NULL) || (strstr(r.fname, ".png") != NULL) || (strstr(r.fname , ".html") !=NULL))
    			{
    				printf("File %s not found\n", &r.fname[1]);
    				log_data[(int)r.arg].status = 404;
    				strcpy(obuf, NOTOK_404);
    				send(client_s, obuf, strlen(obuf), 0);
    				strcpy(obuf, FNF_404);
    				send(client_s, obuf, strlen(obuf), 0);
    			}
    		}
    		else
    		{
    			//printf("File %s is being sent \n", &fname[1]);
    			log_data[(int)r.arg].status = 200;
    			if ((strstr(r.fname, ".jpg") != NULL) ||(strstr(r.fname, ".gif") != NULL) || (strstr(r.fname, ".png") != NULL))
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
    	if (S_ISDIR (st_buf.st_mode))
    	{
    		DIR *dp;
    		int flag=0;
    		struct dirent *dirp;
    		if((dp  = opendir(r.fname)) == NULL)
    			cout << "Error in opening " << r.fname << endl;
    		while ((dirp = readdir(dp)) != NULL)
    		{
    			if(strcmp(dirp->d_name,"index.html")==0)
    		    {
    				flag=1;
    				sprintf(obuf,"%s%s",fname,dirp->d_name);
    				fd = open(&obuf[0], O_RDONLY, S_IREAD | S_IWRITE);
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
    				break;
    		    }
    		}
    		if(flag!=1)
    		{
    			if((dp  = opendir(r.fname)) == NULL)
    			    cout << "Error in opening " << r.fname << endl;
    			strcpy(obuf, OK_HTML);
    			send(client_s, obuf, strlen(obuf), 0);
    			strcpy(obuf,"<html><body> <h1>Directory Contents</h1>");
    			send(client_s, obuf, strlen(obuf), 0);
    			obuf[0]='\0';
    			while ((dirp = readdir(dp)) != NULL)
    			{
    				if(dirp->d_name[0]!='.')
    				{
    					sprintf (obuf,"<a href=%s%s>%s</a><br>",r.fname,dirp->d_name,dirp->d_name);
    					send(client_s, obuf, strlen(obuf), 0);
    					obuf[0]='\0';
    				}
    			}
    			sprintf(obuf,"</body></html>");
    			send(client_s, obuf, strlen(obuf), 0);
    			closedir(dp);
    		}
    	}
    }
    log_status((int)r.arg);
    close(fd);
    close(client_s);
    if(debug==0)
      	newsock=2;
}

struct tpool *poolinit()
{
	struct tpool *t;
	t=(struct tpool*)malloc(sizeof(struct tpool));
	t->tid=(pthread_t*)malloc(nthreads*sizeof(pthread_t));
	if (t->tid==NULL)
	{
		fprintf(stderr, "poolinit(): Memory allocation for thread IDs error\n");
		return NULL;
	}
	t->nthreads=nthreads;

	// Initialise the request queue
	t->reqqueue.njob=0;
	t->reqqueue.queueSem=(sem_t*)malloc(sizeof(sem_t));
	sem_init(t->reqqueue.queueSem, 0, 0);
	/* Make threads in pool */
	int i;
	for (i=0;i<nthreads;i++){
		printf("Created thread %d in pool \n", i);
		pthread_create(&(t->tid[i]), NULL,handlereq, (void *)t);
	}
	return t;
}

void* handlereq(void *th)
{
	struct tpool*t=(struct tpool *)th;
	while(execution || newsock==2)
	{
        if(quetime)
        {
        	sleep(quetime);
            quetime=0;
        }
		if (sem_wait(t->reqqueue.queueSem)) 		//waiting for a request
		{
			perror("handlereq(): Error in semaphore");
			exit(1);
		}
		if (execution)		// Read and handle request from queue
		{
			struct request r;
			pthread_mutex_lock(&get_mutex);                 // get mutex lock
			r = getlast(t);
			log_data[(int)r.arg].sock_fd = (int)r.arg;
			pthread_mutex_unlock(&get_mutex);               //release mutex
			protocol(r);               			 	//execute function
		}
		else
		{
			break;
		}
	}
	pthread_exit(NULL);
}

int quereq(struct tpool* t,struct request r, int size)
{
	pthread_mutex_lock(&get_mutex);
	if(sch==1)
	{
		t->reqqueue.fcfs.push_back(r);
	}
	else
	{
		t->reqqueue.sjf[size]=r;
	}
	t->reqqueue.njob++;
	sem_post(t->reqqueue.queueSem);
	int sval;
	sem_getvalue(t->reqqueue.queueSem, &sval);
	pthread_mutex_unlock(&get_mutex);

	return 0;
}

void delpool(struct tpool* t)
{
	int i;
	execution=0; //end thread's infinite loop
	for (i=0; i<(t->nthreads); i++)	//idle threads waiting at semaphore
	{
		if (sem_post(t->reqqueue.queueSem))
		{
			fprintf(stderr, "delpool(): error in sem_wait()\n");
		}
	}

	if (sem_destroy(t->reqqueue.queueSem)!=0)
	{
		fprintf(stderr, "delpool(): error in destroying semaphore\n");
	}

	for (i=0;i<(t->nthreads); i++)//join so all threads finish before exit
	{
		pthread_join(t->tid[i], NULL);
	}

	delqueue(t);

	free(t->tid);
	free(t->reqqueue.queueSem);
	free(t);
}



int main(int argc,char *argv[])
{
	fd_set ready;
	struct sockaddr_in msgfrom;
	int msgsize,i;
	struct tpool *threadpool;

	log_file[0] = '\0';

	union {
		uint32_t addr;
		char bytes[4];
	} fromaddr;

	if ((progname = rindex(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		progname++;
	server = 1;
	while ((ch = getopt(argc, argv, "adhr:s:n:p:l:t:")) != -1)
		switch(ch) {
			case 'a':
				aflg++;		/* print address in output */
				break;
			case 'd':
				debug = 1;
				break;
			case 'r':
				strcpy(rootdir,optarg);
				break;
			case 's':
                                if(strcmp(optarg,"FCFS"))
                                    sch=1;
                                else if(strcmp(optarg,"SJF"))
                                    sch=2;
				break;
			case 'n':
				nthreads = atoi(optarg);
				break;
			case 'p':
				port = optarg;
				break;
			case 'l':
				strncpy(log_file, optarg, sizeof(log_file));
				break;
			case 't':
				quetime = atoi(optarg);
				break;
			case 'h':
			default:
				usage();
				break;
		}
	argc -= optind;
	if (argc != 0)
		usage();
	if (!server && port == NULL)
		usage();
/*
 * Create socket on local host.
 */
	if ((s = socket(AF_INET, soctype, 0)) < 0) {
		perror("socket");
		exit(1);
	}
	if(server)
	{
		size_t stacksize;
		pthread_t listhread,schque;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		stacksize = 500000;
		pthread_attr_setstacksize (&attr, stacksize);
		pthread_attr_getstacksize (&attr, &stacksize);
		int f=1;
		pthread_create(&listhread,&attr,setup_server,(void*)f);
		threadpool=poolinit();
		pthread_create(&schque,&attr,queue,(void *)threadpool);
		pthread_attr_destroy(&attr);
		pthread_join(listhread, NULL);
		pthread_join(schque, NULL);
		printf("Completed join with thread %d\n",i);
	}
	fclose(log_file_fd);
	return(0);
}

void *queue(void *tp)
{
	static int top=1;
	int suc=0;
	size_t stacksize;
	struct tpool *thread_pool=(struct tpool *)tp;
	char ibuf[BUFSIZE];
	char *fname,temp[30];
	int buffile;
	int retcode;
	char line[256];
	string dir=exec("pwd");
	string ret="\n";
	struct request r;
	dir.erase(dir.find(ret));
	char* home="home";
	char* found;
	int size=0;
	FILE *fd;
	int status;
	struct stat st_buf;
	while(1)
	{
		if(newsock==1)
		{
			if(debug)
				newsock=0;
			retcode = recv((int)clsock, ibuf, BUFSIZE, 0);	//HTTP request
			sscanf(ibuf,"%s",line);
			int i=0;
			for(;ibuf[i]!='\r';i++)
				log_data[(int)clsock].method[i]=ibuf[i];
			log_data[(int)clsock].method[i]='\0';
			if (retcode < 0)
				printf("recv error detected ...\n");
			else
			{
			   	//get the file name
                strtok(ibuf, " ");
			    fname = strtok(NULL, " ");
			    found=strstr(fname,home);
			    if(!found)
			    {
			    	strcpy(temp,fname);
			        fname[0]='\0';
			        if(*rootdir=='\0')
			        	sprintf(rootdir,"%s",(char*)dir.c_str());
			        sprintf(fname,"%s%s",rootdir,temp);
			    }
			        cout<<fname;
			        status = stat (fname, &st_buf);
			        if (status != 0)
			        {
			           	printf ("Error, not an existing file or directory");
			           	continue;
			        }
			        else
			        {
			        	if (S_ISREG (st_buf.st_mode)) {
			        		cout<<"File!";
			        		fd=fopen(fname,"rb");
			        		fseek(fd,0,SEEK_END);
			        		size=ftell(fd);
			        		fclose(fd);
			        	}
			        	if (S_ISDIR (st_buf.st_mode)) {
			        		cout<<"Directory!";
			        		size=0;
			        	}
			        }
			    log_data[(int)clsock].size=size;
			}
			r.arg=(void*)clsock;
			strcpy(r.fname,fname);
			quereq(thread_pool,r,size);
			printf("\n%d Request\n",top);
			top++;
		}
		else if(newsock==2)
			break;
	}
	execution=0;
	delpool(thread_pool);
	pthread_exit(NULL);
}

/*
 * setup_server() - set up socket for mode of soc running as a server.
 */

void *setup_server(void *f) {
	struct sockaddr_in serv, remote,addr;
	struct servent *se;
	socklen_t len;
	int rval;
	socklen_t address_length;
	struct sockaddr socket_address;

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
	if (getsockname(s, (struct sockaddr *)&remote, &len) < 0) {
		perror("getsockname");
		exit(1);
	}
	fprintf(stderr, "Port number is %d\n", ntohs(remote.sin_port));
	while(1){
		listen(s, nthreads);
		if(newsock==0)
		{
			if (soctype == SOCK_STREAM) {
				fprintf(stderr, "Entering accept() waiting for connection.\n");
				clsock = accept(s, (struct sockaddr *)&remote, &len);
				if(clsock!=-1)
					newsock=1;
				address_length = sizeof (socket_address);
				rval = getpeername ((int)clsock, &socket_address, &address_length);
				assert (rval == 0);
				memcpy(&addr, &socket_address, sizeof(socket_address));
				log_data[(int)clsock].client_address=addr.sin_addr;
				time_t curt=time(NULL);
				strcpy(log_data[(int)clsock].receipt_time,asctime(gmtime(&curt)));
			}
		}
		else if(newsock==2)
			break;
	}
	pthread_exit(NULL);
}

/*
 * usage - print usage string and exit
 */

void
usage()
{
	fprintf(stderr, "usage:myhttpd [−d] [−h] [−l file] [−p port] [−r dir] [−t time] [−n threadnum] [−s sched]\n", progname);
	fprintf(stderr, "−d : Enter debugging mode. That is, do not daemonize, only accept one connection at a time and enable logging to stdout. Without this option, the web server should run as a daemon process in the background.\n"
			"−h        : Print a usage summary with all options and exit.\n"
			"−l file   : Log all requests to the given file. See LOGGING for details.\n"
			"−p port   : Listen on the given port. If not provided, myhttpd will listen on port 8080.\n"
			"−r dir    : Set the root directory for the http server to dir.\n"
			"−t time   : Set the queuing time to time seconds. The default should be 60 seconds.\n"
			"−n threadnum: Set number of threads waiting ready in the execution thread pool to threadnum. The default should be 4 execution threads.\n"
			"−s sched  : Set the scheduling policy. It can be either FCFS or SJF. The default will be FCFS.\n", progname);
	exit(1);
}
