#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <set>
#include "avl.cpp"

#define container_of(ptr, type, member) ({                  \
    const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
    (type *)( (char *)__mptr - offsetof(type, member) ); })

struct Data
{
    AVLNode node;
    uint32_t val = 0;
};

struct Container
{
    AVLNode *root = NULL;
};

static void add(Container &c, uint32_t val)
{
    Data *data = new Data();
    avl_init(&data->node);
    data->val = val;
    if (!c.root)
    {
        c.root = &data->node;
        return;
    }

    AVLNode *cur = c.root;
    while (true)
    {
        AVLNode **from = 
            (val < container_of(cur,Data,node)->val)
            ? &cur->left : &cur->right;
        if (!*from)
        {
            *from = &data->node;
            data->node.parent = cur;
            c.root = avl_fix(&data->node);
            break;
        }
        cur = *from;
    }
    
}

static bool del(Container &c,uint32_t val){
    AVLNode *cur = c.root;
    while (cur)
    {
        uint32_t node_val = container_of(cur,Data,node)->val;
        if (val == node_val)
        {
            break;            
        }
        cur = val < node_val ? cur->left : cur->right;        
    }
    if (!cur)
    {
        return false;
    }
    c.root = avl_del(cur);
    delete container_of(cur,Data,node);
    return true;    
    
}