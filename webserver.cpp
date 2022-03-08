#include "webserver.h"

WebServer::WebServer()
{
    //http_conn类对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器数组
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

//LT和ET模式
void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

//初始化数据库连接池并将数据存到服务器中
void WebServer::sql_pool()
{
    //获取连接池实例
    m_connPool = connection_pool::GetInstance();

    //初始化该数据库连接池
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //将数据库中的数据存到服务器中
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen()
{
    //创建socket
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    //优雅关闭连接
    if (0 == m_OPT_LINGER) /*TODO: m_OPT_LINGER*/
    {
        struct linger tmp = {0, 1}; /*TODO:linger */
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp)); /*设置socket的属性*/
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp)); /*设置socket的属性*/
    }

    //建立地址
    int ret = 0;
    struct sockaddr_in address; /*弄一个地址结构体,到时候和socket绑定在一起*/
    bzero(&address, sizeof(address)); /*初始化address*/ 
    address.sin_family = AF_INET; /*协议为IPV4*/
    address.sin_addr.s_addr = htonl(INADDR_ANY); /*TODO:就如同一个参数使其可以接收任何东西,网络地址*/
    address.sin_port = htons(m_port); /*TODO:即telnet那样吗?只不过用浏览器去访问,到时候传入的是9006,端口号*/

    //可以重用地址和端口
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));/*设置socket的属性*/

    //bind操作
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address)); /*相当与现在有一个socket,但是没有地址,然后就把该socket绑定在某个地址上*/
    assert(ret >= 0); /*-1是错误,其他都正常,*/

    //listen操作
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    //将当前socket注册到内核中,选择模式,选择开启EPOLLONESHOT
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    //管道套接字,并设置非阻塞
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);

    //设置管道读端为ET非阻塞,统一事件源
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);
    utils.addsig(SIGPIPE, SIG_IGN);

    //注册给主循环的关注信号 SIGALRM SIGTERM
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    //每隔TIMESLOT触发SIGALRM信号
    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作,方便用其他地方操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    //获取现在的时间
    time_t cur = time(NULL);

    //超出时间为后3个最小超时单位
    timer->expire = cur + 3 * TIMESLOT;

    //调整改变后的计时器的位置
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

//删除过期的定时器,终结相应socket
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    //关闭定时器所绑定的socket
    timer->cb_func(&users_timer[sockfd]);

    //删除该定时器
    if (timer)
    {
        //从升序定时器链表中删除定时器
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

//监听 ET/LT
bool WebServer::dealclinetdata()
{
    //初始化用户地址
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);

    //ET
    if (0 == m_LISTENTrigmode)
    {
        //accept
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            //未能成功处理用户
            return false;
        }

        //用户已满
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            //未能成功处理用户
            return false;
        }

        //建立计时器
        timer(connfd, client_address);
    }

    //LT
    else
    {
        //直到用户被处理,或者accpet报错才退出
        while (1)
        {
            //accept
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }

            //用户已满
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }

            //建立计时器
            timer(connfd, client_address);
        }

        //未能成功处理用户
        return false;
    }

    //成功处理用户
    return true;
}

//epoll中获取管道信号处理
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];

    //从管道读出信号值,返回字节数
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }

    //处理信号值
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
                //计时器过期
                case SIGALRM:
                {
                    timeout = true;
                    break;
                }
                //关闭服务器
                case SIGTERM:
                {
                    stop_server = true;
                    break;
                }
            }
        }
    }

    return true;
}

//Reactor和Proactor方式读取数据
void WebServer::dealwithread(int sockfd)
{
    //获取当前fd的计时器
    util_timer *timer = users_timer[sockfd].timer;

    //reactor 放读就绪到队列上
    if (1 == m_actormodel)
    {
        //调整计时器
        if (timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件直接该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                //重置该socket
                if (1 == users[sockfd].timer_flag)
                {
                    //删除该socket和定时器
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                //重置状态
                users[sockfd].improv = 0;
                break;
            }
        }
    }

    //proactor 放读完成到队列上
    else
    {   //读完成
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //将该事件放入队列
            m_pool->append_p(users + sockfd);

            //调整计时器
            if (timer)
            {
                adjust_timer(timer);
            }
        }

        else
        {
            //删除计时器和相应socket
            deal_timer(timer, sockfd);
        }
    }
}

//Reactor和Proactor方式写入数据
void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor 把就绪的写事件放入队列
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }

    //proactor 直接写
    else
    {
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    //只要服务器在运行,就不断轮询epoll
    while (!stop_server)
    {
        //获取发生事件的fd表
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        //轮询有事件发生的fd表
        for (int i = 0; i < number; i++)
        {
            //获取当前fd
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                //ET成功返回T,反之F  LT必返回F
                bool flag = dealclinetdata();
                //F直接处理下一个fd的事件
                if (false == flag)
                    continue;
            }

            //如有异常，则直接关闭客户连接，并删除该用户的timer
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }

            //处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                //未能处理,打日志
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }

            //事件为可读
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }

            //事件为可写
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout)
        {
            //处理超时事件并重置超时信号警告
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}
