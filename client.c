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

#define PARAM_NAME_MAX  (128)
#define PARAM_VALUE_MAX (128)

struct s_param {
	char name[PARAM_NAME_MAX];
	char value[PARAM_VALUE_MAX];
};

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
			printf("inet_pton error. (src=%s)", ipaddr);
			return -1;
		}
	} else {
		sockaddr->sin_addr.s_addr = htonl(INADDR_ANY);
	}

	return 0;
}

static void udp_send(char *sendData, char *remoteIp, unsigned short remotePort/*, unsigned short localPort*/)
{
	int s=-1;
	int err=0;
	//struct sockaddr_in localAddr;
	struct sockaddr_in remoteAddr;

	if (makeSockaddr(&remoteAddr, remoteIp, remotePort) != 0) {
		printf("remoteIp error");
		goto _clean_;
	}
	
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		printf("socket error: %s (errno=%#x)\n", strerror(errno), errno);
		goto _clean_;
	}

	printf("udp: send \"%s\" to %s:%d\n", sendData, remoteIp, remotePort);
	int sendLen = strlen(sendData);
	err = sendto(s, sendData, sizeof(sendData), 0, (struct sockaddr *)&remoteAddr, sizeof(remoteAddr));
	if (err < 0) {
		printf("send error: %s (errno=%#x)\n", strerror(errno), errno);
		goto _clean_;
	}

_clean_:
	if (s >= 0) {
		printf("close socket\n");
		close(s);
		s = -1;
	}
}

static void tcp_send(char *sendData, char *remoteIp, unsigned short remotePort/*, unsigned short localPort*/)
{
	int s=-1;
	int err=0;
	//struct sockaddr_in localAddr;
	struct sockaddr_in remoteAddr;

	//makeSockaddr(&localAddr, NULL, localPort);
	if (makeSockaddr(&remoteAddr, remoteIp, remotePort) != 0) {
		printf("remoteIp error");
		goto _clean_;
	}
	
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		printf("socket error: %s (errno=%#x)\n", strerror(errno), errno);
		goto _clean_;
	}

	//printf("tcp: bind to 0.0.0.:%hu\n", localPort);
	//err = bind(s, (struct sockaddr*)&localAddr, sizeof(localAddr));
	//if (err != 0) {
	//	printf("bind error: %s (errno=%#x)\n", strerror(errno), errno);
	//	goto _clean_;
	//}

	printf("tcp: connet to %s:%hu\n", remoteIp, remotePort);
	err = connect(s, (struct sockaddr*)&remoteAddr, sizeof(remoteAddr));
	if (err != 0) {
		printf("connect error: %s (errno=%#x)\n", strerror(errno), errno);
		goto _clean_;
	}

	printf("tcp: send \"%s\"\n", sendData);
	int sendLen = strlen(sendData);
	err = send(s, sendData, sizeof(sendData), 0);
	if (err < 0) {
		printf("send error: %s (errno=%#x)\n", strerror(errno), errno);
		goto _clean_;
	}

_clean_:
	if (s >= 0) {
		printf("close socket\n");
		close(s);
		s = -1;
	}
}

/*usage:
 * ./a.out /protocol=tcp /sendData=Hoge! /remoteIp=192.168.0.6 /remotePort=50000
 * ./a.out /protocol=udp /sendData=Hoge! /remoteIp=192.168.0.6 /remotePort=50000
 *
 * protocol: optional. default is tcp.
 * sendData: optional. default is "Hello!"
 * remoteIp: mandatory.
 * remotePort: mandatory.
 */
int main(int argc, char *argv[])
{
	int i, n;
	struct s_param *param;
	char *protocol="tcp";
	char *sendData="Hello!";
	char *remoteIp=NULL;
	int remotePort=-1;

	if (argc == 1) {
		printf("param error.\n");
		return 0;
	}

	n = parse_arg(argc, argv, &param, '/');

	for (i=0; i<n; i++) {
		if (strcmp(param->name, "protocol") == 0) {
			protocol = param->value;
		} else if (strcmp(param->name, "sendData") == 0) {
			sendData = param->value;
		} else if (strcmp(param->name, "remoteIp") == 0) {
			remoteIp = param->value;
		} else if (strcmp(param->name, "remotePort") == 0) {
			sscanf(param->value, "%d", &remotePort);
		}/* else if (strcmp(param->name, "localPort") == 0) {
			sscanf(param->value, "%d", &localPort);
		}*/
		param++;
	}

	if (remoteIp == NULL) {
		printf("remoteIP error.\n");
		return -1;
	}

	if (remotePort == -1) {
		printf("remotePort error.\n");
		return -1;
	}

	if (strcmp(protocol, "tcp") == 0) {
		tcp_send(sendData, remoteIp, (unsigned short)remotePort/*, localPort*/);
	} else if (strcmp(protocol, "udp") == 0) {
		udp_send(sendData, remoteIp, (unsigned short)remotePort/*, localPort*/);
	} else {
		printf("protocol error.\n");
		return -1;
	}

	return 0;
}

