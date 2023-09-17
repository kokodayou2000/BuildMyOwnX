#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string>
#include <vector>
// proj
#include "hashtable.h"
#include "zset.h"
#include "list.h"
#include "heap.h"
#include "thread_pool.h"
#include "common.h"

static void msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg)
{
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static uint64_t get_monotonic_usec()
{
    timespec tv = {0, 0};
    // 从时钟获取时间
    clock_gettime(CLOCK_MONOTONIC, &tv);
    // 获取秒 和 nano秒
    return uint64_t(tv.tv_sec) * 1000000 + tv.tv_nsec / 1000;
}

static void fd_set_nb(int fd)
{
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno)
    {
        die("fcntl error");
        return;
    }
    // 设置为 nio
    flags |= O_NONBLOCK;
    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno)
    {
        die("fcntl error");
    }
}

struct Conn;

static struct
{
    HMap db;
    std::vector<Conn *> fd2conn;
    DList idle_list;
    // 自动变长数组
    std::vector<HeapItem> heap;
    // 线程池 new
    ThreadPool tp;
} g_data;

const size_t k_max_msg = 4096;

enum
{
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_END = 2, // 标记连接已经被删除
};

struct Conn
{
    int fd = -1;
    uint32_t state = 0; // 可能是 STATE_REQ 或者 STATE_RES
    // 读缓存区
    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg];
    // 写缓冲区
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg];
    uint64_t idle_start = 0;
    DList idle_list;
};

// 将连接对象放到集合中
static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn)
{
    if (fd2conn.size() <= (size_t)conn->fd)
    {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

// 接收一个新的连接，通过fd的方式
static int32_t accept_new_conn(int fd)
{
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0)
    {
        msg("accept() error");
        return -1;
    }
    // 设置连接为非阻塞模式
    fd_set_nb(connfd);
    // 创建Conn 结构体
    struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
    if (!conn)
    {
        close(connfd);
        return -1;
    }
    conn->fd = connfd;
    conn->state = STATE_REQ;
    conn->rbuf_size = 0;
    conn->wbuf_size = 0;
    conn->wbuf_sent = 0;
    conn->idle_start = get_monotonic_usec();
    dlist_insert_before(&g_data.idle_list, &conn->idle_list);
    // 将conn放到全局变量中
    conn_put(g_data.fd2conn, conn);
    return 0;
}

static void state_req(Conn *conn);
static void state_res(Conn *conn);

const size_t k_max_args = 1024;
// why 4
static int32_t parse_req(
    const uint8_t *data, size_t len, std::vector<std::string> &out)
{
    if (len < 4)
    {
        return -1;
    }
    uint32_t n = 0;
    // 获取参数的长度
    memcpy(&n, &data[0], 4);
    if (n > k_max_args)
    {
        return -1;
    }

    size_t pos = 4;
    // why pos = 4
    while (n--)
    {
        if (pos + 4 > len)
        {
            return -1;
        }
        uint32_t sz = 0;
        memcpy(&sz, &data[pos], 4);
        if (pos + 4 + sz > len)
        {
            return -1;
        }
        out.push_back(std::string((char *)&data[pos + 4], sz));
        pos += 4 + sz;
    }
    if (pos != len)
    {
        return -1;
    }
    return 0;
}

enum
{
    T_STR = 0,
    T_ZSET = 1,
};

// key的结构
struct Entry
{
    struct HNode node;
    std::string key;
    std::string val;
    uint32_t type = 0;
    ZSet *zset = NULL;
    size_t heap_idx = -1;
};

static bool entry_eq(HNode *lhs, HNode *rhs)
{
    struct Entry *le = container_of(lhs, struct Entry, node);
    struct Entry *re = container_of(rhs, struct Entry, node);
    return lhs->hcode == rhs->hcode && le->key == re->key;
}

enum
{
    ERR_UNKNOWN = 1,
    ERR_2BIG = 2,
    ERR_TYPE = 3,
    ERR_ARG = 4,
};

static void out_nil(std::string &out)
{
    out.push_back(SER_NIL);
}

// 使用char + len 替换 std::string
static void out_str(std::string &out, const char *s, size_t size)
{
    out.push_back(SER_STR);
    uint32_t len = (uint32_t)size;
    out.append((char *)&len, 4);
    out.append(s, len);
}

static void out_str(std::string &out, const std::string &val)
{
    return out_str(out, val.data(), val.size());
}

static void out_int(std::string &out, int64_t val)
{
    out.push_back(SER_INT);
    out.append((char *)&val, 8);
}

static void out_dbl(std::string &out, double val)
{
    out.push_back(SER_DBL);
    out.append((char *)&val, 8);
}

static void out_err(std::string &out, int32_t code, const std::string &msg)
{
    out.push_back(SER_ERR);
    out.append((char *)&code, 4);
    uint32_t len = (uint32_t)msg.size();
    out.append((char *)&len, 4);
    out.append(msg);
}

static void out_arr(std::string &out, uint32_t n)
{
    out.push_back(SER_ARR);
    out.append((char *)&n, 4);
}

static void out_update_arr(std::string &out, uint32_t n)
{
    assert(out[0] == SER_ARR);
    memcpy(&out[1], &n, 4);
}

/**
 * 将返回的res和reslen替换成out
 */
static void do_get(
    std::vector<std::string> &cmd, std::string &out)
{
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    // lookup
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!node)
    {
        return out_nil(out);
    }

    Entry *ent = container_of(node, Entry, node);
    if (ent->type != T_STR)
    {
        return out_err(out, ERR_TYPE, "expect string type");
    }

    return out_str(out, ent->val);
}

static void do_set(
    std::vector<std::string> &cmd, std::string &out)
{
    // 构建 Entry
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    // 先看看是否已经存在了key
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node)
    {
        // 替换
        Entry *ent = container_of(node, Entry, node);
        if (ent->type != T_STR)
        {
            return out_err(out, ERR_TYPE, "expect string type");
        }
        ent->val.swap(cmd[2]);
    }
    else
    {
        // 插入
        Entry *ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->val.swap(cmd[2]);
        hm_insert(&g_data.db, &ent->node);
    }

    return out_nil(out);
}

// 设置或删除 TTL
static void entry_set_ttl(Entry *ent, int64_t ttl_ms)
{
    if (ttl_ms < 0 && ent->heap_idx != (size_t)-1)
    {
        // 从heap中擦除item，通过将item替换到末尾
        size_t pos = ent->heap_idx;
        g_data.heap[pos] = g_data.heap.back();
        g_data.heap.pop_back();
        if (pos < g_data.heap.size())
        {
            heap_update(g_data.heap.data(), pos, g_data.heap.size());
        }
        ent->heap_idx = -1;
    }
    else if (ttl_ms >= 0)
    {
        size_t pos = ent->heap_idx;
        if (pos == (size_t)-1)
        {
            // add an new item to the heap
            HeapItem item;
            item.ref = &ent->heap_idx;
            g_data.heap.push_back(item);
            pos = g_data.heap.size() - 1;
        }
        g_data.heap[pos].val = get_monotonic_usec() + (uint64_t)ttl_ms * 1000;
        heap_update(g_data.heap.data(), pos, g_data.heap.size());
    }
}

static bool str2int(const std::string &s, int64_t &out)
{
    char *endp = NULL;
    out = strtoll(s.c_str(), &endp, 10);
    return endp == s.c_str() + s.size();
}

static void do_expire(std::vector<std::string> &cmd, std::string &out)
{
    int64_t ttl_ms = 0;
    if (!str2int(cmd[2], ttl_ms))
    {
        return out_err(out, ERR_ARG, "expect int64");
    }

    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node)
    {
        Entry *ent = container_of(node, Entry, node);
        entry_set_ttl(ent, ttl_ms);
    }
    return out_int(out, node ? 1 : 0);
}

static void do_ttl(std::vector<std::string> &cmd, std::string &out)
{
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!node)
    {
        return out_int(out, -2);
    }

    Entry *ent = container_of(node, Entry, node);
    if (ent->heap_idx == (size_t)-1)
    {
        return out_int(out, -1);
    }

    uint64_t expire_at = g_data.heap[ent->heap_idx].val;
    uint64_t now_us = get_monotonic_usec();
    return out_int(out, expire_at > now_us ? (expire_at - now_us) / 1000 : 0);
}

//
static void entry_destroy(Entry *ent)
{
    switch (ent->type)
    {
    case T_ZSET:
        zset_dispose(ent->zset);
        delete ent->zset;
        break;
    }
    delete ent;
}

static void entry_del_async(void *arg)
{
    entry_destroy((Entry *)arg);
}

// 重新包装一下
static void entry_del(Entry *ent)
{
    entry_set_ttl(ent, -1);

    const size_t k_large_container_size = 10000;
    bool too_big = false;
    switch (ent->type)
    {
    case T_ZSET:
        too_big = hm_size(&ent->zset->hmap) > k_large_container_size;
        break;
    }

    if (too_big)
    {
        thread_pool_queue(&g_data.tp, &entry_del_async, ent);
    }
    else
    {
        entry_destroy(ent);
    }
}

static void do_del(
    std::vector<std::string> &cmd, std::string &out)
{
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *node = hm_pop(&g_data.db, &key.node, &entry_eq);
    if (node)
    {
        entry_del(container_of(node, Entry, node));
    }
    return out_int(out, node ? 1 : 0);
}

static void h_scan(HTab *tab, void (*f)(HNode *, void *), void *arg)
{
    if (tab->size == 0)
    {
        return;
    }
    for (size_t i = 0; i < tab->mask + 1; i++)
    {
        HNode *node = tab->tab[i];
        while (node)
        {
            f(node, arg);
            node = node->next;
        }
    }
}

static void cb_scan(HNode *node, void *arg)
{
    std::string &out = *(std::string *)arg;
    out_str(out, container_of(node, Entry, node)->key);
}

static void do_keys(std::vector<std::string> &cmd, std::string &out)
{
    (void)cmd;
    out_arr(out, (uint32_t)hm_size(&g_data.db));
    h_scan(&g_data.db.ht1, &cb_scan, &out);
    h_scan(&g_data.db.ht2, &cb_scan, &out);
}
static bool str2dbl(const std::string &s, double &out)
{
    char *endp = NULL;
    // 将字符串转换成浮点数
    out = strtod(s.c_str(), &endp);
    // 返回是否转换成功
    return endp == s.c_str() + s.size() && !isnan(out);
}

static void do_zadd(std::vector<std::string> &cmd, std::string &out)
{
    double score = 0;
    if (!str2dbl(cmd[2], score))
    {
        return out_err(out, ERR_ARG, "expect fp number");
    }
    // 创建
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    // 查找
    HNode *hnode = hm_lookup(&g_data.db, &key.node, &entry_eq);
    Entry *ent = NULL;
    if (!hnode)
    {
        // 如果不存在就新建一个并插入 hashtable中
        ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->type = T_ZSET;
        ent->zset = new ZSet();
        hm_insert(&g_data.db, &ent->node);
    }
    else
    {
        // 如果hashtable中存在，要判定对应的这个node是否为ZSET
        ent = container_of(hnode, Entry, node);
        if (ent->type != T_ZSET)
        {
            return out_err(out, ERR_TYPE, "expect zset");
        }
    }
    // 添加到zset中
    const std::string &name = cmd[3];
    bool added = zset_add(ent->zset, name.data(), name.size(), score);
    return out_int(out, (int64_t)added);
}

static bool expect_zset(std::string &out, std::string &s, Entry **ent)
{
    // 通过s 来判定
    Entry key;
    key.key.swap(s);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *hnode = hm_lookup(&g_data.db, &key.node, &entry_eq);

    if (!hnode)
    {
        out_nil(out);
        return false;
    }

    *ent = container_of(hnode, Entry, node);
    if ((*ent)->type != T_ZSET)
    {
        out_err(out, ERR_TYPE, "expect zset");
        return false;
    }
    return true;
}
// 删除zset 中的一个key
static void do_zrem(std::vector<std::string> &cmd, std::string &out)
{
    Entry *ent = NULL;
    // 判定是否存在zset
    if (!expect_zset(out, cmd[1], &ent))
    {
        return;
    }
    // 要删除的key
    const std::string &name = cmd[2];
    // 在zset中删除节点
    ZNode *znode = zset_pop(ent->zset, name.data(), name.size());
    if (znode)
    {
        // 释放节点本身
        znode_del(znode);
    }
    // 返回删除结果,可能zset中不存在对应key
    return out_int(out, znode ? 1 : 0);
}

// 根据名字获取对应score
static void do_zscore(std::vector<std::string> &cmd, std::string &out)
{
    Entry *ent = NULL;
    if (!expect_zset(out, cmd[1], &ent))
    {
        return;
    }
    // 获取name
    const std::string &name = cmd[2];
    // 根据name获取znode 期中包含 score等信息
    ZNode *znode = zset_lookup(ent->zset, name.data(), name.size());
    // 如果存在通过out返回结果。。。 为啥要用return....
    return znode ? out_dbl(out, znode->score) : out_nil(out);
}

// 查询 命令: zquery zset score name offset limit
static void do_zquery(std::vector<std::string> &cmd, std::string &out)
{
    // 校验参数
    double score = 0;
    if (!str2dbl(cmd[2], score))
    {
        return out_err(out, ERR_ARG, "expect fp number");
    }
    const std::string &name = cmd[3];
    int64_t offset = 0;
    int64_t limit = 0;
    // 获取偏移量
    if (!str2int(cmd[4], offset))
    {
        return out_err(out, ERR_ARG, "expect int");
    }
    // 获取需要查询的数量
    if (!str2int(cmd[5], limit))
    {
        return out_err(out, ERR_ARG, "expect int");
    }

    // 从 zset 中获取数据
    Entry *ent = NULL;
    // 如果zset不存在
    if (!expect_zset(out, cmd[1], &ent))
    {
        if (out[0] == SER_NIL)
        {
            out.clear();
            out_arr(out, 0);
        }
        return;
    }

    // 查询
    if (limit <= 0)
    {
        return out_arr(out, 0);
    }
    ZNode *znode = zset_query(
        ent->zset, score, name.data(), name.size(), offset);

    // 输出
    out_arr(out, 0);
    uint32_t n = 0;
    // 遍历 znode 存在 并且在limit范围内
    while (znode && (int64_t)n < limit)
    {
        // 包装out
        out_str(out, znode->name, znode->len);
        out_dbl(out, znode->score);
        // 通过acl_offset优化查找
        znode = container_of(avl_offset(&znode->tree, +1), ZNode, tree);
        n += 2;
    }
    return out_update_arr(out, n);
}

static bool cmd_is(const std::string &word, const char *cmd)
{
    return 0 == strcasecmp(word.c_str(), cmd);
}

/**
 * 请求
 * @param uint8_t
 * @return
 */
static void do_request(std::vector<std::string> &cmd, std::string &out)
{
    if (cmd.size() == 1 && cmd_is(cmd[0], "keys"))
    {
        do_keys(cmd, out);
    }
    else if (cmd.size() == 2 && cmd_is(cmd[0], "get"))
    {
        do_get(cmd, out);
    }
    else if (cmd.size() == 3 && cmd_is(cmd[0], "set"))
    {
        do_set(cmd, out);
    }
    else if (cmd.size() == 2 && cmd_is(cmd[0], "del"))
    {
        do_del(cmd, out);
    }
    else if (cmd.size() == 3 && cmd_is(cmd[0], "pexpire"))
    {
        do_expire(cmd, out);
    }
    else if (cmd.size() == 2 && cmd_is(cmd[0], "pttl"))
    {
        do_ttl(cmd, out);
    }
    else if (cmd.size() == 4 && cmd_is(cmd[0], "zadd"))
    {
        do_zadd(cmd, out);
    }
    else if (cmd.size() == 3 && cmd_is(cmd[0], "zrem"))
    {
        do_zrem(cmd, out);
    }
    else if (cmd.size() == 3 && cmd_is(cmd[0], "zscore"))
    {
        do_zscore(cmd, out);
    }
    else if (cmd.size() == 6 && cmd_is(cmd[0], "zquery"))
    {
        do_zquery(cmd, out);
    }
    else
    {
        // cmd is not recognized
        out_err(out, ERR_UNKNOWN, "Unknown cmd");
    }
}

static bool try_one_request(Conn *conn)
{
    // 尝试解析来自缓冲区的请求
    if (conn->rbuf_size < 4)
    {
        return false;
    }
    uint32_t len = 0;
    // 填充前4位为字符串长度
    memcpy(&len, &conn->rbuf[0], 4);
    if (len > k_max_msg)
    {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }
    // 还没有塞满
    if (4 + len > conn->rbuf_size)
    {
        return false;
    }
    std::vector<std::string> cmd;
    if (0 != parse_req(&conn->rbuf[4], len, cmd))
    {
        msg("bad req");
        conn->state = STATE_END;
        return false;
    }

    std::string out;
    do_request(cmd, out);

    if (4 + out.size() > k_max_msg)
    {
        out.clear();
        out_err(out, ERR_2BIG, "response is too big");
    }

    uint32_t wlen = (uint32_t)out.size();
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], out.data(), out.size());
    conn->wbuf_size = 4 + wlen;

    // memmove remove request from buffer
    size_t remain = conn->rbuf_size - 4 - len;
    if (remain)
    {
        // 相当于 memcpy ,将 conn->rbuf[4+len]拷贝到 rbuf中，但是比 memcpy更加安全
        // memmove如果出现了重叠的d
        // remain表示要复制的字节数
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }

    conn->rbuf_size = remain;

    conn->state = STATE_RES;
    state_res(conn);
    return (conn->state == STATE_REQ);
}

static bool try_fill_buffer(Conn *conn)
{
    // 尝试填充缓冲
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;
    do
    {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN)
    {
        return false;
    }
    if (rv < 0)
    {
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }
    if (rv == 0)
    {
        if (conn->rbuf_size > 0)
        {
            msg("unexpected EOF");
        }
        else
        {
            msg("EOF");
        }
        conn->state = STATE_END;
        return false;
    }
    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));
    while (try_one_request(conn))
    {
    }
    return (conn->state == STATE_REQ);
}

static void state_req(Conn *conn)
{
    while (try_fill_buffer(conn))
    {
    }
}

// 尝试刷新缓冲
static bool try_flush_buffer(Conn *conn)
{
    size_t rv = 0;
    do
    {
        // 获取剩余的大小
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        // 写入数据，冲 wbuf_sent 开始，不能超过 remain
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN)
    {
        return false;
    }
    if (rv < 0)
    {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }
    // 更新已经发送的index
    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    // 如果写入完成，修改状态，并接收外层循环
    if (conn->wbuf_sent == conn->wbuf_size)
    {
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    return true;
}

//
static void state_res(Conn *conn)
{
    while (try_flush_buffer(conn))
    {
    }
}

// 根据状态来进行处理
static void connection_io(Conn *conn)
{

    conn->idle_start = get_monotonic_usec();
    dlist_detach(&conn->idle_list);
    dlist_insert_before(&g_data.idle_list, &conn->idle_list);

    if (conn->state == STATE_REQ)
    {
        state_req(conn);
    }
    else if (conn->state == STATE_RES)
    {
        state_res(conn);
    }
    else
    {
        assert(0); // 非预期错误
    }
}

const uint64_t k_idle_timeout_ms = 5 * 1000;

static uint32_t next_timer_ms()
{
    uint64_t now_us = get_monotonic_usec();
    uint64_t next_us = (uint64_t)-1;

    // new
    // 空闲定时器
    if (!dlist_empty(&g_data.idle_list))
    {
        Conn *next = container_of(g_data.idle_list.next, Conn, idle_list);
        next_us = next->idle_start + k_idle_timeout_ms * 1000;
    }

    // TTL 定时器
    if (!g_data.heap.empty() && g_data.heap[0].val < next_us)
    {
        next_us = g_data.heap[0].val;
    }

    if (next_us == (uint64_t)-1)
    {
        return 10000; // no timer, the value doesn't matter
    }

    if (next_us <= now_us)
    {
        // missed?
        return 0;
    }
    return (uint32_t)((next_us - now_us) / 1000);
}

static void conn_done(Conn *conn)
{
    g_data.fd2conn[conn->fd] = NULL;
    (void)close(conn->fd);
    dlist_detach(&conn->idle_list);
    free(conn);
}

static bool hnode_same(HNode *lhs, HNode *rhs)
{
    return lhs == rhs;
}

static void process_timers()
{
    // the extra 1000us is for the ms resolution of poll()
    uint64_t now_us = get_monotonic_usec() + 1000;

    // idle timers
    while (!dlist_empty(&g_data.idle_list))
    {
        Conn *next = container_of(g_data.idle_list.next, Conn, idle_list);
        uint64_t next_us = next->idle_start + k_idle_timeout_ms * 1000;
        if (next_us >= now_us)
        {
            // not ready
            break;
        }

        printf("removing idle connection: %d\n", next->fd);
        conn_done(next);
    }

    // TTL timers
    const size_t k_max_works = 2000;
    size_t nworks = 0;
    while (!g_data.heap.empty() && g_data.heap[0].val < now_us)
    {
        Entry *ent = container_of(g_data.heap[0].ref, Entry, heap_idx);
        HNode *node = hm_pop(&g_data.db, &ent->node, &hnode_same);
        assert(node == &ent->node);
        entry_del(ent);
        if (nworks++ >= k_max_works)
        {
            // don't stall the server if too many keys are expiring at once
            break;
        }
    }
}

int main()
{

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        die("socket()");
    }

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // bind
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(0);
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv)
    {
        die("bind()");
    }
    rv = listen(fd, SOMAXCONN);
    if (rv)
    {
        die("listen()");
    }

    // nio
    fd_set_nb(fd);

    dlist_init(&g_data.idle_list);
    thread_pool_init(&g_data.tp, 4);

    // the event loop
    std::vector<struct pollfd> poll_args;
    while (true)
    {
        poll_args.clear();
        // 设置监听监听的fd下标为0
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);
        for (Conn *conn : g_data.fd2conn)
        {
            if (!conn)
            {
                continue;
            }
            struct pollfd pfd = {};
            // 指定fd
            pfd.fd = conn->fd;
            // 指定时间，可能是read or write
            pfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
            pfd.events = pfd.events | POLLERR;
            poll_args.push_back(pfd);
        }

        int timeout_ms = (int)next_timer_ms();
        // 活动的 fds
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), timeout_ms);
        if (rv < 0)
        {
            die("poll");
        }
        // 处理active connection
        for (size_t i = 1; i < poll_args.size(); ++i)
        {
            // TODO
            if (poll_args[i].revents)
            {
                // 根据fd获取连接对象
                Conn *conn = g_data.fd2conn[poll_args[i].fd];
                // 执行连接，并根据状态进行处理
                connection_io(conn);
                // 如果client的连接断开，或者任务完成，就结束，并释放连接
                if (conn->state == STATE_END)
                {
                    conn_done(conn);
                }
            }
        }
        // 处理 timers
        process_timers();

        // 如果监听的fd active 就尝试创建一个新的连接
        if (poll_args[0].revents)
        {
            (void)accept_new_conn(fd);
        }
    }
    return 0;
}
