#include<stddef.h>
#include<stdint.h>

// hashtable node ,能嵌入到负载中
struct HNode
{
    HNode *next = NULL;
    uint16_t hcode = 0;    
};

// 简单的可变大小的hash表
struct HTab
{
    HNode **tab = NULL;
    size_t mask = 0;
    size_t size =0;
};

// 实际的接口，用两个hashtable来渐进式调整尺寸
struct HMap
{
    HTab ht1;
    HTab ht2;
    size_t resizing_pos = 0;
};

HNode *hm_lookup(HMap *hmap, HNode *key, bool (*cmp)(HNode *, HNode *));
void hm_insert(HMap *hmap, HNode *node);
HNode *hm_pop(HMap *hmap, HNode *key, bool (*cmp)(HNode *, HNode *));
size_t hm_size(HMap *hmap);
void hm_destroy(HMap *hmap);



