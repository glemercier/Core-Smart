// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SMARTCASH_CONSENSUS_CONSENSUS_H
#define SMARTCASH_CONSENSUS_CONSENSUS_H

#include <stdint.h>

/** The maximum allowed size for a serialized block, in bytes (only for buffer size limits) */
static const unsigned int MAX_BLOCK_SERIALIZED_SIZE = 16000000;
/** The maximum allowed weight for a block, see BIP 141 (network rule) */
static const unsigned int MAX_BLOCK_WEIGHT = 32000000;
/** The maximum allowed size for a block excluding witness data, in bytes (network rule) */
static const unsigned int MAX_BLOCK_BASE_SIZE = 8000000;
/** The maximum allowed number of signature check operations in a block (network rule) */
static const int64_t MAX_BLOCK_SIGOPS_COST = 640000;
/** Coinbase transaction outputs can only be spent after this number of new blocks (network rule) */
static const int COINBASE_MATURITY = 100;
/** Version 1.0 net start block*/
static const int HF_V1_0_START_HEIGHT = 90000;
/** Smartnode start block*/
static const int HF_V1_1_SMARTNODE_HEIGHT = 300000;
/** Reject zerocoin in/outputs starting with this block */
static const int HF_ZEROCOIN_DISABLE = 266765;
/** Mainnet payment start blocks*/
static const int HF_V1_2_MULTINODE_VOTING_HEIGHT = 535000;
static const int HF_V1_2_MULTINODE_PAYOUT_HEIGHT = 545005;
static const int HF_V1_2_SMARTREWARD_HEIGHT = 574100;
static const int HF_V1_2_8_SMARNODE_NEW_COLLATERAL_HEIGHT = 910000;
static const int HF_V1_3_HEIGHT = 1524100; // Round 33 starts 1524100

/* Mainnet payment intervals*/
static const int HF_V1_2_NODES_PER_BLOCK        = 10;
static const int HF_V1_2_NODES_BLOCK_INTERVAL   = 2;
static const int HF_V1_2_8_NODES_PER_BLOCK      = 1;

/** SmartCash max reward block */
static const int HF_CHAIN_REWARD_END_HEIGHT = 717499999;

/** Testnet payment start blocks*/
static const int TESTNET_V1_2_8_PAYMENTS_HEIGHT = 201;
static const int TESTNET_V1_3_HEIGHT = 451;  //round start is 5 block 400-450 payments start at 460

/** Testnet payment intervals*/
static const int TESTNET_V1_2_8_NODES_PER_BLOCK         = 1;
static const int TESTNET_V1_2_8_NODES_BLOCK_INTERVAL    = 2;
static const int TESTNET_V1_3_NODES_PER_BLOCK           = 1;
static const int TESTNET_V1_3_NODES_BLOCK_INTERVAL      = 2;

inline unsigned int MaxBlockSigOps()
{
    return MAX_BLOCK_SERIALIZED_SIZE / 50;
}

/** Flags for nSequence and nLockTime locks */
enum {
    /* Interpret sequence numbers as relative lock-time constraints. */
    LOCKTIME_VERIFY_SEQUENCE = (1 << 0),

    /* Use GetMedianTimePast() instead of nTime for end point timestamp. */
    LOCKTIME_MEDIAN_TIME_PAST = (1 << 1),
};

#endif // SMARTCASH_CONSENSUS_CONSENSUS_H
