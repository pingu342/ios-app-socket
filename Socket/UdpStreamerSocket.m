#import "TcpStreamerSocket.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <string.h>

typedef struct {
	ushort localPort;
} STREAMER;

static NSInputStream *stream = nil;
static pthread_t thread;


static NSString *getPrintableCFStreamStatus(CFStreamStatus status)
{
	NSString *printableStatus = nil;
	switch (status) {
		case kCFStreamStatusNotOpen:
			printableStatus = @"NotOpen";
			break;
			
		case kCFStreamStatusOpening:
			printableStatus = @"Opening";
			break;
			
		case kCFStreamStatusOpen:
			printableStatus = @"Open";
			break;
			
		case kCFStreamStatusReading:
			printableStatus = @"Reading";
			break;
			
		case kCFStreamStatusWriting:
			printableStatus = @"Writing";
			break;
			
		case kCFStreamStatusAtEnd:
			printableStatus = @"AtEnd";
			break;
			
		case kCFStreamStatusClosed:
			printableStatus = @"Closed";
			break;
			
		case kCFStreamStatusError:
			printableStatus = @"Error";
			break;
			
		default:
			printableStatus = @"?";
			break;
	}
	return printableStatus;
}

static NSString *getPrintableCFStreamEventType(CFStreamEventType eventType)
{
	NSMutableString *printableEventType = [[NSMutableString alloc] init];
	if (eventType & kCFStreamEventOpenCompleted) {
		[printableEventType appendString:@"OpenComplete,"];
	} else if (eventType & kCFStreamEventHasBytesAvailable) {
		[printableEventType appendString:@"HasBytesAvailable,"];
	} else if (eventType & kCFStreamEventCanAcceptBytes) {
		[printableEventType appendString:@"CanAcceptBytes,"];
	} else if (eventType & kCFStreamEventErrorOccurred) {
		[printableEventType appendString:@"ErrorOccurred,"];
	} else if (eventType & kCFStreamEventEndEncountered) {
		[printableEventType appendString:@"EndEncountered,"];
	}
	return printableEventType;
}

static void setVoipTag(int fd)
{
	// CFReadStreamOpenまでやっておけば、バックグラウンドでも受信可能
	
	CFReadStreamRef readStreamRef;
	
	CFStreamCreatePairWithSocket(kCFAllocatorDefault, fd, &readStreamRef, NULL);
	
	CFReadStreamSetProperty(readStreamRef, kCFStreamNetworkServiceType, kCFStreamNetworkServiceTypeVoIP);
	
	CFReadStreamOpen(readStreamRef);
}

static int makeSockaddr(struct sockaddr_in *sockaddr, const char *ipaddr, unsigned short port)
{
	memset(sockaddr, 0, sizeof(struct sockaddr_in));
	
	sockaddr->sin_family = AF_INET;
	sockaddr->sin_port = htons(port);
	
	if (ipaddr != NULL) {
		if (inet_pton(AF_INET, ipaddr, &sockaddr->sin_addr) != 1) {
			printf("inet_pton error. (src=%s)", ipaddr);
			return -1;
		}
	} else {
		sockaddr->sin_addr.s_addr = htonl(INADDR_ANY);
	}
	
	return 0;
}

static void *udpStreamer(void *arg)
{
	STREAMER *streamer = NULL;
	int udpSocket = -1;
	fd_set fds;
	struct sockaddr_in localAddr;
	static char recvData[1024] = "";
	ssize_t recvLen = 0;
	int err = 0;
	
	streamer = (STREAMER *)arg;
	
	// リモートアドレス (このアドレス宛にTCP接続する)
	if (makeSockaddr(&localAddr, "0.0.0.0", streamer->localPort) != 0) {
		NSLog(@"local address error");
		goto _clean_;
	}
	
	// ソケットを作ってリモートアドレスにTCP接続する
	udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
	if (udpSocket < 0) {
		NSLog(@"socket error. errno=%#x",errno);
		goto _clean_;
	}
	
	err = bind(udpSocket, (struct sockaddr*)&localAddr, sizeof(localAddr));
	if (err != 0) {
		NSLog(@"bind error. errno=%#x",errno);
		goto _clean_;
	}
	
#if 0
	err = connect(remoteSocket, (struct sockaddr *)&remoteAddr, sizeof(remoteAddr));
	if (err != 0) {
		NSLog(@"connect error. errno=%#x",errno);
		goto _clean_;
	}
#endif
	
	setVoipTag(udpSocket);
	
	do {
		FD_ZERO(&fds);
		FD_SET(udpSocket, &fds);
		
		NSLog(@"waiting...");
		err = select(udpSocket+1, &fds, NULL, NULL, NULL);
		if (err < 0) {
			NSLog(@"select error: %s (errno=%#x)", strerror(errno), errno);
			goto _clean_;
		}
		if (err == 0) {
			NSLog(@"select timeout");
			continue;
		}
		
		if (FD_ISSET(udpSocket, &fds)) {
			recvLen = recv(udpSocket, recvData, sizeof(recvData), 0);
			if (recvLen < 0) {
				NSLog(@"recv error. %s errno=%#x", strerror(errno), errno);
				break;
			} else if(recvLen == 0) {
				NSLog(@"remote connection was shutdown");
				break;
			}
			
			recvData[recvLen] = '\0';
			
			NSLog(@"%s", recvData);
		}
		
	} while (TRUE);
	
_clean_:
	if (udpSocket >= 0) {
		close(udpSocket);
		udpSocket = -1;
	}
	if (streamer != NULL) {
		free(streamer);
		streamer = NULL;
	}
	
	return NULL;
}

void UdpStreamerSocketStart(ushort localPort)
{
	STREAMER *streamer;

	NSLog(@"Moniter %u(udp) port", localPort);
	
	// UDPポートへのパケット到着を監視するスレッドを起動
	
	streamer = malloc(sizeof(STREAMER));
	streamer->localPort = localPort;

	if (pthread_create(&thread, NULL, udpStreamer, streamer) != 0) {
		perror("pthread_create error");
	}
}
