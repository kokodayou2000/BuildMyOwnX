#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "zset.h"
#include "common.h"

// 初始化节点
static ZNode *znode_new(const char *name, size_t len, double score)
{
    ZNode *node = (ZNode *)malloc(sizeof(ZNode) + len);
    assert(node);
    avl_init(&node->tree);
    node->hmap.next = NULL;
    node->hmap.hcode = str_hash((uint8_t *)name, len);
    node->score = score;
    node->len = len;
    memcpy(&node->name[0], name, len);
    return node;
}

static uint32_t min(size_t lhs, size_t rhs)
{
    return lhs < rhs ? lhs : rhs;
}

// 对比 socre 和 name
static bool zless(
    AVLNode *lhs, double score, const char *name, size_t len)
{
    // 左子树数据
    ZNode *zl = container_of(lhs, ZNode, tree);
    // 如果左子树score 不等于右子树
    if (zl->score != score)
    {
        // 当右子树socre 大于 左子树socre的时候，返回true
        return zl->score < score;
    }
    // 如果左子树和右子树score 大小相等
    // 对比名字 的ascii码
    int rv = memcmp(zl->name, name, min(zl->len, len));
    if (rv != 0)
    {
        return rv < 0;
    }
    // 如果存在 Tom 和 Tommy ，前3个ascii码相同，对比长度
    // 如果完全相同返回false
    return zl->len < len;
}
// 对比左右子树和右子树的
static bool zless(AVLNode *lhs, AVLNode *rhs)
{
    // 右子树数据 zr
    ZNode *zr = container_of(rhs, ZNode, tree);
    // 将左子树和右子树 socre name len 传入
    return zless(lhs, zr->score, zr->name, zr->len);
}

// 将node添加到zset中
static void tree_add(ZSet *zset, ZNode *node)
{
    if (!zset->tree)
    {
        zset->tree = &node->tree;
        return;
    }
    AVLNode *cur = zset->tree;
    while (true)
    {
        // 要插入到左子节点还是右子节点
        AVLNode **from = zless(&node->tree, cur) ? &cur->left : &cur->right;
        if (!*from)
        {
            *from = &node->tree;
            node->tree.parent = cur;
            zset->tree = avl_fix(&node->tree);
            break;
        }
        cur = *from;
    }
}

static void zset_update(ZSet *zset, ZNode *node, double score)
{
    if (node->score == score)
    {
        return;
    }
    zset->tree = avl_del(&node->tree);
    node->score = score;
    avl_init(&node->tree);
    tree_add(zset, node);
}

// add 如果存在name就更新或者插入
bool zset_add(ZSet *zset, const char *name, size_t len, double score)
{
    ZNode *node = zset_lookup(zset, name, len);
    if (node)
    {
        zset_update(zset, node, score);
        return false;
    }
    else
    {
        // 新创建一个node
        node = znode_new(name, len, score);
        // 根据hashtable的规则进行插入
        // 获取 HMap 以及 HNode 将HNode 插入到 HMap中
        hm_insert(&zset->hmap, &node->hmap);
        // 将节点插入到 set中
        tree_add(zset, node);
        return true;
    }
}

// 辅助查找的
struct HKey
{
    HNode node;
    const char *name = NULL;
    size_t len = 0;
};

// 对比两个node是否相等
static bool hcmp(HNode *node, HNode *key)
{
    // 如果hashcode 不同直接返回false
    if (node->hcode != key->hcode)
    {
        return false;
    }
    // HNode 转换成 ZNode
    ZNode *znode = container_of(node, ZNode, hmap);
    // HNode 转换成  HKey
    HKey *hkey = container_of(key, HKey, node);
    // 对比长度是否相等
    if (znode->len != hkey->len)
    {
        return false;
    }
    // 对比name是否相等
    return 0 == memcmp(znode->name, hkey->name, znode->len);
}

// 根据name查询到对应的节点
ZNode *zset_lookup(ZSet *zset, const char *name, size_t len)
{
    if (!zset->tree)
    {
        return NULL;
    }
    // 包装成一个HKey
    HKey key;
    key.node.hcode = str_hash((uint8_t *)name, len);
    key.name = name;
    key.len = len;
    // 查询hmap中是否存在对应name的数据
    HNode *found = hm_lookup(&zset->hmap, &key.node, &hcmp);
    if (!found)
    {
        return NULL;
    }
    // 转换为 ZNode
    return container_of(found, ZNode, hmap);
}

// 删除一个节点
ZNode *zset_pop(ZSet *zset, const char *name, size_t len)
{
    if (!zset->tree)
    {
        return NULL;
    }
    // 包装成一个HKey
    HKey key;
    key.node.hcode = str_hash((uint8_t *)name, len);
    key.name = name;
    key.len = len;
    HNode *found = hm_pop(&zset->hmap, &key.node, &hcmp);
    if (!found)
    {
        return NULL;
    }
    ZNode *node = container_of(found, ZNode, hmap);
    // 从树中删除该节点
    zset->tree = avl_del(&node->tree);
    return node;
}

// 查询大于或等于 (score,name) 的元组，然后相对于它进行偏移
ZNode *zset_query(
    ZSet *zset, double score, const char *name, size_t len, int64_t offset)
{
    AVLNode *found = NULL;
    AVLNode *cur = zset->tree;
    while (cur)
    {
        if (zless(cur, score, name, len))
        {
            cur = cur->right;
        }
        else
        {
            found = cur;
            cur = cur->left;
        }
    }
    if (found)
    {
        found = avl_offset(found, offset);
    }
    return found ? container_of(found, ZNode, tree) : NULL;
}

// 释放节点
void znode_del(ZNode *node)
{
    free(node);
}

// 递归整个树
static void tree_dispose(AVLNode *node)
{
    if (!node)
    {
        return;
    }
    tree_dispose(node->left);
    tree_dispose(node->right);
    znode_del(container_of(node, ZNode, tree));
}

void zset_dispose(ZSet *zset)
{
    // 释放整个树
    tree_dispose(zset->tree);
    // 释放整个hashtable
    hm_destroy(&zset->hmap);
}
