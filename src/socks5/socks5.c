#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#include <fcntl.h>

#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
 
#include <ev.h>

#include "defs.h"
#include "liblog.h"
#include "libdaemon.h"
#include "socks5.h"

static INT32 g_state = SOCKS5_STATE_PREPARE;

static int g_sockfd = 0;
static socks5_cfg_t g_cfg = {0};
struct ev_loop *g_loop = NULL;  
struct ev_io g_io_accept;

static void help();
static INT32 check_para(int argc, char **argv);
// signal信号回调函数
static void signal_func(int sig);
// singal信号注册初始化
static void signal_init();

static INT32 socks5_srv_init(UINT16 port, INT32 backlog);
static INT32 socks5_srv_exit(int sockfd);

static INT32 socks5_sockset(int sockfd);

static void accept_cb(struct ev_loop *loop, struct ev_io *watcher, int revents);
static void read_cb(struct ev_loop *loop, struct ev_io *watcher, int revents);

int main(int argc, char **argv)
{
    if (R_ERROR == check_para(argc, argv))
    {
        PRINTF(LEVEL_ERROR, "check argument error.\n");
        return R_ERROR;
    }

    signal_init();

    PRINTF(LEVEL_INFORM, "socks5 starting ...\n");

    g_sockfd = socks5_srv_init(g_cfg.port, 10);
    if (R_ERROR == g_sockfd)
    {
        PRINTF(LEVEL_ERROR, "socks server init error.\n");
        return R_ERROR;
    }

    g_state = SOCKS5_STATE_RUNNING;

    g_loop = ev_default_loop(0);
    // 初始化,这里监听了io事件,写法参考官方文档的  
    ev_io_init(&g_io_accept, accept_cb, g_sockfd, EV_READ);  
    ev_io_start(g_loop, &g_io_accept);

    ev_loop(g_loop, 0); 

    PRINTF(LEVEL_INFORM, "time to exit.\n");
    socks5_srv_exit(g_sockfd);
    PRINTF(LEVEL_INFORM, "exit socket server.\n");
    return 0;
}

static void help()
{
    printf("Usage: socks5 [options]\n");
    printf("Options:\n");
    printf("    -p <port>       tcp listen port\n");
    printf("    -d <Y|y>        run as a daemon if 'Y' or 'y', otherwise not\n");

    printf("    -l <level>      debug log level,range [0, 5]\n");
    printf("    -h              print help information\n");
}
static INT32 check_para(int argc, char **argv)
{
    int ch;
    INT32 bdaemon = 0;

    memset(&g_cfg, 0, sizeof(g_cfg));

    g_cfg.start_time = time(NULL);
    g_cfg.port = SOCKS5_PORT;

    while ((ch = getopt(argc, argv, ":d:p:l:h")) != -1)
    {
        switch (ch)
        {
            case 'd':
                if (1 == strlen(optarg) && ('Y' == optarg[0] || 'y' == optarg[0]))
                {
                    printf("run as a daemon.\n");
                    bdaemon = 1;
                }
                break;
            case 'p':
                g_cfg.port = atoi(optarg);
                break;
            case 'l':
                if (0 > atoi(optarg) || 5 < atoi(optarg))
                {
                    printf("debug level [%s] out of range [0 - 5].\n", optarg);
                    return R_ERROR;
                }
                liblog_level(atoi(optarg));
                printf("log level [%d].\n", atoi(optarg));
                break;
            case 'h':
                help();
                exit(EXIT_SUCCESS);
                break;
            case '?':
                if (isprint(optopt))
                   printf("unknown option '-%c'.\n", optopt);
                else
                   printf("unknown option character '\\x%x'.\n", optopt);
                break;
            case ':':
                if (isprint(optopt))
                   printf("missing argment for '-%c'.\n", optopt);
                else
                   printf("missing argment for '\\x%x'.\n", optopt);
            default:
                break;
        }
    }

    if (bdaemon)
    {
        daemonize();
    }
    return R_OK;
}

static void signal_init()
{
    int sig;

    // Ctrl + C 信号
    sig = SIGINT;
    if (SIG_ERR == signal(sig, signal_func))
    {
        PRINTF(LEVEL_WARNING, "%s signal[%d] failed.\n", __func__, sig);
    }

    // kill/pkill -15
    sig = SIGTERM;
    if (SIG_ERR == signal(sig, signal_func))
    {
        PRINTF(LEVEL_WARNING, "%s signal[%d] failed.\n", __func__, sig);
    }
}

// signal信号处理函数
static void signal_func(int sig)
{
    switch(sig)
    {
        case SIGINT:
        case SIGTERM:
            ev_io_stop(g_loop, &g_io_accept);
            ev_break(g_loop, EVBREAK_ALL);
            //if (NULL != g_loop) ev_unloop(g_loop, EVUNLOOP_ALL);
            g_state = SOCKS5_STATE_STOP;
            PRINTF(LEVEL_INFORM, "signal [%d], exit.\n", sig);
            exit(0);
            break;
        default:
            PRINTF(LEVEL_INFORM, "signal [%d], not supported.\n", sig);
            break;
    }
}

static INT32 socks5_srv_init(UINT16 port, INT32 backlog)
{
    struct sockaddr_in serv;
    int sockfd;
    int opt;
    int flags;
    
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        PRINTF(LEVEL_ERROR, "socket error!\n");
        return R_ERROR;
    }

    bzero((char *)&serv, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = htonl(INADDR_ANY);
    serv.sin_port = htons(port);
    
    if (-1 ==(flags = fcntl(sockfd, F_GETFL, 0)))
        flags = 0;
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    
    opt = 1;
    if (-1 == setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (INT8 *)&opt, sizeof(opt)))
    {
        PRINTF(LEVEL_ERROR, "setsockopt SO_REUSEADDR fail.\n");
        return R_ERROR;
    }
    #ifdef SO_NOSIGPIPE 
    if (-1 == setsockopt(listen_sock, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt)))
    {
        PRINTF(LEVEL_ERROR, "setsockopt SO_NOSIGPIPE fail.\n");
        return R_ERROR;
    }
    #endif

    if (bind(sockfd, (struct sockaddr *)&serv, sizeof(serv)) < 0)
    {
        PRINTF(LEVEL_ERROR, "bind error [%d]\n", errno);
        return R_ERROR;
    }
    
    if (listen(sockfd, backlog) < 0)
    {
        PRINTF(LEVEL_ERROR, "listen error!\n");
        return R_ERROR;
    }

    return sockfd;
}

static INT32 socks5_srv_exit(int sockfd)
{
    if (0 != sockfd)
        close(sockfd);

    return R_OK;  
}

static INT32 socks5_sockset(int sockfd)
{
    struct timeval tmo = {0};
    int opt = 1;
    
    tmo.tv_sec = 2;
    tmo.tv_usec = 0;
    if (-1 == setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof(tmo)) \
        || -1 == setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tmo, sizeof(tmo)))
    {
         PRINTF(LEVEL_ERROR, "setsockopt error.\n");
         return R_ERROR;
    }

#ifdef SO_NOSIGPIPE
    setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    if (-1 == setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (INT8 *)&opt, sizeof(opt)))
    {
        PRINTF(LEVEL_ERROR, "setsockopt SO_REUSEADDR fail.\n");
        return R_ERROR;
    }
    
    return R_OK;
}

static INT32 socks5_auth(int sockfd)
{
    int remote = 0;
    char buff[BUFFER_SIZE];
    struct sockaddr_in addr;
    int addr_len;
    int ret;

    socks5_sockset(sockfd);

    // VERSION and METHODS
    if (-1 == recv(sockfd, buff, 2, 0)) GOTO_ERR;
    if (SOCKS5_VERSION != ((socks5_method_req_t *)buff)->ver) GOTO_ERR;
    ret = ((socks5_method_req_t *)buff)->nmethods;
    if (-1 == recv(sockfd, buff, ret, 0)) GOTO_ERR;

    // no auth
    memcpy(buff, "\x05\x00", 2);
    if (-1 == send(sockfd, buff, 2, 0)) GOTO_ERR;

    // REQUEST and REPLY
    if (-1 == recv(sockfd, buff, 4, 0)) GOTO_ERR;
    //if (0x05 != buff[0] || 0x01 != buff[1]) //GOTO_ERR;
    if (SOCKS5_VERSION != ((socks5_request_t *)buff)->ver
        || SOCKS5_CMD_CONNECT != ((socks5_request_t *)buff)->cmd)
    {
        PRINTF(LEVEL_DEBUG, "ver : %d\tcmd = %d.\n", \
            ((socks5_request_t *)buff)->ver, ((socks5_request_t *)buff)->cmd);

        ((socks5_response_t *)buff)->ver = SOCKS5_VERSION;
        ((socks5_response_t *)buff)->cmd = SOCKS5_CMD_NOT_SUPPORTED;
        ((socks5_response_t *)buff)->rsv = 0;

        // cmd not supported
        send(sockfd, buff, 4, 0);
        goto _err;
    }

    if (SOCKS5_IPV4 == ((socks5_request_t *)buff)->atype)
    {
        bzero((char *)&addr, sizeof(addr));
        addr.sin_family = AF_INET;

        if (-1 == recv(sockfd, buff, 4, 0)) GOTO_ERR;
        memcpy(&(addr.sin_addr.s_addr), buff, 4);
        if (-1 == recv(sockfd, buff, 2, 0)) GOTO_ERR;
        memcpy(&(addr.sin_port), buff, 2);

        PRINTF(LEVEL_DEBUG, "type : IP, %s:%d.\n", inet_ntoa(addr.sin_addr), htons(addr.sin_port));
    }
    else if (SOCKS5_DOMAIN == ((socks5_request_t *)buff)->atype)
    {
        struct hostent *hptr;

        bzero((char *)&addr, sizeof(addr));
        addr.sin_family = AF_INET;

        if (-1 == recv(sockfd, buff, 1, 0)) GOTO_ERR;
        ret = buff[0];
        buff[ret] = 0;
        if (-1 == recv(sockfd, buff, ret, 0)) GOTO_ERR;
        hptr = gethostbyname(buff);
        PRINTF(LEVEL_DEBUG, "type : domain [%s].\n", buff); 

        if (NULL == hptr) GOTO_ERR;
        if (AF_INET != hptr->h_addrtype) GOTO_ERR;
        if (NULL == *(hptr->h_addr_list)) GOTO_ERR;
        memcpy(&(addr.sin_addr.s_addr), *(hptr->h_addr_list), 4);

        if (-1 == recv(sockfd, buff, 2, 0)) GOTO_ERR;
        memcpy(&(addr.sin_port), buff, 2);
    }
    else
    {
        ((socks5_response_t *)buff)->ver = SOCKS5_VERSION;
        ((socks5_response_t *)buff)->cmd = SOCKS5_ADDR_NOT_SUPPORTED;
        ((socks5_response_t *)buff)->rsv = 0;

        // cmd not supported
        send(sockfd, buff, 4, 0);
        GOTO_ERR;
    }

    if ((remote = socket(AF_INET, SOCK_STREAM, 0)) < 0) GOTO_ERR;
    socks5_sockset(remote);
    
    if (0 > connect(remote, (struct sockaddr *)&addr, sizeof(addr)))// GOTO_ERR;
    {
        PRINTF(LEVEL_ERROR, "connect error.\n");

        // connect error
        memcpy(buff, "\x05\x05\x00\x01\x00\x00\x00\x00\x00\x00", 10);
        send(sockfd, buff, 4, 0);

        goto _err;
    }

    addr_len = sizeof(addr);
    if (0 > getpeername(remote, (struct sockaddr *)&addr, (socklen_t *)&addr_len)) GOTO_ERR;
    // reply remote address info
    memcpy(buff, "\x05\x00\x00\x01", 4);
    memcpy(buff + 4, &(addr.sin_addr.s_addr), 4);
    memcpy(buff + 8, &(addr.sin_port), 2);
    send(sockfd, buff, 10, 0);

    PRINTF(LEVEL_DEBUG, "auth ok.\n");
    return remote;

_err:
    if (0 != remote) close(remote);
    return R_ERROR;
}

static void accept_cb(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
    struct sockaddr_in client_addr;  
    socklen_t client_len = sizeof(client_addr);  
    int client_fd = 0;
    int remote_fd;
      
    //libev的错误处理  
    if(EV_ERROR & revents)  
    {  
        PRINTF(LEVEL_ERROR, "error event in accept.\n");
        return;  
    }  

    //分派客户端的ev io结构  
    struct ev_io *w_client = (struct ev_io*) malloc (sizeof(struct ev_io));
    struct ev_io *w_serv = (struct ev_io*) malloc (sizeof(struct ev_io));
    if (NULL == w_client || NULL == w_serv)
    {
        PRINTF(LEVEL_ERROR, "apply memory error.\n");

        if (w_client) free(w_client);
        if (w_serv) free(w_serv);
        return;
    }
      
    //accept,普通写法  
    client_fd = accept(watcher->fd, (struct sockaddr *)&client_addr, &client_len);  
    if (client_fd < 0)  
    {
        free(w_client);
        free(w_serv);
        return;  
    }  
    
    if (R_ERROR == (remote_fd = socks5_auth(client_fd)))
    {
        PRINTF(LEVEL_ERROR, "auth error.\n");
        close(client_fd);
        free(w_client);
        free(w_serv);
        return;
    }

    w_client->data = w_serv;
    ev_io_init(w_client, read_cb, client_fd, EV_READ);  
    ev_io_start(loop, w_client); 

    w_serv->data = w_client;
    ev_io_init(w_serv, read_cb, remote_fd, EV_READ);  
    ev_io_start(loop, w_serv); 

    return;
}

static void read_cb(struct ev_loop *loop, struct ev_io *watcher, int revents)
{  
    char buffer[BUFFER_SIZE];  
    ssize_t read;  
      
    if(EV_ERROR & revents)  
    {  
      PRINTF(LEVEL_ERROR, "error event in read.\n");  
      return;  
    }  
      
    //recv普通socket写法  
    read = recv(watcher->fd, buffer, BUFFER_SIZE, 0);    
    if(read < 0)  
    {  
        if (104 == errno)
        {
            PRINTF(LEVEL_DEBUG, "close %d:%d.\n", watcher->fd, ((struct ev_io *)watcher->data)->fd);  
            ev_io_stop(loop, watcher);
            ev_io_stop(loop, watcher->data);
            close(watcher->fd);
            close(((struct ev_io *)watcher->data)->fd);
     //       ev_break(loop, EVBREAK_ONE);
    //        ev_break(loop, EVBREAK_ONE);
            free(watcher->data);
            free(watcher);
            return; 
        }

        PRINTF(LEVEL_ERROR, "read error [%d].\n", errno);  
        return;
    }
      
    //断开链接的处理,停掉evnet就可以,同时记得释放客户端的结构体!  
    if(read == 0)  
    {  
        PRINTF(LEVEL_DEBUG, "close %d:%d.\n", watcher->fd, ((struct ev_io *)watcher->data)->fd);  
        ev_io_stop(loop, watcher);
        ev_io_stop(loop, watcher->data);
        close(watcher->fd);
        close(((struct ev_io *)watcher->data)->fd);
 //       ev_break(loop, EVBREAK_ONE);
//        ev_break(loop, EVBREAK_ONE);
        free(watcher->data);
        free(watcher);
        return;  
    }  
    else  
    {  
        send(((struct ev_io *)watcher->data)->fd, buffer, read, 0);
    }

    return; 
}  
