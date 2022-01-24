#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200; /*设置读取文件的名称m_real_file大小*/
    static const int READ_BUFFER_SIZE = 2048; /*设置读缓冲区m_read_buf大小*/
    static const int WRITE_BUFFER_SIZE = 1024; /*设置写缓冲区m_write_buf大小*/
    /*TODO:HEAD,PUT那些为什么要写上来*/
    enum METHOD /*设置报文的请求方式,本项目只用到GET和POST*/
    { GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE /*TODO:主状态机的状态,CHECK_STATE_HEADER这些是什么*/
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    /*TODO:是不是一整个报文*/
    enum HTTP_CODE /*TODO:报文解析的结果,其他是什么意思*/
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    enum LINE_STATUS /*TODO:从状态机的状态,具体意思?*/
    {
        /*TODO:OK,BAD,OPEN是什么意思*/
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    /*TODO:初始化socket的地址和sql啥的*/
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    /*TODO:关闭http连接,具体怎么样不知道*/
    void close_conn(bool real_close = true);
    /*TODO:对进程干了什么?*/
    void process();
    /*TODO:读取浏览器端发来的全部数据,具体怎么样去读*/
    bool read_once();
    /*TODO:响应报文写入函数*/
    bool write();
    /*TODO:用于获取地址的函数?*/
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    /*TODO:CGI使用线程池初始化数据库表,即从池中选择一个来初始化*/
    void initmysql_result(connection_pool *connPool);
    /*TODO:时钟是否到时?*/
    int timer_flag;
    /*TODO*/
    int improv;


private:
    /*TODO:初始化什么?*/
    void init();
    /*TODO:从m_read_buf读取里面的状态,并处理请求报文*/
    HTTP_CODE process_read();
    /*TODO:往m_write_buf中写入响应报文数据,究竟是状态还是主体数据还是什么*/
    bool process_write(HTTP_CODE ret);
    /*TODO:主状态机解析报文中的请求行数据*/
    HTTP_CODE parse_request_line(char *text);
    /*TODO:主状态机解析报文中的请求头数据*/
    HTTP_CODE parse_headers(char *text);
    /*TODO:主状态机解析报文中的请求内容数据*/
    HTTP_CODE parse_content(char *text);
    /*TODO:生成响应报文*/
    HTTP_CODE do_request();
    /*TODO:m_start_line是已经解析的字符,为什么还需要添加m_read_buf*/
    /*get_line用于将指针向后偏移,指向未处理的字符*/
    char *get_line() { return m_read_buf + m_start_line; };
    /*TODO:从状态机读取一行,分析是请求报文的哪一部分,OK,BAD,OPEN*/
    LINE_STATUS parse_line();
    /*TODO*/
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    int m_read_idx;
    int m_checked_idx;
    int m_start_line;
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    int m_content_length;
    bool m_linger;
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;
    int bytes_have_send;
    char *doc_root;

    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
