#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <fcntl.h>
#include <ctype.h>

#define PARAM_NAME_MAX  (128)
#define PARAM_VALUE_MAX (128)

#ifndef TRUE
# define TRUE (1)
#endif

#ifndef FALSE
# define FALSE (0)
#endif

#define LISTENER_SLOT_MAX (1)
#define STREAMER_SLOT_MAX (8)

typedef struct {
	int readSocket;
	int writeSocket;
} LOOPBACK;

typedef struct {
	int busy;
	int slot;
	pthread_t thread;
	ushort localPort;
	LOOPBACK loopback;
	char recvData[1024];
} LISTENER;

typedef struct {
	int busy;
	int slot;
	pthread_t thread;
	int remoteSocket;
	char remoteIp[128];
	unsigned short remotePort;
	LOOPBACK loopback;
	char recvData[1024];
} STREAMER;

struct s_param {
	char name[PARAM_NAME_MAX];
	char value[PARAM_VALUE_MAX];
};

static LISTENER listeners[LISTENER_SLOT_MAX] = {{0}};
static STREAMER streamers[STREAMER_SLOT_MAX] = {{0}};

static int get_param(struct s_param *param, char *form, char cap)
{
	int param_name_len, param_value_len;
	char *eq;

	if (form[0] != cap) {
		return -1;
	}

	if ((eq = strchr(form, '=')) != NULL) {
		param_name_len = eq - (form + 1);   /*スラッシュを除く*/
		param_value_len = strlen(eq + 1);   /*等号を除く*/
	} else {
		param_name_len = strlen(form + 1);  /*スラッシュ除く*/
		param_value_len = 0;
	}

	if (param_name_len >= sizeof(param->name)) {
		return -1;
	}

	if (param_value_len >= sizeof(param->value)) {
		return -1;
	}

	strncpy(param->name, form+1, param_name_len);
	*(param->name + param_name_len) = '\0';

	strncpy(param->value, eq+1, param_value_len);
	*(param->value + param_value_len) = '\0';

	return 0;
}

static int parse_arg(int argc, char *argv[], struct s_param **paramlist, char cap)
{
	int i, param_num=0;
	char *form;
	struct s_param *param;

	if (argc > 1) {
		*paramlist = param = (struct s_param *)malloc(sizeof(struct s_param) * (argc-1));

		memset(param, 0, (sizeof(struct s_param) * (argc-1)));

		if (*paramlist == NULL) {
			printf("アボートエラー。\n");
			exit(1);
		}

		for (i=1; i<argc; i++) {  /*配列の先頭には、実行ファイル名が入っているので、これを無視*/
			form = argv[i];

			if (form == NULL) {
				printf("コマンド引数エラー。\n");
				exit(1);
			}

			if (form[0] != cap) {
				printf("コマンド引数 '%s' エラー。\n", form);
				exit(1);
			}

			get_param(param, form, cap);

			param++;
			param_num++;
		}
	}

	return param_num;
}

static int makeSockaddr(struct sockaddr_in *sockaddr, const char *ipaddr, unsigned short port)
{
	memset(sockaddr, 0, sizeof(struct sockaddr_in));
	
	sockaddr->sin_family = AF_INET;
	sockaddr->sin_port = htons(port);
	
	if (ipaddr != NULL) {
		if (inet_pton(AF_INET, ipaddr, &sockaddr->sin_addr) != 1) {
			printf("inet_pton error. (src=%s)\n", ipaddr);
			return -1;
		}
	} else {
		sockaddr->sin_addr.s_addr = htonl(INADDR_ANY);
	}

	return 0;
}

static int createLoopbackConnection(LOOPBACK *loopback)
{
	int fd[2] = {-1};

	if (socketpair(PF_UNIX, SOCK_STREAM, 0, fd) < 0) {
		printf("socketpair error: %s (errno=%#x)\n", strerror(errno), errno);
		return -1;
	}

	fcntl(fd[0], F_SETFL, O_NONBLOCK);
	fcntl(fd[1], F_SETFL, O_NONBLOCK);

	loopback->readSocket = fd[0];
	loopback->writeSocket = fd[1];

	return 0;
}

static void destroyLopbackConnection(LOOPBACK *loopback)
{
	close(loopback->readSocket);
	close(loopback->writeSocket);
}

static int tcp_send(int socket, const char *sendData)
{
	int s=socket;
	int err=0;

	printf("tcp: send \"%s\"\n", sendData);
	int sendLen = strlen(sendData);
	err = send(s, sendData, sendLen, 0);
	if (err < 0) {
		printf("send error: %s (errno=%#x)\n", strerror(errno), errno);
		return -1;
	}

	return 0;
}

static void *tcpStreamer(void *arg)
{
	STREAMER *streamer = NULL;
	fd_set fds;
	int maxFd = -1;
	int readableSocket = -1;
	ssize_t recvLen = 0;
	int err;

	streamer = (STREAMER *)arg;

	printf("streamer [%02d] : launched\n", streamer->slot);
	
	do {
		FD_ZERO(&fds);
		FD_SET(streamer->remoteSocket, &fds);
		FD_SET(streamer->loopback.readSocket, &fds);
		maxFd = (streamer->remoteSocket > streamer->loopback.readSocket) ?
			streamer->remoteSocket : streamer->loopback.readSocket;

		printf("streamer [%02d] : waiting...\n", streamer->slot);
		err = select(maxFd+1, &fds, NULL, NULL, NULL);
		if (err < 0) {
			printf("select error: %s (errno=%#x)\n", strerror(errno), errno);
			goto _clean_;
		}
		if (err == 0) {
			printf("select timeout");
			continue;
		}

		if (FD_ISSET(streamer->remoteSocket, &fds)) {
			readableSocket = streamer->remoteSocket;
			recvLen = recv(readableSocket, streamer->recvData, sizeof(streamer->recvData), 0);
			if (recvLen < 0) {
				printf("recv error. %s errno=%#x\n", strerror(errno), errno);
				break;
			} else if(recvLen == 0) {
				printf("streamer [%02d] : remote connection was shutdown\n", streamer->slot);
				break;
			}

			streamer->recvData[recvLen] = '\0';
			printf("streamer [%02d] : recv \"%s\" from %s:%u\n", streamer->slot, streamer->recvData, streamer->remoteIp, streamer->remotePort);

		} else if (FD_ISSET(streamer->loopback.readSocket, &fds)) {
			readableSocket = streamer->loopback.readSocket;
			recvLen = recv(readableSocket, streamer->recvData, sizeof(streamer->recvData), 0);
			if (recvLen < 0) {
				printf("recv error. %s errno=%#x\n", strerror(errno), errno);
				break;
			} else if(recvLen == 0) {
				printf("streamer [%02d] : loopback connection was shutdown\n", streamer->slot);
				break;
			}

			streamer->recvData[recvLen] = '\0';

			if (strcmp(streamer->recvData, "shutdown") == 0) {
				break;
			} else if (strcmp(streamer->recvData, "send") == 0) {
				tcp_send(streamer->remoteSocket, "Hello!");
			}
		}

	} while (TRUE);

_clean_:
	if (streamer->remoteSocket >= 0) {
		close(streamer->remoteSocket);
		streamer->remoteSocket = -1;
	}

	destroyLopbackConnection(&streamer->loopback);

	printf("streamer [%02d] : has ended\n", streamer->slot);

	streamer->busy = 0;

	return NULL;
}

static void shutdownTcpStreamer(STREAMER *streamer)
{
	if (streamer->busy) {
		if (tcp_send(streamer->loopback.writeSocket, "shutdown") != 0) {
			printf("send error\n");
		}
	}
}

static int startTcpStreamer(LISTENER *listener, int listenSocket)
{
	STREAMER *streamer = NULL;
	struct sockaddr_in remoteAddr;
	int remoteSocket = -1;
	int i;
	
	for (i=0; i<STREAMER_SLOT_MAX; i++) {
		if (streamers[i].busy == 0) {
			streamer = &streamers[i];
			streamer->slot = i;
			break;
		}
	}

	socklen_t tmp = sizeof(remoteAddr);
	remoteSocket = accept(listenSocket, (struct sockaddr *)&remoteAddr, &tmp);
	if (remoteSocket < 0) {
		printf("accept error. errno=%#x\n", errno);
		goto _clean_;
	}

	if (streamer == NULL) {
		printf("streamer slot full");
		goto _clean_;
	}

	streamer->remotePort = ntohs(remoteAddr.sin_port);
	if (inet_ntop(AF_INET, &remoteAddr, streamer->remoteIp, sizeof(streamer->remoteIp)) == NULL) {
		printf("accept error. errno=%#x\n", errno);
		goto _clean_;
	}

	printf("listener [%02d] : accept connection from %s:%u\n", listener->slot, streamer->remoteIp, streamer->remotePort);

	if (createLoopbackConnection(&streamer->loopback) != 0) {
		printf("create loopback connection error");
		goto _clean_;
	}

	streamer->remoteSocket = remoteSocket;

	streamer->busy = 1;

	if (pthread_create(&streamer->thread, NULL, tcpStreamer, streamer) != 0) {
		printf("create streamer error");
		goto _clean_;
	}

	return 0;

_clean_:
	if (remoteSocket != -1) {
		close(remoteSocket);
		remoteSocket = -1;
	}
	if (streamer != NULL) {
		destroyLopbackConnection(&streamer->loopback);
	}
	return -1;
}

static void *tcpListener(void *arg)
{
	LISTENER *listener = NULL;
	int listenSocket = -1;
	fd_set fds;
	struct sockaddr_in localAddr;
	int recvLen;
	int err = 0;

	listener = (LISTENER *)arg;

	printf("listener [%02d] : launched\n", listener->slot);

	// ローカルアドレス (リスナーのソケットのbind先)
	memset(&localAddr, 0, sizeof(localAddr));
	localAddr.sin_port = htons(listener->localPort);
	localAddr.sin_family = AF_INET;
	localAddr.sin_addr.s_addr = htonl(INADDR_ANY);

	listenSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (listenSocket < 0) {
		printf("socket error. %s errno=%#x\n", strerror(errno), errno);
		goto _clean_;
	}

	err = bind(listenSocket, (struct sockaddr*)&localAddr, sizeof(localAddr));
	if (err != 0) {
		printf("bind error. %s errno=%#x\n", strerror(errno), errno);
		goto _clean_;
	}

	err = listen(listenSocket, 1);
	if (err != 0) {
		printf("listen error. %s errno=%#x\n", strerror(errno), errno);
		goto _clean_;
	}

	do {
		FD_ZERO(&fds);
		FD_SET(listenSocket, &fds);
		FD_SET(listener->loopback.readSocket, &fds);

		printf("listener [%02d] : listening...\n", listener->slot);
		err = select(listenSocket+1, &fds, NULL, NULL, NULL);
		if (err < 0) {
			printf("select error. errno=%#x\n", errno);
			goto _clean_;
		}
		if (err == 0) {
			printf("select timeout.\n");
			continue;
		}
		
		if (FD_ISSET(listenSocket, &fds)) {
			if (startTcpStreamer(listener, listenSocket) != 0) {
				printf("start streamer error\n");
				continue;
			}
		}
		if (FD_ISSET(listener->loopback.readSocket, &fds)) {
			recvLen = recv(listener->loopback.readSocket, listener->recvData, sizeof(listener->recvData), 0);
			if (recvLen < 0) {
				printf("recv error. %s errno=%#x\n", strerror(errno), errno);
				break;
			} else if(recvLen == 0) {
				printf("listener [%02d] : loopback connection was shutdown", listener->slot);
				break;
			}

			listener->recvData[recvLen] = '\0';

			if (strcmp(listener->recvData, "shutdown") == 0) {
				break;
			}
		}

	} while (TRUE);
	
_clean_:
	if (listenSocket >= 0) {
		close(listenSocket);
		listenSocket = -1;
	}
	
	return NULL;
}

static void shutdownTcpListener(void)
{
	int err;
	int i;

	for (i=0; i<LISTENER_SLOT_MAX; i++) {
		if (listeners[i].busy) {
			if (tcp_send(listeners[i].loopback.writeSocket, "shutdown") != 0) {
				printf("send error\n");
				continue;
			}

			err = pthread_join(listeners[i].thread, NULL);
			if (err != 0) {
				printf("join listener [%02d] error: %s (errno=%#x)\n", i, strerror(errno), errno);
				continue;
			}

			destroyLopbackConnection(&listeners[i].loopback);

			listeners[i].busy = 0;

			printf("listener [%02d] : has ended\n", i);
		}
	}
}

static int startTcpListener(unsigned short localPort)
{
	LISTENER *listener = NULL;
	int i;
	
	for (i=0; i<LISTENER_SLOT_MAX; i++) {
		if (listeners[i].busy == 0) {
			listener = &listeners[i];
			listener->slot = i;
			break;
		}
	}

	if (listener == NULL) {
		printf("listener slot full");
		return -1;
	}

	listener->localPort = localPort;

	if (createLoopbackConnection(&listener->loopback) != 0) {
		printf("create loopback connection error");
		return -1;
	}

	if (pthread_create(&listener->thread, NULL, tcpListener, listener) != 0) {
		printf("create listener error");
		destroyLopbackConnection(&listener->loopback);
		return -1;
	}

	listener->busy = 1;

	return 0;
}

static void chop(char *p, int len)
{
	int i=0;
	for (i=0; i<len; i++) {
		char *pp = &p[len - i - 1];
		if (isspace(*pp)) {
			*pp = '\0';
		} else {
			break;
		}
	}
}

/*usage:
 * ./a.out /localPort=50000
 *
 * localPort: mandatory.
 */
int main(int argc, char *argv[])
{
	int i, n;
	struct s_param *param;
	int localPort=-1;
	struct timeval timeout;
	fd_set fds;
	static char recvData[1024];
	int recvLen;
	int err;

	if (argc == 1) {
		printf("param error.\n");
		return 0;
	}

	n = parse_arg(argc, argv, &param, '/');

	for (i=0; i<n; i++) {
		if (strcmp(param->name, "localPort") == 0) {
			sscanf(param->value, "%d\n", &localPort);
		}
		param++;
	}

	if (localPort == -1) {
		printf("localPort error.\n");
		return -1;
	}

	startTcpListener(localPort);

	for (;;) {
		FD_ZERO(&fds);
		FD_SET(0, &fds); //標準入力

		timeout.tv_sec = 0; 
		timeout.tv_usec = 100000;
		err = select(1, &fds, NULL, NULL, &timeout);
		if (err < 0) {
			printf("select error. %s (errno=%#x)\n", strerror(errno), errno);
			break;
		}
		if (err == 0) {
			fflush(stdout); //select実行中は標準出力が止まる???
			continue;
		}

		if (FD_ISSET(0, &fds)) {
			recvLen = read(0, recvData, sizeof(recvData));
			if (recvLen < 0) {
				printf("recv error. %s errno=%#x\n", strerror(errno), errno);
				break;
			} else if(recvLen == 0) {
				printf("connection was shutdown");
				break;
			}

			recvData[recvLen] = '\0';
			chop(recvData, recvLen);
			//printf("%s", recvData);

			if (strncmp(recvData, "shutdown", 8) == 0) {
				shutdownTcpListener();
				for (i=0; i<STREAMER_SLOT_MAX; i++) {
					shutdownTcpStreamer(&streamers[i]);
				}
				break;
			}

			if (strncmp(recvData, "send ", 5) == 0) {
				int streamerSlot=0;
				sscanf(recvData, "send %d", &streamerSlot);
				tcp_send(streamers[streamerSlot].loopback.writeSocket, "send");
			}
		}
	}

	return 0;
}

