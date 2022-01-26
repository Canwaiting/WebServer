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

    enum CHECK_STATE /*主状态机的状态*/
    {
        CHECK_STATE_REQUESTLINE = 0, /*解析请求行*/
        CHECK_STATE_HEADER, /*解析请求头*/
        CHECK_STATE_CONTENT /*解析消息体,仅用于解析post请求*/
    };

    /*TODO:是不是一整个报文*/
    enum HTTP_CODE /*TODO:报文解析的结果,其他是什么意思*/
    {
        NO_REQUEST, /*请求不完整,需要继续读取请求报文数据*/
        GET_REQUEST, /*获取了完整的HTTP请求*/
        BAD_REQUEST, /*HTTP请求报文有语法错误*/
        NO_RESOURCE, /*资源不存在*/
        FORBIDDEN_REQUEST, /*文件不可读,没有访问权限*/
        FILE_REQUEST, /*表示文件存在*/
        INTERNAL_ERROR, /*服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发*/
        CLOSED_CONNECTION /*关闭连接*/
    };
    enum LINE_STATUS /*从状态机的状态*/
    {
        LINE_OK = 0, /*完整读取一行*/
        LINE_BAD, /*报文语法有误*/
        LINE_OPEN /*读取的行不完整*/
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
    //根据响应报文格式,生成对应8个部分,以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd; /*TODO*/
    static int m_user_count; /*TODO:用户数?*/
    MYSQL *mysql; /*TODO*/
    int m_state;  //读为0, 写为1

private:
    int m_sockfd; /*socket*/
    sockaddr_in m_address; /*socket的地址*/
    char m_read_buf[READ_BUFFER_SIZE]; /*存储读取的请求报文数据*/
    int m_read_idx; /*TODO:长度?缓冲区中m_read_buf中数据的最后一个字节的下一个位置*/
    int m_checked_idx; /*TODO:m_read_buf读取的位置,已经读了,不管是否解析?*/
    int m_start_line; /*m_read_buf中已经解析的字符个数*/

    char m_write_buf[WRITE_BUFFER_SIZE]; /*存储发出的响应报文数据*/
    int m_write_idx; /*指示buffer中的长度*/

    CHECK_STATE m_check_state; /*主状态机的状态*/
    METHOD m_method; /*请求方法*/
    /*以下为解析请求报文中对应的6个变量*/
    char m_real_file[FILENAME_LEN]; /*存储读取文件的名称*/
    char *m_url;
    char *m_version;
    char *m_host;
    int m_content_length;
    bool m_linger;

    char *m_file_address; /*读取服务器上的文件地址*/
    struct stat m_file_stat; /*TODO:被读取文件的状态*/
    struct iovec m_iv[2]; /*TODO:IO向量机制iovec,这个是什么来的*/
    int m_iv_count; /*TODO:iovec的数量?*/
    /*TODO:CGI是什么*/
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send; /*剩余发送字节数*/
    int bytes_have_send; /*已发送字节数*/
    char *doc_root; /*TODO:文档的根?*/

    map<string, string> m_users; /*TODO:可能存储的是用户的名字和密码*/
    int m_TRIGMode; /*TODO:ET和LT模式?*/
    int m_close_log; /*关闭LOG?*/

    /*TODO:数据库相关的存储?user里面有passwd和name*/
    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
