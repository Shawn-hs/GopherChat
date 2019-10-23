#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/timeb.h>
#include <fcntl.h>
#include <stdarg.h>
#include <poll.h>
#include <signal.h>
#include <iostream>
#include <fstream>
#define MAX_REQUEST_SIZE 256
#define MAX_CONCURRENCY_LIMIT 32
using namespace std;

int nConns;
struct pollfd peers[MAX_CONCURRENCY_LIMIT+1];
void Error(const char * format, ...) {
	char msg[4096];
	va_list argptr;
	va_start(argptr, format);
	vsprintf(msg, format, argptr);
	va_end(argptr);
	fprintf(stderr, "Error: %s\n", msg);
	exit(-1);
}

void Log(const char * format, ...) {
	char msg[2048];
	va_list argptr;
	va_start(argptr, format);
	vsprintf(msg, format, argptr);
	va_end(argptr);
	fprintf(stderr, "%s\n", msg);
}

void SetNonBlockIO(int fd) {
	int val = fcntl(fd, F_GETFL, 0);
	if (fcntl(fd, F_SETFL, val | O_NONBLOCK) != 0) {
		Error("Cannot set nonblocking I/O.");
	}
}

void RemoveConnection(int i) {
	close(peers[i].fd);
	if (i < nConns) {
		memmove(peers + i, peers + i + 1, (nConns-i) * sizeof(struct pollfd));
	}
	nConns--;

}
int Send_NonBlocking(int sockFd, string data, int len, struct pollfd * pPeer){
    int n = send(sockFd, data, len, 0);
    if(n <= 0 && (errno == EWOULDBLOCK)) {
      pPeer->events |= POLLWRNORM;
			return 0;
    } else if (n < 0 && (errno == ECONNRESET || errno == EPIPE)) {
			Log("Connection closed.");
			close(sockFd);
			return -1;
		} else {
			Error("Unexpected send error %d: %s", errno, strerror(errno));
		}
    pPeer->events &= ~POLLWRNORM;
    return 0;
}

int Recv_NonBlocking(int sockFd, string data, struct pollfd * pPeer) {
    int n = recv(sockFd, data, MAX_REQUEST_SIZE, 0);
    if(n == 0 || (n < 0 && errno == ECONNRESET)) {
      Log("Connection closed.");
			close(sockFd);
			return -1;
    } else if (n < 0 && (errno == EWOULDBLOCK)) {
			//The socket becomes non-readable. Exit now to prevent blocking.
			//OS will notify us when we can read
			return 0;
		} else {
			Error("Unexpected recv error %d: %s.", errno, strerror(errno));
		}
    return 0;
}

int checkComm(string s) {
	string temp = strcpy(temp,s);
	char * comm = strtok(temp, ' ');
	if (strcmp("REGISTER",comm) == 0 ){
		return 1;
	} else if (strcmp("LOGIN",comm) == 0 ){
		return 2;
	} else if (strcmp("LOGOUT",comm) == 0 ){
		return 3;
	} else if (strcmp("SEND",comm) == 0 ){
		return 4;
	} else if (strcmp("SEND2",comm) == 0 ){
		return 5;
	} else if (strcmp("SENDA",comm) == 0 ){
		return 6;
	} else if (strcmp("SENDA2",comm) == 0 ){
		return 7;
	} else if (strcmp("SENDF",comm) == 0 ){
		return 8;
	} else if (strcmp("SENDF2",comm) == 0 ){
		return 9;
	} else if (strcmp("LIST",comm) == 0 ){
		return 10;
	} else if (strcmp("DELAY",comm) == 0 ){
		return 11;
	} else {
		Error("Invalid Command");
	}
}
int userRegist(char* username, char* psw ){
	if(strlen(username) > 8 || strlen(username)<4 || strlen(psw)>8 || strlen(psw)<4){
		Log("The length of username and password must longer than 4 and shorter than 8.");
	}
	else{
		char usrname[256];
		int lineNum;
		ofstream userData;
		userData.open("userinfo.txt",ios::out|ios::in);
		while (!userData.eof()){
			lineNum++;
		}
		int userNum = lineNum/3;
		if(userData << userNum << ','<<("%s",username )<< ','<< ("%s\n",psw) << endl) {
					return 1;
		}
		return 0;
	}
}
int userLogin(string username, string psw){
	ifstream userData;
	string name;
	string pwd;
	string id;
	userData.open("userinfo.txt",ios::out|ios::in);
	while(!userData.eof()){
		string data;
		getline(userData,data,',');
		id = data;
		getline(userData,data,',');
		name = data;
		getline(userData,data,',');
		pwd = data;
		if(username == name && psw == pwd) return 1;
	}
	return 0;
}
void userLogout(int i){
	RemoveConnection(i);
}

void Set_Server(int svrPort, int maxConcurrency) {
  string recvBuf ;
  pair<int,string> loggedInUser[MAX_CONCURRENCY_LIMIT];
  int listenFD = socket(AF_INET, SOCK_STREAM, 0);
  if (listenFD < 0) {
    Error("Cannot create listening socket");
  }
  SetNonBlockIO(listenFD);

  struct sockaddr_in serverAddr;
  memset(&serverAddr, 0, sizeof(sockaddr_in));
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons((unsigned short) svrPort);
  serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	int optval = 1;
  int r = setsockopt(listenFD, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	if (r != 0) {
		Error("Cannot enable SO_REUSEADDR option.");
	}
  signal(SIGPIPE, SIG_IGN);

  if(bind(listenFD, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) != 0) {
    Error("Cannot bind to port %d", svrPort);
  }

  if(listen(listenFD, 16) != 0) {
    Error("Cannot listen to port %d.", svrPort);
  }
  nConns = 0;
  memset(peers, 0, sizeof(peers));
  peers[0].fd = listenFD;
  peers[0].events = POLLRDNORM;

  int conneID = 0;

  while(1) {
    int nReady = poll(peers, nConns + 1, -1);
    if(nReady < 0) {
      Error("Invalid poll return valude.");
    }
    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);
    if((peers[0].revents & POLLRDNORM) && (nConns < MAX_CONCURRENCY_LIMIT)) {
      int fd = accept(listenFD, (struct sockaddr *)&clientAddr, &clientAddrLen);
      if(fd != -1) {
        SetNonBlockIO(fd);
        loggedInUser[nConns].first = fd;
        nConns ++ ;
        peers[nConns].fd =fd ;
        peers[nConns].events = POLLRDNORM;
        peers[nConns].revents = 0;
      }
      if(--nReady < 0) continue;
    }
    for(int i=1; i<= nConns; i++) {
      if(peers[i].revents & (POLLRDNORM | POLLERR | POLLHUP)) {
        int fd = peers[i].fd;
        recvBuf = "";
        int recvd= Recv_NonBlocking(fd, (string)recvBuf, &peers[i]);
        if(recvd < 0) {
          RemoveConnection(i); // Remember to clear the loggedInUser;
          goto NEXT_CONNECTION;

        } else {
          int commID  = checkComm(recvBuf);
          switch(commID){
            case 1:
							//userRegist('aaa','bbb');
            case 2:
            //  userLogin("aaa","bbb");
            case 3:
            //  userLogout(fd);
            case 4:
              ;
            case 5:
              break;
            case 6:
              break;
            case 7:
              break;
            case 8:
              break;
            case 9:
              break;
            case 10:
              break;
            case 11:
              break;
          }

        }

      }
      NEXT_CONNECTION:
      if(--nReady <= 0) break;
    }
  }
}

int main(int argc, char * * argv) {
	if (argc != 3) {
		Log("Usage: %s [server Port] [max concurrency]", argv[0]);
		return -1;
	}

	int port = atoi(argv[1]);
	int maxConcurrency = atoi(argv[2]);
	Set_Server(port, maxConcurrency);

	return 0;
}
