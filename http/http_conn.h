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
    static const int FILENAME_LEN = 200; //设置读取文件的名称m_real_file大小
    static const int READ_BUFFER_SIZE = 2048; /*设置读缓冲区m_read_buf大小*/
    static const int WRITE_BUFFER_SIZE = 1024; /*设置写缓冲区m_write_buf大小*/
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

    //主状态机的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0, /*解析请求行*/
        CHECK_STATE_HEADER, /*解析请求头*/
        CHECK_STATE_CONTENT /*解析消息体,仅用于解析post请求*/
    };

    enum HTTP_CODE
    {
        NO_REQUEST, //请求不完整,需要继续读取请求报文数据
        GET_REQUEST, //获取了完整的HTTP请求
        BAD_REQUEST, /*HTTP请求报文有语法错误*/
        NO_RESOURCE, /*资源不存在*/
        FORBIDDEN_REQUEST, /*文件不可读,没有访问权限*/
        FILE_REQUEST, /*表示文件存在*/
        INTERNAL_ERROR, /*服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发*/
        CLOSED_CONNECTION /*关闭连接*/
    };

    //从状态机的状态
    enum LINE_STATUS
    {
        LINE_OK = 0, //完整读取一行
        LINE_BAD, //报文语法有误
        LINE_OPEN //读取的行不完整
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);
    int timer_flag; //1为要重置,0为不用重置
    int improv; //1为处理完,0为未处理


private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    //get_line用于将指针向后偏移,指向未处理的字符
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();
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
    static int m_user_count; //用户连接数
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd; /*socket*/
    sockaddr_in m_address; /*socket的地址*/
    char m_read_buf[READ_BUFFER_SIZE]; //存储读取的请求报文数据
    int m_read_idx; /*缓冲区中m_read_buf中数据的最后一个字节的下一个位置*/
    int m_checked_idx; /*m_read_buf读取的位置*/
    int m_start_line; //m_read_buf中已经解析的字符个数

    char m_write_buf[WRITE_BUFFER_SIZE]; /*存储发出的响应报文数据*/
    int m_write_idx; /*指示buffer中的长度*/

    CHECK_STATE m_check_state; //主状态机的状态
    METHOD m_method; //请求方法

    //以下为解析请求报文中对应的6个变量
    char m_real_file[FILENAME_LEN]; //客户请求目标文件的完整路径
    char *m_url; //资源
    char *m_version; //HTTP协议版本
    char *m_host; //客户端Host
    int m_content_length;
    bool m_linger; //长连接

    char *m_file_address; /*读取服务器上的文件地址*/
    struct stat m_file_stat; /*TODO:被读取文件的状态*/
    struct iovec m_iv[2]; /*TODO:IO向量机制iovec,这个是什么来的*/
    int m_iv_count; /*TODO:iovec的数量?*/
    int cgi;        //POST-1 , GET-0
    char *m_string; //存储请求头数据
    int bytes_to_send; /*剩余发送字节数*/
    int bytes_have_send; /*已发送字节数*/
    char *doc_root; //文档的根,表现为数字

    map<string, string> m_users; /*TODO:可能存储的是用户的名字和密码*/
    int m_TRIGMode; /*TODO:ET和LT模式?*/
    int m_close_log; /*关闭LOG?*/

    /*TODO:数据库相关的存储?user里面有passwd和name*/
    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
