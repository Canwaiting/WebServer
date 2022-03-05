#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users; //缓存数据库中的信息

//载入数据库中的用户名和密码到服务器中
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    //初始化
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    //读取未被处理的
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        //在缓冲区中获取当前字符
        temp = m_read_buf[m_checked_idx];
        //行的结束为\r\n,所以判断是否可能读取到完整行
        if (temp == '\r')
        {
            //读完了还没有\n,即接收不完整,需要继续接收
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;

            //下一个字符是\n,读完了该行,将\r\n改成\0\0
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }

            //返回语法错误
            return LINE_BAD;
        }
        //上次读到的是\r,但是没有完整读完就会出现这种情况
        else if (temp == '\n')
        {
            /*检查前一个字符是否是\r*/
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                //将\r\n改成\0\0
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //前一个字符不是\r,返回语法有错误
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//读取数据 ET/LT
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //读取数据 LT
    if (0 == m_TRIGMode)
    {
        //接收数据并储存
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;
        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }

    //读取数据 ET
    else
    {
        while (true)
        {
            //接收数据并储存
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);

            if (bytes_read == -1)
            {
                //非阻塞ET模式下,需要一次性把所有数据读完
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }

            //客户端已经关闭了
            else if (bytes_read == 0)
            {
                return false;
            }

            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //跳过一开始含有空格和\t的位置
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }

    //将text文本中的\t改成\0并赋值给mothod,用于下面识别请求方法
    *m_url++ = '\0';
    char *method = text;

    //取出数据,确定请求方式
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;

    //识别并指向资源
    m_url += strspn(m_url, " \t");

    //识别并指向版本
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    //仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    //各种情况的,对请求资源前7个字符进行判断
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");

    //请求行处理后,将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //判断是空行还是请求头
    if (text[0] == '\0')
    {
        //判断是GET还是POST请求
        if (m_content_length != 0)
        {
            //POST需要跳转到消息体处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        //GET
        return GET_REQUEST;
    }

    //解析请求头部连接字段
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }

    //解析请求头部内容长度字段
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }

    //解析请求头部HOST字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }

    //打印错误信息
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }

    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    //判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';

        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//读取报文并进行处理
http_conn::HTTP_CODE http_conn::process_read()
{
    //初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    //就是这里从状态机驱动主状态机
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        //指针指向未被处理的文本
        text = get_line();

        //更新读取的位置
        m_start_line = m_checked_idx;

        LOG_INFO("%s", text);

        //主状态机的三种状态逻辑
        switch (m_check_state)
        {
            //解析请求行
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }

            //解析请求头
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                else if (ret == GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }

            //解析消息体
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST)
                    return do_request();
                line_status = LINE_OPEN;
                break;
            }

            //服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
            default:
                return INTERNAL_ERROR;
        }
    }

    return NO_REQUEST;
}

//将用户想要的资源映射到一个内存中
http_conn::HTTP_CODE http_conn::do_request()
{
    //初始化
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //登录校验-2,注册校验-3
        char flag = m_url[1];

        //初始化,并获取请求资源名称
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);

        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;

        //获取用户名
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        //获取密码
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        //注册校验
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            //只是初始化,并没有执行,所以还是要进行判断
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            //检测是否有重名
            if (users.find(name) == users.end())
            {
                m_lock.lock();

                //执行mysql语句
                int res = mysql_query(mysql, sql_insert);
                //更新服务器缓存中的数据
                users.insert(pair<string, string>(name, password));

                m_lock.unlock();

                //注册成功,跳转登录界面
                if (!res)
                    strcpy(m_url, "/log.html");

                //注册失败,跳转注册失败页面
                else
                    strcpy(m_url, "/registerError.html");
            }

            else
                strcpy(m_url, "/registerError.html");
        }

        //登录校验
        else if (*(p + 1) == '2')
        {
            //校验
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    //注册页面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //登录页面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //图片页面
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //视频页面
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //关注页面
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //通过stat获取请求资源文件信息m_file_stat,成功则将信息更新到m_real_file结构体中
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    //判断文件的权限,是否可读
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    //判断文件的类型,如果是目录
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //以只读方式获取文件描述符,通过mmap将文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//将响应报文发送给浏览器端
bool http_conn::write()
{
    int temp = 0;

    /*如果发送的数据长度为0,即至少响应报文为0,一般不会出现*/
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        /*将响应报文的状态行、消息头、空行和响应正文发送给浏览器端*/
        /*现在又用回socket了*/
        temp = writev(m_sockfd, m_iv, m_iv_count);

        /*报错*/
        if (temp < 0)
        {
            /*缓冲区已经满了*/
            if (errno == EAGAIN)
            {
                /*TODO:重新注册写事件,相当于再来?*/
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            /*TODO:如果发送失败,但不是缓冲区的问题,取消映射*/
            unmap();
            return false;
        }

        bytes_have_send += temp; /*更新还没有发的字节*/
        bytes_to_send -= temp; /*更新已发送了的字节*/
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            /*不再继续发送头部信息*/
            m_iv[0].iov_len = 0; /*TODO:清零?*/
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx); /*TODO*/
            m_iv[1].iov_len = bytes_to_send; /*TODO:开始发资源?*/
        }
        /*继续发地一个iovec头部信息的数据*/
        else
        {
            /*TODO*/
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        /*数据全部发完*/
        if (bytes_to_send <= 0)
        {
            unmap();
            /*TODO:在epoll树上重置EPOLLONESHOT事件,EPOLLONESHOT是什么*/
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            /*如果是长连接*/
            if (m_linger)
            {
                /*重新初始化http对象*/
                /*TODO:不用参数?还是新的一个*/
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

//用于添加响应所要调用的函数
bool http_conn::add_response(const char *format, ...)
{
    //待发送字节大于等于写缓存区的最大缓存数
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;

    //定义可变参数列表,并初始化
    va_list arg_list;
    va_start(arg_list, format);

    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);

    //如果写入的数据长度超过缓冲区剩余空间,则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        //清空可变参列表
        va_end(arg_list);
        return false;
    }

    //成功写后,更新m_write_idx的位置
    m_write_idx += len;

    //清空可变参数列表
    va_end(arg_list);

    /*log*/
    LOG_INFO("request:%s", m_write_buf);

    return true;
}

//添加状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//添加消息报头,具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    //返回是否添加成功
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

/*添加content_length,表示响应报文的长度*/
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

/*添加文本类型,这里是html*/
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

//添加连接状态,通知浏览器是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

//添加文本content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

//根据不同的情况来发送响应,调用上面的写报文的函数
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
        //内部错误,500
        case INTERNAL_ERROR:
            {
                //状态行
                add_status_line(500, error_500_title);
                //报头
                add_headers(strlen(error_500_form));
                //报文主体
                if (!add_content(error_500_form))
                return false;
                break;
            }
        //报文语法有误,404
        case BAD_REQUEST:
                             {
                             add_status_line(404, error_404_title);
                             add_headers(strlen(error_404_form));
                             if (!add_content(error_404_form))
                             return false;
                             break;
                             }

        //资源没有访问权限,403
        case FORBIDDEN_REQUEST:
                                   {
                                   add_status_line(403, error_403_title);
                                   add_headers(strlen(error_403_form));
                                   if (!add_content(error_403_form))
                                   return false;
                                   break;
                                   }

        //文件存在,200
        case FILE_REQUEST:
          {
            //响应行
            add_status_line(200, ok_200_title);
            //如果请求的资源存在
            if (m_file_stat.st_size != 0)
            {
              add_headers(m_file_stat.st_size);
              //第一个指针指向响应报文缓冲区,长度指向m_write_idx
              m_iv[0].iov_base = m_write_buf;
              m_iv[0].iov_len = m_write_idx;
              //第二个指针指向mmap返回的文件指针(即资源),长度指向文件大小
              m_iv[1].iov_base = m_file_address;
              m_iv[1].iov_len = m_file_stat.st_size;
              m_iv_count = 2;
              //发送的全部数据为响应报文头部信息和文件大小
              bytes_to_send = m_write_idx + m_file_stat.st_size;
              return true;
            }

            //如果请求的资源大小为0,则返回空白html文件
            else
            {
              const char *ok_string = "<html><body></body></html>";
              add_headers(strlen(ok_string));
              if (!add_content(ok_string))
              return false;
            }

          }
        default:
            return false;
    }

    //TODO
    //除了访问资源状态,即FILE_REQUEST外,其余状态只有一个指针,指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

//请求报文读取并处理-->处理并写入响应报文-->注册写完成事件
void http_conn::process()
{
    //读取并处理请求报文,没读完就继续读
    HTTP_CODE read_ret = process_read();

    //NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST)
    {
        //注册并监听读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    //写入响应报文
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }

    //注册并监听写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
