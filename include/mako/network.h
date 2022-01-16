/*!
 * network.h - network for mako
 * Copyright (c) 2020, Christopher Jeffrey (MIT License).
 * https://github.com/chjj/mako
 */

#ifndef BTC_NETWORK_H
#define BTC_NETWORK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "common.h"
#include "types.h"

/*
 * Constants
 */

enum btc_network_type {
  BTC_NETWORK_MAINNET,
  BTC_NETWORK_TESTNET,
  BTC_NETWORK_REGTEST,
  BTC_NETWORK_SIMNET,
  BTC_NETWORK_SIGNET
};

/*
 * Types
 */

typedef struct btc_checkpoint_s {
  int32_t height;
  uint8_t hash[32];
} btc_checkpoint_t;

typedef struct btc_deployment_s {
  const char *name;
  int bit;
  int64_t start_time;
  int64_t timeout;
  int32_t threshold;
  int32_t window;
  int required;
  int force;
} btc_deployment_t;

struct btc_network_s {
  /**
   * Network type.
   */
  enum btc_network_type type;

  /**
   * Symbolic network name.
   */
  const char *name;

  /**
   * Default DNS seeds.
   */
  struct btc_network_seeds_s {
    const char **items;
    size_t length;
  } seeds;

  /**
   * Packet magic number.
   */
  uint32_t magic;

  /**
   * Default network port.
   */
  int port;

  /**
   * Checkpoint list.
   */
  struct btc_network_checkpoints_s {
    const btc_checkpoint_t *items;
    size_t length;
  } checkpoints;

  /**
   * Last checkpoint height.
   */
  int32_t last_checkpoint;

  /**
   * Block subsidy halving interval.
   */
  int32_t halving_interval;

  /**
   * Genesis block.
   */
  struct btc_network_genesis_s {
    uint8_t hash[32];
    btc_header_t header;
    const uint8_t *data;
    size_t length;
  } genesis;

  /**
   * POW-related constants.
   */
  struct btc_network_pow_s {
    /**
     * Default target.
     */
    uint8_t limit[32];

    /**
     * Compact pow limit.
     */
    uint32_t bits;

    /**
     * Minimum chainwork for best chain.
     */
    uint8_t chainwork[32];

    /**
     * Desired retarget period in seconds.
     */
    int64_t target_timespan;

    /**
     * Average block time.
     */
    int64_t target_spacing;

    /**
     * Retarget interval in blocks.
     */
    int32_t retarget_interval;

    /**
     * Whether to reset target if a block
     * has not been mined recently.
     */
    int target_reset;

    /**
     * Do not allow retargetting.
     */
    int no_retargeting;
  } pow;

  /**
   * Block constants.
   */
  struct btc_network_block_s {
    /**
     * Safe height to start pruning.
     */
    int32_t prune_after_height;

    /**
     * Safe number of blocks to keep.
     */
    int32_t keep_blocks;

    /**
     * Age used for the time delta to
     * determine whether the chain is synced.
     */
    int64_t max_tip_age;

    /**
     * Height at which block processing is
     * slow enough that we can output
     * logs without spamming.
     */
    int32_t slow_height;
  } block;

  /**
   * Pre-versionbit soft-forks.
   */
  struct btc_network_softforks_s {
    /**
     * Map of historical blocks which create duplicate transactions hashes.
     * See: https://github.com/bitcoin/bips/blob/master/bip-0030.mediawiki
     */
    struct btc_network_bip30_s {
      const btc_checkpoint_t *items;
      size_t length;
    } bip30;

    /**
     * Block which activated bip34.
     * Used for avoiding bip30 checks.
     */
    btc_checkpoint_t bip34;

    /**
     * Block which activated bip65.
     */
    btc_checkpoint_t bip65;

    /**
     * Block which activated bip66.
     */
    btc_checkpoint_t bip66;

    /**
     * Block which activated bip112.
     */
    btc_checkpoint_t csv;

    /**
     * Block which activated bip141.
     */
    btc_checkpoint_t segwit;
  } softforks;

  /**
   * Version bits activation threshold.
   */
  int32_t activation_threshold;

  /**
   * Confirmation window for versionbits.
   */
  int32_t miner_window;

  /**
   * Deployments for versionbits.
   */
  struct btc_network_deployments_s {
    const btc_deployment_t *items;
    size_t length;
  } deployments;

  /**
   * Signet challenge.
   */
  btc_script_t challenge;

  /**
   * Key prefixes.
   */
  struct btc_network_key_s {
    uint8_t privkey;
    uint32_t xpubkey[5];
    uint32_t xprvkey[5];
    uint32_t coin_type;
  } key;

  /**
   * Address prefixes.
   */
  struct btc_network_address_s {
    uint8_t p2pkh;
    uint8_t p2sh;
    const char *bech32;
  } address;

  /**
   * Default value for whether the mempool
   * accepts non-standard transactions.
   */
  int require_standard;

  /**
   * Default rpc port.
   */
  int rpc_port;

  /**
   * Default min relay rate.
   */
  int64_t min_relay;

  /**
   * Default normal relay rate.
   */
  int64_t fee_rate;

  /**
   * Maximum normal relay rate.
   */
  int64_t max_fee_rate;

  /**
   * Whether to allow self-connection.
   */
  int self_connect;

  /**
   * Whether to request mempool on sync.
   */
  int request_mempool;
};

#if defined(__cplusplus)
typedef struct btc_network_s::btc_network_pow_s btc_network_pow_t;
#else
typedef struct btc_network_pow_s btc_network_pow_t;
#endif

/*
 * Helpers
 */

BTC_EXTERN const btc_checkpoint_t *
btc_network_checkpoint(const btc_network_t *network, int32_t height);

BTC_EXTERN const btc_checkpoint_t *
btc_network_bip30(const btc_network_t *network, int32_t height);

BTC_EXTERN const btc_deployment_t *
btc_network_deployment(const btc_network_t *network, const char *name);

/*
 * Networks
 */

BTC_EXTERN extern const btc_network_t *btc_mainnet;
BTC_EXTERN extern const btc_network_t *btc_testnet;
BTC_EXTERN extern const btc_network_t *btc_regtest;
BTC_EXTERN extern const btc_network_t *btc_simnet;
BTC_EXTERN extern const btc_network_t *btc_signet;

#ifdef __cplusplus
}
#endif

#endif /* BTC_NETWORK_H */
