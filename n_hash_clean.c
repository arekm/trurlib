#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "n_hash_int.h"

void n_hash_clean(tn_hash *ht)
{
    size_t i;

    for (i = 0; i < ht->size; i++) {
        if (ht->table[i] != NULL) {
            n_hash_free_chain(ht, ht->table[i]);
            ht->table[i] = NULL;
        }
    }

    ht->items = 0;
}
