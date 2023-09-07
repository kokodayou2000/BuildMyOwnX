#include <stddef.h>
#include <stdint.h>

struct AVLNode
{
    uint32_t depth = 0;
    uint32_t cnt = 0;
    AVLNode *left = NULL;
    AVLNode *right = NULL;
    AVLNode *parent = NULL;
};

static void avl_init(AVLNode *node)
{
    node->depth = 1;
    node->cnt = 1;
    node->left = node->right = node->parent = NULL;
}

// 获取当前节点深度
static uint32_t avl_depth(AVLNode *node)
{
    return node ? node->depth : 0;
}

// 获取当前节点子节点数
static uint32_t avl_cnt(AVLNode *node)
{
    return node ? node->cnt : 0;
}

// 左边还是右边
static uint32_t max(uint32_t lhs, uint32_t rhs)
{
    return lhs < rhs ? rhs : lhs;
}

// 更新节点的状态
static void avl_update(AVLNode *node)
{
    // 当前节点的深度是
    node->depth = 1 + max(avl_depth(node->left), avl_depth(node->right));
    // 当前节点的子节点总数
    node->cnt = 1 + avl_cnt(node->left) + avl_cnt(node->right);
}

// 左旋转 一种逆时针旋转操作
//      A                 B
//     / \               / \
//    B   C             D   A
//   / \                   / \
//  D   E                 C   E
// 以B为中心左旋转
// 将B移动到A，将A移动到C，将B原有的E变成A的E
// 左节点的深度都-1 右节点的深度都+1

//        A             A
//       / \           / \ 
//      B   C   ->    D   C
//     /             / \
//    D             E   B
//   /
//  E
static AVLNode *rot_left(AVLNode *node)
{
    // 新创建一个节点, 指向右节点
    AVLNode *new_node = node->right;
    // 如果存在左节点
    if (new_node->left)
    {
        new_node->left->parent = node;
    }
    node->right = new_node->left;
    new_node->left = node;
    new_node->parent = node->parent;
    node->parent = new_node;
    avl_update(node);
    avl_update(new_node);
    return new_node;
}

//        A                A
//       / \              / \ 
//      B   C   ->       B   D
//           \              / \
//            D            C   E
//             \ 
//              E
// 对B进行旋转
static AVLNode *rot_right(AVLNode *node)
{
    // 新节点指向D
    AVLNode *new_node = node->left;
    // D没有右节点，这个步骤不进行
    if (new_node->right)
    {
        new_node->right->parent = node;
    }
    // B的左节节点指向 D的右节点(NULL)
    node->left = new_node->right;
    // D的右节点指向B
    new_node->right = node;
    // D的父节点指向B的父节点
    new_node->parent = node->parent;
    // B的父节点变成D 完成D B的替换
    node->parent = new_node;
    // 根据节点的状态
    avl_update(node);
    avl_update(new_node);
    return new_node;
}

// 左子树深度过深了
static AVLNode *avl_fix_left(AVLNode *root)
{
    //        A             A
    //       / \           / \ 
    //      B   C   ->    D   C
    //     /             / \
    //    D             E   B
    //   /
    //  E
    // 假如root是A B的左节点是D，B的右节点是NULL
    // D的深度大于0，执行左旋转
    // root的left指向返回的
    if (avl_depth(root->left->left) < avl_depth(root->left->right))
    {
        root->left = rot_left(root->left);
    }
    return rot_right(root);
}

// 右子树深度过深了
static AVLNode *avl_fix_right(AVLNode *root)
{

    //        A                A
    //       / \              / \ 
    //      B   C   ->       B   D
    //           \              / \
    //            D            C   E
    //             \ 
    //              E
    // 假如root是A B的左节点是D，B的右节点是NULL
    // D的深度大于0，执行左旋转
    // root的left指向返回的
    if (avl_depth(root->right->right) < avl_depth(root->right->left))
    {
        root->right = rot_right(root->right);
    }
    return rot_left(root);
}

static AVLNode *avl_fix(AVLNode *node)
{
    while (true)
    {
        // 更新当前节点状态
        avl_update(node);
        // 获取左节点深度
        uint32_t l = avl_depth(node->left);
        // 获取右节点深度
        uint32_t r = avl_depth(node->right);
        AVLNode **from = NULL;
        // 如果该节点存在父节点
        if (node->parent)
        {
            // 看看当前节点是父节点的子节点还是右节点
            // 并让 from 指向 node
            from = (node->parent->left == node)
                       ? &node->parent->left
                       : &node->parent->right;
        }
        // 如果左节点的深度过深
        if (l == r + 2)
        {
            node = avl_fix_left(node);
        }
        else if (l + 2 == r)
        {
            node = avl_fix_right(node);
        }
        // 如果 不存在父节点，就把node返回，node就是根节点了，直接结束即可
        // 想递归，但又没有递归
        if (!from)
        {
            return node;
        }
        // 如果 存在父节点了，让 from 指向 node，记录node的地址
        *from = node;
        // node 指向 父节点，一种替换？
        node = node->parent;
    }
}

// 删除一个节点，并返回一个新的根节点

//       A
//      / \ 
//     D   C
//    / \
//   E   B
static AVLNode *avl_del(AVLNode *node)
{
    // 删除D节点，其中D的右节点不存在
    //       A
    //      / \ 
    //     D   C
    //    /
    //   E
    if (node->right == NULL)
    {
        // parent 指向要删除节点的父节点
        AVLNode *parent = node->parent;
        if (node->left)
        {
            // 将左子树的parent连接到parent，将D架空 A 直接链接 E
            node->left->parent = parent;
        }
        // 并不是root节点
        if (parent)
        {
            // 如果要删除的节点是左节点就让父节点的左节点指向左孙子节点，同理
            (parent->left == node ? parent->left : parent->right) = node->left;
            // 避免过深
            return avl_fix(parent);
        }
        else
        {
            // 如果要删除的是根节点，并且右节点为NULL，返回左节点即可
            // 并没有free
            return node->left;
        }
    }
    else
    {
        // 假设要删除A节点
        //       A
        //      / \ 
        //     D   C
        //    /   / \ 
        //   E   F   G
        //      /
        //     H
        // 存在右节点的情况
        // 将节点与下一同级节点交换
        AVLNode *victim = node->right;
        while (victim->left)
        {
            // victim一直指向到最左的节点，比如 G
            victim = victim->left;
        }
        AVLNode *root = avl_del(victim);
        *victim = *node;
        if (victim->left)
        {
            victim->left->parent = victim;
        }
        if (victim->right)
        {
            victim->right->parent = victim;
        }
        AVLNode *parent = node->parent;
        if (parent)
        {
            (parent->left == node ? parent->left : parent->right) = victim;
            return root;
        }
        else
        {
            return victim;
        }
    }
}