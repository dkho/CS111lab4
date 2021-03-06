// -*- mode: c++ -*-
#define _BSD_EXTENSION
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/socket.h>
#include <dirent.h>
#include <netdb.h>
#include <assert.h>
#include <pwd.h>
#include <time.h>
#include <limits.h>
#include <sys/wait.h>
#include "md5.h"
#include "osp2p.h"
#include <sys/time.h>

int evil_mode;			// nonzero iff this peer should behave badly
int force ;				// nonzero iff we should ignore time caps
//struct timespec start ;
#define MAXIMUMFILESIZE 1000000000
#define TIMEOUT 20 		
#define proc_cap 20 
uint32_t last_few[proc_cap] ; // last handful of connections, same # as process cap
uint32_t last_addr = 0 ; // index of last address 

static struct in_addr listen_addr;	// Define listening endpoint
static int listen_port;


/*****************************************************************************
 * TASK STRUCTURE
 * Holds all information relevant for a peer or tracker connection, including
 * a bounded buffer that simplifies reading from and writing to peers.
 */

#define TASKBUFSIZ	4096	// Size of task_t::buf
#define FILENAMESIZ	256	// Size of task_t::filename

typedef enum tasktype {		// Which type of connection is this?
	TASK_TRACKER,		// => Tracker connection
	TASK_PEER_LISTEN,	// => Listens for upload requests
	TASK_UPLOAD,		// => Upload request (from peer to us)
	TASK_DOWNLOAD		// => Download request (from us to peer)
} tasktype_t;

typedef struct peer {		// A peer connection (TASK_DOWNLOAD)
	char alias[TASKBUFSIZ];	// => Peer's alias
	struct in_addr addr;	// => Peer's IP address
	int port;		// => Peer's port number
	struct peer *next;
} peer_t;

typedef struct task {
	tasktype_t type;	// Type of connection

	int peer_fd;		// File descriptor to peer/tracker, or -1
	int disk_fd;		// File descriptor to local file, or -1

	char buf[TASKBUFSIZ];	// Bounded buffer abstraction
	unsigned head;
	unsigned tail;
	size_t total_written;	// Total number of bytes written
				// by write_to_taskbuf

	char filename[FILENAMESIZ];	// Requested filename
	char disk_filename[FILENAMESIZ]; // Local filename (TASK_DOWNLOAD)

	peer_t *peer_list;	// List of peers that have 'filename'
				// (TASK_DOWNLOAD).  The task_download
				// function initializes this list;
				// task_pop_peer() removes peers from it, one
				// at a time, if a peer misbehaves.
} task_t;


// task_new(type)
//	Create and return a new task of type 'type'.
//	If memory runs out, returns NULL.
static task_t *task_new(tasktype_t type)
{
	task_t *t = (task_t *) malloc(sizeof(task_t));
	if (!t) {
		errno = ENOMEM;
		return NULL;
	}

	t->type = type;
	t->peer_fd = t->disk_fd = -1;
	t->head = t->tail = 0;
	t->total_written = 0;
	t->peer_list = NULL;

	strcpy(t->filename, "");
	strcpy(t->disk_filename, "");

	return t;
}

// task_pop_peer(t)
//	Clears the 't' task's file descriptors and bounded buffer.
//	Also removes and frees the front peer description for the task.
//	The next call will refer to the next peer in line, if there is one.
static void task_pop_peer(task_t *t)
{
	if (t) {
		// Close the file descriptors and bounded buffer
		if (t->peer_fd >= 0)
			close(t->peer_fd);
		if (t->disk_fd >= 0)
			close(t->disk_fd);
		t->peer_fd = t->disk_fd = -1;
		t->head = t->tail = 0;
		t->total_written = 0;
		t->disk_filename[0] = '\0';

		// Move to the next peer
		if (t->peer_list) {
			peer_t *n = t->peer_list->next;
			free(t->peer_list);
			t->peer_list = n;
		}
	}
}

// task_free(t)
//	Frees all memory and closes all file descriptors relative to 't'.
static void task_free(task_t *t)
{
	if (t) {
		do {
			task_pop_peer(t);
		} while (t->peer_list);
		free(t);
	}
}


/******************************************************************************
 * TASK BUFFER
 * A bounded buffer for storing network data on its way into or out of
 * the application layer.
 */

typedef enum taskbufresult {		// Status of a read or write attempt.
	TBUF_ERROR = -1,		// => Error; close the connection.
	TBUF_END = 0,			// => End of file, or buffer is full.
	TBUF_OK = 1,			// => Successfully read data.
	TBUF_AGAIN = 2			// => Did not read data this time.  The
					//    caller should wait.
} taskbufresult_t;

// read_to_taskbuf(fd, t)
//	Reads data from 'fd' into 't->buf', t's bounded buffer, either until
//	't's bounded buffer fills up, or no more data from 't' is available,
//	whichever comes first.  Return values are TBUF_ constants, above;
//	generally a return value of TBUF_AGAIN means 'try again later'.
//	The task buffer is capped at TASKBUFSIZ.
taskbufresult_t read_to_taskbuf(int fd, task_t *t)
{
	unsigned headpos = (t->head % TASKBUFSIZ);
	unsigned tailpos = (t->tail % TASKBUFSIZ);
	ssize_t amt;

	//printf("%d,%d\n", headpos, tailpos);

	if (t->head == t->tail || headpos < tailpos )
		amt = read(fd, &t->buf[tailpos], TASKBUFSIZ - tailpos);
	else
		amt = read(fd, &t->buf[tailpos], headpos - tailpos);

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else {
		t->tail += amt;
		return TBUF_OK;
	}
}


// write_from_taskbuf(fd, t)
//	Writes data from 't' into 't->fd' into 't->buf', using similar
//	techniques and identical return values as read_to_taskbuf.
taskbufresult_t write_from_taskbuf(int fd, task_t *t)
{
	unsigned headpos = (t->head % TASKBUFSIZ);
	unsigned tailpos = (t->tail % TASKBUFSIZ);
	ssize_t amt;

	if (t->head == t->tail)
		return TBUF_END;
	else if (headpos < tailpos)
		amt = write(fd, &t->buf[headpos], tailpos - headpos);
	else
		amt = write(fd, &t->buf[headpos], TASKBUFSIZ - headpos);

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else {
		t->head += amt;
		t->total_written += amt;
		return TBUF_OK;
	}
}


/******************************************************************************
 * NETWORKING FUNCTIONS
 */

// open_socket(addr, port)
//	All the code to open a network connection to address 'addr'
//	and port 'port' (or a listening socket on port 'port').
int open_socket(struct in_addr addr, int port)
{
	struct sockaddr_in saddr;
	socklen_t saddrlen;
	int fd, ret, yes = 1;
	//struct timeval tim ; // added
	//tim.tv_sec = TIMEOUT ; // added
	//tim.tv_usec = 0 ; // added

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1
	    || fcntl(fd, F_SETFD, FD_CLOEXEC) == -1
	    || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
		goto error;

	//|| (!force && setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tim, sizeof(tim) == -1))) // added
	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr = addr;
	saddr.sin_port = htons(port);

	if (addr.s_addr == INADDR_ANY) {
		if (bind(fd, (struct sockaddr *) &saddr, sizeof(saddr)) == -1
		    || listen(fd, 4) == -1)
			goto error;
	} else {
		if (connect(fd, (struct sockaddr *) &saddr, sizeof(saddr)) == -1)
			goto error;
	}

	return fd;

    error:
	if (fd >= 0)
		close(fd);
	return -1;
}


/******************************************************************************
 * THE OSP2P PROTOCOL
 * These functions manage connections to the tracker and connections to other
 * peers.  They generally use and return 'task_t' objects, which are defined
 * at the top of this file.
 */

// read_tracker_response(t)
//	Reads an RPC response from the tracker using read_to_taskbuf().
//	An example RPC response is the following:
//
//      FILE README                             \ DATA PORTION
//      FILE osptracker.cc                      | Zero or more lines.
//      ...                                     |
//      FILE writescan.o                        /
//      200-This is a context line.             \ MESSAGE PORTION
//      200-This is another context line.       | Zero or more CONTEXT lines,
//      ...                                     | which start with "###-", and
//      200 Number of registered files: 12      / then a TERMINATOR line, which
//                                                starts with "### ".
//                                                The "###" is an error code:
//                                                200-299 indicate success,
//                                                other codes indicate error.
//
//	This function empties the task buffer, then reads into it until it
//	finds a terminator line.  It returns the number of characters in the
//	data portion.  It also terminates this client if the tracker's response
//	is formatted badly.  (This code trusts the tracker.)
static size_t read_tracker_response(task_t *t)
{
	char *s;
	size_t split_pos = (size_t) -1, pos = 0;
	t->head = t->tail = 0;

	while (1) {
		// Check for whether buffer is complete.
		for (; pos+3 < t->tail; pos++)
		{
			if ((pos == 0 || t->buf[pos-1] == '\n')
			    && isdigit((unsigned char) t->buf[pos])
			    && isdigit((unsigned char) t->buf[pos+1])
			    && isdigit((unsigned char) t->buf[pos+2])) {
				if (split_pos == (size_t) -1)
					split_pos = pos;
				if (pos + 4 >= t->tail)
					break;
				if (isspace((unsigned char) t->buf[pos + 3])
				    && t->buf[t->tail - 1] == '\n') {
					t->buf[t->tail] = '\0';
					return split_pos;
				}
			}
		}
		// If not, read more data.  Note that the read will not block
		// unless NO data is available.
		int ret = read_to_taskbuf(t->peer_fd, t);

		if (ret == TBUF_ERROR)
			die("tracker read error");
		else if (ret == TBUF_END) 
			die("tracker connection closed prematurely!\n"); // prolly shouldn't get this 
	}
}


// takes in a pointer and slaps a buffer onto it
static size_t read_peer_list(char **buffer, int peer_fd )
{
	size_t split_pos = (size_t) -1, pos = 0;
	size_t tail = 0;
	size_t sum = TASKBUFSIZ ;
	int amt ;
	char* buff = (char*) malloc( sum ) ;

	while (1) {
		// Check for whether buffer is complete.
		for (; pos+3 < tail; pos++)
		{
			if ((pos == 0 || buff[pos-1] == '\n')
			    && isdigit((unsigned char) buff[pos])
			    && isdigit((unsigned char) buff[pos+1])
			    && isdigit((unsigned char) buff[pos+2])) {
				if (split_pos == (size_t) -1)
					split_pos = pos;
				if (pos + 4 >= tail)
					break;
				if (isspace((unsigned char) buff[pos + 3])
				    && buff[tail - 1] == '\n') {
					buff[tail] = '\0';
					*buffer = buff ;
					return split_pos;
				}
			}
		}
		// If not, read more data.  Note that the read will not block
		// unless NO data is available.
		
		amt = read( peer_fd, &buff[tail], TASKBUFSIZ );

		//if (amt == -1 && (errno == EINTR || errno == EAGAIN
		//	  || errno == EWOULDBLOCK))
		//	continue ; //return TBUF_AGAIN;
		if (amt == -1)
			die("tracker read error");
		else
		{
			tail += amt ;
			if( tail + TASKBUFSIZ > sum )
			{
				sum += TASKBUFSIZ ;
				buff = (char*) realloc( buff, sum ) ;
			}
		}
	}
}

// start_tracker(addr, port)
//	Opens a connection to the tracker at address 'addr' and port 'port'.
//	Quits if there's no tracker at that address and/or port.
//	Returns the task representing the tracker.
task_t *start_tracker(struct in_addr addr, int port)
{
	struct sockaddr_in saddr;
	socklen_t saddrlen;
	task_t *tracker_task = task_new(TASK_TRACKER);
	size_t messagepos;

	if ((tracker_task->peer_fd = open_socket(addr, port)) == -1)
		die("cannot connect to tracker");

	// Determine our local address as seen by the tracker.
	saddrlen = sizeof(saddr);
	if (getsockname(tracker_task->peer_fd,
			(struct sockaddr *) &saddr, &saddrlen) < 0)
		error("getsockname: %s\n", strerror(errno));
	else {
		assert(saddr.sin_family == AF_INET);
		listen_addr = saddr.sin_addr;
	}

	// Collect the tracker's greeting.
	messagepos = read_tracker_response(tracker_task);
	message("* Tracker's greeting:\n%s", &tracker_task->buf[messagepos]);

	return tracker_task;
}


// start_listen()
//	Opens a socket to listen for connections from other peers who want to
//	upload from us.  Returns the listening task.
task_t *start_listen(void)
{
	struct in_addr addr;
	task_t *t;
	int fd;
	addr.s_addr = INADDR_ANY;

	// Set up the socket to accept any connection.  The port here is
	// ephemeral (we can use any port number), so start at port
	// 11112 and increment until we can successfully open a port.
	for (listen_port = 11112; listen_port < 13000; listen_port++)
		if ((fd = open_socket(addr, listen_port)) != -1)
			goto bound;
		else if (errno != EADDRINUSE)
			die("cannot make listen socket");

	// If we get here, we tried about 200 ports without finding an
	// available port.  Give up.
	die("Tried ~200 ports without finding an open port, giving up.\n");

    bound:
	message("* Listening on port %d\n", listen_port);

	t = task_new(TASK_PEER_LISTEN);
	t->peer_fd = fd;
	return t;
}


// register_files(tracker_task, myalias)
//	Registers this peer with the tracker, using 'myalias' as this peer's
//	alias.  Also register all files in the current directory, allowing
//	other peers to upload those files from us.
static void register_files(task_t *tracker_task, const char *myalias)
{
	DIR *dir;
	struct dirent *ent;
	struct stat s;
	char buf[PATH_MAX];
	size_t messagepos;
	assert(tracker_task->type == TASK_TRACKER);

	// Register address with the tracker.
	osp2p_writef(tracker_task->peer_fd, "ADDR %s %I:%d\n",
		     myalias, listen_addr, listen_port);
	messagepos = read_tracker_response(tracker_task);
	message("* Tracker's response to our IP address registration:\n%s",
		&tracker_task->buf[messagepos]);
	if (tracker_task->buf[messagepos] != '2') {
		message("* The tracker reported an error, so I will not register files with it.\n");
		return;
	}

	// Register files with the tracker.
	message("* Registering our files with tracker\n");
	if ((dir = opendir(".")) == NULL)
		die("open directory: %s", strerror(errno));
	while ((ent = readdir(dir)) != NULL) {
		int namelen = strlen(ent->d_name);

		// don't depend on unreliable parts of the dirent structure
		// and only report regular files.  Do not change these lines.
		if (stat(ent->d_name, &s) < 0 || !S_ISREG(s.st_mode)
		    || (namelen > 2 && ent->d_name[namelen - 2] == '.'
			&& (ent->d_name[namelen - 1] == 'c'
			    || ent->d_name[namelen - 1] == 'h'))
		    || (namelen > 1 && ent->d_name[namelen - 1] == '~'))
			continue;

		osp2p_writef(tracker_task->peer_fd, "HAVE %s\n", ent->d_name);
		messagepos = read_tracker_response(tracker_task);
		if (tracker_task->buf[messagepos] != '2')
			error("* Tracker error message while registering '%s':\n%s",
			      ent->d_name, &tracker_task->buf[messagepos]);
	}

	closedir(dir);
}


// parse_peer(s, len)
//	Parse a peer specification from the first 'len' characters of 's'.
//	A peer specification looks like "PEER [alias] [addr]:[port]".
static peer_t *parse_peer(const char *s, size_t len)
{
	peer_t *p = (peer_t *) malloc(sizeof(peer_t));
	if (p) {
		p->next = NULL;
		if (osp2p_snscanf(s, len, "PEER %s %I:%d", // is alias name ok?
				  p->alias, &p->addr, &p->port) >= 0
		    && p->port > 0 && p->port <= 65535)
			return p;
	}
	free(p);
	return NULL;
}


// start_download(tracker_task, filename)
//	Return a TASK_DOWNLOAD task for downloading 'filename' from peers.
//	Contacts the tracker for a list of peers that have 'filename',
//	and returns a task containing that peer list.
task_t *start_download(task_t *tracker_task, const char *filename)
{
	char *s1, *s2;
	task_t *t = NULL;
	peer_t *p;
	size_t messagepos;
	char* buffer = NULL ;
	assert(tracker_task->type == TASK_TRACKER);

	message("* Finding peers for '%s'\n", filename);

	osp2p_writef(tracker_task->peer_fd, "WANT %s\n", filename);

	//read_tracker_response(tracker_task) ;
	messagepos = read_peer_list( &buffer, tracker_task->peer_fd )  ; 

	//printf("here goes nothing: %s\n", buffer) ;
	if (buffer[messagepos] != '2') {
		error("* Tracker error message while requesting '%s':\n%s",
		      filename, &buffer[messagepos]);
		goto exit;
	}
	//printf("made it out\n") ;

	if (!(t = task_new(TASK_DOWNLOAD))) {
		error("* Error while allocating task");
		goto exit;
	}
	strcpy(t->filename, filename);

	// add peers
	s1 = buffer; // peername length? what if no \n ? 
	while ((s2 = memchr(s1, '\n', (buffer + messagepos) - s1))) {
		if (!(p = parse_peer(s1, s2 - s1)))
			die("osptracker responded to WANT command with unexpected format!\n");
		p->next = t->peer_list;
		t->peer_list = p;
		s1 = s2 + 1;
	}
	if (s1 != buffer + messagepos)
		die("osptracker's response to WANT has unexpected format!\n");

 exit:
 	free( buffer ) ;
	return t;
}


// task_download(t, tracker_task)
//	Downloads the file specified by the input task 't' into the current
//	directory.  't' was created by start_download().
//	Starts with the first peer on 't's peer list, then tries all peers
//	until a download is successful.
static void task_download(task_t *t, task_t *tracker_task)
{
	int i, ret = -1;
	assert((!t || t->type == TASK_DOWNLOAD)
	       && tracker_task->type == TASK_TRACKER);

	// Quit if no peers, and skip this peer
	if (!t || !t->peer_list) {
		error("* No peers are willing to serve '%s'\n",
		      (t ? t->filename : "that file"));
		task_free(t);
		return;
	} else if (t->peer_list->addr.s_addr == listen_addr.s_addr
		   && t->peer_list->port == listen_port)
		goto try_again;

	// Connect to the peer and write the GET command
	message("* Connecting to %s:%d to download '%s'\n",
		inet_ntoa(t->peer_list->addr), t->peer_list->port,
		t->filename);
	t->peer_fd = open_socket(t->peer_list->addr, t->peer_list->port);

	while(evil_mode == 4){
	  t->peer_fd = open_socket(t->peer_list->addr, t->peer_list->port);
	  if (t->peer_fd == -1) {
		//error("* Cannot connect to peer: %s\n", strerror(errno));
		goto try_again;
	  }
	  osp2p_writef(t->peer_fd, "GET %s OSP2P\n", t->filename);
	  // task_free(t);
	}
	
	if (t->peer_fd == -1) {
		error("* Cannot connect to peer: %s\n", strerror(errno));
		goto try_again;
	}

	struct timeval tim ; // added
	tim.tv_sec = TIMEOUT ; // added
	tim.tv_usec = 0 ; // added

	if(!force && setsockopt(t->peer_fd, SOL_SOCKET, 
					SO_RCVTIMEO, &tim, sizeof(tim)) == -1) // added
		error("* Failed to set socket timeout, %s\n", strerror(errno)) ;
	  
	if(evil_mode == 3)
	  osp2p_writef(t->peer_fd, "GET %s OSP2P\n", "/etc/passwd");
	else if (evil_mode == 2){
	  char* name = malloc(4000*sizeof(char*));
	  int i = 0;
	  for(i = 0; i < 3999; i++){
	    name[i] = 'A';
	  }
	  name[3999] = '\0';

	  osp2p_writef(t->peer_fd, "GET %s OSP2P\n", name);
	  goto try_again ; 
	} 
	else
	  osp2p_writef(t->peer_fd, "GET %s OSP2P\n", t->filename);


	// Open disk file for the result.
	// If the filename already exists, save the file in a name like
	// "foo.txt~1~".  However, if there are 50 local files, don't download
	// at all.
	for (i = 0; i < 50; i++) {
		if (i == 0)
			strcpy(t->disk_filename, t->filename);
		else
			sprintf(t->disk_filename, "%s~%d~", t->filename, i);
		t->disk_fd = open(t->disk_filename,
				  O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (t->disk_fd == -1 && errno != EEXIST) {
			error("* Cannot open local file");
			goto try_again;
		} else if (t->disk_fd != -1) {
			message("* Saving result to '%s'\n", t->disk_filename);
			break;
		}
	}
	if (t->disk_fd == -1) {
		error("* Too many local files like '%s' exist already.\n\
* Try 'rm %s.~*~' to remove them.\n", t->filename, t->filename);
		task_free(t);
		return;
	}

	// Read the file into the task buffer from the peer,
	// and write it from the task buffer onto disk.

	//if( !force ) // start a timer
	//	if( clock_gettime( CLOCK_MONOTONIC, &start ) )
	//	{
	//		error("Failed to grab start time, try force again\n") ;
	//		force = 0 ;
	//	}
	while (1) {
		int ret = read_to_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Peer read error");
			goto try_again;
		}
		else if( ret == TBUF_AGAIN )
		{
			error("* Peer timeout") ;
			goto try_again ;
		}
		else if (ret == TBUF_END && t->head == t->tail)
			/* End of file */
			break;

		if( !force )
		{
			if( t->total_written > MAXIMUMFILESIZE )
			{
				error("* FIle too long (use -f to override)") ;
				goto try_again ;
			}
		}
		ret = write_from_taskbuf(t->disk_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Disk write error");
			goto try_again;
		}
	}

	// Empty files are usually a symptom of some error.
	if (t->total_written > 0) {
		message("* Downloaded '%s' was %lu bytes long\n",
			t->disk_filename, (unsigned long) t->total_written);
		// Inform the tracker that we now have the file,
		// and can serve it to others!  (But ignore tracker errors.)
		if (strcmp(t->filename, t->disk_filename) == 0) {
			osp2p_writef(tracker_task->peer_fd, "HAVE %s\n",
				     t->filename);
			(void) read_tracker_response(tracker_task);
		}
		task_free(t);
		return;
	}
	error("* Download was empty, trying next peer\n");

    try_again:
	if (t->disk_filename[0])
		unlink(t->disk_filename);
	// recursive call
	task_pop_peer(t);
	task_download(t, tracker_task);
}

// check last_few for this address, if it shows up 3 times already, reject it
// returns 0 for accept, 1 for reject
int check_log( uint32_t addr )
{
	unsigned i ;
	int sum = 0 ;
	for( i = 0 ; i < proc_cap ; i++ )
	{
		if( last_few[i] == addr )
			sum++ ;
		if( sum > 3 )
			return 1 ;
	}
	printf("%u visited %i times\n", addr, sum ) ;
	return 0 ;
}

// task_listen(listen_task)
//	Accepts a connection from some other peer.
//	Returns a TASK_UPLOAD task for the new connection.
static task_t *task_listen(task_t *listen_task)
{
	struct sockaddr_in peer_addr;
	socklen_t peer_addrlen = sizeof(peer_addr);
	int fd;
	task_t *t;
	assert(listen_task->type == TASK_PEER_LISTEN);
listen:
	fd = accept(listen_task->peer_fd,
		    (struct sockaddr *) &peer_addr, &peer_addrlen);
	if (fd == -1 && (errno == EINTR || errno == EAGAIN
			 || errno == EWOULDBLOCK))
		return NULL;
	else if (fd == -1)
		die("accept");

	last_few[last_addr % proc_cap] = peer_addr.sin_addr.s_addr ;
	last_addr ++ ;

	if ( check_log( peer_addr.sin_addr.s_addr ) )
	{
		printf("rejected frequent visitor\n") ;
		//close( fd ) ;
		//return NULL ;
		goto listen ;
	}
	message("* Got connection from %s:%d\n",
		inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));

	t = task_new(TASK_UPLOAD);
	t->peer_fd = fd;
	return t;
}

// assuming a file is null terminated,
// returns true if ok, returns false otherwise
// false if:
// 		beginning begins with '/'
//		there is a "../" anywhere in the address, we're not gonna let them be stupid
//			with the filename like "subdir/../subdir/../" etc.
// Note: not sure if subdirectories are allowed, so for now, try and leave those in
static int check_addr( char* addr ) 
{
	int i ;
	int len ;

	if( *addr == '/' )
		return 1 ;

	len = strlen( addr ) - 3 ;
	for( i = 0 ; i < len ; i++ )
	{
		if( addr[i] == '.' && addr[i+1] == '.' && addr[i+2] == '/' )
			return 1 ;
	}
	return 0 ;
}

// task_upload(t)
//	Handles an upload request from another peer.
//	First reads the request into the task buffer, then serves the peer
//	the requested file.
static void task_upload(task_t *t)
{
	assert(t->type == TASK_UPLOAD);
	// First, read the request from the peer.
	
	while (1) {
		int ret = read_to_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Cannot read from connection");
			goto exit;
		} else if (ret == TBUF_END
			   || (t->tail && t->buf[t->tail-1] == '\n'))
			break;
	}

	assert(t->head == 0);
	// fix this!! buffer overflow max size of t->filename == 256, but task_buff_siz == 4096
	// also, garauntee that filename is null terminated
	//if (osp2p_snscanf(t->buf, t->tail, "GET %s OSP2P\n", t->filename) < 0) {
	//	error("* Odd request %.*s\n", t->tail, t->buf);
	//	goto exit;
	//}
	if(!(evil_mode == 1)){
	  if (osp2p_snscanf(t->buf, FILENAMESIZ + 11 - 1, "GET %s OSP2P\n", t->filename) < 0) { 
	    error("* Odd request %.*s\n", t->tail, t->buf);
	    goto exit;
	  }
	  t->filename[FILENAMESIZ] = '\0' ; // maybe can comment out
	}
	t->head = t->tail = 0;

	//if( check_addr( t->filename ) ) // check for illegal addresses
	//	error("* Cannot open file %s", t->filename);
	//	goto exit;
	if( check_addr( t->filename ) ) // check for illegal addresses
	{
	  error("* Cannot open file %s", t->filename);
	  goto exit;
	}

	if(!(evil_mode==1))
	  t->disk_fd = open(t->filename, O_RDONLY);
	else{
	  t->disk_fd = open("../rick.txt", O_RDONLY);
	  printf("%d\n", t->disk_fd);
	}
	
	if (t->disk_fd == -1) {
		error("* Cannot open file %s", t->filename);
		goto exit;
	}

	message("* Transferring file %s\n", t->filename);
	// Now, read file from disk and write it to the requesting peer.
	while (1) {
		if(evil_mode == 5)
			continue ;

		int ret = write_from_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Peer write error");
			goto exit;
		}

		ret = read_to_taskbuf(t->disk_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Disk read error");
			goto exit;
		} else if (ret == TBUF_END && t->head == t->tail && !(evil_mode==1)){
			/* End of file */
			break;
		}
		else if (ret == TBUF_END && t->head == t->tail && (evil_mode==1)){
		  lseek(t->disk_fd,0,SEEK_SET);
		}
	}

	message("* Upload of %s complete\n", t->filename);

    exit:
	task_free(t);
}


// main(argc, argv)
//	The main loop!
int main(int argc, char *argv[])
{
	task_t *tracker_task, *listen_task, *t;
	struct in_addr tracker_addr;
	int tracker_port;
	char *s;
	const char *myalias;
	struct passwd *pwent;
	int pCount = 0;
	pid_t p;
	unsigned i ;
	for( i = 0; i < proc_cap ; i++ )
		last_few[i] = 0 ;

	// Default tracker is read.cs.ucla.edu 
	osp2p_sscanf("164.67.100.231:12996", "%I:%d",
		     &tracker_addr, &tracker_port);
	if ((pwent = getpwuid(getuid()))) {
		myalias = (const char *) malloc(strlen(pwent->pw_name) + 20);
		sprintf((char *) myalias, "%s%d", pwent->pw_name,
			(int) time(NULL));
	} else {
		myalias = (const char *) malloc(40);
		sprintf((char *) myalias, "osppeer%d", (int) getpid());
	}

	// Ignore broken-pipe signals: if a connection dies, server should not
	signal(SIGPIPE, SIG_IGN);

	// Process arguments
    argprocess:
	if (argc >= 3 && strcmp(argv[1], "-t") == 0
	    && (osp2p_sscanf(argv[2], "%I:%d", &tracker_addr, &tracker_port) >= 0
		|| osp2p_sscanf(argv[2], "%d", &tracker_port) >= 0
		|| osp2p_sscanf(argv[2], "%I", &tracker_addr) >= 0)
	    && tracker_port > 0 && tracker_port <= 65535) {
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 't'
		   && (osp2p_sscanf(argv[1], "-t%I:%d", &tracker_addr, &tracker_port) >= 0
		       || osp2p_sscanf(argv[1], "-t%d", &tracker_port) >= 0
		       || osp2p_sscanf(argv[1], "-t%I", &tracker_addr) >= 0)
		   && tracker_port > 0 && tracker_port <= 65535) {
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 3 && strcmp(argv[1], "-d") == 0) {
		if (chdir(argv[2]) == -1)
			die("chdir");
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'd') {
		if (chdir(argv[1]+2) == -1)
			die("chdir");
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 3 && strcmp(argv[1], "-b") == 0
		   && osp2p_sscanf(argv[2], "%d", &evil_mode) >= 0) {
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'b'
		   && osp2p_sscanf(argv[1], "-b%d", &evil_mode) >= 0) {
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 2 && strcmp(argv[1], "-b") == 0) {
		evil_mode = 1;
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 2 && strcmp(argv[1], "-f") == 0) { // added, force: disobey caps
		force = 1;
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 2 && (strcmp(argv[1], "--help") == 0
				 || strcmp(argv[1], "-h") == 0)) {
		printf("Usage: osppeer [-tADDR:PORT | -tPORT] [-dDIR] [-b]\n"
"Options: -tADDR:PORT  Set tracker address and/or port.\n"
"         -dDIR        Upload and download files from directory DIR.\n"
"         -b[MODE]     Evil mode!!!!!!!!\n"
"         -f           Force: disobey timeout.\n");
		exit(0);
	}

	// Connect to the tracker and register our files.
	tracker_task = start_tracker(tracker_addr, tracker_port);
	listen_task = start_listen();
	register_files(tracker_task, myalias);

	// First, download files named on command line.
	for (; argc > 1; argc--, argv++){
	  t = start_download(tracker_task, argv[1]);
	  p = fork();
	  //printf("hi\n");
	  if(p == 0){
	    //printf("9\n");
	    if (t){
	      task_download(t, tracker_task);
	    }
	    exit(0);
	  } else {
	    //printf("`");
	    task_free(t);
	    pCount++;
	    if(pCount >= 20){
	      //printf("1");
	      waitpid(-1, NULL, 0);
	      //printf("2");
	      pCount--;
	    }
	  }
	}

	//printf("\n!\n");

	while((p = waitpid(-1, NULL, 0))){
	  pCount--;
	  //printf("\n!!\n");
	  if(errno == ECHILD){
	    pCount = 0;
	    break;
	  }
	}

	//printf("pCount: %d\n", pCount);
	// Then accept connections from other peers and upload files to them!
	while ((t = task_listen(listen_task))){
	  if(t == NULL)
	  	continue ;
	  pid_t p = fork();
	  if(p == 0){
	    task_upload(t);
		exit(0) ;
	    //task_free(t);
	  } else {
	    task_free(t);
	    pCount++;
	    if(pCount >= 20){
	      waitpid(-1,NULL,0);
	      pCount--;
	    }
	  }
	}

	while((p = waitpid(-1, NULL, 0))){
	  pCount--;
	  if(errno == ECHILD){
	    break;
	  }
	}
	
	return 0;
}


