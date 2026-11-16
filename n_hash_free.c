#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "n_hash_int.h"

void n_hash_free_chain(tn_hash *ht, struct hash_bucket *node)
{
    register struct hash_bucket *next_node;

    for (; node != NULL; node = next_node) {
        next_node = node->next;

        if (ht->free_fn != NULL && node->data != NULL) {
            DBGF("free %s %p\n", node->key, node->data);
            ht->free_fn(node->data);
        }
        if (ht->na)
            ht->na->na_free(ht->na, node);
        else
            free(node);
    }
}


void n_hash_free(tn_hash *ht)
{
    size_t i;

    if (ht->_refcnt > 0) {
        ht->_refcnt--;
        return;
    }

#if ENABLE_TRACE    
    n_hash_stats(ht);
#endif
    
    for (i = 0; i < ht->size; i++) {
        if (ht->table[i])
            n_hash_free_chain(ht, ht->table[i]);
    }

    free(ht->table);
    
    ht->table = NULL;
    ht->size = 0;
    ht->items = 0;

    if (ht->na == NULL)
        free(ht);
    else {
        tn_alloc *na = ht->na;

        na->na_free(na, ht);
        n_alloc_free(na);
    }
}
