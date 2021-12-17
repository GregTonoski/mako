/*!
 * map.h - hash tables for mako
 * Copyright (c) 2021, Christopher Jeffrey (MIT License).
 * https://github.com/chjj/mako
 */

#ifndef BTC_MAP_H
#define BTC_MAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "common.h"
#include "types.h"

/*
 * Map
 */

#define BTC_DEFINE_MAP(name, key_t, val_t, scope)            \
                                                             \
scope name##_t *                                             \
name##_create(void);                                         \
                                                             \
scope void                                                   \
name##_destroy(name##_t *map);                               \
                                                             \
scope void                                                   \
name##_reset(name##_t *map);                                 \
                                                             \
scope void                                                   \
name##_resize(name##_t *map, size_t size);                   \
                                                             \
scope size_t                                                 \
name##_size(const name##_t *map);                            \
                                                             \
scope size_t                                                 \
name##_buckets(const name##_t *map);                         \
                                                             \
scope int                                                    \
name##_has(const name##_t *map, const key_t key);            \
                                                             \
scope val_t                                                  \
name##_get(const name##_t *map, const key_t key);            \
                                                             \
scope int                                                    \
name##_put(name##_t *map, const key_t key, const val_t val); \
                                                             \
scope key_t                                                  \
name##_del(name##_t *map, const key_t key);                  \
                                                             \
scope val_t                                                  \
name##_rem(name##_t *map, const key_t key);                  \
                                                             \
scope void                                                   \
name##_iterate(name##iter_t *iter, const name##_t *map);     \
                                                             \
scope int                                                    \
name##_next(name##iter_t *iter)

/*
 * Set
 */

#define BTC_DEFINE_SET(name, key_t, scope)               \
                                                         \
scope name##_t *                                         \
name##_create(void);                                     \
                                                         \
scope void                                               \
name##_destroy(name##_t *map);                           \
                                                         \
scope void                                               \
name##_reset(name##_t *map);                             \
                                                         \
scope void                                               \
name##_resize(name##_t *map, size_t size);               \
                                                         \
scope size_t                                             \
name##_size(const name##_t *map);                        \
                                                         \
scope size_t                                             \
name##_buckets(const name##_t *map);                     \
                                                         \
scope int                                                \
name##_has(const name##_t *map, const key_t key);        \
                                                         \
scope int                                                \
name##_put(name##_t *map, const key_t key);              \
                                                         \
scope key_t                                              \
name##_del(name##_t *map, const key_t key);              \
                                                         \
scope void                                               \
name##_iterate(name##iter_t *iter, const name##_t *map); \
                                                         \
scope int                                                \
name##_next(name##iter_t *iter)

/*
 * Maps (Key->Pointer)
 */

BTC_DEFINE_MAP(btc_intmap, uint32_t, void *, BTC_EXTERN);
BTC_DEFINE_MAP(btc_longmap, uint64_t, void *, BTC_EXTERN);
BTC_DEFINE_MAP(btc_hashmap, uint8_t *, void *, BTC_EXTERN);
BTC_DEFINE_MAP(btc_outmap, btc_outpoint_t *, void *, BTC_EXTERN);
/* BTC_DEFINE_MAP(btc_invmap, btc_invitem_t *, void *, BTC_EXTERN); */
BTC_DEFINE_MAP(btc_addrmap, btc_netaddr_t *, void *, BTC_EXTERN);
/* BTC_DEFINE_MAP(btc_addrmap, btc_address_t *, void *, BTC_EXTERN); */

/*
 * Tables (Key->Integer)
 */

/* BTC_DEFINE_MAP(btc_inttab, uint32_t, int64_t, BTC_EXTERN); */
BTC_DEFINE_MAP(btc_longtab, uint64_t, int64_t, BTC_EXTERN);
BTC_DEFINE_MAP(btc_hashtab, uint8_t *, int64_t, BTC_EXTERN);
/* BTC_DEFINE_MAP(btc_outtab, btc_outpoint_t *, int64_t, BTC_EXTERN); */
/* BTC_DEFINE_MAP(btc_invtab, btc_invitem_t *, int64_t, BTC_EXTERN); */
/* BTC_DEFINE_MAP(btc_addrtab, btc_netaddr_t *, uint64_t, BTC_EXTERN); */
/* BTC_DEFINE_MAP(btc_addrtab, btc_address_t *, uint64_t, BTC_EXTERN); */

/*
 * Sets (Key)
 */

/* BTC_DEFINE_SET(btc_intset, uint32_t, BTC_EXTERN); */
BTC_DEFINE_SET(btc_longset, uint64_t, BTC_EXTERN);
BTC_DEFINE_SET(btc_hashset, uint8_t *, BTC_EXTERN);
BTC_DEFINE_SET(btc_outset, btc_outpoint_t *, BTC_EXTERN);
/* BTC_DEFINE_SET(btc_invset, btc_invitem_t *, BTC_EXTERN); */
/* BTC_DEFINE_SET(btc_addrset, btc_netaddr_t *, BTC_EXTERN); */
/* BTC_DEFINE_SET(btc_addrset, btc_address_t *, BTC_EXTERN); */

#ifdef __cplusplus
}
#endif

#endif /* BTC_MAP_H */
