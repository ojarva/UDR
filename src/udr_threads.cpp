/*****************************************************************************
Copyright 2012 Laboratory for Advanced Computing at the University of Chicago

This file is part of UDR.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions
and limitations under the License.
*****************************************************************************/

#include <unistd.h>
#include <pthread.h>
#include <sstream>
#include <signal.h>
#include <netdb.h>
#include <errno.h>
#include <syslog.h>
#include <sys/types.h>
#include <glob.h>
#include <udt.h>
#include "udr_util.h"
#include "udr_threads.h"

#include <arpa/inet.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/select.h>

using std::string;

int ppid_poll = 5;
bool thread_log = false;

//for debugging
string local_logfile_dir = "../log";

// Ever TRANSFER_TIMEOUT interval, check to see if data has been exchanged
// if timeout_sem = 1 then data has been exchanged
// if timeout_sem = 2 then data has not been exchanged but
//     the connection has not been established so don't exit
int timeout_sem;
void *monitor_timeout(void* _arg) {

    timeout_mon_args *args = (timeout_mon_args*) _arg;
    FILE* logfile = args->logfile;

    while (1){

	sleep(args->timeout);

	if (timeout_sem == 0){

	    if(logfile){
	    	fprintf(logfile, "Data transfer timeout. Exiting\n");
	    	fclose(logfile);
	    }
	    exit(1);

	} else {
	    // continue on as normal
	}

	// If timeout_sem == 2, the connection has not been made -> no timeout next round
	if (timeout_sem != 2)
	    timeout_sem = 0;

    }
}


void print_bytes(FILE* file, const void *object, size_t size) {
    size_t i;

    fprintf(file, "[ ");
    for(i = 0; i < size; i++)
    {
	fprintf(file, "%02x ", ((const unsigned char *) object)[i] & 0xff);
    }
    fprintf(file, "]\n");
}

string convert_int(int number) {
    stringstream ss;
    ss << number;
    return ss.str();
}

//perhaps want a timeout here now with server mode?
string udt_recv_string( int udt_handle ) {
    char buf[ 2 ];
    buf[ 1 ] = '\0';

    string str = "";

    for( ;; ) {
	int bytes_read = UDT::recv( udt_handle , buf , 1 , 0 );
	if ( bytes_read == UDT::ERROR ){
	    cerr << "recv:" << UDT::getlasterror().getErrorMessage() << endl;
	    break;
	}
	if ( bytes_read == 1 ) {
	    if ( buf[ 0 ] == '\0' )
		break;
	    str += buf;
	}
	else {
	    sleep(1);
	}
    }
    return str;
}

void sigexit(int signum) {
    exit(EXIT_SUCCESS);
}    /* Exit successfully */




void *handle_to_udt(void *threadarg) {
    signal(SIGUSR1,sigexit);

    struct thread_data *my_args = (struct thread_data *) threadarg;
    char indata[max_block_size];
    char outdata[max_block_size];
    FILE*  logfile;

    if(my_args->log) {
	string filename = my_args->logfile_dir + convert_int(my_args->id) + "_log.txt";
	logfile = fopen(filename.c_str(), "w");
    }
    //struct timeval tv;
    //fd_set readfds;
    int bytes_read;
    while(true) {
	int ss;

	if(my_args->log) {
	    fprintf(logfile, "%d: Should be reading from process...\n", my_args->id);
	    fflush(logfile);
	}
	
	if(my_args->crypt != NULL)
	    bytes_read = read(my_args->fd, indata, max_block_size);
	else
	    bytes_read = read(my_args->fd, outdata, max_block_size);

	timeout_sem = 1;



	if(bytes_read < 0){
	    if(my_args->log){
		fprintf(logfile, "Error: bytes_read %d %s\n", bytes_read, strerror(errno));
		fclose(logfile);
	    }
	    my_args->is_complete = true;
	    return NULL;
	}
	if(bytes_read == 0) {
	    if(my_args->log){
		fprintf(logfile, "%d Got %d bytes_read, exiting\n", my_args->id, bytes_read);
		fclose(logfile);
	    }
	    my_args->is_complete = true;
	    return NULL;
	}

	if(my_args->crypt != NULL)
	    my_args->crypt->encrypt(indata, outdata, bytes_read);

	if(my_args->log){
	    fprintf(logfile, "%d bytes_read: %d\n", my_args->id, bytes_read);
	    // print_bytes(logfile, outdata, bytes_read);
	    fflush(logfile);
	}

	int ssize = 0;
	while(ssize < bytes_read) {
	    if (UDT::ERROR == (ss = UDT::send(*my_args->udt_socket, outdata + ssize, bytes_read - ssize, 0))) {

		if(my_args->log) {
		    fprintf(logfile, "%d send error: %s\n", my_args->id, UDT::getlasterror().getErrorMessage());
		    fclose(logfile);
		}
		my_args->is_complete = true;
		return NULL;
	    }

	    ssize += ss;
	    if(my_args->log) {
		fprintf(logfile, "%d sender on socket %d bytes read: %d ssize: %d\n", my_args->id, *my_args->udt_socket, bytes_read, ssize);
		fflush(logfile);
	    }
	}
    }
    my_args->is_complete = true;
}

void *udt_to_handle(void *threadarg) {
    struct thread_data *my_args = (struct thread_data *) threadarg;
    char indata[max_block_size];
    char outdata[max_block_size];
    FILE* logfile;

    if(my_args->log) {
	string filename = my_args->logfile_dir + convert_int(my_args->id) + "_log.txt";
	logfile = fopen(filename.c_str(), "w");
    }

    while(true) {
	int rs;

	if(my_args->log) {
	    fprintf(logfile, "%d: Should now be receiving from udt...\n", my_args->id);
	    fflush(logfile);
	}

	if (UDT::ERROR == (rs = UDT::recv(*my_args->udt_socket, indata, max_block_size, 0))) {
	    if(my_args->log){
		fprintf(logfile, "%d recv error: %s\n", my_args->id, UDT::getlasterror().getErrorMessage());
		fclose(logfile);
	    }
	    my_args->is_complete = true;
	    return NULL;
	}

	int written_bytes;
	if(my_args->crypt != NULL) {
	    my_args->crypt->encrypt(indata, outdata, rs);
	    written_bytes = write(my_args->fd, outdata, rs);
	}
	else {
	    written_bytes = write(my_args->fd, indata, rs);
	}
	timeout_sem = 1;

	if(my_args->log) {
	    fprintf(logfile, "%d recv on socket %d rs: %d written bytes: %d\n", my_args->id, *my_args->udt_socket, rs, written_bytes);
	    fflush(logfile);
	}

	if(written_bytes < 0) {
	    if(my_args->log){
		fprintf(logfile, "Error: written_bytes: %d %s\n", written_bytes, strerror(errno));
		fclose(logfile);
	    }
	    my_args->is_complete = true;
	    return NULL;
	}
    }
    my_args->is_complete = true;
}

#define SERVER_PORT     3005
#define BUFFER_LENGTH    250
#define FALSE              0

int connect_to_receiver(UDR_Options *udr_options, UDTSOCKET *client)
{

    struct addrinfo hints, *local, *peer;

    UDTSOCKET sock_fd;
    // int sock_fd;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_flags = AI_PASSIVE;
    // hints.ai_family = AF_UNSPEC;

    fprintf(stderr, "host %s\n", udr_options->host);

    unsigned char buf[sizeof(struct in6_addr)];
    if (inet_pton(AF_INET6, udr_options->host, buf)){
        fprintf(stderr, "IPV6\n\n\n");
        hints.ai_family = AF_INET6;
    } else {
        fprintf(stderr, "IPV4\n\n\n");
        hints.ai_family = AF_INET;
    }

    hints.ai_socktype = SOCK_STREAM;

    if (0 != getaddrinfo(NULL, udr_options->port_num, &hints, &local)) {
        cerr << "[udr sender] incorrect network address.\n" << endl;
        return 1;
    }

    sock_fd = UDT::socket(local->ai_family, local->ai_socktype, local->ai_protocol);
    // sock_fd = socket(local->ai_family, local->ai_socktype, local->ai_protocol);

    if (sock_fd <= 0){
        perror("[udr sender] unable to create socket");
        return 1;
    }

    freeaddrinfo(local);

    if (getaddrinfo(udr_options->host, udr_options->port_num, &hints, &peer)) {
        cerr << "[udr sender] unable to get info about server/peer address " << 
            udr_options->host << ":" << udr_options->port_num << endl;
        return 1;
    }

    int connected = 0;
    while (peer){

        // if (connect(sock_fd, peer->ai_addr, peer->ai_addrlen)){
        //     perror("[udr sender] unable to connect normally");
        //     // return 1;
        //     peer = peer->ai_next;
        // } else {
        //     fprintf(stderr, "WORKED NORMALLY\n\n");
        //     return 0;
        // }

        
        if (UDT::ERROR == UDT::connect(sock_fd, peer->ai_addr, peer->ai_addrlen)) {
            cerr << "[udr sender] connect attempt: " << UDT::getlasterror().getErrorMessage() << endl;
            peer = peer->ai_next;
        } else {
            break;
        }

    }

    if (!peer){
        cerr << "[udr sender] unable to connect to receiver\n";
        return 1;
    }

    *client = sock_fd;
    
    freeaddrinfo(peer);

    return 0;

}


int run_sender(UDR_Options * udr_options, unsigned char * passphrase, const char* cmd, int argc, char ** argv) {

    UDT::startup();
    UDTSOCKET client;

    if (connect_to_receiver(udr_options, &client)){
        cerr << "[udr sender] connect: unable to connect to receiver\n";
        return 1;
    }

    char* data = new char[max_block_size];

    ssize_t n;

    //very first thing we send is the rsync argument so that the rsync server can be started and piped to from the UDT connection
    n = strlen(cmd) + 1;
    int ssize = 0;
    int ss;
    while(ssize < n) {
	if (UDT::ERROR == (ss = UDT::send(client, cmd + ssize, n - ssize, 0)))
	{
	    cerr << "[udr sender] Send:" << UDT::getlasterror().getErrorMessage() << endl;
	    break;
	}

	ssize += ss;
    }

    struct thread_data sender_to_udt;
    sender_to_udt.udt_socket = &client;
    sender_to_udt.fd = STDIN_FILENO; //stdin of this process, from stdout of rsync
    sender_to_udt.id = 0;
    sender_to_udt.log = thread_log;
    sender_to_udt.logfile_dir = local_logfile_dir;
    sender_to_udt.is_complete = false;

    struct thread_data udt_to_sender;
    udt_to_sender.udt_socket = &client;
    udt_to_sender.fd = STDOUT_FILENO; //stdout of this process, going to stdin of rsync, rsync defaults to set this is non-blocking
    udt_to_sender.id = 1;
    udt_to_sender.log = thread_log;
    udt_to_sender.logfile_dir = local_logfile_dir;
    udt_to_sender.is_complete = false;

    if(udr_options->encryption){
	crypto encrypt(EVP_ENCRYPT, PASSPHRASE_SIZE, (unsigned char *) passphrase,
        udr_options->encryption_type);
	crypto decrypt(EVP_DECRYPT, PASSPHRASE_SIZE, (unsigned char *) passphrase,
        udr_options->encryption_type);
	// free_key(passphrase);
	sender_to_udt.crypt = &encrypt;
	udt_to_sender.crypt = &decrypt;
    }
    else{
	sender_to_udt.crypt = NULL;
	udt_to_sender.crypt = NULL;
    }

    pthread_t sender_to_udt_thread;
    pthread_create(&sender_to_udt_thread, NULL, handle_to_udt, (void *)&sender_to_udt);

    pthread_t udt_to_sender_thread;
    pthread_create(&udt_to_sender_thread, NULL, udt_to_handle, (void*)&udt_to_sender);

    int rc1 = pthread_join(udt_to_sender_thread, NULL);

    if(udr_options->verbose)
	fprintf(stderr, "[udr sender] joined on udt_to_sender_thread %d\n", rc1);

    pthread_kill(sender_to_udt_thread, SIGUSR1);

    int rc2 = pthread_join(sender_to_udt_thread, NULL);

    if(udr_options->verbose)
        fprintf(stderr, "[udr sender] joined on sender_to_udt_thread %d\n", rc2);

    UDT::close(client);
    UDT::cleanup();

    delete [] data;
    return 0;
}


int listen_for_sender(UDR_Options *udr_options, UDTSOCKET *serv, char *receiver_port)
{
    addrinfo hints;
    addrinfo* res;

    struct sockaddr_in my_addr;

    // switch to turn on ip specification or not
    int specify_ip = !!(udr_options->specify_ip);

    if (udr_options->verbose && specify_ip)
        fprintf(stderr, "Specifying on specific ip: %s\n", udr_options->specify_ip);

    memset(&hints, 0, sizeof(struct addrinfo));

    if (udr_options->ipv6){
        fprintf(stderr, "IPV6\n\n\n");
        hints.ai_family = AF_INET6;
    } else {
        fprintf(stderr, "IPV4\n\n\n");
        hints.ai_family = AF_INET;
    }
    
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;

    bool bad_port = false;

    if(udr_options->start_port > udr_options->end_port){
        fprintf(stderr, "[udr receiver] ERROR: invalid port range %d - %d\n", 
                udr_options->start_port, udr_options->end_port);
        return 1;
    }

    int r;
    for(int port_num = udr_options->start_port; port_num <= udr_options->end_port; port_num++) {
        bad_port = false;

        snprintf(receiver_port, sizeof(receiver_port), "%d", port_num);

        if (0 != getaddrinfo(NULL, receiver_port, &hints, &res)) {
            bad_port = true;

        } else {

            *serv = UDT::socket(res->ai_family, res->ai_socktype, res->ai_protocol);

            if (specify_ip){ 

                my_addr.sin_family = AF_INET6;     
                my_addr.sin_port = htons(port_num); 
                my_addr.sin_addr.s_addr = inet_addr(udr_options->specify_ip);
                bzero(&(my_addr.sin_zero), 8);    

                r = UDT::bind(*serv, (struct sockaddr *)&my_addr, sizeof(struct sockaddr));
            } else {
                r = UDT::bind(*serv, res->ai_addr, res->ai_addrlen);
            }

            if (UDT::ERROR == r){
                bad_port = true;
            }
        }

        freeaddrinfo(res);

        if(!bad_port)
            break;

    }

    if(bad_port){
        fprintf(stderr, "[udr receiver] ERROR: could not bind to any port in range %d - %d\n", udr_options->start_port, udr_options->end_port);
        return 1;
    }

    
    
    return 0;
    
}


int run_receiver(UDR_Options * udr_options) {

    string filename = local_logfile_dir + "receiver_log.txt";
    int orig_ppid = getppid();


    // Create socket, bind, listen and accept sender
    UDT::startup();
    UDTSOCKET serv;
    char receiver_port[NI_MAXSERV];
    if (listen_for_sender(udr_options, &serv, receiver_port)){
        fprintf(stderr, "[udr receiver] ERROR: unable to connect to sender\n");
        return 1;
    }

    
    unsigned char rand_pp[PASSPHRASE_SIZE];
    int success = RAND_bytes((unsigned char *) rand_pp, PASSPHRASE_SIZE);

    //stdout port number and password -- to send back to the client
    printf("%s ", receiver_port);

    for(int i = 0; i < PASSPHRASE_SIZE; i++) {
	printf("%02x", rand_pp[i]);
    }
    printf(" \n");
    fflush(stdout);

    if(udr_options->verbose)
	fprintf(stderr, "[udr receiver] server is ready at port %s\n", receiver_port);

    if (UDT::ERROR == UDT::listen(serv, 10)) {
	cerr << "[udr receiver] listen: " << UDT::getlasterror().getErrorMessage() << endl;
	return 0;
    }

    sockaddr_storage clientaddr;
    int addrlen = sizeof(clientaddr);

    UDTSOCKET recver;

    if (UDT::INVALID_SOCK == (recver = UDT::accept(serv, (sockaddr*)&clientaddr, &addrlen))) {
	fprintf(stderr, "[udr receiver] accept: %s\n", UDT::getlasterror().getErrorMessage());
	return 0;
    }

    char clienthost[NI_MAXHOST];
    char clientservice[NI_MAXSERV];
    getnameinfo((sockaddr *)&clientaddr, addrlen, clienthost, sizeof(clienthost), clientservice, sizeof(clientservice), NI_NUMERICHOST|NI_NUMERICSERV);


    //If in server mode, need to check that --sender is a option (read-only) and change the directory to be in the directory that is being served up.
//  const char * sender_flag = "--sender";
    bool seen_sender = false;
//  bool after_dot = false;
//  int file_idx = -1;
//  bool called_glob = false;


    string cmd_str = udt_recv_string(recver);
    const char * cmd = cmd_str.c_str();

    //perhaps want to at least check that starts with rsync?
    if(strncmp(cmd, "rsync ", 5) != 0){
//      const char * error_msg = "UDR ERROR: non-rsync command detected\n";
	exit(1);
    }


    char * rsync_cmd;

    if(udr_options->server_connect){
        if(udr_options->verbose)
            fprintf(stderr, "[udr receiver] server connect mode\n");

        rsync_cmd = (char *)malloc(100);

        if(strlen(udr_options->server_config) > 0){
            sprintf(rsync_cmd, "%s%s %s", "rsync --config=", udr_options->server_config, " --server --daemon .");
        }
        else{
            strcpy(rsync_cmd, "rsync --server --daemon .");
        }
    }
    else{
        rsync_cmd = (char *)malloc(strlen(cmd) + 1);
        strcpy(rsync_cmd, cmd);
    }

    if(udr_options->verbose){
        fprintf(stderr, "[udr receiver] rsync cmd: %s\n", rsync_cmd);
    }

    char ** sh_cmd = (char **)malloc(sizeof(char *) * 4);
    sh_cmd[0] = udr_options->shell_program;
    sh_cmd[1] = "-c";
    sh_cmd[2] = rsync_cmd;
    sh_cmd[3] = NULL;

    //now fork and exec the rsync on the remote side using sh (so that wildcards will be expanded properly)
    int child_to_parent, parent_to_child;

    int rsync_pid = fork_execvp(udr_options->shell_program, sh_cmd, &parent_to_child, &child_to_parent);

    //now if we're in server mode need to drop privileges if specified
    if(udr_options->rsync_gid > 0){
        setgid(udr_options->rsync_gid);
    }
    if(udr_options->rsync_uid > 0){
        setuid(udr_options->rsync_uid);
    }

    if(udr_options->verbose){
	    fprintf(stderr, "[udr receiver] rsync pid: %d\n", rsync_pid);
    }

    struct thread_data recv_to_udt;
    recv_to_udt.udt_socket = &recver;
    recv_to_udt.fd = child_to_parent; //stdout of rsync server process
    recv_to_udt.id = 2;
    recv_to_udt.log = thread_log;
    recv_to_udt.logfile_dir = local_logfile_dir;
    recv_to_udt.is_complete = false;

    struct thread_data udt_to_recv;
    udt_to_recv.udt_socket = &recver;
    udt_to_recv.fd = parent_to_child; //stdin of rsync server process
    udt_to_recv.id = 3;
    udt_to_recv.log = thread_log;
    udt_to_recv.logfile_dir = local_logfile_dir;
    udt_to_recv.is_complete = false;

    if(udr_options->encryption){
	crypto encrypt(EVP_ENCRYPT, PASSPHRASE_SIZE, rand_pp,
        udr_options->encryption_type);
	crypto decrypt(EVP_DECRYPT, PASSPHRASE_SIZE, rand_pp,
        udr_options->encryption_type);
	recv_to_udt.crypt = &encrypt;
	udt_to_recv.crypt = &decrypt;
    }
    else{
	recv_to_udt.crypt = NULL;
	udt_to_recv.crypt = NULL;
    }

    pthread_t recv_to_udt_thread;
    pthread_create(&recv_to_udt_thread, NULL, handle_to_udt, (void *)&recv_to_udt);

    pthread_t udt_to_recv_thread;
    pthread_create(&udt_to_recv_thread, NULL, udt_to_handle, (void*)&udt_to_recv);


    timeout_sem = 2;
    pthread_t counter_thread;
    FILE* timeout_log = NULL;
    timeout_mon_args timeout_args;
    timeout_args.logfile = timeout_log;
    timeout_args.timeout = udr_options->timeout;

    if(thread_log) {
	string filename = local_logfile_dir + "timeout_monitor_log.txt";
	timeout_log = fopen(filename.c_str(), "w");
    }
    pthread_create(&counter_thread, NULL, &monitor_timeout, &timeout_args);


    if(udr_options->verbose){
	fprintf(stderr, "[udr receiver] waiting to join on recv_to_udt_thread\n");
	fprintf(stderr, "[udr receiver] ppid %d pid %d\n", getppid(), getpid());
    }

    //going to poll if the ppid changes then we know it's exited and then we exit all of our threads and exit as well
    //also going to check if either thread is complete, if one is then the other should also be killed
    //bit of a hack to deal with the pthreads
    while(true){
        if(getppid() != orig_ppid){
            pthread_kill(recv_to_udt_thread, SIGUSR1);
            pthread_kill(udt_to_recv_thread, SIGUSR1);
            break;
        }
        if(recv_to_udt.is_complete && udt_to_recv.is_complete){
            if(udr_options->verbose){
                fprintf(stderr, "[udr receiver] both threads are complete: recv_to_udt.is_complete %d udt_to_recv.is_complete %d\n", recv_to_udt.is_complete, udt_to_recv.is_complete);
            }
            break;
        }
        else if(recv_to_udt.is_complete){
            if(udr_options->verbose){
                fprintf(stderr, "[udr receiver] recv_to_udt is complete: recv_to_udt.is_complete %d udt_to_recv.is_complete %d\n", recv_to_udt.is_complete, udt_to_recv.is_complete);
            }
            break;
        }
        else if(udt_to_recv.is_complete){
            if(udr_options->verbose){
                fprintf(stderr, "[udr receiver] udt_to_recv is complete: recv_to_udt.is_complete %d udt_to_recv.is_complete %d\n", recv_to_udt.is_complete, udt_to_recv.is_complete);
            }
            break;
        }

        sleep(ppid_poll);
    }

    if(udr_options->verbose){
	fprintf(stderr, "[udr receiver] Trying to close recver\n");
    }
    UDT::close(recver);
    //int rc1 = pthread_join(recv_to_udt_thread, NULL);
    //if(udr_options->verbose){
    //fprintf(stderr, "[udr receiver] Joined recv_to_udt_thread %d\n", rc1);
    //}

    if(udr_options->verbose){
	fprintf(stderr, "[udr receiver] Closed recver\n");
    }


    UDT::close(serv);

    if(udr_options->verbose){
        fprintf(stderr, "[udr receiver] Closed serv\n");
    }

    UDT::cleanup();

    if(udr_options->verbose){
	fprintf(stderr, "[udr receiver] UDT cleaned up\n");
    }

    //int rc2 = pthread_join(udt_to_recv_thread, NULL);

    //if(udr_options->verbose){
    //fprintf(stderr, "[udr receiver] Joined udt_to_recv_thread %d Should be closing recver now\n", rc2);
    //}


    return 0;
}
