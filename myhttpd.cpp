#define	BUF_LEN	8192
#include	<iostream>
#include	<vector>
#include	<map>
#include	<stdio.h>
#include 	<syslog.h>
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
#define HTTP_OK	 "HTTP/1.0 200 OK\n"
#define HTTP_NOTOK "HTTP/1.0 404 Not Found\n"
#define IMAGE    "Content-Type:image/gif\n"
#define HTML     "Content-Type:text/html\n"
#define FNF_404    	"<html><body><h1>FILE NOT FOUND</h1></body></html>"
#define DNF_404    	"<html><body><h1>DIRECTORY NOT FOUND</h1></body></html>"
#define DFNF_404    "<html><body><h1>NOT A EXISTING FILE OR DIRECTORY</h1></body></html>"
#define BUFSIZE 1024
char *progname;
char buf[BUF_LEN];
char log_file[256];
char rootdir[256],updir[256];
pthread_mutex_t get_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t debug_mutex=PTHREAD_MUTEX_INITIALIZER;
void usage();
void *setup_server(void *);
void *queue(void *);

int s, sock, ch, server, done, bytes, nthreads=4, debug=0, dflag=0;
int soctype = SOCK_STREAM;
char *port = "8080", sched[5];
extern char *optarg;
extern int optind;
int newsock=0, clsock, sch=1, quetime=60;
static int execution=1;
//request structure
struct request {
	int clientfd;		//Client File descriptor
	char fname[BUFSIZE];//File name
	int rtype;			//Request Type 0->Invalid,1->HEAD,2->GET
	int size;			//File size
};

//Queue to store requests
struct trque {
	vector<struct request> fcfs; //For FCFS
	map<int,struct request> sjf; //For SJF, map is always sorted
	int nreq;					 //No of requests
	sem_t *queueSem;			 //to inform threads about requests in queue
};

//threadpool
		struct tpool {
			pthread_t* tid;		//thread ID
			int nthreads;		//no of threads requested, default=4;
			struct trque reqqueue; //assign queue to pool
		};

		typedef struct logging {
			int sock_fd;				//socket file descriptor of clients
			char receipt_time[BUFSIZE]; //time request received
			char sched_time[BUFSIZE];	//time request scheduled
			int status;					//status of request returned by server
			size_t size;				//size of file being processed
			char method[256];			//first line of request
			struct in_addr client_address; //address of client
		}log;
map<int, log>log_data;	//map to keep track of logs of different requests, indexed on client fd

FILE *log_file_fd = NULL;
//To get output of system commands
string exec(char*cmd)
{
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
//Print log in file or terminal
void log_status(int index) {
	char logs[1024];
	static int lcount = 0;
	if (lcount == 0) {
		log_file_fd = fopen(log_file, "w");
		lcount++;
	} else
		log_file_fd = fopen(log_file, "a");
	if (!log_file_fd) {
		if (debug == 0)
			syslog(LOG_ERR, "Error in opening log file");
		else
			perror("Error in opening log file");
	}
	sprintf(logs, "%s %s %s %s %d %zu\n",
			inet_ntoa(log_data[index].client_address),
			log_data[index].receipt_time, log_data[index].sched_time,
			log_data[index].method, log_data[index].status,
			log_data[index].size);
	if (debug == 1) {
		printf("%s", logs);
	} else
		fprintf(log_file_fd, "%s", logs);

	fclose(log_file_fd);
}

void* handlereq(void *th);
//get the request to be scheduled
struct request getlast(struct tpool* t) {
	request r;
	int index = t->reqqueue.nreq - 1;
	if (sch == 1) {
		r = t->reqqueue.fcfs[index];
		t->reqqueue.fcfs.erase(t->reqqueue.fcfs.begin() + index);
	} else {
		r = (*t->reqqueue.sjf.begin()).second;
		t->reqqueue.sjf.erase(t->reqqueue.sjf.begin());
	}
	t->reqqueue.nreq--;
	return r;
}
//delete request from queue
void delqueue(struct tpool* t) {
	t->reqqueue.fcfs.clear();
	t->reqqueue.sjf.clear();
}
//process the request and produce required output
void *protocol(struct request r) {
	int client_s = r.clientfd; //copy socket
	char ibuf[BUFSIZE]; // for GET request
	char obuf[BUF_LEN]; // for HTML response
	char dirname[BUFSIZE]; // for directory name
	char *fname = r.fname;
	int fd;
	int buffile;
	char line[256];
	char *ptr, lsbuf[BUF_LEN];
	int status;
	struct stat st_buf,dir_buf;
	time_t curt = time(NULL);
	char *SERVER = "Apache";
	//scheduling time
	strcpy(log_data[(int) r.clientfd].sched_time, asctime(gmtime(&curt)));
	status = stat(r.fname, &st_buf);
	char timeStr[100];
	time_t ltime;
	char datebuf[9];
	char timebuf[9];
	//modified time of file
	strftime(timeStr, 100, "%d-%m-%Y %H:%M:%S", localtime(&st_buf.st_mtime));
	if (status != 0) { //not a file or directory
		if (debug == 0)
			syslog(LOG_ERR, "Error, not an existing file or directory");
		else
			printf("Error, not an existing file or directory!");
		log_data[r.clientfd].status = 404; //save status
		sprintf(
				obuf,
				"%s Date:%s SERVER:%s\n Last Modified:%s\n %s Content-Length:%d\n\n",
				HTTP_NOTOK, asctime(gmtime(&curt)), SERVER, timeStr, HTML,
				r.size);//header creation
		send(client_s, obuf, strlen(obuf), 0);//send header
		strcpy(obuf, DFNF_404);
		send(client_s, obuf, strlen(obuf), 0);//send webpage
	} else if (r.rtype == 0) {//wrong request type
		if (debug == 0)
			syslog(LOG_ERR, "Error in Request type or Protocol not supported");
		else
			printf("Error in Request type or Protocol not supported!");
	} else if (r.rtype == 1) {//HEAD request
		fd = open(&r.fname[0], O_RDONLY, S_IREAD | S_IWRITE);
		if (fd == -1) {//File not found
			if ((strstr(r.fname, ".jpg") != NULL)
					|| (strstr(r.fname, ".gif") != NULL)
					|| (strstr(r.fname, ".png") != NULL)
					|| (strstr(r.fname, ".html") != NULL)) {
				sprintf(
						obuf,
						"%s Date:%s SERVER:%s\n Last Modified:%s\n %s Content-Length:%d\n\n",
						HTTP_NOTOK, asctime(gmtime(&curt)), SERVER, timeStr,
						HTML, r.size);
				send(client_s, obuf, strlen(obuf), 0);
				log_data[r.clientfd].status = 404;
			}
		} else {// File found, header sent according to image or html
			log_data[r.clientfd].status = 200;
			if ((strstr(r.fname, ".jpg") != NULL)
					|| (strstr(r.fname, ".gif") != NULL)
					|| (strstr(r.fname, ".png") != NULL))
				sprintf(
						obuf,
						"%s Date:%s SERVER:%s\n Last Modified:%s\n %s Content-Length:%d\n\n",
						HTTP_OK, asctime(gmtime(&curt)), SERVER, timeStr, IMAGE,
						r.size);
			else
				sprintf(
						obuf,
						"%s Date:%s SERVER:%s\n Last Modified:%s\n %s Content-Length:%d\n\n",
						HTTP_OK, asctime(gmtime(&curt)), SERVER, timeStr, HTML,
						r.size);
			send(client_s, obuf, strlen(obuf), 0);
		}
		close(fd);
	} else if (r.rtype == 2) {//GET request,Send the requested file to client
		if (S_ISREG (st_buf.st_mode)) {
			fd = open(&r.fname[0], O_RDONLY, S_IREAD | S_IWRITE);
			if (fd == -1) {//File not found
				if ((strstr(r.fname, ".jpg") != NULL)
						|| (strstr(r.fname, ".gif") != NULL)
						|| (strstr(r.fname, ".png") != NULL)
						|| (strstr(r.fname, ".html") != NULL)) {
					if (debug == 0)
						syslog(LOG_ERR, "File %s not found", r.fname);
					else
						printf("File %s not found\n", r.fname);
					log_data[r.clientfd].status = 404;
					sprintf(
							obuf,
							"%s Date:%s SERVER:%s\n Last Modified:%s\n %s Content-Length:%d\n\n",
							HTTP_NOTOK, asctime(gmtime(&curt)), SERVER, timeStr,
							HTML, r.size);
					send(client_s, obuf, strlen(obuf), 0);
					strcpy(obuf, FNF_404);
					send(client_s, obuf, strlen(obuf), 0);
				}
			} else {// File found, file sent according to image or html
				log_data[r.clientfd].status = 200;
				if ((strstr(r.fname, ".jpg") != NULL)
						|| (strstr(r.fname, ".gif") != NULL)
						|| (strstr(r.fname, ".png") != NULL))
					sprintf(
							obuf,
							"%s Date:%s SERVER:%s\n Last Modified:%s\n %s Content-Length:%d\n\n",
							HTTP_OK, asctime(gmtime(&curt)), SERVER, timeStr,
							IMAGE, r.size);
				else
					sprintf(
							obuf,
							"%s Date:%s SERVER:%s\n Last Modified:%s\n %s Content-Length:%d\n\n",
							HTTP_OK, asctime(gmtime(&curt)), SERVER, timeStr,
							HTML, r.size);
				send(client_s, obuf, strlen(obuf), 0);
				buffile = 1;
				while (buffile > 0) {
					buffile = read(fd, obuf, BUFSIZE);
					if (buffile > 0) {
						send(client_s, obuf, buffile, 0);
					}
				}
			}
		} else if (S_ISDIR (st_buf.st_mode)) {//Is Directory
			DIR *dp;
			int flag = 0;
			struct dirent *dirp;
			strcat(r.fname,"/");
			if ((dp = opendir(r.fname)) == NULL) {//directory doesn't exist
				if (debug == 0)
					syslog(LOG_ERR, "Error in opening %s", r.fname);
				else
					printf("Error in opening %s\n", r.fname);
				log_data[r.clientfd].status = 404;
				sprintf(
						obuf,
						"%s Date:%s SERVER:%s\n Last Modified:%s\n %s Content-Length:%d\n\n",
						HTTP_NOTOK, asctime(gmtime(&curt)), SERVER, timeStr,
						HTML, r.size);
				send(client_s, obuf, strlen(obuf), 0);
				strcpy(obuf, DNF_404);
				send(client_s, obuf, strlen(obuf), 0);
			}
			while ((dirp = readdir(dp)) != NULL) {//check for index.html
				if (strcasecmp(dirp->d_name, "index.html") == 0||strcasecmp(dirp->d_name, "index.htm") == 0) {//chec
					flag = 1;
					sprintf(obuf, "%s%s", fname, dirp->d_name);
					log_data[r.clientfd].status = 200;
					fd = open(&obuf[0], O_RDONLY, S_IREAD | S_IWRITE);
					sprintf(
							obuf,
							"%s Date:%s SERVER:%s\n Last Modified:%s\n %s Content-Length:%d\n\n",
							HTTP_OK, asctime(gmtime(&curt)), SERVER, timeStr,
							HTML, r.size);
					send(client_s, obuf, strlen(obuf), 0);
					buffile = 1;
					while (buffile > 0) {
						buffile = read(fd, obuf, BUFSIZE);
						if (buffile > 0) {
							send(client_s, obuf, buffile, 0);
						}
					}
					break;
				}
			}
			if (flag != 1) {//display directory contents
				if ((dp = opendir(r.fname)) == NULL) {
					if (debug == 0)
						syslog(LOG_ERR, "Error in opening %s", r.fname);
					else
						printf("Error in opening %s", r.fname);
					log_data[r.clientfd].status = 404;
					sprintf(
							obuf,
							"%s Date:%s SERVER:%s\n Last Modified:%s\n %s Content-Length:%d\n\n",
							HTTP_NOTOK, asctime(gmtime(&curt)), SERVER, timeStr,
							HTML, r.size);
					send(client_s, obuf, strlen(obuf), 0);
					strcpy(obuf, DNF_404);
					send(client_s, obuf, strlen(obuf), 0);
				}
				sprintf(
						obuf,
						"%s Date:%s SERVER:%s\n Last Modified:%s\n %s Content-Length:%d\n\n",
						HTTP_OK, asctime(gmtime(&curt)), SERVER, timeStr, HTML,
						r.size);
				send(client_s, obuf, strlen(obuf), 0);
				strcpy(obuf, "<html><body> <h1>Directory Contents</h1><br><h2>Name&emsp;Last Modified Time</h2><br>");
				send(client_s, obuf, strlen(obuf), 0);
				obuf[0] = '\0';
				char temp[BUFSIZE],*found2,uptemp[BUFSIZE];
				int tpos,i;
				temp[0]='\0';
				//get parent directory
				strncpy(temp,r.fname,strlen(fname)-3);
				found2 = strrchr(temp, '/');
				if (found2!=NULL) {
					tpos = found2 - temp;
					for (i = 0; i<tpos; i++)
						uptemp[i] = temp[i];
					uptemp[i]='\0';
					sprintf(updir,"%s",uptemp);
				}
				sprintf(obuf, "<a href=%s><b><i>Parent Directory</b></i></a><br>", updir);
				send(client_s, obuf, strlen(obuf), 0);
				obuf[0] = '\0';
				//display all the contents
				while ((dirp = readdir(dp)) != NULL) {
					if (dirp->d_name[0] != '.') {
						sprintf(dirname,"%s%s",r.fname,dirp->d_name);
						stat(dirname, &dir_buf);
						strftime(timeStr, 100, "%d-%m-%Y %H:%M:%S", localtime(&dir_buf.st_atime));
						sprintf(obuf, "<a href=%s%s>%s</a>&emsp;&emsp;&emsp;%s&emsp;<br>",r.fname,
								dirp->d_name, dirp->d_name,timeStr);
						send(client_s, obuf, strlen(obuf), 0);
						obuf[0] = '\0';
					}
				}
				sprintf(obuf, "</body></html>");
				send(client_s, obuf, strlen(obuf), 0);
				closedir(dp);
			}
		} else {
			if (debug == 0)
				syslog(LOG_ERR, "Error in opening %s", r.fname);
			else
				printf("Error in opening %s\n", r.fname);
			log_data[r.clientfd].status = 404;
			sprintf(
					obuf,
					"%s Date:%s SERVER:%s\n Last Modified:%s\n %s Content-Length:%d\n\n",
					HTTP_NOTOK, asctime(gmtime(&curt)), SERVER, timeStr, HTML,
					r.size);
			send(client_s, obuf, strlen(obuf), 0);
			strcpy(obuf, DNF_404);
			send(client_s, obuf, strlen(obuf), 0);
		}
		close(fd);
	}
	log_status(r.clientfd);
	close(client_s);
}
//Thread pool creation
struct tpool *poolinit() {
	struct tpool *t;
	t = (struct tpool*) malloc(sizeof(struct tpool));
	t->tid = (pthread_t*) malloc(nthreads * sizeof(pthread_t));
	if (t->tid == NULL) {
		if (debug == 0)
			syslog(LOG_ERR, "Memory allocation for thread ISs error");
		else
			printf("Memory allocation for thread IDs error\n");
		return NULL;
	}
	t->nthreads = nthreads;

	// Initialise the request queue
	t->reqqueue.nreq = 0;
	t->reqqueue.queueSem = (sem_t*) malloc(sizeof(sem_t));
	sem_init(t->reqqueue.queueSem, 0, 0);
	// Create threads in pool
	int i;
	for (i = 0; i < nthreads; i++) {
		if (debug == 0)
			syslog(LOG_NOTICE, "Created thread %d in pool \n", i);
		else
			printf("Created thread %d in pool \n", i);
		pthread_create(&(t->tid[i]), NULL, handlereq, (void *) t);
	}
	return t;
}
//assign requests to queue
void* handlereq(void *th) {
	struct tpool*t = (struct tpool *) th;
	while (execution) {
		if (quetime) {
			sleep(quetime);
			quetime = 0;
		}
		if (sem_wait(t->reqqueue.queueSem)) //waiting for a request
		{
			if (debug == 0)
				syslog(LOG_ERR, "Error in semaphore wait");
			else
				perror("Error in semaphore wait");
			exit(1);
		}
		if (execution) // Read and handle request from queue
		{
			struct request r;
			pthread_mutex_lock(&get_mutex); // get mutex lock
			r = getlast(t);
			log_data[r.clientfd].sock_fd = r.clientfd;
			pthread_mutex_unlock(&get_mutex); //release mutex
			protocol(r); //execute function
			if (debug == 1)
				pthread_mutex_unlock(&debug_mutex);
		}
		else {
			break;
		}
	}

	pthread_exit(NULL);
}
//add requests to queue
int quereq(struct tpool* t, struct request r, int size) {
	pthread_mutex_lock(&get_mutex);
	//sch:1->FCFS,2->SJF
	if (sch == 1) {
		t->reqqueue.fcfs.push_back(r);
	} else {
		t->reqqueue.sjf[size] = r;
	}
	t->reqqueue.nreq++;
	sem_post(t->reqqueue.queueSem);//keeps track of no requests in queue
	int sval;
	sem_getvalue(t->reqqueue.queueSem, &sval);
	pthread_mutex_unlock(&get_mutex);

	return 0;
}
//destroy the pool
void delpool(struct tpool* t) {
	int i;
	execution = 0; //end thread's infinite loop
	for (i = 0; i < (t->nthreads); i++) //idle threads waiting at semaphore
			{
		if (sem_post(t->reqqueue.queueSem)) {
			if (debug == 0)
				syslog(LOG_ERR, "Error in sem_post");
			else
				printf("Error in sem_post()\n");
		}
	}

	if (sem_destroy(t->reqqueue.queueSem) != 0) {
		if (debug == 0)
			syslog(LOG_ERR, "Error in destroying semaphore");
		else
			printf("Error in destroying semaphore\n");
	}

	for (i = 0; i < (t->nthreads); i++) //join so all threads finish before exit
	{
		pthread_join(t->tid[i], NULL);
	}

	delqueue(t);

	free(t->tid);
	free(t->reqqueue.queueSem);
	free(t);
}

int main(int argc, char *argv[]) {
	fd_set ready;
	struct sockaddr_in msgfrom;
	int msgsize, i;
	struct tpool *threadpool;

	log_file[0] = '\0';
	//syslog
	openlog("myhttpd", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL0);
	union {
		uint32_t addr;
		char bytes[4];
	} fromaddr;

	if ((progname = rindex(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		progname++;
	server = 1;
	//check various options
	while ((ch = getopt(argc, argv, "dhr:s:n:p:l:t:")) != -1)
		switch (ch) {
		case 'd': //debug mode
			debug = 1;
			dflag = 1;
			break;
		case 'r': //set root directory
			strcpy(rootdir, optarg);
			break;
		case 's': //set scheduling policy
			if (strcmp(optarg, "FCFS"))
				sch = 1;
			else if (strcmp(optarg, "SJF"))
				sch = 2;
			break;
		case 'n': //no of threads
			nthreads = atoi(optarg);
			break;
		case 'p': //set port no
			port = optarg;
			break;
		case 'l': //address of log file
			strncpy(log_file, optarg, sizeof(log_file));
			break;
		case 't': //queuing time
			quetime = atoi(optarg);
			break;
		case 'h': //help
		default:
			usage();
			break;
		}
	int derr;
	argc -= optind;
	if (argc != 0)
		usage();
	if (!server && port == NULL)
		usage();
	//creation of daemon process
	if (debug == 0) {
		if(fork() > 0 ){
			//Parent killed
			exit(1);
		}
		//Child Running
		setsid();
		chdir(rootdir);
		umask(0);
	}

	// Create socket on local host
	if ((s = socket(AF_INET, soctype, 0)) < 0) {
		if (debug == 0)
			syslog(LOG_ERR, "Error in socket creation!");
		else
			perror("socket");
		exit(1);
	}
	if (server) {
		size_t stacksize;
		pthread_t listhread, schque;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		stacksize = 500000;
		pthread_attr_setstacksize(&attr, stacksize);
		pthread_attr_getstacksize(&attr, &stacksize);
		int f = 1;
		pthread_create(&listhread, &attr, setup_server, (void*) f); //Listening thread
		threadpool = poolinit();
		pthread_create(&schque, &attr, queue, (void *) threadpool); //Queuing thread
		pthread_attr_destroy(&attr);
		pthread_join(listhread, NULL);
		pthread_join(schque, NULL);
	}
	//closelog();
	fclose(log_file_fd);
	return (0);
}

void *queue(void *tp) {
	static int top = 1;
	struct tpool *thread_pool = (struct tpool *) tp;
	char ibuf[BUFSIZE];
	char fname[BUFSIZE], temp[30];
	int buffile;
	int retcode;
	char action[20], path[100], host[20];
	string dir = exec("pwd");
	string ret = "\n";
	struct request r;
	dir.erase(dir.find(ret));
	char* home = "home";
	char* found;
	int size = 0;
	FILE *fd;
	int status;
	struct stat st_buf;
	char *tilda,username[50];
	int tpos, i = 0;
	while (1) {
		if (newsock == 1) {
			newsock = 0;
			retcode = recv((int) clsock, ibuf, BUFSIZE, 0); //HTTP request
			sscanf(ibuf, "%s %s %s", action, fname, host);
			int i = 0;
			sprintf(log_data[(int) clsock].method, "%s %s %s", action, fname,
					host);
			if (retcode < 0) {
				if (debug == 0)
					syslog(LOG_ERR, "Error in receive!");
				else
					printf("Error in receive!\n");
			} else {//get the file name
				//handle tilda
				tilda = strchr(fname, '~');
				if (tilda != NULL) {
					tpos = tilda - fname + 1;
					for (i = 0; fname[tpos] != '/'; i++, tpos++)
						username[i] = fname[tpos];
					username[i] = '\0';
					fname[0] = '\0';
					sprintf(updir,"/home/%s/",username);
					sprintf(fname, "/home/%s/myhttpd/", username);
				} else { //get file or directory to open
					found = strstr(fname, home);
					if (!found) {
						strcpy(temp, fname);
						fname[0] = '\0';
						if (*rootdir == '\0')
							sprintf(rootdir, "%s", (char*) dir.c_str());
						sprintf(fname, "%s%s", rootdir, temp);
					}
				}
				//get file size
				status = stat(fname, &st_buf);
				if (status != 0) {
					if (debug == 0)
						syslog(LOG_ERR,
								"Error, not an existing file or directory");
					else
						printf("Error, not an existing file or directory");
					size = 0;
				} else {
					if (S_ISREG (st_buf.st_mode)) {
						fd = fopen(fname, "rb");
						fseek(fd, 0, SEEK_END);
						size = ftell(fd);
						fclose(fd);
					} else if (S_ISDIR (st_buf.st_mode))
						size = 0;
					else
						size = 0;
				}
				log_data[(int) clsock].size = size;
			}
			r.clientfd = (int) clsock;
			strcpy(r.fname, fname);
			r.size = size;
			if (strcasecmp(host, "HTTP/1.0") == 0
					|| strcasecmp(host, "HTTP/1.1") == 0) {
				if (strcasecmp(action, "HEAD") == 0)
					r.rtype = 1;
				else if (strcasecmp(action, "GET") == 0)
					r.rtype = 2;
				else
					r.rtype = 0;
			} else
				r.rtype = 0;
			//queue the request
			quereq(thread_pool, r, size);
		}
	}
	execution = 0;
	delpool(thread_pool);
	pthread_exit(NULL);
}

// setup_server() - set up socket for mode of soc running as a server

void *setup_server(void *f) {
	struct sockaddr_in serv, remote, addr;
	struct servent *se;
	socklen_t len;
	int rval;
	socklen_t address_length;
	struct sockaddr socket_address;

	len = sizeof(remote);
	memset((void *) &serv, 0, sizeof(serv));
	serv.sin_family = AF_INET;
	if (port == NULL)
		serv.sin_port = htons(0);
	else if (isdigit(*port))
		serv.sin_port = htons(atoi(port));
	else {
		if ((se = getservbyname(port, (char *) NULL)) < (struct servent *) 0) {
			if (debug == 0)
				syslog(LOG_ERR, "Error in port");
			else
				perror(port);
			exit(1);
		}
		serv.sin_port = se->s_port;
	}
	if (bind(s, (struct sockaddr *) &serv, sizeof(serv)) < 0) {
		if (debug == 0)
			syslog(LOG_ERR, "Error in binding");
		else
			perror("bind");
		exit(1);
	}
	if (getsockname(s, (struct sockaddr *) &remote, &len) < 0) {
		if (debug == 0)
			syslog(LOG_ERR, "Error in getsockname");
		else
			perror("getsockname");
		exit(1);
	}
	if (debug == 0)
		syslog(LOG_NOTICE, "Port number is %d\n", ntohs(remote.sin_port));
	else
		printf("Port number is %d\n", ntohs(remote.sin_port));
	while (1) {
		listen(s, nthreads);
		if (debug == 1)
			pthread_mutex_lock(&debug_mutex);
		if (newsock == 0) {
			if (soctype == SOCK_STREAM) {
				if (debug == 0)
					syslog(LOG_NOTICE,"Entering accept() waiting for connection.");
				else
					printf("Entering accept() waiting for connection.\n");
				clsock = accept(s, (struct sockaddr *) &remote, &len);
				if (clsock != -1)
					newsock = 1;
				address_length = sizeof(socket_address);
				rval = getpeername((int) clsock, &socket_address,
						&address_length);
				assert(rval == 0);
				memcpy(&addr, &socket_address, sizeof(socket_address));
				log_data[(int) clsock].client_address = addr.sin_addr;
				time_t curt = time(NULL);
				strcpy(log_data[(int) clsock].receipt_time,
						asctime(gmtime(&curt)));
			}
		}
	}
	close(s);
	pthread_exit(NULL);
}

//print usage string and exit

void usage() {
	fprintf(
			stderr,
			"usage:myhttpd [−d] [−h] [−l file] [−p port] [−r dir] [−t time] [−n threadnum] [−s sched]\n",
			progname);
	fprintf(
			stderr,
			"−d : Enter debugging mode. That is, do not daemonize, only accept one connection at a time and enable logging to stdout. Without this option, the web server should run as a daemon process in the background.\n"
					"−h        : Print a usage summary with all options and exit.\n"
					"−l file   : Log all requests to the given file. See LOGGING for details.\n"
					"−p port   : Listen on the given port. If not provided, myhttpd will listen on port 8080.\n"
					"−r dir    : Set the root directory for the http server to dir.\n"
					"−t time   : Set the queuing time to time seconds. The default should be 60 seconds.\n"
					"−n threadnum: Set number of threads waiting ready in the execution thread pool to threadnum. The default should be 4 execution threads.\n"
					"−s sched  : Set the scheduling policy. It can be either FCFS or SJF. The default will be FCFS.\n",
			progname);
	exit(1);
}
