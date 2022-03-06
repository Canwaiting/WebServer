#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}

//创建并返回连接池实例
connection_pool *connection_pool::GetInstance()
{
    //创建一个连接池
	static connection_pool connPool;
    //返回连接池
	return &connPool;
}

//初始化数据库连接池
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
    //初始化私有变量
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

    //创建MaxConn条数据库连接
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		con = mysql_init(con);
		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}

		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);
		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}

		connList.push_back(con);
		++m_FreeConn;
	}

    //将信号量初始化为最大连接次数
	reserve = sem(m_FreeConn);
	m_MaxConn = m_FreeConn;
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL; /*TODO:初始化连接?*/

    /*如果数据库池中没有连接,返回null*/
	if (0 == connList.size())
		return NULL;

    /*TODO:取出连接，信号量原子减一,为0则等待.那么这行代码是怎么实现,信号量原子又是什么*/
	reserve.wait();
	
	lock.lock(); /*TODO:互斥锁*/

	con = connList.front(); /*拿出LIST中的第一个连接出来*/
	connList.pop_front(); /*然后把它从数据库池中删除*/

	--m_FreeConn; /*可用的连接减少*/
	++m_CurConn; /*现用的连接增加*/

	lock.unlock(); /*解锁*/
	return con; /*返回连接*/
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
    /*判断是否为空*/
	if (NULL == con)
		return false;

    /*锁住,避免我要释放时,你还在使用*/
	lock.lock();

    /*把连接放回去*/
	connList.push_back(con);
	++m_FreeConn; /*可用的连接增加*/
	--m_CurConn; /*现用的连接减少*/

	lock.unlock(); /*解锁*/

    /*TODO:释放连接原子加1*/
	reserve.post();
	return true; /*TODO:应该是给别人做错误处理*/
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{

    /*锁住*/
	lock.lock();

    /*如果连接池非空*/
	if (connList.size() > 0)
	{
        /*TODO:cpp 通过迭代器遍历,关闭数据库链接*/
		list<MYSQL *>::iterator it; /*TODO:cpp*/
		for (it = connList.begin(); it != connList.end(); ++it) /*从头到尾不断*/
		{
			MYSQL *con = *it; /*TODO:相当于暂存?*/
			mysql_close(con); /*调用关闭函数*/
		}
        /*更新参数*/
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear(); /*TODO:cpp中的函数?还是自己的*/
	}

    /*解锁*/
	lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

/*不直接调用获取和释放连接的接口，将其封装起来，通过RAII机制进行获取和释放*/
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();

    /*TODO:直接把连接传递给RAII,然后他就会用构造函数处理?*/
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}