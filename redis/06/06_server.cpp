#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <vector>


static void msg (const char *msg){
    fprintf(stderr,"%s\n",msg);
}

static void die(const char *msg){
    int err = errno;
    fprintf(stderr,"[%d] %s\n",err,msg);
    abort();
}

static void fd_set_nb(int fd){
    errno = 0;
    int flags = fcntl(fd,F_GETFL,0);
    if(errno){
        die("fcntl error");
        return;
    }
    // 设置为 nio
    flags |= O_NONBLOCK;
    errno = 0;
    (void) fcntl(fd,F_SETFL,flags);
    if (errno){
        die("fcntl error");
    }
}

const size_t k_max_msg = 4096;

enum {
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_END = 2, // 标记连接已经被删除
};

struct Conn {
    int fd = -1;
    uint32_t state = 0; // 可能是 STATE_REQ 或者 STATE_RES
    // 读缓存区
    size_t rbuf_size =0;
    uint8_t rbuf[ 4 + k_max_msg];
    // 写缓冲区
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg];
};

// 将连接对象放到集合中
static void conn_put(std::vector<Conn *> &fd2conn,struct Conn *conn){
    if (fd2conn.size() <= (size_t)conn->fd){
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

//接收一个新的连接，通过fd的方式
static int32_t accept_new_conn(std::vector<Conn *> &fd2conn,int fd){
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd,(struct sockaddr *)&client_addr,&socklen);
    if (connfd < 0){
        msg("accept() error");
        return  -1;
    }
    // 设置连接为非阻塞模式
    fd_set_nb(connfd);
    // 创建Conn 结构体
    struct Conn *conn = (struct Conn *) malloc(sizeof (struct Conn));
    if (!conn){
        close(connfd);
        return -1;
    }
    conn->fd = connfd;
    conn->state = STATE_REQ;
    conn->rbuf_size = 0;
    conn->wbuf_size = 0;
    conn->wbuf_sent = 0;
    conn_put(fd2conn,conn);
    return 0;
}

static void state_req(Conn *conn);
static void state_res(Conn *conn);

static bool try_one_request(Conn *conn){
    // 尝试解析来自缓冲区的请求
    if (conn->rbuf_size < 4){
        return false;
    }
    uint32_t  len = 0;
    // 填充前4位为字符串长度
    memcpy(&len,&conn->rbuf[0],4);
    if (len > k_max_msg){
        msg("too long");
        conn->state = STATE_END;
        return false;
    }
    // 还没有塞满
    if (4 + len > conn->rbuf_size){
        return false;
    }
    // 得到一个请求，做一些事情
    printf("client say: %.*s\n",len,&conn->rbuf[4]);

    // 生成响应
    memcpy(&conn->wbuf[0],&len,4);
    memcpy(&conn->wbuf[4],&conn->rbuf[4],len);
    // 写缓冲的大小增大
    conn->wbuf_size = 4+len;

    // memmove remove request from buffer
    size_t remain = conn->rbuf_size - 4 -len;
    if (remain) {
        // 相当于 memcpy ,将 conn->rbuf[4+len]拷贝到 rbuf中，但是比 memcpy更加安全
        // memmove如果出现了重叠的d
        // remain表示要复制的字节数
        memmove(conn->rbuf,&conn->rbuf[4+len],remain);
    }

    conn->rbuf_size = remain;

    conn->state = STATE_RES;
    state_res(conn);
    return (conn->state == STATE_REQ);

}

static bool try_fill_buffer(Conn *conn){
    // 尝试填充缓冲
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd,&conn->rbuf[conn->rbuf_size],cap);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EINTR){
        return false;
    }
    if (rv < 0){
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }
    if (rv == 0 ){
        if (conn->rbuf_size > 0){
            msg("unexpected EOF");
        }else{
            msg("EOF");
        }
        conn->state = STATE_END;
        return false;
    }
    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));
    while (try_one_request(conn)){}
    return (conn->state == STATE_REQ);
}

static void state_req(Conn *conn){
    while (try_fill_buffer(conn)){}
}

// 尝试刷新缓冲
static bool try_flush_buffer(Conn *conn){
    size_t rv = 0;
    do {
        // 获取剩余的大小
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        // 写入数据，冲 wbuf_sent 开始，不能超过 remain
        rv = write(conn->fd,&conn->wbuf[conn->wbuf_sent],remain);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN){
        return false;
    }
    if (rv < 0){
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }
    // 更新已经发送的index
    conn->wbuf_sent += (size_t) rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    // 如果写入完成，修改状态，并接收外层循环
    if (conn->wbuf_sent == conn->wbuf_size){
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    return true;
}

//
static void state_res(Conn *conn){
    while(try_flush_buffer(conn)){}
}

//根据状态来进行处理
static void connection_io(Conn *conn){
    if (conn->state == STATE_REQ){
        state_req(conn);
    }else if (conn->state == STATE_RES){
        state_res(conn);
    }else{
        assert(0); // 非预期错误
    }
}


int main() {
    int fd = socket(AF_INET,SOCK_STREAM,0);
    if (fd < 0)
    {
        die("socket()");
    }

    int val = 1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&val,sizeof(val));

    // bind
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(0);
    int rv = bind(fd,(const sockaddr *)&addr,sizeof(addr));
    if (rv)
    {
        die("bind()");
    }
    rv = listen(fd,SOMAXCONN);
    if (rv)
    {
        die("listen()");
    }
    // fd 集合
    std::vector<Conn *> fd2conn;

    // nio
    fd_set_nb(fd);
    // the event loop
    std::vector<struct pollfd> poll_args;
    while (true){
        poll_args.clear();
        // 设置监听监听的fd下标为0
        struct pollfd pfd = {fd,POLLIN,0};
        poll_args.push_back(pfd);
        for (Conn *conn: fd2conn){
            if (!conn){
                continue;
            }  
            struct pollfd pfd = {};
            // 指定fd
            pfd.fd = conn->fd;
            // 指定时间，可能是read or write
            pfd.events = (conn->state == STATE_REQ) ? POLLIN:POLLOUT;
            pfd.events = pfd.events | POLLERR;
            poll_args.push_back(pfd);
        }
        // 活动的 fds
        int rv = poll(poll_args.data(),(nfds_t)poll_args.size(),1000);
        if (rv < 0){
            die("poll");
        }
        // 处理active connection
        for (size_t i = 1; i < poll_args.size();++i){
            // TODO
            if (poll_args[i].revents){
                // 根据fd获取连接对象
                Conn *conn = fd2conn[poll_args[i].fd];
                // 执行连接，并根据状态进行处理
                connection_io(conn);
                // 如果client的连接断开，或者任务完成，就结束，并释放连接
                if (conn->state == STATE_END){
                    fd2conn[conn->fd] = NULL;
                    (void) close(conn->fd);
                    free(conn);
                }
            }
        }
        // 如果监听的fd active 就尝试创建一个新的连接
        if (poll_args[0].revents){
            (void) accept_new_conn(fd2conn,fd);
        }
    }
    return 0;
}
