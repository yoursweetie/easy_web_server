#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sysinfo.h>
#include <sys/epoll.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>

#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: ziyahttpd/0.1.0\r\n"

#define DEFAULT_PORT 8080
#define MAX_EVENT_NUM 1024
#define INFTIM -1

void process(int);

void handle_subprocess_exit(int);

void accept_request(int);
// 与accpet_request()函数相关的函数：
void bad_request(int);
void cat(int, FILE *);
void cannot_execute(int);
void execute_cgi(int, const char *, const char *, const char *);
int get_line(int, char *, int);
void headers(int, const char *);
void not_found(int);
void serve_file(int, const char *); // use headers(int, const char *); and cat(int, FILE *);
void unimplemented(int);

int main(int argc, char *argv[])
{
    struct sockaddr_in server_addr;
    int listen_fd;
    int cpu_core_num;
    int on = 1;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(listen_fd, F_SETFL, O_NONBLOCK);    // 设置 listen_fd 为非阻塞
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DEFAULT_PORT);

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind error, message: ");
        exit(1);
    }

    if (listen(listen_fd, 5) == -1) {
        perror("listen error, message: ");
        exit(1);
    }

    printf("listening 8080\n");

    signal(SIGCHLD, handle_subprocess_exit);

    cpu_core_num = get_nprocs();
    printf("cpu core num: %d\n", cpu_core_num);

    // 根据 CPU 数量创建子进程，我的机器是4核8线程的CPU,所以会创建8个进程
    for (int i = 0; i < cpu_core_num * 2; i++) {
        pid_t pid = fork();
        // 每一个进程一个epoll处理http请求
        if (pid == 0) {
            process(listen_fd);
            exit(0);
        }
    }

    // 父进程不停的循环
    while (1) {
        sleep(1);
    }

    return 0;
}

void process(int listen_fd)
{
    int conn_fd;
    int ready_fd_num;
    struct sockaddr_in client_addr;
    socklen_t client_addr_size = sizeof(client_addr);
    char buf[128];

    struct epoll_event ev, events[MAX_EVENT_NUM];
    // 创建 epoll 实例，并返回 epoll 文件描述符
    int epoll_fd = epoll_create(MAX_EVENT_NUM);
    ev.data.fd = listen_fd;
    ev.events = EPOLLIN;

    // 将 listen_fd 注册到刚刚创建的 epoll 中
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("epoll_ctl error, message: ");
        exit(1);
    }

    while(1) {
        // 等待事件发生
        ready_fd_num = epoll_wait(epoll_fd, events, MAX_EVENT_NUM, INFTIM);
        printf("[pid %d]  进程唤醒...\n", getpid());
        if (ready_fd_num == -1) {
            perror("epoll_wait error, message: ");
            continue;
        }
        for(int i = 0; i < ready_fd_num; i++) {
            if (events[i].data.fd == listen_fd) { // 有新的连接
                conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_size);
                if (conn_fd == -1) {
                    sprintf(buf, "[pid %d]  accept 出错: ", getpid());
                    perror(buf);
                    continue;
                }

                // 设置 conn_fd 为非阻塞
                if (fcntl(conn_fd, F_SETFL, fcntl(conn_fd, F_GETFD, 0) | O_NONBLOCK) == -1) {
                    continue;
                }

                ev.data.fd = conn_fd;
                ev.events = EPOLLIN;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) == -1) {
                    perror("epoll_ctl error, message: ");
                    close(conn_fd);
                }
                printf("[pid %d]  收到来自 %s:%d 的请求\n", getpid(), inet_ntoa(client_addr.sin_addr), client_addr.sin_port);

            } else if (events[i].events & EPOLLIN) {    // 某个 socket 数据已准备好，可以读取了
                printf("[pid %d]  处理来自 %s:%d 的请求\n", getpid(), inet_ntoa(client_addr.sin_addr), client_addr.sin_port);
                conn_fd = events[i].data.fd;
                // 调用 accept_request 函数处理请求
                accept_request(conn_fd);
                close(conn_fd);
            } else if (events[i].events & EPOLLERR) {
                fprintf(stderr, "epoll error\n");
                close(conn_fd);
            }
        }
    }
}

void handle_subprocess_exit(int signo)
{
    printf("clean subprocess.\n");
    int status;
    while(waitpid(-1, &status, WNOHANG) > 0);
}


/**********************************************************************/
/* A request has caused a call to accept() on the server port to
 * return.  Process the request appropriately(恰当的).
 * Parameters: the socket connected to the client */
/**********************************************************************/
void accept_request(int client)
{
    char buf[1024];
    size_t numchars;
    char method[255];
    char url[255];
    char path[512];
    size_t i, j;
    struct stat st;

    numchars = get_line(client, buf, sizeof(buf));
    i = 0; j = 0;
    while (!ISspace(buf[i]) && (i < sizeof(method) - 1))
    {
        method[i] = buf[i];
        i++;
    }
    j=i;
    method[i] = '\0';

    if (strcasecmp(method, "GET") && strcasecmp(method, "POST"))
    {
        unimplemented(client);
        return;
    }

    i = 0;
    while (ISspace(buf[j]) && (j < numchars))
        j++;
    while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < numchars))
    {
        url[i] = buf[j];
        i++; j++;
    }
    url[i] = '\0';

    if (strcasecmp(method, "GET") == 0)
    {
        sprintf(path, "htdocs%s", url);
        if (path[strlen(path) - 1] == '/')
            strcat(path, "index.html");
        if (stat(path, &st) == -1) {
            while ((numchars > 0) && strcmp("\n", buf))  /* read & discard(丢弃) headers */
                numchars = get_line(client, buf, sizeof(buf));
            not_found(client);
        }
        else
        {
            serve_file(client, path);
        }
    }


    close(client);
}

/**********************************************************************/
/* Inform the client that a request it has made has a problem.
 * Parameters: client socket */
/**********************************************************************/
void bad_request(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "<P>Your browser sent a bad request, ");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "such as a POST without a Content-Length.\r\n");
    send(client, buf, sizeof(buf), 0);
}

/**********************************************************************/
/* Put the entire contents of a file out on a socket.  This function
 * is named after the UNIX "cat" command, because it might have been
 * easier just to do something like pipe, fork, and exec("cat").
 * Parameters: the client socket descriptor
 *             FILE pointer for the file to cat */
/**********************************************************************/
void cat(int client, FILE *resource)
{
    char buf[1024];

    fgets(buf, sizeof(buf), resource);
    while (!feof(resource))
    {
        send(client, buf, strlen(buf), 0);
        fgets(buf, sizeof(buf), resource);
    }
}

/**********************************************************************/
/* Get a line from a socket, whether the line ends in a newline,
 * carriage return, or a CRLF combination.  Terminates the string read
 * with a null character.  If no newline indicator is found before the
 * end of the buffer, the string is terminated with a null.  If any of
 * the above three line terminators is read, the last character of the
 * string will be a linefeed and the string will be terminated with a
 * null character.
 * Parameters: the socket descriptor
 *             the buffer to save the data in
 *             the size of the buffer
 * Returns: the number of bytes stored (excluding null) */
/**********************************************************************/
int get_line(int sock, char *buf, int size)
{
    int i = 0;
    char c = '\0';
    int n;

    while ((i < size - 1) && (c != '\n'))
    {
        n = recv(sock, &c, 1, 0);
        /* DEBUG printf("%02X\n", c); */
        if (n > 0)
        {
            if (c == '\r')
            {
                n = recv(sock, &c, 1, MSG_PEEK);
                /* DEBUG printf("%02X\n", c); */
                if ((n > 0) && (c == '\n'))
                    recv(sock, &c, 1, 0);
                else
                    c = '\n';
            }
            buf[i] = c;
            i++;
        }
        else
            c = '\n';
    }
    buf[i] = '\0';

    return(i);
}

/**********************************************************************/
/* Return the informational HTTP headers about a file. */
/* Parameters: the socket to print the headers on
 *             the name of the file */
/**********************************************************************/
void headers(int client, const char *filename)
{
    char buf[1024];
    (void)filename;  /* could use filename to determine file type */

    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Give a client a 404 not found status message. */
/**********************************************************************/
void not_found(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "your request because the resource specified\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "is unavailable or nonexistent.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Send a regular file to the client.  Use headers, and report
 * errors to client if they occur.
 * Parameters: a pointer to a file structure produced from the socket
 *              file descriptor
 *             the name of the file to serve */
/**********************************************************************/
void serve_file(int client, const char *filename)
{
    FILE *resource = NULL;
    int numchars = 1;
    char buf[1024];

    buf[0] = 'A'; buf[1] = '\0';
    while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
        numchars = get_line(client, buf, sizeof(buf));

    resource = fopen(filename, "r");
    if (resource == NULL)
        not_found(client);
    else
    {
        headers(client, filename);
        cat(client, resource);
    }
    fclose(resource);
}

/**********************************************************************/
/* Inform the client that the requested web method has not been
 * implemented.
 * Parameter: the client socket */
/**********************************************************************/
void unimplemented(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</TITLE></HEAD>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}
