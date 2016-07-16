/* ELSTER Interface program */

#include <stdio.h>	// for FILE
#include <stdlib.h>	// for timeval
#include <string.h>	// for strlen etc
#include <time.h>	// for ctime
#include <sys/types.h>	// for fd_set
// #include <sys/socket.h>
// #include <netinet/in.h>
#include <netdb.h>	// for sockaddr_in 
#include <fcntl.h>	// for O_RDWR
#include <termios.h>	// for termios
#include <unistd.h>		// for getopt
#ifdef linux
#include <errno.h>		// for Linux
#endif
#include "../Common/common.h"

// Confused.  This is defined in stdlib.h as float,
// but compiler seems to think it returns an int.
float strtof(const char * a, char ** b);

#define REVISION "$Revision: 1.4 $"
/*  1.1 08/05/2008 Initial version created by copying from Elster 1.4
	1.2 09/05/2008 Working version
	1.3 09/10/2008 Remove CRTSCTS
	1.4 16/11/2008 Changed logon from elster to meter. Added -Z
	1.5 2011/11/10 Added common serial framework to allow it to use xuarts
*/

static char* id="@(#)$Id: elster3.c,v 1.4 2008/11/16 10:52:33 martin Exp $";

#define PORTNO 10010
#define PROGNAME "Elster3"
const char progname[] = "Elster3";
#define LOGON "meter"
#define LOGFILE "/tmp/elster3-%d.log"
#define SERIALNAME "/dev/ttyAM1"	/* although it MUST be supplied on command line */
#define BAUD B2400

// Severity levels.  ERROR and FATAL terminate program
// #define INFO	0
#define	WARN	1
#define	ERROR	2
#define	FATAL	3
// Socket retry params
#define NUMRETRIES 3
int numretries = NUMRETRIES;
#define RETRYDELAY	1000000	/* microseconds */
int retrydelay = RETRYDELAY;
// Elster values expected
#define NUMPARAMS 15
// If defined, send fixed data instead of timeout message
// #define DEBUGCOMMS

/* SOCKET CLIENT */

/* Command line params: 
1 - device name
2 - device timeout. Default to 60 seconds
3 - optional 'nolog' to suppress writing to local logfile
*/

#ifndef linux
extern
#endif
int errno;  

// Procedures in this file
int processSocket(void);			// process server message
void usage(void);					// standard usage message
char * getversion(void);
int getbuf(int fd, int tmout);	// get a buffer full of message
void printbuf();			// decode a bufferfull
time_t timeMod(time_t interval);
void writedata();			// send meter data
void init_table();					// initialise translation table
unsigned char getbyte(int fd);		// Get and translate a byte
void translate();

/* GLOBALS */
FILE * logfp = NULL;
int sockfd[1];
int debug = 0;
int noserver = 0;		// prevents socket connection when set to 1

// Common Serial Framework
#define BUFSIZE 800	/* should be longer than max possible line of text */
struct data {	// The serial buffer
	int count;
	unsigned char buf[BUFSIZE];
	int escape;		// Count the escapes in this message
	// int sentlength;
} data;
int controllernum = -1;	//	only used in logon message
unsigned char trans[256];	// translation map for wierdo A1100C protocol

#define debugfp stderr
int commfd;
char * serialName = SERIALNAME;
char buffer[512];	// For messages

/********/
/* MAIN */
/********/
int main(int argc, char *argv[])
// arg1: serial device file
// arg2: optional timeout in seconds, default 60
// arg3: optional 'nolog' to carry on when filesystem full
{
	int nolog = 0;

	int interval = 500;
	time_t next;

	int run = 1;		// set to 0 to stop main loop
	fd_set readfd; 
	int numfds;
	struct timeval timeout;
	int tmout = 90;
	int logerror = 0;
	int online = 1;		// used to prevent messages every minute in the event of disconnection
	int option; 
	// Command line arguments
	
	// optind = -1;
	opterr = 0;
	while ((option = getopt(argc, argv, "dt:slVi:Z")) != -1) {
		switch (option) {
			case 's': noserver = 1; break;
			case 'l': nolog = 1; break;
			case '?': usage(); exit(1);
			case 't': tmout = atoi(optarg); break;
			case 'd': debug++; break;
			case 'i': interval = atoi(optarg); break;
			case 'V': printf("Version %s %s\n", getversion(), id); exit(0);
			case 'Z': decode("(b+#Gjv~z`mcx-@ndd`rxbwcl9Vox=,/\x10\x17\x0e\x11\x14\x15\x11\x0b\x1a" 
							 "\x19\x1a\x13\x0cx@NEEZ\\F\\ER\\\x19YTLDWQ'a-1d()#!/#(-9' >q\"!;=?51-??r"); exit(0);
				
		}
	}
	
	DEBUG printf("Debug on. optind %d argc %d\n", optind, argc);
	
	if (optind < argc) serialName = argv[optind];		// get serial/device name: parameter 1
	optind++;
	if (optind < argc) controllernum = atoi(argv[optind]);	// get optional controller number: parameter 2
	
	sprintf(buffer, LOGFILE, controllernum);
	
	if (!nolog) if ((logfp = fopen(buffer, "a")) == NULL) logerror = errno;	
	
	// There is no point in logging the failure to open the logfile
	// to the logfile, and the socket is not yet open.

	sprintf(buffer, "STARTED %s on %s as %d timeout %d %s", argv[0], serialName, controllernum, tmout, nolog ? "nolog" : "");
	logmsg(INFO, buffer);
	
	// Open serial port
	if ((commfd = openSerial(serialName, BAUD, 0, CS8, 1)) < 0) {
		sprintf(buffer, "ERROR " PROGNAME " %d Failed to open %s: %s", controllernum, serialName, strerror(errno));
#ifdef DEBUGCOMMS
		logmsg(INFO, buffer);			// FIXME AFTER TEST
		printf("Using stdio\n");
		commfd = 0;		// use stdin
#else
		logmsg(FATAL, buffer);
#endif
	}

	// Set up socket 
	if (!noserver) {
		openSockets(0, 1, "meter", REVISION, PROGNAME, 0);
		
		if (flock(commfd, LOCK_EX | LOCK_NB) == -1) {
			sprintf(buffer, "FATAL " PROGNAME " is already running, cannot start another one on %s", serialName);
			logmsg(FATAL, buffer);
		}
	
		// Logon to server
		sprintf(buffer, "logon " LOGON " %s %d %d", getversion(), getpid(), controllernum);
		sockSend(sockfd[0], buffer);
	}
	else	sockfd[0] = 1;		// noserver: use stdout
	
	// If we failed to open the logfile and were NOT called with nolog, warn server
	// Obviously don't use logmsg!
	if (logfp == NULL && nolog == 0) {
		sprintf(buffer, "event WARN " PROGNAME " %d could not open logfile %s: %s", controllernum, LOGFILE, strerror(logerror));
		sockSend(sockfd[0], buffer);
	}
		
	numfds = (sockfd[0] > commfd ? sockfd[0] : commfd) + 1;		// nfds parameter to select. One more than highest descriptor
	init_table();		// Set up translation table

	// Main Loop
	FD_ZERO(&readfd); 
	next = timeMod(interval);
	DEBUG fprintf(stderr, "Now is %u next is %d\n", (unsigned int)time(NULL), (unsigned int)next);
	while(run) {
		timeout.tv_sec = tmout;
		timeout.tv_usec = 0;
		FD_SET(sockfd[0], &readfd);
		FD_SET(commfd, &readfd);
		if (select(numfds, &readfd, NULL, NULL, &timeout) == 0) {	// select timed out. Bad news 
#ifdef DEBUGCOMMS
			processLine("63;59;59;1976;53000;30;100;31;32;33;34;1;371;73;12345;");
#else
			if (online == 1) {
				logmsg(WARN, "WARN " PROGNAME " No data for last period");
				online = 0;	// Don't send a message every minute from now on
			}
#endif
			continue;
		}
		if (FD_ISSET(commfd, &readfd)) { 
			int num;
			num = getbuf(328, 200);  // 512 chars; 200mSec
			translate();
			online = 1;	// Back online
			DEBUG2 fprintf(debugfp, "Got packet %d long\n", num);
			if (num < 0) {
				fprintf(stderr, "Error reading %s : %s\n", serialName, strerror(errno));
				run = 0;
				break;
			} else {
				if (time(NULL) > next) {
					next = timeMod(interval);
					DEBUG2 fprintf(stderr, "Calling writedata. Next = %d\n", (unsigned int)next);
					writedata();
					DEBUG2 fprintf(stderr, "Back from writedata\n");
				}
			}
		}
		if ((noserver == 0) && FD_ISSET(sockfd[0], &readfd))
			run = processSocket();	// the server may request a shutdown by setting run to 0
			if (run == 2) {
				printbuf();	// this doesn't stop the loop
				run = 1;
			}
	}
	logmsg(INFO,"INFO " PROGNAME " Shutdown requested");
	close(sockfd[0]);
	closeSerial(commfd);

	return 0;
}

/*********/
/* USAGE */
/*********/
void usage(void) {
	printf("Usage: elster3 [-t timeout] [-l] [-s] [-d] [-V] /dev/ttyname controllernum\n");
	printf("-l: no log  -s: no server  -d: debug on\n -V: version -i: interval in seconds\n");
	return;
}


// Static data
struct termios oldSettings, newSettings; 

/*****************/
/* PROCESSSOCKET */
/*****************/
int processSocket(void){
// Deal with commands from MCP.  Return to 0 to do a shutdown
	short int msglen, numread;
	char buffer[128], buffer2[192];	// about 128 is good but rather excessive since longest message is 'truncate'
	char * cp = &buffer[0];
	int retries = NUMRETRIES;
		
	if (read(sockfd[0], &msglen, 2) != 2) {
		logmsg(WARN, "WARN " PROGNAME " Failed to read length from socket");
		return 1;
	}
	msglen =  ntohs(msglen);
	while ((numread = read(sockfd[0], cp, msglen)) < msglen) {
		cp += numread;
		msglen -= numread;
		if (--retries == 0) {
			logmsg(WARN, "WARN " PROGNAME " Timed out reading from server");
			return 1;
		}
		usleep(RETRYDELAY);
	}
	cp[numread] = '\0';	// terminate the buffer 
	
	if (strcmp(buffer, "exit") == 0)
		return 0;	// Terminate program
	if (strcmp(buffer, "Ok") == 0)
		return 1;	// Just acknowledgement
	if (strcmp(buffer, "truncate") == 0) {
		if (logfp) {
		// ftruncate(logfp, 0L);
		// lseek(logfp, 0L, SEEK_SET);
			freopen(NULL, "w", logfp);
			logmsg(INFO, "INFO " PROGNAME " Truncated log file");
		} else
			logmsg(INFO, "INFO " PROGNAME " Log file not truncated as it is not open");
		return 1;
	}
	if (strcmp(buffer, "debug 0") == 0) {	// turn off debug
		debug = 0;
		return 1;
	}
	if (strcmp(buffer, "debug 1") == 0) {	// enable debugging
		debug = 1;
		return 1;
	}
	if (strcmp(buffer, "help") == 0) {
		strcpy(buffer2, "INFO " PROGNAME " Commands are: debug 0|1, exit, truncate, read");
		logmsg(INFO, buffer2);
		return 1;
	}
	if (strcmp(buffer, "read") == 0) {
		return 2;	// to signal s full read
	}
	strcpy(buffer2, "INFO " PROGNAME " Unknown message from server: ");
	strcat(buffer2, buffer);
	logmsg(INFO, buffer2);	// Risk of loop: sending unknown message straight back to server
	
	return 1;	
};

/**************/
/* GETVERSION */
/**************/
char *getversion(void) {
// return pointer to version part of REVISION macro
	static char version[10] = "";	// Room for xxxx.yyyy
	if (!strlen(version)) {
		strcpy(version, REVISION+11);
		version[strlen(version)-2] = '\0';
	}
	return version;
}

/**********/
/* GETBUF */
/**********/
int old_getbuf(char * buf, int fd, int max) {
// Read up to max chars into supplied buf. Return number
// of chars read or negative error code if applicable
// This is version for the A1100C three-phase meter.
	int numread, bcc;
sync:
	numread = 0;
	bcc = 0;
	buf[numread] = getbyte(fd);
	if (buf[numread] != 1) {
		DEBUG fprintf(debugfp, "Warning: header is 0x%02x not 0x01, discarding\n", buf[numread]);
		goto sync;
	}
	for (numread = 1; buf[numread-2] != 3; numread++) {
		if (numread >= max) {
			DEBUG fprintf(debugfp, "Error - exceeded buffer size of %d as numread = %d\n", max, numread);
			return numread;
		}
		buf[numread] = getbyte(fd);
		DEBUG if (buf[numread] >= 0x20 && buf[numread] < 0x7f) 
			fprintf(debugfp, "%c", buf[numread]);
		else 
			fprintf(debugfp, "[%02x]", buf[numread]);
	}
	DEBUG fprintf(debugfp, "Found ETX at byte %d (0x%02x) and checksum is 0x%02x\n", numread, buf[numread-2], buf[numread-1]);
	if (numread != 328) {
		DEBUG fprintf(debugfp, "Error - got %d bytes instead of 328\n", numread);
	}
	buf[numread] = 0;	// Null terminate it!
	return numread;
}

/* TRANSLATE */
void translate() {
	// Halve the number of bytes by decoding them.
	
	// Set data.count to 0 if error found.
	
	// Sanity checks: Buffer must hold 2 x 328 = 656 bytes.
	// Bte 0 must be 0x01 - SOH
	// Byte 1 = 'O'
	// Byte 2 = 'B'
	// Byte 3 = 0x02 - STX
	// Byte 326 must be ETX
	// Byte 327 is checksum
	
	int i, bcc;
	if (data.count != 656) {
		sprintf(buffer, "WARN " PROGNAME " Got %d instead of 656 - discarding", data.count);
		data.count = 0;
		logmsg(WARN, buffer);
		return;
	}
	
	for (i = 0; i < data.count / 2; i++) {
		int val;
		val = trans[data.buf[i * 2]] + (trans[data.buf[i * 2 + 1]] << 4);
		if (val > 255) {
			fprintf(stderr, "Translate[%d] = invalid byte pair %02x %02x\n", i, data.buf[i*2], data.buf[i*2+1]);
		}
		data.buf[i] = val;
	}
	if (data.buf[0] != 0x01) {
		sprintf(buffer, "WARN " PROGNAME " Got %d instead of SOH (0x01) - discarding", data.buf[0]);
		data.count = 0;
		logmsg(WARN, buffer);
		return;
	}
	if (data.buf[1] != 'O') {
		sprintf(buffer, "WARN " PROGNAME " Got 0x%02x instead of 0x%02x - discarding", data.buf[1], 'O');
		data.count = 0;
		logmsg(WARN, buffer);
		return;
	}
	if (data.buf[2] != 'B') {
		sprintf(buffer, "WARN " PROGNAME " Got 0x%02x instead of 0x%02x - discarding", data.buf[2], 'B');
		data.count = 0;
		logmsg(WARN, buffer);
		return;
	}	
	if (data.buf[3] != 0x02) {
		sprintf(buffer, "WARN " PROGNAME " Got 0x%02x instead of STX(0x01) - discarding", data.buf[2]);
		data.count = 0;
		logmsg(WARN, buffer);
		return;
	}	
	if (data.buf[326] != 0x03) {
		sprintf(buffer, "WARN " PROGNAME " Got 0x%02x instead of ETX(0x01) - discarding", data.buf[326]);
		data.count = 0;
		logmsg(WARN, buffer);
		return;
	}
	bcc = 0;
	for (i  =0; i < 326; i++) bcc += data.buf[i];
	if (data.buf[327] != bcc) {
		sprintf(buffer, "WARN " PROGNAME " Got 0x%02x instead of 0x%02x for checksum  - discarding", data.buf[2], bcc);
		data.count = 0;
		logmsg(WARN, buffer);
		return;
	}	
}
	
/**********/
/* GETBUF */
/**********/
int getbuf(int max, int tmout) {
	// Read up to max chars into supplied buf. Return number
	// of chars read or negative error code if applicable
	int ready, numtoread, now;
	fd_set readfd; 
	struct timeval timeout;
	data.escape = 0;
	// numread = 0;
	numtoread = max;
	DEBUG2 fprintf(stderr, "Getbuf entry %d count=%d ", max ,data.count);
	
	while(1) {
		FD_ZERO(&readfd);
		FD_SET(commfd, &readfd);
		timeout.tv_sec = tmout / 1000;
		timeout.tv_usec = (tmout % 1000) * 1000;	 // 0.5sec
		ready = select(commfd + 1, &readfd, NULL, NULL, &timeout);
		DEBUG4 {
			gettimeofday(&timeout, NULL);
			fprintf(stderr, "%03ld.%03d ", timeout.tv_sec%100, timeout.tv_usec / 1000);
		}
		if (ready == 0) {
			DEBUG2 fprintf(stderr, "Gotbuf %d bytes ", data.count);
			return data.count;		// timed out - return what we've got
		}
		DEBUG4 fprintf(stderr, "Getbuf: before read1 ");
		now = read(commfd, data.buf + data.count, 1);
		DEBUG4 fprintf(stderr, "After read1\n");
		DEBUG3 fprintf(stderr, "0x%02x ", data.buf[data.count]);
		if (now < 0)
			return now;
		if (now == 0) {
			sprintf(buffer, "ERROR " PROGNAME " %d fd %d was ready but got no data", controllernum, commfd);
			logmsg(ERROR, buffer);
			// VBUs / LAN  - can't use standard Reopenserial as device name hostname: port is not valid
			commfd = reopenSerial(commfd, serialName, BAUD, 0, CS8, 1);
			continue;
		}
		
		data.count += now;
		numtoread -= now;
		DEBUG3 fprintf(stderr, "[%d] ", data.count - now);
		if (numtoread == 0) return data.count;
		if (numtoread < 0) {	// CANT HAPPEN
			sprintf(buffer, "ERROR " PROGNAME " %d buffer overflow - increase max from %d (numtoread = %d numread = %d)\n", 
					controllernum, max, numtoread, data.count);
			logmsg(ERROR, buffer);
			return data.count;
		}
	}
}

/***********/
/* GETBYTE */
/***********/
unsigned char getbyte(int fd) {
	// Get and translate a single byte from two input bytes
	unsigned char readbuf[2];
	int val;
	while ((val = read(fd, readbuf, 2)) < 2) {		// discard initial short reads;
		DEBUG2 fprintf(debugfp, "Discarding short read of %d bytes\n", val);
		if (val < 0)
			return val;	// error with read
		usleep(1);
	}
	val = trans[readbuf[0]] + (trans[readbuf[1]] << 4);
	if (val >= 0xff) {
		DEBUG fprintf(debugfp, "Warning: discarding invalid byte %02x %02x\n", readbuf[0], readbuf[1]);
		val = 0;
	}
	DEBUG2 fprintf(debugfp, "Getbyte: %x ", val);
	return val & 0xFF;
}

/*************/
/* WRITEDATA */
/*************/
void writedata() {
// write a data record of two values
// The data is the XXX is the string '1.8.0(xxxxxxxx.xxkWh)' and the first 'x' is at offset 170.
// Total message length is 328, with ETX at buf[326].
	char msg[60], *cp;
	int i, sum;
	float val;
	sum = 0;
	for (i = 0; i < 327; i++) sum += data.buf[i];
	if (((sum + data.buf[327]) & 0xff) != 0xFF) { logmsg(WARN, "WARN " PROGNAME " CHECKSUM FAILURE");
		DEBUG fprintf(debugfp, "Got sum as %x, buf[327] = %x\n", sum, data.buf[327]);
		return;
	}
	
	val = strtof(&data.buf[170], &cp);
	sprintf(msg, "meter 1 %.3f", val);	
	sockSend(sockfd[0], msg);
}

/************/
/* PRINTBUF */
/************/	
void printbuf() {
// Write it out as three consecutive info messages with unique headers
// INFO Elster config
// INFO Elster registers
// INFO Elster status

	char buffer[256];
	char space[] = " ", closebracket[] = ")";
	char *cp;
	char *buf = & data.buf[0];

	strcpy(buffer, "INFO " PROGNAME " config ");
	cp = strtok(buf + 11, closebracket);			// Product Code (Elster A1100)
	strcat(buffer, cp);
	strcat(buffer, space);
	cp = strtok(buf + 30, closebracket);			// Firmware Rev ((2-01666-E)
	strcat(buffer, cp);
	strcat(buffer, " Mfg:");
	cp = strtok(buf + 47, closebracket);			// Manufacturer Number (000000)
	strcat(buffer, cp);
	strcat(buffer, " Ser:");
	cp = strtok(buf + 60, closebracket);			// Utility Serial Number (-------07000840)
	strcat(buffer, cp);
	strcat(buffer, " Cfg:");						
	cp = strtok(buf + 83, closebracket);			// Configuration Number
	strcat(buffer, cp);
	logmsg(INFO, buffer);

	sprintf(buffer, "INFO " PROGNAME " registers R1Imp:");
	cp = strtok(buf + 94, closebracket);		// Rate 1 Import
	strcat(buffer, cp);
	strcat(buffer, " Exp:");
	cp = strtok(buf + 113, closebracket);		// Rate 1 export
	strcat(buffer, cp);
	strcat(buffer, " R2Imp:");
	cp = strtok(buf + 132, closebracket);		// Rate 2 import
	strcat(buffer, cp);
	strcat(buffer, " Exp:");
	cp = strtok(buf + 151, closebracket);		// Rate 2 export
	strcat(buffer, cp);
	strcat(buffer, " CumImp:");					// Cumulative import
	cp = strtok(buf + 170, closebracket);
	strcat(buffer, cp);
	strcat(buffer, " Exp:");				
	cp = strtok(buf + 189, closebracket);		// Cumulative export
	strcat(buffer, cp);
	logmsg(INFO, buffer);
	
	
	sprintf(buffer, "INFO " PROGNAME " status St: 0x%02lx Phase: 0x%02lx Err: 0x%02lx", strtol(buf + 209, NULL, 16),
		strtol(buf + 219, NULL, 16), strtol(buf + 230, NULL, 16));	// Status, Phase Fail, Errors
	strcat(buffer, " Hours R1:");
	cp = strtok(buf + 240, closebracket);		// Hours Rate 1
	strcat(buffer, cp);
	strcat(buffer, " R2:");
	cp = strtok(buf + 257, closebracket);		// Hours Rate 2
	strcat(buffer, cp);
	strcat(buffer, " Counts PF: ");
	cp = strtok(buf + 274, closebracket);		// Counts power fail
	strcat(buffer, cp);
	strcat(buffer, " WD:");
	cp = strtok(buf + 289, closebracket);		// Cnouts watchdog reset
	strcat(buffer, cp);
	strcat(buffer, " Rev: ");
	cp = strtok(buf + 304, closebracket);		// Reverse energy events
	strcat(buffer, cp);
	strcat(buffer, " PhaseFail:");
	cp = strtok(buf + 319, closebracket);		// Phase fail events
	strcat(buffer, cp);
	logmsg(INFO, buffer);
}

void init_table() {
// Initialise translation table
memset(&trans, sizeof(trans), 0xff);
trans[0x55] = 0;
trans[0x57] = 1;
trans[0x5d] = 2;
trans[0x5f] = 3;
trans[0x75] = 4;
trans[0x77] = 5;
trans[0x7d] = 6;
trans[0x7f] = 7;
trans[0xd5] = 8;
trans[0xd7] = 9;
trans[0xdd] = 10;
trans[0xdf] = 11;
trans[0xf5] = 12;
trans[0xf7] = 13;
trans[0xfd] = 14;
trans[0xff] = 15;
}
