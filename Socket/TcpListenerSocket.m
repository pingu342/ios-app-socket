#import "TcpListenerSocket.h"
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
	ushort listenPort;
} LISTENER;

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

static void streamClientCallBack(CFReadStreamRef stream, CFStreamEventType eventType, void *clientCallBackInfo)
{
	NSLog(@"callback! eventType=%@", getPrintableCFStreamEventType(eventType));
	
	if (eventType == kCFStreamEventHasBytesAvailable) {
		//static UInt8 buffer[1024];
		//CFIndex readBytes;
		//readBytes = CFReadStreamRead(stream, buffer, sizeof(buffer));
		//NSLog(@"readBytes=%ld", readBytes);
	}
}

static void setVoipTag(int fd)
{
	// どうやっても、リスナーのソケットに届いた着信をバックグラウンドで処理することはできない
	// CFReadStreamがTCPのリッスンに対応してないみたい
	
	CFReadStreamRef readStreamRef;
	CFWriteStreamRef writeStreamRef;
	
	CFStreamCreatePairWithSocket(kCFAllocatorDefault, fd, &readStreamRef, &writeStreamRef);
	
	CFReadStreamSetProperty(readStreamRef, kCFStreamNetworkServiceType, kCFStreamNetworkServiceTypeVoIP);
	CFWriteStreamSetProperty(writeStreamRef, kCFStreamNetworkServiceType, kCFStreamNetworkServiceTypeVoIP);

	CFStreamClientContext clientContext = {0, NULL, NULL, NULL, NULL};
	CFOptionFlags flags = kCFStreamEventOpenCompleted + kCFStreamEventHasBytesAvailable + kCFStreamEventCanAcceptBytes + kCFStreamEventErrorOccurred +  kCFStreamEventEndEncountered;
	CFReadStreamSetClient(readStreamRef, flags, streamClientCallBack, &clientContext);
	
	CFRunLoopRef runLoopRef = CFRunLoopGetMain();
	CFReadStreamScheduleWithRunLoop(readStreamRef, runLoopRef, kCFRunLoopDefaultMode);
	
	CFStreamStatus status = CFReadStreamGetStatus(readStreamRef);
	NSLog(@"status=%@", getPrintableCFStreamStatus(status));
	
	if (CFReadStreamOpen(readStreamRef)) {
		NSLog(@"read stream was successfully opened");
	} else {
		NSLog(@"read stream open failed");
	}
	
	if (CFWriteStreamOpen(writeStreamRef)) {
		NSLog(@"write stream was successfully opened");
	} else {
		NSLog(@"write stream open failed");
	}
}

static void *tcpListener(void *arg)
{
	LISTENER *listener = NULL;
	int listenSocket = -1;
	int remoteSocket = -1;
	fd_set fds;
	struct sockaddr_in localAddr;
	struct sockaddr_in remoteAddr;
	static char recvData[1024] = "";
	ssize_t recvLen = 0;
	static char remoteIpAddr[128] = "";
	ushort remotePort = 0;
	int err = 0;

	listener = (LISTENER *)arg;

	// ローカルアドレス (このアドレス宛に届くTCP接続要求を監視する)
	memset(&localAddr, 0, sizeof(localAddr));
	localAddr.sin_port = htons(listener->listenPort);
	localAddr.sin_family = AF_INET;
	localAddr.sin_addr.s_addr = htonl(INADDR_ANY);

	// ソケットを作ってローカルアドレスにバインドしてリッスンする
	listenSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (listenSocket < 0) {
		NSLog(@"socket error. errno=%#x",errno);
		goto _clean_;
	}

	err = bind(listenSocket, (struct sockaddr*)&localAddr, sizeof(localAddr));
	if (err != 0) {
		NSLog(@"bind error. errno=%#x",errno);
		goto _clean_;
	}

	err = listen(listenSocket, 1);
	if (err != 0) {
		NSLog(@"listen error. errno=%#x",errno);
		goto _clean_;
	}

	// バックグラウンドでもリッスンできるようにおまじない -> リッスンには効かない
	setVoipTag(listenSocket);
	
_listen_:
	do {
		// リモートからTCP接続要求が届くまでオヤスミ
		FD_ZERO(&fds);
		FD_SET(listenSocket, &fds);

		NSLog(@"listening...");
		err = select(listenSocket+1, &fds, NULL, NULL, NULL);
		if (err < 0) {
			NSLog(@"select error. errno=%#x",errno);
			goto _clean_;
		}
		if (err == 0) {
			NSLog(@"select timeout.");
			continue;
		}
		
		if (FD_ISSET(listenSocket, &fds)) {
			
			// リモートからのTCP接続要求を受け入れて、リモートとのデータ送受信用のソケットを得る
			socklen_t tmp = sizeof(remoteAddr);
			remoteSocket = accept(listenSocket, (struct sockaddr *)&remoteAddr, &tmp);
			if (remoteSocket < 0) {
				NSLog(@"accept error. errno=%#x",errno);
				continue;
			}
			
			remotePort = ntohs(remoteAddr.sin_port);
			if (inet_ntop(AF_INET, &remoteAddr, remoteIpAddr, sizeof(remoteIpAddr)) == NULL) {
				NSLog(@"accept error. errno=%#x",errno);
				continue;
			}
			
			// リモートからのデータ受信待ちフェーズに移行
			NSLog(@"accept connection from %s:%u", remoteIpAddr, remotePort);
		} else {
			
			// リッスンを継続
			continue;
		}

	} while (0);
	
_recvData_:
	do {
		// リモートからデータが届くまでオヤスミ
		recvLen = recv(remoteSocket, recvData, sizeof(recvData), 0);
		if (recvLen	< 0) {
			NSLog(@"recv error. errno=%#x",errno);
			break;
		} else if(recvLen == 0) {
			NSLog(@"connection was shutdown");
			break;
		}
		
		// リモートから届いたデータを表示
		recvData[recvLen] = '\0';
		NSLog(@"recv \"%s\" from %s:%u", recvData, remoteIpAddr, remotePort);
		
	} while (TRUE);
	
	// リモートとのTCP接続を閉じる
	close(remoteSocket);
	
	// リッスンに戻る
	goto _listen_;

_clean_:
	if (listenSocket >= 0) {
		close(listenSocket);
		listenSocket = -1;
	}
	if (listener != NULL) {
		free(listener);
		listener = NULL;
	}
	
	return NULL;
}

void TcpListenerSocketStart(ushort listenPort)
{
	LISTENER *listener;

	NSLog(@"Listen %u(tcp) port.", listenPort);

	// TCPポートをリッスンするスレッドを起動
	
	listener = malloc(sizeof(LISTENER));
	listener->listenPort = listenPort;

	if (pthread_create(&thread, NULL, tcpListener, listener) != 0) {
		perror("pthread_create error");
	}
}
