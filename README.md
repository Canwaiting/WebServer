## Linux下C++轻量级Web服务器 
> + 使用 线程池 + 非阻塞socket + epoll(ET和LT均实现) + 事件处理(Reactor和模拟Proactor均实现) 的并发模型
> + 使用状态机解析HTTP请求报文，支持解析GET和POST请求
> + 访问服务器数据库实现web端用户注册、登录功能，可以请求服务器图片和视频文件
> + 实现同步/异步日志系统，记录服务器运行状态
> + 经Webbench压力测试可以实现上万的并发连接数据交换
 
源代码参考:https://github.com/qinguoyi/TinyWebServer


## 开发目的
以解决项目为目标,推动相关知识的应用或学习薄弱方面的知识。
以至于不会浪费时间在现阶段还用不了的知识,同时以项目导向,使得所学知识的性价比更高。
## 开发所用知识及参考书籍 
+ 《Linux高性能服务器编程》
+ 《UNIX环境高级编程》
+ 《UNIX网络编程》
+ 计算机网络
+ 部分操作系统
+ 数据库MySQL基础

## 概述
> * C/C++
> * B/S模型
> * 线程同步机制包装类
> * http连接请求处理类
> * 半同步/半反应堆线程池
> * 定时器处理非活动连接
> * 同步/异步日志系统 
> * 数据库连接池
> * 同步线程注册和登录校验 
> * 简易服务器压力测试





