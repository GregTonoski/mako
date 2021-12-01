/*!
 * chaindb.c - chaindb for mako
 * Copyright (c) 2021, Christopher Jeffrey (MIT License).
 * https://github.com/chjj/mako
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lmdb.h>

#include <io/core.h>
#include <node/chaindb.h>
#include <mako/block.h>
#include <mako/coins.h>
#include <mako/consensus.h>
#include <mako/crypto/hash.h>
#include <mako/entry.h>
#include <mako/list.h>
#include <mako/map.h>
#include <mako/network.h>
#include <mako/tx.h>
#include <mako/util.h>
#include <mako/vector.h>

#include "../bio.h"
#include "../impl.h"
#include "../internal.h"

/*
 * Constants
 */

static const uint8_t tip_key[1] = {'R'};
static const uint8_t blockfile_key[1] = {'B'};
static const uint8_t undofile_key[1] = {'U'};

#define WRITE_FLAGS (BTC_O_RDWR | BTC_O_CREAT | BTC_O_APPEND)
#define READ_FLAGS (BTC_O_RDONLY | BTC_O_RANDOM)
#define MAX_FILE_SIZE (128 << 20)

/*
 * Chain File
 */

typedef struct btc_chainfile_s {
  int fd;
  uint8_t type;
  int32_t id;
  int32_t pos;
  int32_t items;
  int64_t min_time;
  int64_t max_time;
  int32_t min_height;
  int32_t max_height;
  struct btc_chainfile_s *prev;
  struct btc_chainfile_s *next;
} btc_chainfile_t;

DEFINE_SERIALIZABLE_OBJECT(btc_chainfile, SCOPE_STATIC)

static void
btc_chainfile_init(btc_chainfile_t *z) {
  z->fd = -1;
  z->type = 0;
  z->id = 0;
  z->pos = 0;
  z->items = 0;
  z->min_time = -1;
  z->max_time = -1;
  z->min_height = -1;
  z->max_height = -1;
  z->prev = NULL;
  z->next = NULL;
}

static void
btc_chainfile_clear(btc_chainfile_t *z) {
  btc_chainfile_init(z);
}

static void
btc_chainfile_copy(btc_chainfile_t *z, const btc_chainfile_t *x) {
  z->fd = -1;
  z->type = x->type;
  z->id = x->id;
  z->pos = x->pos;
  z->items = x->items;
  z->min_time = x->min_time;
  z->max_time = x->max_time;
  z->min_height = x->min_height;
  z->max_height = x->max_height;
  z->prev = NULL;
  z->next = NULL;
}

static size_t
btc_chainfile_size(const btc_chainfile_t *x) {
  (void)x;
  return 37;
}

static uint8_t *
btc_chainfile_write(uint8_t *zp, const btc_chainfile_t *x) {
  zp = btc_uint8_write(zp, x->type);
  zp = btc_int32_write(zp, x->id);
  zp = btc_int32_write(zp, x->pos);
  zp = btc_int32_write(zp, x->items);
  zp = btc_int64_write(zp, x->min_time);
  zp = btc_int64_write(zp, x->max_time);
  zp = btc_int32_write(zp, x->min_height);
  zp = btc_int32_write(zp, x->max_height);
  return zp;
}

static int
btc_chainfile_read(btc_chainfile_t *z, const uint8_t **xp, size_t *xn) {
  if (!btc_uint8_read(&z->type, xp, xn))
    return 0;

  if (!btc_int32_read(&z->id, xp, xn))
    return 0;

  if (!btc_int32_read(&z->pos, xp, xn))
    return 0;

  if (!btc_int32_read(&z->items, xp, xn))
    return 0;

  if (!btc_int64_read(&z->min_time, xp, xn))
    return 0;

  if (!btc_int64_read(&z->max_time, xp, xn))
    return 0;

  if (!btc_int32_read(&z->min_height, xp, xn))
    return 0;

  if (!btc_int32_read(&z->max_height, xp, xn))
    return 0;

  return 1;
}

static void
btc_chainfile_update(btc_chainfile_t *z, const btc_entry_t *entry) {
  z->items += 1;

  if (z->min_time == -1 || entry->header.time < z->min_time)
    z->min_time = entry->header.time;

  if (z->max_time == -1 || entry->header.time > z->max_time)
    z->max_time = entry->header.time;

  if (z->min_height == -1 || entry->height < z->min_height)
    z->min_height = entry->height;

  if (z->max_height == -1 || entry->height > z->max_height)
    z->max_height = entry->height;
}

/*
 * Chain Database
 */

struct btc_chaindb_s {
  const btc_network_t *network;
  char prefix[BTC_PATH_MAX - 26];
  unsigned int flags;
  size_t map_size;
  MDB_env *env;
  MDB_dbi db_meta;
  MDB_dbi db_coin;
  MDB_dbi db_index;
  MDB_dbi db_tip;
  MDB_dbi db_file;
  btc_hashmap_t *hashes;
  btc_vector_t heights;
  btc_entry_t *head;
  btc_entry_t *tail;
  struct btc_chainfiles_s {
    btc_chainfile_t *head;
    btc_chainfile_t *tail;
    size_t length;
  } files;
  btc_chainfile_t block;
  btc_chainfile_t undo;
  uint8_t *slab;
};

static void
btc_chaindb_path(btc_chaindb_t *db, char *path, int type, int id) {
  const char *tag = (type == 0 ? "blk" : "rev");

#if defined(_WIN32)
  sprintf(path, "%s\\blocks\\%s%.5d.dat", db->prefix, tag, id);
#else
  sprintf(path, "%s/blocks/%s%.5d.dat", db->prefix, tag, id);
#endif
}

static void
btc_chaindb_init(btc_chaindb_t *db, const btc_network_t *network) {
  uint64_t gb = sizeof(void *) >= 8 ? 16 : 1;

  memset(db, 0, sizeof(*db));

  db->network = network;
  db->prefix[0] = '/';
  db->hashes = btc_hashmap_create();
  db->flags = BTC_CHAIN_DEFAULT_FLAGS;
  db->map_size = (size_t)(gb << 30);

  btc_vector_init(&db->heights);

  db->slab = (uint8_t *)btc_malloc(24 + BTC_MAX_RAW_BLOCK_SIZE);
}

static void
btc_chaindb_clear(btc_chaindb_t *db) {
  btc_hashmap_destroy(db->hashes);
  btc_vector_clear(&db->heights);
  btc_free(db->slab);
  memset(db, 0, sizeof(*db));
}

btc_chaindb_t *
btc_chaindb_create(const btc_network_t *network) {
  btc_chaindb_t *db = (btc_chaindb_t *)btc_malloc(sizeof(btc_chaindb_t));
  btc_chaindb_init(db, network);
  return db;
}

void
btc_chaindb_destroy(btc_chaindb_t *db) {
  btc_chaindb_clear(db);
  btc_free(db);
}

void
btc_chaindb_set_mapsize(btc_chaindb_t *db, size_t map_size) {
  db->map_size = map_size;
}

static int
btc_chaindb_load_prefix(btc_chaindb_t *db, const char *prefix) {
  char path[BTC_PATH_MAX];

  if (!btc_path_resolve(db->prefix, sizeof(db->prefix), prefix, 0))
    return 0;

  if (!btc_fs_mkdirp(db->prefix, 0755))
    return 0;

  if (!btc_path_join(path, sizeof(path), db->prefix, "blocks", 0))
    return 0;

  if (!btc_fs_exists(path) && !btc_fs_mkdir(path, 0755))
    return 0;

  if (!btc_path_join(path, sizeof(path), db->prefix, "chain", 0))
    return 0;

  if (!btc_fs_exists(path) && !btc_fs_mkdir(path, 0755))
    return 0;

  return 1;
}

static int
btc_chaindb_load_database(btc_chaindb_t *db) {
  char path[BTC_PATH_MAX];
  unsigned int flags;
  MDB_txn *txn;
  int rc;

  flags = MDB_WRITEMAP
        | MDB_NOMETASYNC
        | MDB_NOSYNC
        | MDB_MAPASYNC
        | MDB_NOTLS
        | MDB_NOLOCK;

  rc = mdb_env_create(&db->env);

  if (rc != 0) {
    fprintf(stderr, "mdb_env_create: %s\n", mdb_strerror(rc));
    return 0;
  }

  rc = mdb_env_set_mapsize(db->env, db->map_size);

  if (rc != 0) {
    fprintf(stderr, "mdb_env_set_mapsize: %s\n", mdb_strerror(rc));
    mdb_env_close(db->env);
    return 0;
  }

  rc = mdb_env_set_maxdbs(db->env, 10);

  if (rc != 0) {
    fprintf(stderr, "mdb_env_set_maxdbs: %s\n", mdb_strerror(rc));
    mdb_env_close(db->env);
    return 0;
  }

  if (!btc_path_join(path, sizeof(path), db->prefix, "chain", 0)) {
    fprintf(stderr, "mdb_env_open: path too long\n");
    mdb_env_close(db->env);
    return 0;
  }

  rc = mdb_env_open(db->env, path, flags, 0644);

  if (rc != 0) {
    fprintf(stderr, "mdb_env_open: %s\n", mdb_strerror(rc));
    mdb_env_close(db->env);
    return 0;
  }

  rc = mdb_txn_begin(db->env, NULL, 0, &txn);

  if (rc != 0) {
    fprintf(stderr, "mdb_txn_begin: %s\n", mdb_strerror(rc));
    mdb_env_close(db->env);
    return 0;
  }

  rc = mdb_dbi_open(txn, "meta", MDB_CREATE, &db->db_meta);

  if (rc != 0) {
    fprintf(stderr, "mdb_dbi_open: %s\n", mdb_strerror(rc));
    mdb_txn_abort(txn);
    mdb_env_close(db->env);
    return 0;
  }

  rc = mdb_dbi_open(txn, "coin", MDB_CREATE, &db->db_coin);

  if (rc != 0) {
    fprintf(stderr, "mdb_dbi_open: %s\n", mdb_strerror(rc));
    mdb_txn_abort(txn);
    mdb_env_close(db->env);
    return 0;
  }

  rc = mdb_dbi_open(txn, "index", MDB_CREATE, &db->db_index);

  if (rc != 0) {
    fprintf(stderr, "mdb_dbi_open: %s\n", mdb_strerror(rc));
    mdb_txn_abort(txn);
    mdb_env_close(db->env);
    return 0;
  }

  rc = mdb_dbi_open(txn, "tip", MDB_CREATE, &db->db_tip);

  if (rc != 0) {
    fprintf(stderr, "mdb_dbi_open: %s\n", mdb_strerror(rc));
    mdb_txn_abort(txn);
    mdb_env_close(db->env);
    return 0;
  }

  rc = mdb_dbi_open(txn, "file", MDB_CREATE, &db->db_file);

  if (rc != 0) {
    fprintf(stderr, "mdb_dbi_open: %s\n", mdb_strerror(rc));
    mdb_txn_abort(txn);
    mdb_env_close(db->env);
    return 0;
  }

  rc = mdb_txn_commit(txn);

  if (rc != 0) {
    fprintf(stderr, "mdb_txn_commit: %s\n", mdb_strerror(rc));
    mdb_env_close(db->env);
    return 0;
  }

  return 1;
}

static void
btc_chaindb_unload_database(btc_chaindb_t *db) {
  mdb_dbi_close(db->env, db->db_meta);
  mdb_dbi_close(db->env, db->db_coin);
  mdb_dbi_close(db->env, db->db_index);
  mdb_dbi_close(db->env, db->db_tip);
  mdb_dbi_close(db->env, db->db_file);
  mdb_env_close(db->env);
}

static int
btc_chaindb_load_files(btc_chaindb_t *db) {
  char path[BTC_PATH_MAX];
  btc_chainfile_t *file;
  MDB_val mkey, mval;
  MDB_cursor *cur;
  MDB_txn *txn;
  int rc;

  CHECK(mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn) == 0);

  /* Read best block file. */
  mkey.mv_data = (uint8_t *)blockfile_key;
  mkey.mv_size = sizeof(blockfile_key);

  rc = mdb_get(txn, db->db_meta, &mkey, &mval);

  if (rc != MDB_NOTFOUND) {
    CHECK(rc == 0);
    CHECK(btc_chainfile_import(&db->block, mval.mv_data, mval.mv_size));
    CHECK(db->block.type == 0);
  } else {
    btc_chainfile_init(&db->block);
    db->block.type = 0;
  }

  /* Read best undo file. */
  mkey.mv_data = (uint8_t *)undofile_key;
  mkey.mv_size = sizeof(undofile_key);

  rc = mdb_get(txn, db->db_meta, &mkey, &mval);

  if (rc != MDB_NOTFOUND) {
    CHECK(rc == 0);
    CHECK(btc_chainfile_import(&db->undo, mval.mv_data, mval.mv_size));
    CHECK(db->undo.type == 1);
  } else {
    btc_chainfile_init(&db->undo);
    db->undo.type = 1;
  }

  /* Read file index and build vector. */
  CHECK(mdb_cursor_open(txn, db->db_file, &cur) == 0);

  rc = mdb_cursor_get(cur, &mkey, &mval, MDB_FIRST);

  while (rc == 0) {
    file = btc_chainfile_create();

    CHECK(btc_chainfile_import(file, mval.mv_data, mval.mv_size));

    btc_list_push(&db->files, file, btc_chainfile_t);

    rc = mdb_cursor_get(cur, &mkey, &mval, MDB_NEXT);
  }

  CHECK(rc == MDB_NOTFOUND);

  mdb_cursor_close(cur);
  mdb_txn_abort(txn);

  /* Open block file for writing. */
  btc_chaindb_path(db, path, 0, db->block.id);

  db->block.fd = btc_fs_open(path, WRITE_FLAGS, 0644);

  CHECK(db->block.fd != -1);

  /* Open undo file for writing. */
  btc_chaindb_path(db, path, 1, db->undo.id);

  db->undo.fd = btc_fs_open(path, WRITE_FLAGS, 0644);

  CHECK(db->undo.fd != -1);

  return 1;
}

static void
btc_chaindb_unload_files(btc_chaindb_t *db) {
  btc_chainfile_t *file, *next;

  btc_fs_fsync(db->block.fd);
  btc_fs_fsync(db->undo.fd);

  btc_fs_close(db->block.fd);
  btc_fs_close(db->undo.fd);

  for (file = db->files.head; file != NULL; file = next) {
    next = file->next;
    btc_chainfile_destroy(file);
  }

  btc_list_reset(&db->files);
}

static int
btc_chaindb_init_index(btc_chaindb_t *db) {
  btc_view_t *view = btc_view_create();
  btc_entry_t *entry = btc_entry_create();
  btc_block_t block;

  btc_block_init(&block);
  btc_block_import(&block, db->network->genesis.data,
                           db->network->genesis.length);

  btc_entry_set_block(entry, &block, NULL);

  CHECK(btc_chaindb_save(db, entry, &block, view));

  btc_block_clear(&block);
  btc_view_destroy(view);

  return 1;
}

static int
btc_chaindb_load_index(btc_chaindb_t *db) {
  btc_entry_t *entry, *tip;
  btc_entry_t *gen = NULL;
  btc_hashmapiter_t iter;
  uint8_t tip_hash[32];
  MDB_val mkey, mval;
  MDB_cursor *cur;
  MDB_txn *txn;
  int rc;

  CHECK(mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn) == 0);

  /* Read tip hash. */
  {
    mkey.mv_data = (uint8_t *)tip_key;
    mkey.mv_size = sizeof(tip_key);

    rc = mdb_get(txn, db->db_meta, &mkey, &mval);

    if (rc == MDB_NOTFOUND) {
      mdb_txn_abort(txn);
      return btc_chaindb_init_index(db);
    }

    CHECK(rc == 0);
    CHECK(mval.mv_size == 32);

    memcpy(tip_hash, mval.mv_data, 32);
  }

  /* Read block index and create hash->entry map. */
  CHECK(mdb_cursor_open(txn, db->db_index, &cur) == 0);

  rc = mdb_cursor_get(cur, &mkey, &mval, MDB_FIRST);

  while (rc == 0) {
    entry = btc_entry_create();

    CHECK(btc_entry_import(entry, mval.mv_data, mval.mv_size));
    CHECK(btc_hashmap_put(db->hashes, entry->hash, entry));

    rc = mdb_cursor_get(cur, &mkey, &mval, MDB_NEXT);
  }

  CHECK(rc == MDB_NOTFOUND);

  mdb_cursor_close(cur);
  mdb_txn_abort(txn);

  /* Create `prev` links and retrieve genesis block. */
  btc_hashmap_iterate(&iter, db->hashes);

  while (btc_hashmap_next(&iter)) {
    entry = iter.val;

    if (entry->height == 0) {
      gen = entry;
      continue;
    }

    entry->prev = btc_hashmap_get(db->hashes, entry->header.prev_block);

    CHECK(entry->prev != NULL);
  }

  CHECK(gen != NULL);

  /* Retrieve tip. */
  tip = btc_hashmap_get(db->hashes, tip_hash);

  CHECK(tip != NULL);

  /* Create height->entry vector. */
  btc_vector_grow(&db->heights, (btc_hashmap_size(db->hashes) * 3) / 2);
  btc_vector_resize(&db->heights, tip->height + 1);

  /* Populate height vector and create `next` links. */
  entry = tip;

  do {
    CHECK((size_t)entry->height < db->heights.length);

    db->heights.items[entry->height] = entry;

    if (entry->prev != NULL)
      entry->prev->next = entry;

    entry = entry->prev;
  } while (entry != NULL);

  db->head = gen;
  db->tail = tip;

  return 1;
}

static void
btc_chaindb_unload_index(btc_chaindb_t *db) {
  btc_hashmapiter_t iter;

  btc_hashmap_iterate(&iter, db->hashes);

  while (btc_hashmap_next(&iter))
    btc_entry_destroy(iter.val);

  btc_hashmap_reset(db->hashes);
  btc_vector_clear(&db->heights);

  db->head = NULL;
  db->tail = NULL;
}

int
btc_chaindb_open(btc_chaindb_t *db,
                 const char *prefix,
                 unsigned int flags) {
  db->flags = flags;

  if (!btc_chaindb_load_prefix(db, prefix))
    return 0;

  if (!btc_chaindb_load_database(db))
    return 0;

  if (!btc_chaindb_load_files(db))
    return 0;

  if (!btc_chaindb_load_index(db))
    return 0;

  return 1;
}

void
btc_chaindb_close(btc_chaindb_t *db) {
  btc_chaindb_unload_index(db);
  btc_chaindb_unload_files(db);
  btc_chaindb_unload_database(db);
}

static btc_coin_t *
read_coin(const btc_outpoint_t *prevout, void *arg1, void *arg2) {
  btc_chaindb_t *db = (btc_chaindb_t *)arg1;
  MDB_txn *txn = (MDB_txn *)arg2;
  MDB_val mkey, mval;
  btc_coin_t *coin;
  uint8_t key[36];
  int rc;

  btc_outpoint_write(key, prevout);

  mkey.mv_data = key;
  mkey.mv_size = sizeof(key);

  rc = mdb_get(txn, db->db_coin, &mkey, &mval);

  if (rc != 0) {
    if (rc != MDB_NOTFOUND)
      fprintf(stderr, "mdb_get: %s\n", mdb_strerror(rc));

    return NULL;
  }

  coin = btc_coin_create();

  CHECK(btc_coin_import(coin, mval.mv_data, mval.mv_size));

  return coin;
}

int
btc_chaindb_spend(btc_chaindb_t *db,
                  btc_view_t *view,
                  const btc_tx_t *tx) {
  MDB_txn *txn;
  int rc;

  rc = mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn);

  if (rc != 0) {
    fprintf(stderr, "mdb_txn_begin: %s\n", mdb_strerror(rc));
    return 0;
  }

  rc = btc_view_spend(view, tx, read_coin, db, txn);

  mdb_txn_abort(txn);

  return rc;
}

int
btc_chaindb_fill(btc_chaindb_t *db,
                 btc_view_t *view,
                 const btc_tx_t *tx) {
  MDB_txn *txn;
  int rc;

  rc = mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn);

  if (rc != 0) {
    fprintf(stderr, "mdb_txn_begin: %s\n", mdb_strerror(rc));
    return 0;
  }

  rc = btc_view_fill(view, tx, read_coin, db, txn);

  mdb_txn_abort(txn);

  return rc;
}

static int
btc_chaindb_save_view(btc_chaindb_t *db,
                      MDB_txn *txn,
                      const btc_view_t *view) {
  uint8_t *val = db->slab;
  const btc_coin_t *coin;
  btc_viewiter_t iter;
  MDB_val mkey, mval;
  uint8_t key[36];
  int rc;

  btc_view_iterate(&iter, view);

  while (btc_view_next(&coin, &iter)) {
    btc_raw_write(key, iter.hash, 32);
    btc_uint32_write(key + 32, iter.index);

    mkey.mv_data = key;
    mkey.mv_size = sizeof(key);

    if (coin->spent) {
      rc = mdb_del(txn, db->db_coin, &mkey, NULL);
    } else {
      mval.mv_data = val;
      mval.mv_size = btc_coin_export(val, coin);

      rc = mdb_put(txn, db->db_coin, &mkey, &mval, 0);
    }

    if (rc != 0 && rc != MDB_NOTFOUND) {
      fprintf(stderr, "mdb_put: %s\n", mdb_strerror(rc));
      return 0;
    }
  }

  return 1;
}

static int
btc_chaindb_read(btc_chaindb_t *db,
                 uint8_t **raw,
                 size_t *len,
                 const btc_chainfile_t *file,
                 int id,
                 int pos) {
  char path[BTC_PATH_MAX];
  uint8_t *data = NULL;
  uint8_t tmp[4];
  size_t size;
  int ret = 0;
  int fd;

  if (id == file->id) {
    fd = file->fd;
  } else {
    btc_chaindb_path(db, path, file->type, id);

    fd = btc_fs_open(path, READ_FLAGS, 0);

    if (fd == -1)
      return 0;
  }

  if (!btc_fs_pread(fd, tmp, 4, pos + 16))
    goto fail;

  size = 24 + btc_read32le(tmp);
  data = (uint8_t *)malloc(size);

  if (data == NULL)
    goto fail;

  if (!btc_fs_pread(fd, data, size, pos))
    goto fail;

  *raw = data;
  *len = size;

  data = NULL;
  ret = 1;
fail:
  if (data != NULL)
    free(data);

  if (fd != file->fd)
    btc_fs_close(fd);

  return ret;
}

static btc_block_t *
btc_chaindb_read_block(btc_chaindb_t *db, const btc_entry_t *entry) {
  btc_block_t *block;
  uint8_t *buf;
  size_t len;

  if (entry->block_pos == -1)
    return NULL;

  if (!btc_chaindb_read(db, &buf, &len, &db->block, entry->block_file,
                                                    entry->block_pos)) {
    return NULL;
  }

  block = btc_block_decode(buf + 24, len - 24);

  free(buf);

  return block;
}

static btc_undo_t *
btc_chaindb_read_undo(btc_chaindb_t *db, const btc_entry_t *entry) {
  btc_undo_t *undo;
  uint8_t *buf;
  size_t len;

  if (entry->undo_pos == -1)
    return btc_undo_create();

  if (!btc_chaindb_read(db, &buf, &len, &db->undo, entry->undo_file,
                                                   entry->undo_pos)) {
    return NULL;
  }

  undo = btc_undo_decode(buf + 24, len - 24);

  free(buf);

  return undo;
}

static int
should_sync(const btc_entry_t *entry) {
  if (entry->header.time >= btc_now() - 24 * 60 * 60)
    return 1;

  if ((entry->height % 20000) == 0)
    return 1;

  return 0;
}

static int
btc_chaindb_alloc(btc_chaindb_t *db,
                  MDB_txn *txn,
                  btc_chainfile_t *file,
                  size_t len) {
  char path[BTC_PATH_MAX];
  MDB_val mkey, mval;
  uint8_t raw[37];
  uint8_t key[5];
  int fd;

  if (file->pos + len <= MAX_FILE_SIZE)
    return 1;

  btc_uint8_write(key + 0, file->type);
  btc_uint32_write(key + 1, file->id);

  mkey.mv_data = key;
  mkey.mv_size = sizeof(key);
  mval.mv_data = raw;
  mval.mv_size = btc_chainfile_export(raw, file);

  if (mdb_put(txn, db->db_file, &mkey, &mval, 0) != 0)
    return 0;

  btc_chaindb_path(db, path, file->type, file->id + 1);

  fd = btc_fs_open(path, WRITE_FLAGS, 0644);

  if (fd == -1)
    return 0;

  btc_fs_fsync(file->fd);
  btc_fs_close(file->fd);

  btc_list_push(&db->files, btc_chainfile_clone(file),
                            btc_chainfile_t);

  file->fd = fd;
  file->id++;
  file->pos = 0;
  file->items = 0;
  file->min_time = -1;
  file->max_time = -1;
  file->min_height = -1;
  file->max_height = -1;

  return 1;
}

static int
btc_chaindb_write_block(btc_chaindb_t *db,
                        MDB_txn *txn,
                        btc_entry_t *entry,
                        const btc_block_t *block) {
  MDB_val mkey, mval;
  uint8_t hash[32];
  uint8_t raw[37];
  size_t len;

  len = btc_block_export(db->slab + 24, block);

  btc_hash256(hash, db->slab + 24, len);

  /* Store in network format. */
  btc_uint32_write(db->slab +  0, db->network->magic);
  btc_uint32_write(db->slab +  4, 0x636f6c62);
  btc_uint32_write(db->slab +  8, 0x0000006b);
  btc_uint32_write(db->slab + 12, 0x00000000);
  btc_uint32_write(db->slab + 16, len);

  btc_raw_write(db->slab + 20, hash, 4);

  len += 24;

  if (!btc_chaindb_alloc(db, txn, &db->block, len))
    return 0;

  if (!btc_fs_write(db->block.fd, db->slab, len))
    return 0;

  if (should_sync(entry))
    btc_fs_fsync(db->block.fd);

  entry->block_file = db->block.id;
  entry->block_pos = db->block.pos;

  db->block.pos += len;

  btc_chainfile_update(&db->block, entry);

  mkey.mv_data = (uint8_t *)blockfile_key;
  mkey.mv_size = sizeof(blockfile_key);
  mval.mv_data = raw;
  mval.mv_size = btc_chainfile_export(raw, &db->block);

  if (mdb_put(txn, db->db_file, &mkey, &mval, 0) != 0)
    return 0;

  return 1;
}

static int
btc_chaindb_write_undo(btc_chaindb_t *db,
                       MDB_txn *txn,
                       btc_entry_t *entry,
                       const btc_undo_t *undo) {
  size_t len = btc_undo_size(undo);
  uint8_t *buf = db->slab;
  MDB_val mkey, mval;
  uint8_t hash[32];
  uint8_t raw[37];
  int ret = 0;

  if (len > BTC_MAX_RAW_BLOCK_SIZE)
    buf = (uint8_t *)btc_malloc(24 + len);

  len = btc_undo_export(buf + 24, undo);

  btc_hash256(hash, buf + 24, len);

  btc_uint32_write(buf +  0, db->network->magic);
  btc_uint32_write(buf +  4, 0x00000000);
  btc_uint32_write(buf +  8, 0x00000000);
  btc_uint32_write(buf + 12, 0x00000000);
  btc_uint32_write(buf + 16, len);

  btc_raw_write(buf + 20, hash, 4);

  len += 24;

  if (!btc_chaindb_alloc(db, txn, &db->undo, len))
    goto fail;

  if (!btc_fs_write(db->undo.fd, buf, len))
    goto fail;

  if (should_sync(entry))
    btc_fs_fsync(db->undo.fd);

  entry->undo_file = db->undo.id;
  entry->undo_pos = db->undo.pos;

  db->undo.pos += len;

  btc_chainfile_update(&db->undo, entry);

  mkey.mv_data = (uint8_t *)undofile_key;
  mkey.mv_size = sizeof(undofile_key);
  mval.mv_data = raw;
  mval.mv_size = btc_chainfile_export(raw, &db->undo);

  if (mdb_put(txn, db->db_file, &mkey, &mval, 0) != 0)
    goto fail;

  ret = 1;
fail:
  if (buf != db->slab)
    btc_free(buf);

  return ret;
}

static int
btc_chaindb_prune_files(btc_chaindb_t *db,
                        MDB_txn *txn,
                        const btc_entry_t *entry) {
  btc_chainfile_t *file, *next;
  char path[BTC_PATH_MAX];
  int32_t target;
  uint8_t key[5];
  MDB_val mkey;

  if (!(db->flags & BTC_CHAIN_PRUNE))
    return 1;

  if (entry->height < db->network->block.keep_blocks)
    return 1;

  target = entry->height - db->network->block.keep_blocks;

  if (target <= db->network->block.prune_after_height)
    return 1;

  for (file = db->files.head; file != NULL; file = file->next) {
    if (file->max_height >= target)
      continue;

    btc_uint8_write(key + 0, file->type);
    btc_uint32_write(key + 1, file->id);

    mkey.mv_data = key;
    mkey.mv_size = sizeof(key);

    if (mdb_del(txn, db->db_file, &mkey, NULL) != 0)
      return 0;
  }

  for (file = db->files.head; file != NULL; file = next) {
    next = file->next;

    if (file->max_height >= target)
      continue;

    btc_chaindb_path(db, path, file->type, file->id);

    btc_fs_unlink(path);

    btc_list_remove(&db->files, file, btc_chainfile_t);

    btc_chainfile_destroy(file);
  }

  return 1;
}

static int
btc_chaindb_connect_block(btc_chaindb_t *db,
                          MDB_txn *txn,
                          btc_entry_t *entry,
                          const btc_block_t *block,
                          const btc_view_t *view) {
  const btc_undo_t *undo;

  (void)block;

  /* Genesis block's coinbase is unspendable. */
  if (entry->height == 0)
    return 1;

  /* Commit new coin state. */
  if (!btc_chaindb_save_view(db, txn, view))
    return 0;

  /* Write undo coins (if there are any). */
  undo = btc_view_undo(view);

  if (undo->length != 0 && entry->undo_pos == -1) {
    if (!btc_chaindb_write_undo(db, txn, entry, undo))
      return 0;
  }

  /* Prune height-288 if pruning is enabled. */
  return btc_chaindb_prune_files(db, txn, entry);
}

static btc_view_t *
btc_chaindb_disconnect_block(btc_chaindb_t *db,
                             MDB_txn *txn,
                             const btc_entry_t *entry,
                             const btc_block_t *block) {
  btc_undo_t *undo = btc_chaindb_read_undo(db, entry);
  const btc_input_t *input;
  const btc_tx_t *tx;
  btc_coin_t *coin;
  btc_view_t *view;
  size_t i, j;

  if (undo == NULL)
    return NULL;

  view = btc_view_create();

  /* Disconnect all transactions. */
  for (i = block->txs.length - 1; i != (size_t)-1; i--) {
    tx = block->txs.items[i];

    for (j = tx->inputs.length - 1; j != (size_t)-1; j--) {
      input = tx->inputs.items[j];
      coin = btc_undo_pop(undo);

      btc_view_put(view, &input->prevout, coin);
    }

    /* Remove any created coins. */
    btc_view_add(view, tx, entry->height, 1);
  }

  /* Undo coins should be empty. */
  CHECK(undo->length == 0);

  btc_undo_destroy(undo);

  /* Commit new coin state. */
  if (!btc_chaindb_save_view(db, txn, view)) {
    btc_view_destroy(view);
    return NULL;
  }

  return view;
}

static int
btc_chaindb_save_block(btc_chaindb_t *db,
                       MDB_txn *txn,
                       btc_entry_t *entry,
                       const btc_block_t *block,
                       const btc_view_t *view) {
  /* Write actual block data. */
  if (entry->block_pos == -1) {
    if (!btc_chaindb_write_block(db, txn, entry, block))
      return 0;
  }

  if (view == NULL)
    return 1;

  return btc_chaindb_connect_block(db, txn, entry, block, view);
}

int
btc_chaindb_save(btc_chaindb_t *db,
                 btc_entry_t *entry,
                 const btc_block_t *block,
                 const btc_view_t *view) {
  uint8_t raw[BTC_ENTRY_SIZE];
  MDB_val mkey, mval;
  MDB_txn *txn;

  /* Sanity checks. */
  CHECK(entry->prev != NULL || entry->height == 0);
  CHECK(entry->next == NULL);

  /* Begin transaction. */
  if (mdb_txn_begin(db->env, NULL, 0, &txn) != 0)
    return 0;

  /* Connect block and save data. */
  if (!btc_chaindb_save_block(db, txn, entry, block, view))
    goto fail;

  /* Write entry data. */
  mkey.mv_data = entry->hash;
  mkey.mv_size = 32;
  mval.mv_data = raw;
  mval.mv_size = btc_entry_export(raw, entry);

  if (mdb_put(txn, db->db_index, &mkey, &mval, 0) != 0)
    goto fail;

  /* Clear old tip. */
  if (entry->height != 0) {
    mkey.mv_data = entry->header.prev_block;
    mkey.mv_size = 32;

    if (mdb_del(txn, db->db_tip, &mkey, NULL) != 0)
      goto fail;
  }

  /* Write new tip. */
  mkey.mv_data = entry->hash;
  mkey.mv_size = 32;
  mval.mv_data = raw;
  mval.mv_size = 1;

  if (mdb_put(txn, db->db_tip, &mkey, &mval, 0) != 0)
    goto fail;

  /* Write state (main chain only). */
  if (view != NULL) {
    mkey.mv_data = (uint8_t *)tip_key;
    mkey.mv_size = sizeof(tip_key);
    mval.mv_data = entry->hash;
    mval.mv_size = 32;

    /* Commit new chain state. */
    if (mdb_put(txn, db->db_meta, &mkey, &mval, 0) != 0)
      goto fail;
  }

  /* Commit transaction. */
  if (mdb_txn_commit(txn) != 0)
    return 0;

  /* Flush OS buffers. */
  if (should_sync(entry)) {
    if (mdb_env_sync(db->env, 1) != 0)
      return 0;
  }

  /* Update hashes. */
  CHECK(btc_hashmap_put(db->hashes, entry->hash, entry));

  /* Main-chain-only stuff. */
  if (view != NULL) {
    /* Set next pointer. */
    if (entry->prev != NULL)
      entry->prev->next = entry;

    /* Update heights. */
    CHECK(db->heights.length == (size_t)entry->height);
    btc_vector_push(&db->heights, entry);

    /* Update tip. */
    if (entry->height == 0)
      db->head = entry;

    db->tail = entry;
  }

  return 1;
fail:
  mdb_txn_abort(txn);
  return 0;
}

int
btc_chaindb_reconnect(btc_chaindb_t *db,
                      btc_entry_t *entry,
                      const btc_block_t *block,
                      const btc_view_t *view) {
  uint8_t raw[BTC_ENTRY_SIZE];
  MDB_val mkey, mval;
  MDB_txn *txn;

  /* Begin transaction. */
  if (mdb_txn_begin(db->env, NULL, 0, &txn) != 0)
    return 0;

  /* Connect inputs. */
  if (!btc_chaindb_connect_block(db, txn, entry, block, view))
    goto fail;

  /* Re-write entry data (we may have updated the undo pos). */
  mkey.mv_data = entry->hash;
  mkey.mv_size = 32;
  mval.mv_data = raw;
  mval.mv_size = btc_entry_export(raw, entry);

  if (mdb_put(txn, db->db_index, &mkey, &mval, 0) != 0)
    goto fail;

  /* Write state. */
  mkey.mv_data = (uint8_t *)tip_key;
  mkey.mv_size = sizeof(tip_key);
  mval.mv_data = entry->hash;
  mval.mv_size = 32;

  /* Commit new chain state. */
  if (mdb_put(txn, db->db_meta, &mkey, &mval, 0) != 0)
    goto fail;

  /* Commit transaction. */
  if (mdb_txn_commit(txn) != 0)
    return 0;

  /* Flush OS buffers. */
  if (mdb_env_sync(db->env, 1) != 0)
    return 0;

  /* Set next pointer. */
  CHECK(entry->prev != NULL);
  CHECK(entry->next == NULL);
  entry->prev->next = entry;

  /* Update heights. */
  CHECK(db->heights.length == (size_t)entry->height);
  btc_vector_push(&db->heights, entry);

  /* Update tip. */
  db->tail = entry;

  return 1;
fail:
  mdb_txn_abort(txn);
  return 0;
}

btc_view_t *
btc_chaindb_disconnect(btc_chaindb_t *db,
                       btc_entry_t *entry,
                       const btc_block_t *block) {
  MDB_val mkey, mval;
  btc_view_t *view;
  MDB_txn *txn;

  /* Begin transaction. */
  if (mdb_txn_begin(db->env, NULL, 0, &txn) != 0)
    return NULL;

  /* Disconnect inputs. */
  view = btc_chaindb_disconnect_block(db, txn, entry, block);

  if (view == NULL)
    goto fail;

  /* Revert chain state to previous tip. */
  mkey.mv_data = (uint8_t *)tip_key;
  mkey.mv_size = sizeof(tip_key);
  mval.mv_data = entry->header.prev_block;
  mval.mv_size = 32;

  /* Commit new chain state. */
  if (mdb_put(txn, db->db_meta, &mkey, &mval, 0) != 0)
    goto fail;

  /* Commit transaction. */
  if (mdb_txn_commit(txn) != 0) {
    txn = NULL;
    goto fail;
  }

  /* Flush OS buffers. */
  if (mdb_env_sync(db->env, 1) != 0) {
    txn = NULL;
    goto fail;
  }

  /* Set next pointer. */
  CHECK(entry->prev != NULL);
  CHECK(entry->next == NULL);
  entry->prev->next = NULL;

  /* Update heights. */
  CHECK((btc_entry_t *)btc_vector_pop(&db->heights) == entry);

  /* Revert tip. */
  db->tail = entry->prev;

  return view;
fail:
  if (view != NULL)
    btc_view_destroy(view);

  if (txn != NULL)
    mdb_txn_abort(txn);

  return NULL;
}

const btc_entry_t *
btc_chaindb_head(btc_chaindb_t *db) {
  return db->head;
}

const btc_entry_t *
btc_chaindb_tail(btc_chaindb_t *db) {
  return db->tail;
}

int32_t
btc_chaindb_height(btc_chaindb_t *db) {
  return db->tail->height;
}

const btc_entry_t *
btc_chaindb_by_hash(btc_chaindb_t *db, const uint8_t *hash) {
  return btc_hashmap_get(db->hashes, hash);
}

const btc_entry_t *
btc_chaindb_by_height(btc_chaindb_t *db, int32_t height) {
  if ((size_t)height >= db->heights.length)
    return NULL;

  return (btc_entry_t *)db->heights.items[height];
}

int
btc_chaindb_is_main(btc_chaindb_t *db, const btc_entry_t *entry) {
  if ((size_t)entry->height >= db->heights.length)
    return 0;

  return (btc_entry_t *)db->heights.items[entry->height] == entry;
}

int
btc_chaindb_has_coins(btc_chaindb_t *db, const btc_tx_t *tx) {
  MDB_val mkey, mval;
  uint8_t key[36];
  MDB_txn *txn;
  size_t i;
  int rc;

  CHECK(mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn) == 0);

  btc_raw_write(key, tx->hash, 32);

  for (i = 0; i < tx->outputs.length; i++) {
    btc_uint32_write(key + 32, i);

    mkey.mv_data = key;
    mkey.mv_size = sizeof(key);

    rc = mdb_get(txn, db->db_coin, &mkey, &mval);

    if (rc == 0) {
      mdb_txn_abort(txn);
      return 1;
    }

    CHECK(rc == MDB_NOTFOUND);
  }

  mdb_txn_abort(txn);

  return 0;
}

btc_block_t *
btc_chaindb_get_block(btc_chaindb_t *db, const btc_entry_t *entry) {
  return btc_chaindb_read_block(db, entry);
}

int
btc_chaindb_get_raw_block(btc_chaindb_t *db,
                          uint8_t **data,
                          size_t *length,
                          const btc_entry_t *entry) {
  if (entry->block_pos == -1)
    return 0;

  return btc_chaindb_read(db, data, length, &db->block, entry->block_file,
                                                        entry->block_pos);

}
