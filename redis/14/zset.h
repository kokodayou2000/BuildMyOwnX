#pragma once

#include "avl.h"
#include "hashtable.h"

struct ZSet
{
    AVLNode *tree = NULL;
    HMap hmap;
};

struct ZNode
{
    AVLNode tree;
    HNode hmap;
    double score = 0;
    size_t len = 0;
    char name[0];
};

// 向zset中添加
bool zset_add(ZSet *zset, const char *name, size_t len, double score);
// 查找 by name
ZNode *zset_lookup(ZSet *zset, const char *name, size_t len);
// 查找并弹出
ZNode *zset_pop(ZSet *zset, const char *name, size_t len);
// 范围查询
ZNode *zset_query(
    ZSet *zset, double score, const char *name, size_t len, int64_t offset);
// 消耗
void zset_dispose(ZSet *zset);
// 删除一个节点
void znode_del(ZNode *node);
