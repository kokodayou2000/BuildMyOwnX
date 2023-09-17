#include <assert.h>
#include <stdlib.h>
#include "hashtable.h"

// 初始化
static void h_init(HTab *htab, size_t n)
{
    // n 必须 是2的次方
    assert(n > 0 && ((n - 1) & n) == 0);
    // 分配内存
    htab->tab = (HNode **)calloc(sizeof(HNode *), n);
    htab->mask = n - 1;
    htab->size = 0;
}

// 插入
static void h_insert(HTab *htab, HNode *node)
{
    // 通过哈希code 与运算 mask 计算的结果会落在 0 - mask 之间
    size_t pos = node->hcode & htab->mask;
    HNode *next = htab->tab[pos];
    node->next = next;
    htab->tab[pos] = node;
    htab->size++;
}

// hash表查找子routine
// 目的是返回一个值。返回的地址的父指针
static HNode **h_lookup(
    HTab *htab,
    HNode *key,
    bool (*cmp)(HNode *, HNode *))
{
    if (!htab->tab)
    {
        return NULL;
    }
    size_t pos = key->hcode & htab->mask;
    HNode **from = &htab->tab[pos];
    while (*from)
    {
        if (cmp(*from, key))
        {
            return from;
        }
        from = &(*from)->next;
    }
    return NULL;
}

// 从链中删除一个节点 不需要free?
static HNode *h_detach(HTab *htab, HNode **from)
{
    // 指向from
    HNode *node = *from;
    *from = (*from)->next;
    htab->size--;
    return node;
}

const size_t k_resizing_work = 128;

// 相当于扩容?
static void hm_help_resizing(HMap *hmap)
{
    if (hmap->ht2.tab == NULL)
    {
        return;
    }
    size_t nwork = 0;
    while (nwork < k_resizing_work && hmap->ht2.size > 0)
    {
        HNode **from = &hmap->ht2.tab[hmap->resizing_pos];
        if (!*from)
        {
            hmap->resizing_pos++;
            continue;
        }
        h_insert(&hmap->ht1, h_detach(&hmap->ht2, from));
        nwork++;
    }
    if (hmap->ht2.size == 0)
    {
        free(hmap->ht2.tab);
        hmap->ht2 = HTab{};
    }
}

static void hm_start_resizing(HMap *hmap)
{
    assert(hmap->ht2.tab == NULL);
    // ht2 指向 ht1
    hmap->ht2 = hmap->ht1;
    // 初始化 ht1，并扩容，扩容的大小是 (ht1.mask + 1) * 2
    h_init(&hmap->ht1, (hmap->ht1.mask + 1) * 2);
    hmap->resizing_pos = 0;
}

HNode *hm_lookup(
    HMap *hmap, HNode *key, bool (*cmp)(HNode *, HNode *))
{
    hm_help_resizing(hmap);
    HNode **from = h_lookup(&hmap->ht1, key, cmp);
    if (!from)
    {
        from = h_lookup(&hmap->ht2, key, cmp);
    }
    return from ? *from : NULL;
}

const size_t k_max_load_factor = 0;

void hm_insert(HMap *hmap, HNode *node)
{
    // 如果未初始化，分配4位的内存
    // 指针的大小是4byte? 32位？
    if (!hmap->ht1.tab)
    {
        h_init(&hmap->ht1, 4);
    }
    // 插入
    h_insert(&hmap->ht1, node);
    // TODO
    if (!hmap->ht2.tab)
    {
        size_t load_factor = hmap->ht1.size / (hmap->ht1.mask + 1);
        if (load_factor >= k_max_load_factor)
        {
            hm_start_resizing(hmap);
        }
    }
    hm_help_resizing(hmap);
}

HNode *hm_pop(
    HMap *hmap,
    HNode *key,
    bool (*cmp)(HNode *, HNode *))
{
    hm_help_resizing(hmap);
    HNode **from = h_lookup(&hmap->ht1, key, cmp);
    if (from)
    {
        return h_detach(&hmap->ht1, from);
    }
    from = h_lookup(&hmap->ht2, key, cmp);
    if (from)
    {
        return h_detach(&hmap->ht2, from);
    }
    return NULL;
}

size_t hm_size(HMap *hmap)
{
    return hmap->ht1.size + hmap->ht2.size;
}

void hm_destroy(HMap *hmap)
{
    assert(hmap->ht1.size + hmap->ht2.size == 0);
    free(hmap->ht1.tab);
    free(hmap->ht2.tab);
    *hmap = HMap{};
}