// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <math.h>
#include "alert.h"
#include "checkpoints.h"
#include "db.h"
#include "txdb.h"
#include "net.h"
#include "init.h"
#include "ui_interface.h"
#include "checkqueue.h"
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include "fixed.h"
#include <stdint.h>

using namespace std;
using namespace boost;
using namespace numeric;

#if defined(NDEBUG)
# error "smartcash cannot be compiled without assertions."
#endif

#define ZEROCOIN_MODULUS   "25195908475657893494027183240048398571429282126204032027777137836043662020707595556264018525880784406918290641249515082189298559149176184502808489120072844992687392807287776735971418347270261896375014971824691165077613379859095700097330459748808428401797429100642458691817195118746121515172654632282216869987549182422433637259085141865462043576798423387184774447920739934236584823824281198163815010674810451660377306056201619676256133844143603833904414952634432190114657544454178424020924616515723350778707749817125772467962926386356373289912154831438167899885040445364023527381951378636564391212010397122822120720357"


//
// Global state
//

CCriticalSection cs_setpwalletRegistered;
set<CWallet*> setpwalletRegistered;

CCriticalSection cs_main;

CTxMemPool mempool;
unsigned int nTransactionsUpdated = 0;

BlockMap mapBlockIndex;
uint256 hashGenesisBlock("0x000007acc6970b812948d14ea5a0a13db0fdd07d5047c7e69101fa8b361e05a4");
static CBigNum bnProofOfWorkLimit(~uint256(0) >> 20); // smartcash: starting difficulty
CBlockIndex* pindexGenesisBlock = NULL;
int nBestHeight = -1;
uint256 nBestChainWork = 0;
uint256 nBestInvalidWork = 0;
uint256 hashBestChain = 0;
int64 nStartRewardTime = 1499789471; // 07/11/2017 @ 11:11am (CST)
CBlockIndex* pindexBest = NULL;
set<CBlockIndex*, CBlockIndexWorkComparator> setBlockIndexValid; // may contain all CBlockIndex*'s that have validness >=BLOCK_VALID_TRANSACTIONS, and must contain those who aren't failed
int64 nTimeBestReceived = 0;
int nScriptCheckThreads = 0;
bool fImporting = false;
bool fReindex = false;
bool fBenchmark = false;
bool fTxIndex = false;
unsigned int nCoinCacheSize = 5000;

/** Fees smaller than this (in ztoshi) are considered zero fee (for transaction creation) */
int64 CTransaction::nMinTxFee = 1000000; // 0.01 smartcash
/** Fees smaller than this (in ztoshi) are considered zero fee (for relaying) */
int64 CTransaction::nMinRelayTxFee = 1000000; // 0.01 smartcash

CMedianFilter<int> cPeerBlockCounts(8, 0); // Amount of blocks that other nodes claim to have

map<uint256, CBlock*> mapOrphanBlocks;
multimap<uint256, CBlock*> mapOrphanBlocksByPrev;

map<uint256, CTransaction> mapOrphanTransactions;
map<uint256, set<uint256> > mapOrphanTransactionsByPrev;

// Constant stuff for coinbase transactions we create:
CScript COINBASE_FLAGS;

const string strMessageMagic = "SmartCash Signed Message:\n";

double dHashesPerSec = 0.0;
int64 nHPSTimerStart = 0;

// Settings
int64 nTransactionFee = 0;
int64 nMinimumInputValue = DUST_HARD_LIMIT;




//////////////////////////////////////////////////////////////////////////////
//
// dispatching functions
//

// These functions dispatch to one or all registered wallets


void RegisterWallet(CWallet* pwalletIn)
{
    {
        LOCK(cs_setpwalletRegistered);
        setpwalletRegistered.insert(pwalletIn);
    }
}

void UnregisterWallet(CWallet* pwalletIn)
{
    {
        LOCK(cs_setpwalletRegistered);
        setpwalletRegistered.erase(pwalletIn);
    }
}

// get the wallet transaction with the given hash (if it exists)
bool static GetTransaction(const uint256& hashTx, CWalletTx& wtx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        if (pwallet->GetTransaction(hashTx,wtx))
            return true;
    return false;
}

// erases transaction with the given hash from all wallets
void static EraseFromWallets(uint256 hash)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->EraseFromWallet(hash);
}

// make sure all wallets know about the given transaction, in the given block
void SyncWithWallets(const uint256 &hash, const CTransaction& tx, const CBlock* pblock, bool fUpdate)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->AddToWalletIfInvolvingMe(hash, tx, pblock, fUpdate);
}

// notify wallets about a new best chain
void static SetBestChain(const CBlockLocator& loc)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->SetBestChain(loc);
}

// notify wallets about an updated transaction
void static UpdatedTransaction(const uint256& hashTx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->UpdatedTransaction(hashTx);
}

// dump all wallets
void static PrintWallets(const CBlock& block)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->PrintWallet(block);
}

// notify wallets about an incoming inventory (for request counts)
void static Inventory(const uint256& hash)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->Inventory(hash);
}

// ask wallets to resend their transactions
void static ResendWalletTransactions()
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->ResendWalletTransactions();
}







//////////////////////////////////////////////////////////////////////////////
//
// CCoinsView implementations
//

bool CCoinsView::GetCoins(const uint256 &txid, CCoins &coins) { return false; }
bool CCoinsView::SetCoins(const uint256 &txid, const CCoins &coins) { return false; }
bool CCoinsView::HaveCoins(const uint256 &txid) { return false; }
CBlockIndex *CCoinsView::GetBestBlock() { return NULL; }
bool CCoinsView::SetBestBlock(CBlockIndex *pindex) { return false; }
bool CCoinsView::BatchWrite(const std::map<uint256, CCoins> &mapCoins, CBlockIndex *pindex) { return false; }
bool CCoinsView::GetStats(CCoinsStats &stats) { return false; }


CCoinsViewBacked::CCoinsViewBacked(CCoinsView &viewIn) : base(&viewIn) { }
bool CCoinsViewBacked::GetCoins(const uint256 &txid, CCoins &coins) { return base->GetCoins(txid, coins); }
bool CCoinsViewBacked::SetCoins(const uint256 &txid, const CCoins &coins) { return base->SetCoins(txid, coins); }
bool CCoinsViewBacked::HaveCoins(const uint256 &txid) { return base->HaveCoins(txid); }
CBlockIndex *CCoinsViewBacked::GetBestBlock() { return base->GetBestBlock(); }
bool CCoinsViewBacked::SetBestBlock(CBlockIndex *pindex) { return base->SetBestBlock(pindex); }
void CCoinsViewBacked::SetBackend(CCoinsView &viewIn) { base = &viewIn; }
bool CCoinsViewBacked::BatchWrite(const std::map<uint256, CCoins> &mapCoins, CBlockIndex *pindex) { return base->BatchWrite(mapCoins, pindex); }
bool CCoinsViewBacked::GetStats(CCoinsStats &stats) { return base->GetStats(stats); }

CCoinsViewCache::CCoinsViewCache(CCoinsView &baseIn, bool fDummy) : CCoinsViewBacked(baseIn), pindexTip(NULL) { }

bool CCoinsViewCache::GetCoins(const uint256 &txid, CCoins &coins) {
    if (cacheCoins.count(txid)) {
        coins = cacheCoins[txid];
        return true;
    }
    if (base->GetCoins(txid, coins)) {
        cacheCoins[txid] = coins;
        return true;
    }
    return false;
}

std::map<uint256,CCoins>::iterator CCoinsViewCache::FetchCoins(const uint256 &txid) {
    std::map<uint256,CCoins>::iterator it = cacheCoins.lower_bound(txid);
    if (it != cacheCoins.end() && it->first == txid)
        return it;
    CCoins tmp;
    if (!base->GetCoins(txid,tmp))
        return cacheCoins.end();
    std::map<uint256,CCoins>::iterator ret = cacheCoins.insert(it, std::make_pair(txid, CCoins()));
    tmp.swap(ret->second);
    return ret;
}

CCoins &CCoinsViewCache::GetCoins(const uint256 &txid) {
    std::map<uint256,CCoins>::iterator it = FetchCoins(txid);
    assert(it != cacheCoins.end());
    return it->second;
}

bool CCoinsViewCache::SetCoins(const uint256 &txid, const CCoins &coins) {
    cacheCoins[txid] = coins;
    return true;
}

bool CCoinsViewCache::HaveCoins(const uint256 &txid) {
    return FetchCoins(txid) != cacheCoins.end();
}

CBlockIndex *CCoinsViewCache::GetBestBlock() {
    if (pindexTip == NULL)
        pindexTip = base->GetBestBlock();
    return pindexTip;
}

bool CCoinsViewCache::SetBestBlock(CBlockIndex *pindex) {
    pindexTip = pindex;
    return true;
}

bool CCoinsViewCache::BatchWrite(const std::map<uint256, CCoins> &mapCoins, CBlockIndex *pindex) {
    for (std::map<uint256, CCoins>::const_iterator it = mapCoins.begin(); it != mapCoins.end(); it++)
        cacheCoins[it->first] = it->second;
    pindexTip = pindex;
    return true;
}

bool CCoinsViewCache::Flush() {
    bool fOk = base->BatchWrite(cacheCoins, pindexTip);
    if (fOk)
        cacheCoins.clear();
    return fOk;
}

unsigned int CCoinsViewCache::GetCacheSize() {
    return cacheCoins.size();
}

/** CCoinsView that brings transactions from a memorypool into view.
    It does not check for spendings by memory pool transactions. */
CCoinsViewMemPool::CCoinsViewMemPool(CCoinsView &baseIn, CTxMemPool &mempoolIn) : CCoinsViewBacked(baseIn), mempool(mempoolIn) { }

bool CCoinsViewMemPool::GetCoins(const uint256 &txid, CCoins &coins) {
    if (base->GetCoins(txid, coins))
        return true;
    if (mempool.exists(txid)) {
        const CTransaction &tx = mempool.lookup(txid);
        coins = CCoins(tx, MEMPOOL_HEIGHT);
        return true;
    }
    return false;
}

bool CCoinsViewMemPool::HaveCoins(const uint256 &txid) {
    return mempool.exists(txid) || base->HaveCoins(txid);
}

CCoinsViewCache *pcoinsTip = NULL;
CBlockTreeDB *pblocktree = NULL;

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CTransaction& tx)
{
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    if (sz > 5000)
    {
        printf("ignoring large orphan tx (size: %u, hash: %s)\n", sz, hash.ToString().c_str());
        return false;
    }

    mapOrphanTransactions[hash] = tx;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    printf("stored orphan tx %s (mapsz %" PRIszu")\n", hash.ToString().c_str(),
        mapOrphanTransactions.size());
    return true;
}

void static EraseOrphanTx(uint256 hash)
{
    if (!mapOrphanTransactions.count(hash))
        return;
    const CTransaction& tx = mapOrphanTransactions[hash];
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        mapOrphanTransactionsByPrev[txin.prevout.hash].erase(hash);
        if (mapOrphanTransactionsByPrev[txin.prevout.hash].empty())
            mapOrphanTransactionsByPrev.erase(txin.prevout.hash);
    }
    mapOrphanTransactions.erase(hash);
}

unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    unsigned int nEvicted = 0;
    while (mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        map<uint256, CTransaction>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}







//////////////////////////////////////////////////////////////////////////////
//
// CTransaction / CTxOut
//

bool CTxOut::IsDust() const
{
    // SmartCash: IsDust() detection disabled, allows any valid dust to be relayed.
    // The fees imposed on each dust txo is considered sufficient spam deterrant. 
    return false;
}

bool CTransaction::IsStandard(string& strReason) const
{
    if (nVersion > CTransaction::CURRENT_VERSION || nVersion < 1) {
        strReason = "version";
        return false;
    }

    if (!IsFinal()) {
        strReason = "not-final";
        return false;
    }

    // Extremely large transactions with lots of inputs can cost the network
    // almost as much to process as they cost the sender in fees, because
    // computing signature hashes is O(ninputs*txsize). Limiting transactions
    // to MAX_STANDARD_TX_SIZE mitigates CPU exhaustion attacks.
    unsigned int sz = this->GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    if (sz >= MAX_STANDARD_TX_SIZE) {
        strReason = "tx-size";
        return false;
    }

    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        // Biggest 'standard' txin is a 3-signature 3-of-3 CHECKMULTISIG
        // pay-to-script-hash, which is 3 ~80-byte signatures, 3
        // ~65-byte public keys, plus a few script ops.
        if (txin.scriptSig.size() > 500 && !txin.scriptSig.IsZerocoinSpend()) {
            strReason = "scriptsig-size";
            return false;
        }

        if (txin.scriptSig.IsZerocoinSpend() && txin.scriptSig.size() > 50000){
            strReason = "scriptsig-size";
            return false;
        }

        if(!txin.scriptSig.IsZerocoinSpend()){
            if (!txin.scriptSig.IsPushOnly()) {
                strReason = "scriptsig-not-pushonly";
                return false;
            }
            if (!txin.scriptSig.HasCanonicalPushes()) {
                strReason = "non-canonical-push";
                return false;
            }
        }
    }

    unsigned int nDataOut = 0;
    txnouttype whichType;
    BOOST_FOREACH(const CTxOut& txout, vout) {
        if (!::IsStandard(txout.scriptPubKey, whichType)) {
            strReason = "scriptpubkey";
            return false;
        }
        if (whichType == TX_NULL_DATA)
            nDataOut++;
        else if (txout.IsDust()) {
            strReason = "dust";
            return false;
        }
    }

    // only one OP_RETURN txout is permitted
    if (nDataOut > 1) {
        strReason = "multi-op-return";
        return false;
    }

    return true;
}

//
// Check transaction inputs, and make sure any
// pay-to-script-hash transactions are evaluating IsStandard scripts
//
// Why bother? To avoid denial-of-service attacks; an attacker
// can submit a standard HASH... OP_EQUAL transaction,
// which will get accepted into blocks. The redemption
// script can be anything; an attacker could use a very
// expensive-to-check-upon-redemption script like:
//   DUP CHECKSIG DROP ... repeated 100 times... OP_1
//
bool CTransaction::AreInputsStandard(CCoinsViewCache& mapInputs) const
{
    if (IsCoinBase() || IsZerocoinSpend())
        return true; // Coinbases don't use vin normally

    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const CTxOut& prev = GetOutputFor(vin[i], mapInputs);

        vector<vector<unsigned char> > vSolutions;
        txnouttype whichType;
        // get the scriptPubKey corresponding to this input:
        const CScript& prevScript = prev.scriptPubKey;
        if (!Solver(prevScript, whichType, vSolutions))
            return false;
        int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);
        if (nArgsExpected < 0)
            return false;

        // Transactions with extra stuff in their scriptSigs are
        // non-standard. Note that this EvalScript() call will
        // be quick, because if there are any operations
        // beside "push data" in the scriptSig the
        // IsStandard() call returns false
        vector<vector<unsigned char> > stack;
        if (!EvalScript(stack, vin[i].scriptSig, *this, i, false, 0))
            return false;

        if (whichType == TX_SCRIPTHASH)
        {
            if (stack.empty())
                return false;
            CScript subscript(stack.back().begin(), stack.back().end());
            vector<vector<unsigned char> > vSolutions2;
            txnouttype whichType2;
            if (!Solver(subscript, whichType2, vSolutions2))
                return false;
            if (whichType2 == TX_SCRIPTHASH)
                return false;

            int tmpExpected;
            tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);
            if (tmpExpected < 0)
                return false;
            nArgsExpected += tmpExpected;
        }

        if (stack.size() != (unsigned int)nArgsExpected)
            return false;
    }

    return true;
}

unsigned int CTransaction::GetLegacySigOpCount() const
{
    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}


int CMerkleTx::SetMerkleBranch(const CBlock* pblock)
{
    CBlock blockTmp;

    if (pblock == NULL) {
        CCoins coins;
        if (pcoinsTip->GetCoins(GetHash(), coins)) {
            CBlockIndex *pindex = FindBlockByHeight(coins.nHeight);
            if (pindex) {
                if (!blockTmp.ReadFromDisk(pindex))
                    return 0;
                pblock = &blockTmp;
            }
        }
    }

    if (pblock) {
        // Update the tx's hashBlock
        hashBlock = pblock->GetHash();

        // Locate the transaction
        for (nIndex = 0; nIndex < (int)pblock->vtx.size(); nIndex++)
            if (pblock->vtx[nIndex] == *(CTransaction*)this)
                break;
        if (nIndex == (int)pblock->vtx.size())
        {
            vMerkleBranch.clear();
            nIndex = -1;
            printf("ERROR: SetMerkleBranch() : couldn't find tx in block\n");
            return 0;
        }

        // Fill in merkle branch
        vMerkleBranch = pblock->GetMerkleBranch(nIndex);
    }

    // Is the tx in a block that's in the main chain
    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    return pindexBest->nHeight - pindex->nHeight + 1;
}



// zerocoin init
static CBigNum bnTrustedModulus;

bool setParams = bnTrustedModulus.SetHexBool(ZEROCOIN_MODULUS);

// Set up the Zerocoin Params object
static libzerocoin::Params *ZCParams = new libzerocoin::Params(bnTrustedModulus);


bool CTransaction::CheckTransaction(CValidationState &state, uint256 hashTx, bool isVerifyDB, int nHeight) const
{
    // Basic checks that don't depend on any context
    if (vin.empty())
        return state.DoS(10, error("CTransaction::CheckTransaction() : vin empty"));
    if (vout.empty())
        return state.DoS(10, error("CTransaction::CheckTransaction() : vout empty"));
    // Size limits
    if (::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return state.DoS(100, error("CTransaction::CheckTransaction() : size limits failed"));

    // Check for negative or overflow output values
    int64 nValueOut = 0;
    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        if (txout.nValue < 0)
            return state.DoS(100, error("CTransaction::CheckTransaction() : txout.nValue negative"));
        if (txout.nValue > MAX_MONEY)
            return state.DoS(100, error("CTransaction::CheckTransaction() : txout.nValue too high"));
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return state.DoS(100, error("CTransaction::CheckTransaction() : txout total out of range"));
    }


    // Check for duplicate inputs
    set<COutPoint> vInOutPoints;
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        if (vInOutPoints.count(txin.prevout))
            return state.DoS(100, error("CTransaction::CheckTransaction() : duplicate inputs"));
        vInOutPoints.insert(txin.prevout);
    }

    if (IsCoinBase())
    {
        if (vin[0].scriptSig.size() < 2 || vin[0].scriptSig.size() > 100)
            return state.DoS(100, error("CTransaction::CheckTransaction() : coinbase script size"));

        // Check for founders inputs
        if ((nHeight > 0) && (nHeight < 717499999)) {

            bool found_1 = false;
            bool found_2 = false;
            bool found_3 = false;
            bool found_4 = false;
            bool found_5 = false;

            CScript FOUNDER_1_SCRIPT;
            CScript FOUNDER_2_SCRIPT;
            CScript FOUNDER_3_SCRIPT;
            CScript FOUNDER_4_SCRIPT;
            CScript FOUNDER_5_SCRIPT;

            if(!fTestNet && (GetAdjustedTime() > nStartRewardTime)){
                FOUNDER_1_SCRIPT.SetDestination(CBitcoinAddress("Siim7T5zMH3he8xxtQzhmHs4CQSuMrCV1M").Get());
                FOUNDER_2_SCRIPT.SetDestination(CBitcoinAddress("SW2FbVaBhU1Www855V37auQzGQd8fuLR9x").Get());
                FOUNDER_3_SCRIPT.SetDestination(CBitcoinAddress("SPusYr5tUdUyRXevJg7pnCc9Sm4HEzaYZF").Get());
                FOUNDER_4_SCRIPT.SetDestination(CBitcoinAddress("SU5bKb35xUV8aHG5dNarWHB3HBVjcCRjYo").Get());
                FOUNDER_5_SCRIPT.SetDestination(CBitcoinAddress("SXun9XDHLdBhG4Yd1ueZfLfRpC9kZgwT1b").Get());
            }else if(!fTestNet && (GetAdjustedTime() <= nStartRewardTime)){
                return state.DoS(100, error("CTransaction::CheckTransaction() : transaction is too early"));
            }else{
                FOUNDER_1_SCRIPT.SetDestination(CBitcoinAddress("TBizCRSozKpCbheftmzs75fZnc7h6HocJ3").Get());
                FOUNDER_2_SCRIPT.SetDestination(CBitcoinAddress("THc8faox1kKZ3aegLdU4cwCJwgehLHSe9M").Get());
                FOUNDER_3_SCRIPT.SetDestination(CBitcoinAddress("TK7CPJ2BS2UxAc7KBbUYySCBczww97Qr7p").Get());
                FOUNDER_4_SCRIPT.SetDestination(CBitcoinAddress("TUPAY3ziYY7znMLxRJJNuvfuFWS1snrjiM").Get());
                FOUNDER_5_SCRIPT.SetDestination(CBitcoinAddress("TMtxkvmAMyL5siHX1n3zKAvAKnev8if8KA").Get());
            }

            BOOST_FOREACH(const CTxOut& output, vout) {
                if (output.scriptPubKey == FOUNDER_1_SCRIPT && output.nValue == (int64)(0.08 * (GetBlockValue(pindexBest->nHeight+1, 0, pindexBest->nTime)))) {
                    found_1 = true;
                }
                if (output.scriptPubKey == FOUNDER_2_SCRIPT && output.nValue == (int64)(0.08 * (GetBlockValue(pindexBest->nHeight+1, 0, pindexBest->nTime)))) {
                    found_2 = true;
                }
                if (output.scriptPubKey == FOUNDER_3_SCRIPT && output.nValue == (int64)(0.08 * (GetBlockValue(pindexBest->nHeight+1, 0, pindexBest->nTime)))) {
                    found_3 = true;
                }
                if (output.scriptPubKey == FOUNDER_4_SCRIPT && output.nValue == (int64)(0.15 * (GetBlockValue(pindexBest->nHeight+1, 0, pindexBest->nTime)))) {
                    found_4 = true;
                }
                if (output.scriptPubKey == FOUNDER_5_SCRIPT && output.nValue == (int64)(0.56 * (GetBlockValue(pindexBest->nHeight+1, 0, pindexBest->nTime)))) {
                    found_5 = true;
                }
printf("Block number with pindexbest on verification%5=",pindexBest->nHeight+1);
printf("Block Value on verification%d=",(GetBlockValue(pindexBest->nHeight+1, 0, pindexBest->nTime)/100000000));
            }

            if (!(found_1 && found_2 && found_3 && found_4 && found_5)) {
                return state.DoS(100, error("CTransaction::CheckTransaction() : founders reward missing"));
            }
        }
    }
    else
    {

        BOOST_FOREACH(const CTxIn& txin, vin) {
            if (txin.prevout.IsNull() && !txin.scriptSig.IsZerocoinSpend()) {
                return state.DoS(100, error("CTransaction::CheckTransaction() : prevout is null"));
            }
        }

        // Check Mint Zerocoin Transaction
        BOOST_FOREACH(const CTxOut txout, vout) {
            if (!txout.scriptPubKey.empty() && txout.scriptPubKey.IsZerocoinMint()) {


                vector<unsigned char> vchZeroMint;
                vchZeroMint.insert(vchZeroMint.end(), txout.scriptPubKey.begin() + 6, txout.scriptPubKey.begin() + txout.scriptPubKey.size());


                CBigNum pubCoin;
                pubCoin.setvch(vchZeroMint);

                libzerocoin::CoinDenomination denomination;

                if (txout.nValue == libzerocoin::ZQ_LOVELACE * COIN) {
                    denomination = libzerocoin::ZQ_LOVELACE;
                    libzerocoin::PublicCoin checkPubCoin(ZCParams, pubCoin, libzerocoin::ZQ_LOVELACE);
                    if (!checkPubCoin.validate()) {
                        return state.DoS(100, error("CTransaction::CheckTransaction() : PubCoin is not validate"));
                    }
                }
                else if (txout.nValue == libzerocoin::ZQ_GOLDWASSER * COIN) {
                    denomination = libzerocoin::ZQ_GOLDWASSER;
                    libzerocoin::PublicCoin checkPubCoin(ZCParams, pubCoin, libzerocoin::ZQ_GOLDWASSER);
                    if (!checkPubCoin.validate()) {
                        return state.DoS(100, error("CTransaction::CheckTransaction() : PubCoin is not validate"));
                    }
                }
                else if (txout.nValue == libzerocoin::ZQ_RACKOFF * COIN) {
                    denomination = libzerocoin::ZQ_RACKOFF;
                    libzerocoin::PublicCoin checkPubCoin(ZCParams, pubCoin, libzerocoin::ZQ_RACKOFF);
                    if (!checkPubCoin.validate()) {
                        return state.DoS(100, error("CTransaction::CheckTransaction() : PubCoin is not validate"));
                    }
                }
                else if (txout.nValue == libzerocoin::ZQ_PEDERSEN * COIN) {
                    denomination = libzerocoin::ZQ_PEDERSEN;
                    libzerocoin::PublicCoin checkPubCoin(ZCParams, pubCoin, libzerocoin::ZQ_PEDERSEN);
                    if (!checkPubCoin.validate()) {
                        return state.DoS(100, error("CTransaction::CheckTransaction() : PubCoin is not validate"));
                    }
                }
                else if (txout.nValue == libzerocoin::ZQ_WILLIAMSON * COIN) {
                    denomination = libzerocoin::ZQ_WILLIAMSON;
                    libzerocoin::PublicCoin checkPubCoin(ZCParams, pubCoin, libzerocoin::ZQ_WILLIAMSON);
                    if (!checkPubCoin.validate()) {
                        return state.DoS(100, error("CTransaction::CheckTransaction() : PubCoin is not validate"));
                    }
                }
                else {
                    return state.DoS(100, error("CTransaction::CheckTransaction() : txout.nValue is not correct"));
                }

                if (!isVerifyDB) {                    

                    // Check the pubCoinValue didn't already store in the wallet
                    CZerocoinEntry pubCoinTx;
                    list<CZerocoinEntry> listPubCoin = list<CZerocoinEntry>();

                    CWalletDB walletdb(pwalletMain->strWalletFile);
                    walletdb.ListPubCoin(listPubCoin);
                    bool isAlreadyStored = false;

                    // CHECKING PROCESS
                    BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                        if (pubCoinItem.value == pubCoin && pubCoinItem.denomination == denomination)
                        {
                            isAlreadyStored = true;
                        }
                    }

                    // INSERT PROCESS
                    if (!isAlreadyStored) {
                        // TX DOES NOT INCLUDE IN DB
                        printf("INSERTING\n");
                        pubCoinTx.id = -1;
                        pubCoinTx.denomination = denomination;
                        pubCoinTx.value = pubCoin;
                        pubCoinTx.randomness = 0;
                        pubCoinTx.serialNumber = 0;
                        pubCoinTx.nHeight = -1;
                        printf("INSERT PUBCOIN ID: %d\n", pubCoinTx.id);
                        walletdb.WriteZerocoinEntry(pubCoinTx);
                    }
                }
            }
        }

        // Check Spend Zerocoin Transaction
        // (vin.size() == 1 && vin[0].prevout.IsNull() && (vin[0].scriptSig[0] == OP_ZEROCOINSPEND) );
        if (IsZerocoinSpend())
        {
            // Check vOut

            // Only one loop, we checked on the format before enter this case
            BOOST_FOREACH(const CTxOut txout, vout) {

                CZerocoinEntry pubCoinTx;
                list<CZerocoinEntry> listPubCoin;
                listPubCoin.clear();

                if (!isVerifyDB) {
                    CWalletDB walletdb(pwalletMain->strWalletFile);
                    walletdb.ListPubCoin(listPubCoin);

                    listPubCoin.sort(CompHeight);

                    if (txout.nValue == libzerocoin::ZQ_LOVELACE * COIN)
                    {
                        // Check vIn
                        BOOST_FOREACH(const CTxIn& txin, vin) {

                            if (txin.scriptSig.IsZerocoinSpend()) {


                                // Deserialize the CoinSpend intro a fresh object

                                std::vector<char, zero_after_free_allocator<char> > dataTxIn;
                                dataTxIn.insert(dataTxIn.end(), txin.scriptSig.begin() + 4, txin.scriptSig.end());

                                CDataStream serializedCoinSpend(SER_NETWORK, PROTOCOL_VERSION);
                                serializedCoinSpend.vch = dataTxIn;

                                libzerocoin::CoinSpend newSpend(ZCParams, serializedCoinSpend);

                                // Create a new metadata object to contain the hash of the received
                                // ZEROCOIN_SPEND transaction. If we were a real client we'd actually
                                // compute the hash of the received transaction here.
                                libzerocoin::SpendMetaData newMetadata(0, 0);

                                libzerocoin::Accumulator accumulator(ZCParams, libzerocoin::ZQ_LOVELACE);
                                libzerocoin::Accumulator accumulatorRev(ZCParams, libzerocoin::ZQ_LOVELACE);

                                bool passVerify = false;

                                /////////////////////////////////////////////////////////////////////////////////////////////////

                                // CHECK PUBCOIN ID
                                int pubcoinId = txin.nSequence;
                                //printf("====================== pubcoinId = %d\n", pubcoinId);
                                if (pubcoinId < 1 || pubcoinId >= INT_MAX) { // IT BEGINS WITH 1
                                    return state.DoS(100, error("CTransaction::CheckTransaction() : Error: nSequence is not correct format"));
                                }

                                // VERIFY COINSPEND TX
                                int countPubcoin = 0;
                                BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                                    //printf("denomination = %d, id = %d, pubcoinId = %d height = %d\n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId, pubCoinItem.nHeight);

                                    if (pubCoinItem.denomination == libzerocoin::ZQ_LOVELACE && pubCoinItem.id == pubcoinId && pubCoinItem.nHeight != -1) {
                                        printf("## denomination = %d, id = %d, pubcoinId = %d height = %d\n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId, pubCoinItem.nHeight);
                                        libzerocoin::PublicCoin pubCoinTemp(ZCParams, pubCoinItem.value, libzerocoin::ZQ_LOVELACE);
                                        if (!pubCoinTemp.validate()) {
                                            return state.DoS(100, error("CTransaction::CheckTransaction() : Error: Public Coin for Accumulator is not valid !!!"));
                                        }
                                        countPubcoin++;
                                        accumulator += pubCoinTemp;
                                        if (countPubcoin >= 2) { // MINIMUM REQUIREMENT IS 2 PUBCOINS
                                            if (newSpend.Verify(accumulator, newMetadata)) {
                                                printf("COIN SPEND TX DID VERIFY!\n");
                                                passVerify = true;
                                                break;
                                            }
                                        }
                                    }
                                }

                                // It does not have this mint coins id, still sync
                                if(countPubcoin == 0){
                                    return state.DoS(0, error("CTransaction::CheckTransaction() : Error: Node does not have mint zerocoin to verify, please wait until "));

                                }

                                if(!passVerify){
                                    int countPubcoin = 0;
                                    printf("PROCESS REVERSE\n");
                                    BOOST_REVERSE_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                                        //printf("denomination = %d, id = %d, pubcoinId = %d height = %d\n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId, pubCoinItem.nHeight);
                                        if (pubCoinItem.denomination == libzerocoin::ZQ_LOVELACE && pubCoinItem.id == pubcoinId && pubCoinItem.nHeight != -1) {
                                            printf("## denomination = %d, id = %d, pubcoinId = %d height = %d\n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId, pubCoinItem.nHeight);
                                            libzerocoin::PublicCoin pubCoinTemp(ZCParams, pubCoinItem.value, libzerocoin::ZQ_LOVELACE);
                                            if (!pubCoinTemp.validate()) {
                                                return state.DoS(100, error("CTransaction::CheckTransaction() : Error: Public Coin for Accumulator is not valid !!!"));
                                            }
                                            countPubcoin++;
                                            accumulatorRev += pubCoinTemp;
                                            if (countPubcoin >= 2) { // MINIMUM REQUIREMENT IS 2 PUBCOINS
                                                if (newSpend.Verify(accumulatorRev, newMetadata)) {
                                                    printf("COIN SPEND TX DID VERIFY!\n");
                                                    passVerify = true;
                                                    break;
                                                }
                                            }
                                        }
                                    }

                                    // It does not have this mint coins id, still sync
                                    if(countPubcoin == 0){
                                        return state.DoS(0, error("CTransaction::CheckTransaction() : Error: Node does not have mint zerocoin to verify, please wait until "));

                                    }
                                }

                                if (passVerify) {
                                    // Pull the serial number out of the CoinSpend object. If we
                                    // were a real Zerocoin client we would now check that the serial number
                                    // has not been spent before (in another ZEROCOIN_SPEND) transaction.
                                    // The serial number is stored as a Bignum.
                                    if (!isVerifyDB) {
                                        // chceck already store
                                        bool isAlreadyStored = false;

                                        CBigNum serialNumber = newSpend.getCoinSerialNumber();
                                        CWalletDB walletdb(pwalletMain->strWalletFile);

                                        std::list<CZerocoinSpendEntry> listCoinSpendSerial;
                                        walletdb.ListCoinSpendSerial(listCoinSpendSerial);
                                        BOOST_FOREACH(const CZerocoinSpendEntry& item, listCoinSpendSerial) {
                                            if (item.coinSerial == serialNumber
                                                && item.denomination == libzerocoin::ZQ_LOVELACE
                                                && item.id == pubcoinId
                                                && item.hashTx != hashTx) {
                                                return state.DoS(100, error("CTransaction::CheckTransaction() : The CoinSpend serial has been used"));
                                            }
                                            else if (item.coinSerial == serialNumber
                                                && item.hashTx == hashTx
                                                && item.denomination == libzerocoin::ZQ_LOVELACE
                                                && item.id == pubcoinId
                                                && item.pubCoin != 0) {
                                                // UPDATING PROCESS
                                                BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                                                    if (pubCoinItem.value == item.pubCoin) {
                                                        pubCoinTx.nHeight = pubCoinItem.nHeight;
                                                        pubCoinTx.denomination = pubCoinItem.denomination;
                                                        // UPDATE FOR INDICATE IT HAS BEEN USED
                                                        pubCoinTx.IsUsed = true;
                                                        // REMOVE RANDOMNESS FOR PREVENT FUTURE USE
                                                        // pubCoinTx.randomness = 0;
                                                        // pubCoinTx.serialNumber = 0;

                                                        pubCoinTx.value = pubCoinItem.value;
                                                        pubCoinTx.id = pubCoinItem.id;
                                                        walletdb.WriteZerocoinEntry(pubCoinTx);
                                                        // Update UI wallet
                                                        pwalletMain->NotifyZerocoinChanged(pwalletMain, pubCoinItem.value.GetHex(), "Used", CT_UPDATED);
                                                        break;
                                                    }
                                                }
                                                isAlreadyStored = true;
                                                break;
                                            }
                                            else if (item.coinSerial == serialNumber
                                                && item.hashTx == hashTx
                                                && item.denomination == libzerocoin::ZQ_LOVELACE
                                                && item.id == pubcoinId
                                                && item.pubCoin == 0) {
                                                isAlreadyStored = true;
                                                break;
                                            }
                                        }

                                        if (!isAlreadyStored) {
                                            // INSERTING COINSPEND TO DB
                                            CZerocoinSpendEntry zccoinSpend;
                                            zccoinSpend.coinSerial = serialNumber;
                                            zccoinSpend.hashTx = hashTx;
                                            zccoinSpend.pubCoin = 0;
                                            zccoinSpend.id = pubcoinId;
                                            if(nHeight > 1 && nHeight < INT_MAX){
                                                zccoinSpend.denomination = libzerocoin::ZQ_LOVELACE;
                                            }
                                            walletdb.WriteCoinSpendSerialEntry(zccoinSpend);
                                        }
                                    }
                                }
                                else {
                                    //printf("isVerifyDB = %d\n", isVerifyDB);
                                    return state.DoS(100, error("CTransaction::CheckTransaction() : COINSPEND TX IN ZQ_LOVELACE DID NOT VERIFY!"));
                                }
                                /////////////////////////////////////////////////////////////////////////////////////////////////
                            }
                        }
                    }
                    else if (txout.nValue == libzerocoin::ZQ_GOLDWASSER * COIN) {
                        // Check vIn
                        BOOST_FOREACH(const CTxIn& txin, vin) {

                            if (txin.scriptSig.IsZerocoinSpend()) {


                                // Deserialize the CoinSpend intro a fresh object

                                std::vector<char, zero_after_free_allocator<char> > dataTxIn;
                                dataTxIn.insert(dataTxIn.end(), txin.scriptSig.begin() + 4, txin.scriptSig.end());

                                CDataStream serializedCoinSpend(SER_NETWORK, PROTOCOL_VERSION);
                                serializedCoinSpend.vch = dataTxIn;

                                libzerocoin::CoinSpend newSpend(ZCParams, serializedCoinSpend);

                                // Create a new metadata object to contain the hash of the received
                                // ZEROCOIN_SPEND transaction. If we were a real client we'd actually
                                // compute the hash of the received transaction here.
                                libzerocoin::SpendMetaData newMetadata(0, 0);

                                libzerocoin::Accumulator accumulator(ZCParams, libzerocoin::ZQ_GOLDWASSER);
                                libzerocoin::Accumulator accumulatorRev(ZCParams, libzerocoin::ZQ_GOLDWASSER);

                                bool passVerify = false;

                                /////////////////////////////////////////////////////////////////////////////////////////////////

                                // CHECK PUBCOIN ID
                                unsigned int pubcoinId = txin.nSequence;
                                if (pubcoinId < 1 || pubcoinId >= INT_MAX) { // IT BEGINS WITH 1
                                    return state.DoS(100, error("CTransaction::CheckTransaction() : Error: nSequence is not correct format"));
                                }

                                // VERIFY COINSPEND TX
                                unsigned int countPubcoin = 0;
                                BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {

                                    if (pubCoinItem.denomination == libzerocoin::ZQ_GOLDWASSER && pubCoinItem.id == pubcoinId && pubCoinItem.nHeight != -1) {
                                        libzerocoin::PublicCoin pubCoinTemp(ZCParams, pubCoinItem.value, libzerocoin::ZQ_GOLDWASSER);
                                        if (!pubCoinTemp.validate()) {
                                            return state.DoS(100, error("CTransaction::CheckTransaction() : Error: Public Coin for Accumulator is not valid !!!"));
                                        }
                                        countPubcoin++;
                                        accumulator += pubCoinTemp;
                                        if (countPubcoin >= 1) { // MINIMUM REQUIREMENT IS 1 PUBCOINS (changed from default of 2)
                                            if (newSpend.Verify(accumulator, newMetadata)) {
                                                printf("COIN SPEND TX DID VERIFY!\n");
                                                passVerify = true;
                                                break;
                                            }
                                        }
                                    }
                                }

                                // It does not have this mint coins id, still sync
                                if(countPubcoin == 0){
                                    return state.DoS(0, error("CTransaction::CheckTransaction() : Error: Node does not have mint zerocoin to verify, please wait until "));

                                }

                                if(!passVerify){
                                    int countPubcoin = 0;
                                    printf("PROCESS REVERSE\n");
                                    BOOST_REVERSE_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                                        //printf("pubCoinItem.denomination = %d, pubCoinItem.id = %d, pubcoinId = %d \n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId);
                                        if (pubCoinItem.denomination == libzerocoin::ZQ_GOLDWASSER && pubCoinItem.id == pubcoinId && pubCoinItem.nHeight != -1) {
                                            printf("## denomination = %d, id = %d, pubcoinId = %d height = %d\n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId, pubCoinItem.nHeight);
                                            libzerocoin::PublicCoin pubCoinTemp(ZCParams, pubCoinItem.value, libzerocoin::ZQ_GOLDWASSER);
                                            if (!pubCoinTemp.validate()) {
                                                return state.DoS(100, error("CTransaction::CheckTransaction() : Error: Public Coin for Accumulator is not valid !!!"));
                                            }
                                            countPubcoin++;
                                            accumulatorRev += pubCoinTemp;
                                            if (countPubcoin >= 2) { // MINIMUM REQUIREMENT IS 2 PUBCOINS
                                                if (newSpend.Verify(accumulatorRev, newMetadata)) {
                                                    printf("COIN SPEND TX DID VERIFY!\n");
                                                    passVerify = true;
                                                    break;
                                                }
                                            }
                                        }
                                    }

                                    // It does not have this mint coins id, still sync
                                    if(countPubcoin == 0){
                                        return state.DoS(0, error("CTransaction::CheckTransaction() : Error: Node does not have mint zerocoin to verify, please wait until "));

                                    }

                                }

                                if (passVerify) {
                                    // Pull the serial number out of the CoinSpend object. If we
                                    // were a real Zerocoin client we would now check that the serial number
                                    // has not been spent before (in another ZEROCOIN_SPEND) transaction.
                                    // The serial number is stored as a Bignum.
                                    if (!isVerifyDB) {
                                        // chceck already store
                                        bool isAlreadyStored = false;

                                        CBigNum serialNumber = newSpend.getCoinSerialNumber();
                                        CWalletDB walletdb(pwalletMain->strWalletFile);

                                        std::list<CZerocoinSpendEntry> listCoinSpendSerial;
                                        walletdb.ListCoinSpendSerial(listCoinSpendSerial);
                                        BOOST_FOREACH(const CZerocoinSpendEntry& item, listCoinSpendSerial) {
                                            if (item.coinSerial == serialNumber
                                                && item.denomination == libzerocoin::ZQ_GOLDWASSER
                                                && item.id == pubcoinId
                                                && item.hashTx != hashTx) {
                                                return state.DoS(100, error("CTransaction::CheckTransaction() : The CoinSpend serial has been used"));
                                            }
                                            else if (item.coinSerial == serialNumber
                                                && item.hashTx == hashTx
                                                && item.denomination == libzerocoin::ZQ_GOLDWASSER
                                                && item.id == pubcoinId
                                                && item.pubCoin != 0) {
                                                // UPDATING PROCESS
                                                BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                                                    if (pubCoinItem.value == item.pubCoin) {
                                                        pubCoinTx.nHeight = pubCoinItem.nHeight;
                                                        pubCoinTx.denomination = pubCoinItem.denomination;
                                                        // UPDATE FOR INDICATE IT HAS BEEN USED
                                                        pubCoinTx.IsUsed = true;
                                                        // REMOVE RANDOMNESS FOR PREVENT FUTURE USE
                                                        // pubCoinTx.randomness = 0;
                                                        // pubCoinTx.serialNumber = 0;

                                                        pubCoinTx.value = pubCoinItem.value;
                                                        pubCoinTx.id = pubCoinItem.id;
                                                        walletdb.WriteZerocoinEntry(pubCoinTx);
                                                        // Update UI wallet
                                                        pwalletMain->NotifyZerocoinChanged(pwalletMain, pubCoinItem.value.GetHex(), "Used", CT_UPDATED);
                                                        break;
                                                    }
                                                }
                                                isAlreadyStored = true;
                                                break;
                                            }
                                            else if (item.coinSerial == serialNumber
                                                && item.hashTx == hashTx
                                                && item.denomination == libzerocoin::ZQ_GOLDWASSER
                                                && item.id == pubcoinId
                                                && item.pubCoin == 0) {
                                                isAlreadyStored = true;
                                                break;
                                            }
                                        }

                                        if (!isAlreadyStored) {
                                            // INSERTING COINSPEND TO DB
                                            CZerocoinSpendEntry zccoinSpend;
                                            zccoinSpend.coinSerial = serialNumber;
                                            zccoinSpend.hashTx = hashTx;
                                            zccoinSpend.pubCoin = 0;
                                            zccoinSpend.id = pubcoinId;
                                            if(nHeight > 1 && nHeight < INT_MAX){
                                                zccoinSpend.denomination = libzerocoin::ZQ_GOLDWASSER;
                                            }
                                            walletdb.WriteCoinSpendSerialEntry(zccoinSpend);
                                        }
                                    }
                                }
                                else {
                                    return state.DoS(100, error("CTransaction::CheckTransaction() : COIN SPEND TX IN ZQ_GOLDWASSER DID NOT VERIFY!"));
                                }
                                /////////////////////////////////////////////////////////////////////////////////////////////////
                            }
                        }
                    }
                    else if (txout.nValue == libzerocoin::ZQ_RACKOFF * COIN) {
                        // Check vIn
                        BOOST_FOREACH(const CTxIn& txin, vin) {

                            if (txin.scriptSig.IsZerocoinSpend()) {


                                // Deserialize the CoinSpend intro a fresh object

                                std::vector<char, zero_after_free_allocator<char> > dataTxIn;
                                dataTxIn.insert(dataTxIn.end(), txin.scriptSig.begin() + 4, txin.scriptSig.end());

                                CDataStream serializedCoinSpend(SER_NETWORK, PROTOCOL_VERSION);
                                serializedCoinSpend.vch = dataTxIn;

                                libzerocoin::CoinSpend newSpend(ZCParams, serializedCoinSpend);

                                // Create a new metadata object to contain the hash of the received
                                // ZEROCOIN_SPEND transaction. If we were a real client we'd actually
                                // compute the hash of the received transaction here.
                                libzerocoin::SpendMetaData newMetadata(0, 0);

                                libzerocoin::Accumulator accumulator(ZCParams, libzerocoin::ZQ_RACKOFF);
                                libzerocoin::Accumulator accumulatorRev(ZCParams, libzerocoin::ZQ_RACKOFF);

                                bool passVerify = false;

                                /////////////////////////////////////////////////////////////////////////////////////////////////

                                // CHECK PUBCOIN ID
                                unsigned int pubcoinId = txin.nSequence;
                                if (pubcoinId < 1 || pubcoinId >= INT_MAX) { // IT BEGINS WITH 1
                                    return state.DoS(100, error("CTransaction::CheckTransaction() : Error: nSequence is not correct format"));
                                }

                                // VERIFY COINSPEND TX
                                unsigned int countPubcoin = 0;
                                BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {

                                    if (pubCoinItem.denomination == libzerocoin::ZQ_RACKOFF && pubCoinItem.id == pubcoinId && pubCoinItem.nHeight != -1) {
                                        libzerocoin::PublicCoin pubCoinTemp(ZCParams, pubCoinItem.value, libzerocoin::ZQ_RACKOFF);
                                        if (!pubCoinTemp.validate()) {
                                            return state.DoS(100, error("CTransaction::CheckTransaction() : Error: Public Coin for Accumulator is not valid !!!"));
                                        }
                                        countPubcoin++;
                                        accumulator += pubCoinTemp;
                                        if (countPubcoin >= 2) { // MINIMUM REQUIREMENT IS 2 PUBCOINS
                                            if (newSpend.Verify(accumulator, newMetadata)) {
                                                printf("COIN SPEND TX DID VERIFY!\n");
                                                passVerify = true;
                                                break;
                                            }
                                        }
                                    }
                                }

                                // It does not have this mint coins id, still sync
                                if(countPubcoin == 0){
                                    return state.DoS(0, error("CTransaction::CheckTransaction() : Error: Node does not have mint zerocoin to verify, please wait until "));

                                }

                                if(!passVerify){
                                    int countPubcoin = 0;
                                    printf("PROCESS REVERSE\n");
                                    BOOST_REVERSE_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                                        //printf("pubCoinItem.denomination = %d, pubCoinItem.id = %d, pubcoinId = %d \n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId);
                                        if (pubCoinItem.denomination == libzerocoin::ZQ_RACKOFF && pubCoinItem.id == pubcoinId && pubCoinItem.nHeight != -1) {
                                            printf("## denomination = %d, id = %d, pubcoinId = %d height = %d\n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId, pubCoinItem.nHeight);
                                            libzerocoin::PublicCoin pubCoinTemp(ZCParams, pubCoinItem.value, libzerocoin::ZQ_RACKOFF);
                                            if (!pubCoinTemp.validate()) {
                                                return state.DoS(100, error("CTransaction::CheckTransaction() : Error: Public Coin for Accumulator is not valid !!!"));
                                            }
                                            countPubcoin++;
                                            accumulatorRev += pubCoinTemp;
                                            if (countPubcoin >= 2) { // MINIMUM REQUIREMENT IS 2 PUBCOINS
                                                if (newSpend.Verify(accumulatorRev, newMetadata)) {
                                                    printf("COIN SPEND TX DID VERIFY!\n");
                                                    passVerify = true;
                                                    break;
                                                }
                                            }
                                        }
                                    }

                                    // It does not have this mint coins id, still sync
                                    if(countPubcoin == 0){
                                        return state.DoS(0, error("CTransaction::CheckTransaction() : Error: Node does not have mint zerocoin to verify, please wait until "));

                                    }
                                }

                                if (passVerify) {
                                    // Pull the serial number out of the CoinSpend object. If we
                                    // were a real Zerocoin client we would now check that the serial number
                                    // has not been spent before (in another ZEROCOIN_SPEND) transaction.
                                    // The serial number is stored as a Bignum.
                                    if (!isVerifyDB) {
                                        // chceck already store
                                        bool isAlreadyStored = false;

                                        CBigNum serialNumber = newSpend.getCoinSerialNumber();
                                        CWalletDB walletdb(pwalletMain->strWalletFile);

                                        std::list<CZerocoinSpendEntry> listCoinSpendSerial;
                                        walletdb.ListCoinSpendSerial(listCoinSpendSerial);
                                        BOOST_FOREACH(const CZerocoinSpendEntry& item, listCoinSpendSerial) {
                                            if (item.coinSerial == serialNumber
                                                && item.denomination == libzerocoin::ZQ_RACKOFF
                                                && item.id == pubcoinId
                                                && item.hashTx != hashTx) {
                                                return state.DoS(100, error("CTransaction::CheckTransaction() : The CoinSpend serial has been used"));
                                            }
                                            else if (item.coinSerial == serialNumber
                                                && item.hashTx == hashTx
                                                && item.denomination == libzerocoin::ZQ_RACKOFF
                                                && item.id == pubcoinId
                                                && item.pubCoin != 0) {
                                                // UPDATING PROCESS
                                                BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                                                    if (pubCoinItem.value == item.pubCoin) {
                                                        pubCoinTx.nHeight = pubCoinItem.nHeight;
                                                        pubCoinTx.denomination = pubCoinItem.denomination;
                                                        // UPDATE FOR INDICATE IT HAS BEEN USED
                                                        pubCoinTx.IsUsed = true;
                                                        // REMOVE RANDOMNESS FOR PREVENT FUTURE USE
                                                        // pubCoinTx.randomness = 0;
                                                        // pubCoinTx.serialNumber = 0;

                                                        pubCoinTx.value = pubCoinItem.value;
                                                        pubCoinTx.id = pubCoinItem.id;
                                                        walletdb.WriteZerocoinEntry(pubCoinTx);
                                                        // Update UI wallet
                                                        pwalletMain->NotifyZerocoinChanged(pwalletMain, pubCoinItem.value.GetHex(), "Used", CT_UPDATED);
                                                        break;
                                                    }
                                                }
                                                isAlreadyStored = true;
                                                break;
                                            }
                                            else if (item.coinSerial == serialNumber
                                                && item.hashTx == hashTx
                                                && item.denomination == libzerocoin::ZQ_RACKOFF
                                                && item.id == pubcoinId
                                                && item.pubCoin == 0) {
                                                isAlreadyStored = true;
                                                break;
                                            }
                                        }

                                        if (!isAlreadyStored) {
                                            // INSERTING COINSPEND TO DB
                                            CZerocoinSpendEntry zccoinSpend;
                                            zccoinSpend.coinSerial = serialNumber;
                                            zccoinSpend.hashTx = hashTx;
                                            zccoinSpend.pubCoin = 0;
                                            zccoinSpend.id = pubcoinId;
                                            if(nHeight > 1 && nHeight < INT_MAX){
                                                zccoinSpend.denomination = libzerocoin::ZQ_RACKOFF;
                                            }
                                            walletdb.WriteCoinSpendSerialEntry(zccoinSpend);
                                        }
                                    }
                                }
                                else {
                                    return state.DoS(100, error("CTransaction::CheckTransaction() : COIN SPEND TX IN ZQ_RACKOFF DID NOT VERIFY!"));
                                }
                                /////////////////////////////////////////////////////////////////////////////////////////////////
                            }
                        }
                    }
                    else if (txout.nValue == libzerocoin::ZQ_PEDERSEN * COIN) {
                        // Check vIn
                        BOOST_FOREACH(const CTxIn& txin, vin) {

                            if (txin.scriptSig.IsZerocoinSpend()) {


                                // Deserialize the CoinSpend intro a fresh object

                                std::vector<char, zero_after_free_allocator<char> > dataTxIn;
                                dataTxIn.insert(dataTxIn.end(), txin.scriptSig.begin() + 4, txin.scriptSig.end());

                                CDataStream serializedCoinSpend(SER_NETWORK, PROTOCOL_VERSION);
                                serializedCoinSpend.vch = dataTxIn;

                                libzerocoin::CoinSpend newSpend(ZCParams, serializedCoinSpend);

                                // Create a new metadata object to contain the hash of the received
                                // ZEROCOIN_SPEND transaction. If we were a real client we'd actually
                                // compute the hash of the received transaction here.
                                libzerocoin::SpendMetaData newMetadata(0, 0);

                                libzerocoin::Accumulator accumulator(ZCParams, libzerocoin::ZQ_PEDERSEN);
                                libzerocoin::Accumulator accumulatorRev(ZCParams, libzerocoin::ZQ_PEDERSEN);

                                bool passVerify = false;

                                /////////////////////////////////////////////////////////////////////////////////////////////////

                                // CHECK PUBCOIN ID
                                unsigned int pubcoinId = txin.nSequence;
                                if (pubcoinId < 1 || pubcoinId >= INT_MAX) { // IT BEGINS WITH 1
                                    return state.DoS(100, error("CTransaction::CheckTransaction() : Error: nSequence is not correct format"));
                                }

                                // VERIFY COINSPEND TX
                                unsigned int countPubcoin = 0;
                                BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {

                                    if (pubCoinItem.denomination == libzerocoin::ZQ_PEDERSEN && pubCoinItem.id == pubcoinId && pubCoinItem.nHeight != -1) {
                                        libzerocoin::PublicCoin pubCoinTemp(ZCParams, pubCoinItem.value, libzerocoin::ZQ_PEDERSEN);
                                        if (!pubCoinTemp.validate()) {
                                            return state.DoS(100, error("CTransaction::CheckTransaction() : Error: Public Coin for Accumulator is not valid !!!"));
                                        }
                                        countPubcoin++;
                                        accumulator += pubCoinTemp;
                                        if (countPubcoin >= 2) { // MINIMUM REQUIREMENT IS 2 PUBCOINS
                                            if (newSpend.Verify(accumulator, newMetadata)) {
                                                printf("COIN SPEND TX DID VERIFY!\n");
                                                passVerify = true;
                                                break;
                                            }
                                        }
                                    }
                                }

                                // It does not have this mint coins id, still sync
                                if(countPubcoin == 0){
                                    return state.DoS(0, error("CTransaction::CheckTransaction() : Error: Node does not have mint zerocoin to verify, please wait until "));

                                }

                                if(!passVerify){
                                    int countPubcoin = 0;
                                    printf("PROCESS REVERSE\n");
                                    BOOST_REVERSE_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                                        //printf("pubCoinItem.denomination = %d, pubCoinItem.id = %d, pubcoinId = %d \n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId);
                                        if (pubCoinItem.denomination == libzerocoin::ZQ_PEDERSEN && pubCoinItem.id == pubcoinId && pubCoinItem.nHeight != -1) {
                                            printf("## denomination = %d, id = %d, pubcoinId = %d height = %d\n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId, pubCoinItem.nHeight);
                                            libzerocoin::PublicCoin pubCoinTemp(ZCParams, pubCoinItem.value, libzerocoin::ZQ_PEDERSEN);
                                            if (!pubCoinTemp.validate()) {
                                                return state.DoS(100, error("CTransaction::CheckTransaction() : Error: Public Coin for Accumulator is not valid !!!"));
                                            }
                                            countPubcoin++;
                                            accumulatorRev += pubCoinTemp;
                                            if (countPubcoin >= 2) { // MINIMUM REQUIREMENT IS 2 PUBCOINS
                                                if (newSpend.Verify(accumulatorRev, newMetadata)) {
                                                    printf("COIN SPEND TX DID VERIFY!\n");
                                                    passVerify = true;
                                                    break;
                                                }
                                            }
                                        }
                                    }

                                    // It does not have this mint coins id, still sync
                                    if(countPubcoin == 0){
                                        return state.DoS(0, error("CTransaction::CheckTransaction() : Error: Node does not have mint zerocoin to verify, please wait until "));

                                    }
                                }

                                if (passVerify) {
                                    // Pull the serial number out of the CoinSpend object. If we
                                    // were a real Zerocoin client we would now check that the serial number
                                    // has not been spent before (in another ZEROCOIN_SPEND) transaction.
                                    // The serial number is stored as a Bignum.
                                    if (!isVerifyDB) {
                                        // chceck already store
                                        bool isAlreadyStored = false;

                                        CBigNum serialNumber = newSpend.getCoinSerialNumber();
                                        CWalletDB walletdb(pwalletMain->strWalletFile);

                                        std::list<CZerocoinSpendEntry> listCoinSpendSerial;
                                        walletdb.ListCoinSpendSerial(listCoinSpendSerial);
                                        BOOST_FOREACH(const CZerocoinSpendEntry& item, listCoinSpendSerial) {
                                            if (item.coinSerial == serialNumber
                                                && item.denomination == libzerocoin::ZQ_PEDERSEN
                                                && item.id == pubcoinId
                                                && item.hashTx != hashTx) {
                                                return state.DoS(100, error("CTransaction::CheckTransaction() : The CoinSpend serial has been used"));
                                            }
                                            else if (item.coinSerial == serialNumber
                                                && item.hashTx == hashTx
                                                && item.denomination == libzerocoin::ZQ_PEDERSEN
                                                && item.id == pubcoinId
                                                && item.pubCoin != 0) {
                                                // UPDATING PROCESS
                                                BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                                                    if (pubCoinItem.value == item.pubCoin) {
                                                        pubCoinTx.nHeight = pubCoinItem.nHeight;
                                                        pubCoinTx.denomination = pubCoinItem.denomination;
                                                        // UPDATE FOR INDICATE IT HAS BEEN USED
                                                        pubCoinTx.IsUsed = true;
                                                        // REMOVE RANDOMNESS FOR PREVENT FUTURE USE
                                                        // pubCoinTx.randomness = 0;
                                                        // pubCoinTx.serialNumber = 0;

                                                        pubCoinTx.value = pubCoinItem.value;
                                                        pubCoinTx.id = pubCoinItem.id;
                                                        walletdb.WriteZerocoinEntry(pubCoinTx);
                                                        // Update UI wallet
                                                        pwalletMain->NotifyZerocoinChanged(pwalletMain, pubCoinItem.value.GetHex(), "Used", CT_UPDATED);
                                                        break;
                                                    }
                                                }
                                                isAlreadyStored = true;
                                                break;
                                            }
                                            else if (item.coinSerial == serialNumber
                                                && item.hashTx == hashTx
                                                && item.denomination == libzerocoin::ZQ_PEDERSEN
                                                && item.id == pubcoinId
                                                && item.pubCoin == 0) {
                                                isAlreadyStored = true;
                                                break;
                                            }
                                        }

                                        if (!isAlreadyStored) {
                                            // INSERTING COINSPEND TO DB
                                            CZerocoinSpendEntry zccoinSpend;
                                            zccoinSpend.coinSerial = serialNumber;
                                            zccoinSpend.hashTx = hashTx;
                                            zccoinSpend.pubCoin = 0;
                                            zccoinSpend.id = pubcoinId;
                                            if(nHeight > 1 && nHeight < INT_MAX){
                                                zccoinSpend.denomination = libzerocoin::ZQ_PEDERSEN;
                                            }
                                            walletdb.WriteCoinSpendSerialEntry(zccoinSpend);
                                        }
                                    }
                                }
                                else {
                                    return state.DoS(100, error("CTransaction::CheckTransaction() : COIN SPEND TX IN ZQ_PEDERSEN DID NOT VERIFY!"));
                                }
                                /////////////////////////////////////////////////////////////////////////////////////////////////
                            }
                        }
                    }
                    else if (txout.nValue == libzerocoin::ZQ_WILLIAMSON * COIN) {
                        // Check vIn
                        BOOST_FOREACH(const CTxIn& txin, vin) {

                            if (txin.scriptSig.IsZerocoinSpend()) {


                                // Deserialize the CoinSpend intro a fresh object

                                std::vector<char, zero_after_free_allocator<char> > dataTxIn;
                                dataTxIn.insert(dataTxIn.end(), txin.scriptSig.begin() + 4, txin.scriptSig.end());

                                CDataStream serializedCoinSpend(SER_NETWORK, PROTOCOL_VERSION);
                                serializedCoinSpend.vch = dataTxIn;

                                libzerocoin::CoinSpend newSpend(ZCParams, serializedCoinSpend);

                                // Create a new metadata object to contain the hash of the received
                                // ZEROCOIN_SPEND transaction. If we were a real client we'd actually
                                // compute the hash of the received transaction here.
                                libzerocoin::SpendMetaData newMetadata(0, 0);

                                libzerocoin::Accumulator accumulator(ZCParams, libzerocoin::ZQ_WILLIAMSON);
                                libzerocoin::Accumulator accumulatorRev(ZCParams, libzerocoin::ZQ_WILLIAMSON);

                                bool passVerify = false;

                                /////////////////////////////////////////////////////////////////////////////////////////////////

                                // CHECK PUBCOIN ID
                                unsigned int pubcoinId = txin.nSequence;
                                if (pubcoinId < 1 || pubcoinId >= INT_MAX) { // IT BEGINS WITH 1
                                    return state.DoS(100, error("CTransaction::CheckTransaction() : Error: nSequence is not correct format"));
                                }

                                // VERIFY COINSPEND TX
                                unsigned int countPubcoin = 0;
                                BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {

                                    if (pubCoinItem.denomination == libzerocoin::ZQ_WILLIAMSON && pubCoinItem.id == pubcoinId && pubCoinItem.nHeight != -1) {
                                        printf("## denomination = %d, id = %d, pubcoinId = %d height = %d\n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId, pubCoinItem.nHeight);
                                        libzerocoin::PublicCoin pubCoinTemp(ZCParams, pubCoinItem.value, libzerocoin::ZQ_WILLIAMSON);
                                        if (!pubCoinTemp.validate()) {
                                            return state.DoS(100, error("CTransaction::CheckTransaction() : Error: Public Coin for Accumulator is not valid !!!"));
                                        }
                                        countPubcoin++;
                                        accumulator += pubCoinTemp;
                                        if (countPubcoin >= 2) { // MINIMUM REQUIREMENT IS 2 PUBCOINS
                                            if (newSpend.Verify(accumulator, newMetadata)) {
                                                printf("COIN SPEND TX DID VERIFY!\n");
                                                passVerify = true;
                                                break;
                                            }
                                        }
                                    }
                                }

                                // It does not have this mint coins id, still sync
                                if(countPubcoin == 0){
                                    return state.DoS(0, error("CTransaction::CheckTransaction() : Error: Node does not have mint zerocoin to verify, please wait until "));

                                }

                                if(!passVerify){
                                    int countPubcoin = 0;
                                    printf("PROCESS REVERSE\n");
                                    BOOST_REVERSE_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                                        //printf("pubCoinItem.denomination = %d, pubCoinItem.id = %d, pubcoinId = %d \n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId);
                                        if (pubCoinItem.denomination == libzerocoin::ZQ_WILLIAMSON && pubCoinItem.id == pubcoinId && pubCoinItem.nHeight != -1) {
                                            printf("## denomination = %d, id = %d, pubcoinId = %d height = %d\n", pubCoinItem.denomination, pubCoinItem.id, pubcoinId, pubCoinItem.nHeight);
                                            libzerocoin::PublicCoin pubCoinTemp(ZCParams, pubCoinItem.value, libzerocoin::ZQ_WILLIAMSON);
                                            if (!pubCoinTemp.validate()) {
                                                return state.DoS(100, error("CTransaction::CheckTransaction() : Error: Public Coin for Accumulator is not valid !!!"));
                                            }
                                            countPubcoin++;
                                            accumulatorRev += pubCoinTemp;
                                            if (countPubcoin >= 2) { // MINIMUM REQUIREMENT IS 2 PUBCOINS
                                                if (newSpend.Verify(accumulatorRev, newMetadata)) {
                                                    printf("COIN SPEND TX DID VERIFY!\n");
                                                    passVerify = true;
                                                    break;
                                                }
                                            }
                                        }
                                    }

                                    // It does not have this mint coins id, still sync
                                    if(countPubcoin == 0){
                                        return state.DoS(0, error("CTransaction::CheckTransaction() : Error: Node does not have mint zerocoin to verify, please wait until "));

                                    }
                                }

                                if (passVerify) {
                                    // Pull the serial number out of the CoinSpend object. If we
                                    // were a real Zerocoin client we would now check that the serial number
                                    // has not been spent before (in another ZEROCOIN_SPEND) transaction.
                                    // The serial number is stored as a Bignum.
                                    if (!isVerifyDB) {
                                        // chceck already store
                                        bool isAlreadyStored = false;

                                        CBigNum serialNumber = newSpend.getCoinSerialNumber();
                                        CWalletDB walletdb(pwalletMain->strWalletFile);

                                        std::list<CZerocoinSpendEntry> listCoinSpendSerial;
                                        walletdb.ListCoinSpendSerial(listCoinSpendSerial);
                                        BOOST_FOREACH(const CZerocoinSpendEntry& item, listCoinSpendSerial) {
                                            if (item.coinSerial == serialNumber
                                                && item.denomination == libzerocoin::ZQ_WILLIAMSON
                                                && item.id == pubcoinId
                                                && item.hashTx != hashTx) {
                                                return state.DoS(100, error("CTransaction::CheckTransaction() : The CoinSpend serial has been used"));
                                            }
                                            else if (item.coinSerial == serialNumber
                                                && item.hashTx == hashTx
                                                && item.denomination == libzerocoin::ZQ_WILLIAMSON
                                                && item.id == pubcoinId
                                                && item.pubCoin != 0) {
                                                // UPDATING PROCESS
                                                BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                                                    if (pubCoinItem.value == item.pubCoin) {
                                                        pubCoinTx.nHeight = pubCoinItem.nHeight;
                                                        pubCoinTx.denomination = pubCoinItem.denomination;
                                                        // UPDATE FOR INDICATE IT HAS BEEN USED
                                                        pubCoinTx.IsUsed = true;
                                                        // REMOVE RANDOMNESS FOR PREVENT FUTURE USE
                                                        // pubCoinTx.randomness = 0;
                                                        // pubCoinTx.serialNumber = 0;

                                                        pubCoinTx.value = pubCoinItem.value;
                                                        pubCoinTx.id = pubCoinItem.id;
                                                        walletdb.WriteZerocoinEntry(pubCoinTx);
                                                        // Update UI wallet
                                                        pwalletMain->NotifyZerocoinChanged(pwalletMain, pubCoinItem.value.GetHex(), "Used", CT_UPDATED);
                                                        break;
                                                    }
                                                }
                                                isAlreadyStored = true;
                                                break;
                                            }
                                            else if (item.coinSerial == serialNumber
                                                && item.hashTx == hashTx
                                                && item.denomination == libzerocoin::ZQ_WILLIAMSON
                                                && item.id == pubcoinId
                                                && item.pubCoin == 0) {
                                                isAlreadyStored = true;
                                                break;
                                            }
                                        }

                                        if (!isAlreadyStored) {
                                            // INSERTING COINSPEND TO DB
                                            CZerocoinSpendEntry zccoinSpend;
                                            zccoinSpend.coinSerial = serialNumber;
                                            zccoinSpend.hashTx = hashTx;
                                            zccoinSpend.pubCoin = 0;
                                            zccoinSpend.id = pubcoinId;
                                            if(nHeight > 1 && nHeight < INT_MAX){
                                                zccoinSpend.denomination = libzerocoin::ZQ_WILLIAMSON;
                                            }
                                            walletdb.WriteCoinSpendSerialEntry(zccoinSpend);
                                        }
                                    }
                                }
                                else {
                                    return state.DoS(100, error("CTransaction::CheckTransaction() : COIN SPEND TX IN ZQ_WILLIAMSON DID NOT VERIFY!"));
                                }
                                /////////////////////////////////////////////////////////////////////////////////////////////////
                            }
                        }
                    }
                    else {
                        return state.DoS(100, error("CTransaction::CheckTransaction() : Your spending txout value does not match"));
                    }
                }

            }



        }
    }

    return true;
}

int64 CTransaction::GetMinFee(unsigned int nBlockSize, bool fAllowFree,
                              enum GetMinFee_mode mode) const
{
    // Base fee is either nMinTxFee or nMinRelayTxFee
    int64 nBaseFee = (mode == GMF_RELAY) ? nMinRelayTxFee : nMinTxFee;

    unsigned int nBytes = ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION);
    unsigned int nNewBlockSize = nBlockSize + nBytes;
    int64 nMinFee = (1 + (int64)nBytes / 1000) * nBaseFee;


    if (fAllowFree)
    {
        // There is a free transaction area in blocks created by most miners,
        // * If we are relaying we allow transactions up to DEFAULT_BLOCK_PRIORITY_SIZE - 1000
        //   to be considered to fall into this category. We don't want to encourage sending
        //   multiple transactions instead of one big transaction to avoid fees.
        // * If we are creating a transaction we allow transactions up to 5,000 bytes
        //   to be considered safe and assume they can likely make it into this section.
        if (nBytes < (mode == GMF_SEND ? 5000 : (DEFAULT_BLOCK_PRIORITY_SIZE - 1000)))
            nMinFee = 100000;
    }

    // SmartCash
    // To limit dust spam, add nBaseFee for each output less than DUST_SOFT_LIMIT
    BOOST_FOREACH(const CTxOut& txout, vout)
        if (txout.nValue < DUST_SOFT_LIMIT)
            nMinFee += nBaseFee;

    // Raise the price as the block approaches full
    if (nBlockSize != 1 && nNewBlockSize >= MAX_BLOCK_SIZE_GEN/2)
    {
        if (nNewBlockSize >= MAX_BLOCK_SIZE_GEN)
            return MAX_MONEY;
        nMinFee *= MAX_BLOCK_SIZE_GEN / (MAX_BLOCK_SIZE_GEN - nNewBlockSize);
    }

    if (!MoneyRange(nMinFee))
        nMinFee = MAX_MONEY;
    return nMinFee;
}

void CTxMemPool::pruneSpent(const uint256 &hashTx, CCoins &coins)
{
    LOCK(cs);

    std::map<COutPoint, CInPoint>::iterator it = mapNextTx.lower_bound(COutPoint(hashTx, 0));

    // iterate over all COutPoints in mapNextTx whose hash equals the provided hashTx
    while (it != mapNextTx.end() && it->first.hash == hashTx) {
        coins.Spend(it->first.n); // and remove those outputs from coins
        it++;
    }
}

bool CTxMemPool::accept(CValidationState &state, CTransaction &tx, bool fCheckInputs, bool fLimitFree,
                        bool* pfMissingInputs, bool fRejectInsaneFee)
{
    if (pfMissingInputs)
        *pfMissingInputs = false;

    if (!tx.CheckTransaction(state, tx.GetHash(), false, INT_MAX))
        return error("CTxMemPool::accept() : CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, error("CTxMemPool::accept() : coinbase as individual tx"));


    // To help v0.1.5 clients who would see it as a negative number
    if ((int64)tx.nLockTime > std::numeric_limits<int>::max())
        return error("CTxMemPool::accept() : not accepting nLockTime beyond 2038 yet");

    // Rather not work on nonstandard transactions (unless -testnet)
    string strNonStd;
    if (!fTestNet && !tx.IsStandard(strNonStd))
        return error("CTxMemPool::accept() : nonstandard transaction (%s)",
                     strNonStd.c_str());

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    {
        LOCK(cs);
        if (mapTx.count(hash))
            return false;
    }


    /*if(tx.IsZerocoinSpend()){
        printf("CALL addUnchecked\n");
        printf("hash = %s\n", hash.ToString().c_str());
        printf("tx = %s\n", tx.ToString().c_str());
        addUnchecked(hash, tx);
        return true;
    }*/

    // Check for conflicts with in-memory transactions
    CTransaction* ptxOld = NULL;
    if(!tx.IsZerocoinSpend()){
        for (unsigned int i = 0; i < tx.vin.size(); i++)
        {
            COutPoint outpoint = tx.vin[i].prevout;
            if (mapNextTx.count(outpoint))
            {
                // Disable replacement feature for now
                return false;

                // Allow replacing with a newer version of the same transaction
                if (i != 0)
                    return false;
                ptxOld = mapNextTx[outpoint].ptx;
                if (ptxOld->IsFinal())
                    return false;
                if (!tx.IsNewerThan(*ptxOld))
                    return false;
                for (unsigned int i = 0; i < tx.vin.size(); i++)
                {
                    COutPoint outpoint = tx.vin[i].prevout;
                    if (!mapNextTx.count(outpoint) || mapNextTx[outpoint].ptx != ptxOld)
                        return false;
                }
                break;
            }
        }
    }

    if (fCheckInputs)
    {
        CCoinsView dummy;
        CCoinsViewCache view(dummy);

        {
        LOCK(cs);
        CCoinsViewMemPool viewMemPool(*pcoinsTip, *this);
        view.SetBackend(viewMemPool);

        // do we already have it?
        if (view.HaveCoins(hash))
            return false;

        // do all inputs exist?
        // Note that this does not check for the presence of actual outputs (see the next check for that),
        // only helps filling in pfMissingInputs (to determine missing vs spent).
        BOOST_FOREACH(const CTxIn txin, tx.vin) {
            if (!view.HaveCoins(txin.prevout.hash)) {
                if (pfMissingInputs)
                    *pfMissingInputs = true;
                return false;
            }
        }

        // are the actual inputs available?
        if (!tx.HaveInputs(view))
            return state.Invalid(error("CTxMemPool::accept() : inputs already spent"));

        // Bring the best block into scope
        view.GetBestBlock();

        // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
        view.SetBackend(dummy);
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (!tx.AreInputsStandard(view) && !fTestNet)
            return error("CTxMemPool::accept() : nonstandard transaction input");

        // Note: if you modify this code to accept non-standard transactions, then
        // you should add code here to check that the transaction does a
        // reasonable number of ECDSA signature verifications.

        int64 nFees = tx.GetValueIn(view)-tx.GetValueOut();
        unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);

        // Don't accept it if it can't get into a block
        int64 txMinFee = tx.GetMinFee(1000, true, GMF_RELAY);
        if (fLimitFree && nFees < txMinFee)
            return error("CTxMemPool::accept() : not enough fees %s, %" PRI64d" < %" PRI64d,
                         hash.ToString().c_str(),
                         nFees, txMinFee);

        // Continuously rate-limit free transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make others' transactions take longer to confirm.
        if (fLimitFree && nFees < CTransaction::nMinRelayTxFee)
        {
            static double dFreeCount;
            static int64 nLastTime;
            int64 nNow = GetTime();

            LOCK(cs);

            // Use an exponentially decaying ~10-minute window:
            dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
            nLastTime = nNow;
            // -limitfreerelay unit is thousand-bytes-per-minute
            // At default rate it would take over a month to fill 1GB
            if (dFreeCount >= GetArg("-limitfreerelay", 15)*10*1000)
                return error("CTxMemPool::accept() : free transaction rejected by rate limiter");
            if (fDebug)
                printf("Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
            dFreeCount += nSize;
        }

        if (fRejectInsaneFee && nFees > CTransaction::nMinRelayTxFee * 1000)
            return error("CTxMemPool::accept() : insane fees %s, %" PRI64d" > %" PRI64d,
                         hash.ToString().c_str(),
                         nFees, CTransaction::nMinRelayTxFee * 1000);

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!tx.CheckInputs(state, view, true, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC))
        {
            return error("CTxMemPool::accept() : ConnectInputs failed %s", hash.ToString().c_str());
        }
    }

    // Store transaction in memory
    {
        LOCK(cs);
        if (ptxOld)
        {
            printf("CTxMemPool::accept() : replacing tx %s with new version\n", ptxOld->GetHash().ToString().c_str());
            remove(*ptxOld);
        }
        if(tx.IsZerocoinSpend()){
            countZCSpend++;
        }
        addUnchecked(hash, tx);
    }

    ///// are we sure this is ok when loading transactions or restoring block txes
    // If updated, erase old tx from wallet
    if (ptxOld)
        EraseFromWallets(ptxOld->GetHash());
    SyncWithWallets(hash, tx, NULL, true);

    return true;
}

bool CTransaction::AcceptToMemoryPool(CValidationState &state, bool fCheckInputs, bool fLimitFree, bool* pfMissingInputs, bool fRejectInsaneFee)
{
    try {
        return mempool.accept(state, *this, fCheckInputs, fLimitFree, pfMissingInputs, fRejectInsaneFee);
    } catch(std::runtime_error &e) {
        return state.Abort(_("System error: ") + e.what());
    }
}

bool CTxMemPool::addUnchecked(const uint256& hash, const CTransaction &tx)
{

    // Add to memory pool without checking anything.  Don't call this directly,
    // call CTxMemPool::accept to properly check the transaction first.
    {
        mapTx[hash] = tx;
        if(!tx.IsZerocoinSpend()){
            for (unsigned int i = 0; i < tx.vin.size(); i++)
                mapNextTx[tx.vin[i].prevout] = CInPoint(&mapTx[hash], i);
        }
        nTransactionsUpdated++;
    }
    return true;
}


bool CTxMemPool::remove(const CTransaction &tx, bool fRecursive)
{
    // Remove transaction from memory pool
    {
        LOCK(cs);
        uint256 hash = tx.GetHash();
        if (fRecursive) {
            for (unsigned int i = 0; i < tx.vout.size(); i++) {
                std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(COutPoint(hash, i));
                if (it != mapNextTx.end())
                    remove(*it->second.ptx, true);
            }
        }
        if (mapTx.count(hash))
        {
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
                mapNextTx.erase(txin.prevout);
            mapTx.erase(hash);
            nTransactionsUpdated++;
        }
    }
    return true;
}

bool CTxMemPool::removeConflicts(const CTransaction &tx)
{
    // Remove transactions which depend on inputs of tx, recursively
    LOCK(cs);
    BOOST_FOREACH(const CTxIn &txin, tx.vin) {
        std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(txin.prevout);
        if (it != mapNextTx.end()) {
            const CTransaction &txConflict = *it->second.ptx;
            if (txConflict != tx)
                remove(txConflict, true);
        }
    }
    return true;
}

void CTxMemPool::clear()
{
    LOCK(cs);
    mapTx.clear();
    mapNextTx.clear();
    ++nTransactionsUpdated;
}

void CTxMemPool::queryHashes(std::vector<uint256>& vtxid)
{
    vtxid.clear();

    LOCK(cs);
    vtxid.reserve(mapTx.size());
    for (map<uint256, CTransaction>::iterator mi = mapTx.begin(); mi != mapTx.end(); ++mi)
        vtxid.push_back((*mi).first);
}




int CMerkleTx::GetDepthInMainChainINTERNAL(CBlockIndex* &pindexRet) const
{
    if (hashBlock == 0 || nIndex == -1)
        return 0;

    // Find the block it claims to be in
    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    // Make sure the merkle branch connects to this block
    if (!fMerkleVerified)
    {
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            return 0;
        fMerkleVerified = true;
    }

    pindexRet = pindex;
    return pindexBest->nHeight - pindex->nHeight + 1;
}

int CMerkleTx::GetDepthInMainChain(CBlockIndex* &pindexRet) const
{
    int nResult = GetDepthInMainChainINTERNAL(pindexRet);
    if (nResult == 0 && !mempool.exists(GetHash()))
        return -1; // Not in chain, not in mempool

    return nResult;
}

int CMerkleTx::GetBlocksToMaturity() const
{
    if (!IsCoinBase())
        return 0;
    return max(0, (COINBASE_MATURITY+20) - GetDepthInMainChain());
}


bool CMerkleTx::AcceptToMemoryPool(bool fCheckInputs, bool fLimitFree)
{
    CValidationState state;
    return CTransaction::AcceptToMemoryPool(state, fCheckInputs, fLimitFree);
}



bool CWalletTx::AcceptWalletTransaction(bool fCheckInputs)
{
    {
        LOCK(mempool.cs);
        // Add previous supporting transactions first
        BOOST_FOREACH(CMerkleTx& tx, vtxPrev)
        {
            if (!tx.IsCoinBase() && !tx.IsZerocoinSpend())
            {
                uint256 hash = tx.GetHash();
                if (!mempool.exists(hash) && pcoinsTip->HaveCoins(hash))
                    tx.AcceptToMemoryPool(fCheckInputs, false);
            }
        }
        return AcceptToMemoryPool(fCheckInputs, false);
    }
    return false;
}


// Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool GetTransaction(const uint256 &hash, CTransaction &txOut, uint256 &hashBlock, bool fAllowSlow)
{
    CBlockIndex *pindexSlow = NULL;
    {
        LOCK(cs_main);
        {
            LOCK(mempool.cs);
            if (mempool.exists(hash))
            {
                txOut = mempool.lookup(hash);
                return true;
            }
        }

        if (fTxIndex) {
            CDiskTxPos postx;
            if (pblocktree->ReadTxIndex(hash, postx)) {
                CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
                CBlockHeader header;
                try {
                    file >> header;
                    fseek(file, postx.nTxOffset, SEEK_CUR);
                    file >> txOut;
                } catch (std::exception &e) {
                    return error("%s() : deserialize or I/O error", __PRETTY_FUNCTION__);
                }
                hashBlock = header.GetHash();
                if (txOut.GetHash() != hash)
                    return error("%s() : txid mismatch", __PRETTY_FUNCTION__);
                return true;
            }
        }

        if (fAllowSlow) { // use coin database to locate block that contains transaction, and scan it
            int nHeight = -1;
            {
                CCoinsViewCache &view = *pcoinsTip;
                CCoins coins;
                if (view.GetCoins(hash, coins))
                    nHeight = coins.nHeight;
            }
            if (nHeight > 0)
                pindexSlow = FindBlockByHeight(nHeight);
        }
    }

    if (pindexSlow) {
        CBlock block;
        if (block.ReadFromDisk(pindexSlow)) {
            BOOST_FOREACH(const CTransaction &tx, block.vtx) {
                if (tx.GetHash() == hash) {
                    txOut = tx;
                    hashBlock = pindexSlow->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}






//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

static CBlockIndex* pblockindexFBBHLast;
CBlockIndex* FindBlockByHeight(int nHeight)
{
    CBlockIndex *pblockindex;
    if (nHeight < nBestHeight / 2)
        pblockindex = pindexGenesisBlock;
    else
        pblockindex = pindexBest;
    if (pblockindexFBBHLast && abs(nHeight - pblockindex->nHeight) > abs(nHeight - pblockindexFBBHLast->nHeight))
        pblockindex = pblockindexFBBHLast;
    while (pblockindex->nHeight > nHeight)
        pblockindex = pblockindex->pprev;
    while (pblockindex->nHeight < nHeight)
        pblockindex = pblockindex->pnext;
    pblockindexFBBHLast = pblockindex;
    return pblockindex;
}

bool CBlock::ReadFromDisk(const CBlockIndex* pindex)
{
//    LastHeight = pindex->nHeight - 1;
    if (!ReadFromDisk(pindex->GetBlockPos()))
        return false;
    if (GetHash() != pindex->GetBlockHash())
        return error("CBlock::ReadFromDisk() : GetHash() doesn't match index");
    return true;
}

uint256 static GetOrphanRoot(const CBlockHeader* pblock)
{
    // Work back to the first block in the orphan chain
    while (mapOrphanBlocks.count(pblock->hashPrevBlock))
        pblock = mapOrphanBlocks[pblock->hashPrevBlock];
    return pblock->GetHash();
}

int64 static GetBlockValue(int nHeight, int64 nFees, unsigned int nTime)
{
    // 0 rewards prior to start time and on genesis block.
    if ((nTime < nStartRewardTime && !fTestNet) || nHeight ==0)
        return 0 * COIN;
    // Maximum block reward is 5000 coins
    if (nHeight > 0 && nHeight <= 143499)
	return 5000 * COIN + nFees;
    // Block rewards taper off after block 143500
    if (nHeight > 143499 && nHeight <= 717499999)
	return floor(0.5+((5000 * 143500)/(nHeight +1))/100)*100 * COIN + nFees;
    // Stop rewards when blocks size is less than 1.
    if (nHeight > 717499999)
        return nFees;
}
static const int64 nTargetTimespan = 2 * 55; //1 minute 50 seconds between retargets
static const int64 nTargetSpacing = 55; // 55 second blocks
static const int64 nInterval = nTargetTimespan / nTargetSpacing; // retargets every 2 blocks

//
// minimum amount of work that could possibly be required nTime after
// minimum work required was nBase
//

unsigned int ComputeMinWork(unsigned int nBase, int64 nTime)
{
    // Testnet has min-difficulty blocks
    // after nTargetSpacing*6 time between blocks:
    if (fTestNet && nTime > nTargetSpacing*2)
        return bnProofOfWorkLimit.GetCompact();

    CBigNum bnResult;
    bnResult.SetCompact(nBase);
    while (nTime > 0 && bnResult < bnProofOfWorkLimit)
    {
        // Maximum 400% adjustment...
        bnResult *= 4;
        // ... in best-case exactly 4-times-normal target time
        nTime -= nTargetTimespan;
    }
    if (bnResult > bnProofOfWorkLimit)
        bnResult = bnProofOfWorkLimit;

    return bnResult.GetCompact();
}


unsigned int static BorisRidiculouslyNamedDifficultyFunction(const CBlockIndex* pindexLast, uint32_t TargetBlocksSpacingSeconds, uint32_t PastBlocksMin, uint32_t PastBlocksMax) {
 
        const CBlockIndex *BlockLastSolved = pindexLast;
        const CBlockIndex *BlockReading    = pindexLast;

        typedef Fixed<32, 32> fixed;
        
        uint32_t     nPastBlocks               = 0;
        int32_t      nActualSeconds            = 0;
        int32_t      nTargetSeconds    	       = 0;
        fixed        nBlockTimeRatio	       = 1;
        CBigNum      bnPastTargetAverage;
        CBigNum      bnPastTargetAveragePrev;
        
        float FastBlocksLimit[5040] = { 317.772675, 136.233047, 83.194504, 58.732189, 44.894745, 36.089561, 30.038040, 25.646383, 22.327398, 19.739056, 17.669304, 15.980045, 14.577670, 13.396597, 12.389578, 11.521759, 10.766892, 10.104856, 9.519974, 8.999868, 8.534638, 8.116274, 7.738231, 7.395114, 7.082433, 6.796428, 6.533921, 6.292217, 6.069008, 5.862312, 5.670416, 5.491832, 5.325264, 5.169573, 5.023761, 4.886943, 4.758339, 4.637252, 4.523063, 4.415215, 4.313211, 4.216604, 4.124989, 4.038000, 3.955308, 3.876611, 3.801635, 3.730131, 3.661870, 3.596645, 3.534262, 3.474546, 3.417336, 3.362480, 3.309842, 3.259294, 3.210718, 3.164004, 3.119051, 3.075764, 3.034055, 2.993842, 2.955048, 2.917603, 2.881439, 2.846493, 2.812708, 2.780028, 2.748401, 2.717779, 2.688117, 2.659371, 2.631500, 2.604468, 2.578238, 2.552775, 2.528048, 2.504027, 2.480681, 2.457985, 2.435913, 2.414439, 2.393541, 2.373197, 2.353385, 2.334085, 2.315279, 2.296949, 2.279077, 2.261647, 2.244643, 2.228050, 2.211855, 2.196043, 2.180601, 2.165517, 2.150779, 2.136376, 2.122297, 2.108531, 2.095068, 2.081899, 2.069015, 2.056406, 2.044064, 2.031982, 2.020151, 2.008564, 1.997213, 1.986092, 1.975194, 1.964513, 1.954042, 1.943775, 1.933707, 1.923833, 1.914146, 1.904642, 1.895315, 1.886162, 1.877177, 1.868356, 1.859695, 1.851189, 1.842834, 1.834627, 1.826564, 1.818642, 1.810856, 1.803203, 1.795680, 1.788284, 1.781012, 1.773861, 1.766828, 1.759910, 1.753104, 1.746408, 1.739819, 1.733335, 1.726954, 1.720672, 1.714488, 1.708400, 1.702405, 1.696502, 1.690688, 1.684962, 1.679321, 1.673764, 1.668288, 1.662894, 1.657577, 1.652337, 1.647173, 1.642082, 1.637064, 1.632116, 1.627238, 1.622427, 1.617683, 1.613004, 1.608389, 1.603837, 1.599346, 1.594915, 1.590544, 1.586230, 1.581973, 1.577772, 1.573626, 1.569533, 1.565493, 1.561504, 1.557567, 1.553679, 1.549840, 1.546049, 1.542306, 1.538608, 1.534956, 1.531349, 1.527786, 1.524266, 1.520788, 1.517352, 1.513956, 1.510601, 1.507286, 1.504009, 1.500770, 1.497570, 1.494406, 1.491278, 1.488186, 1.485129, 1.482107, 1.479118, 1.476164, 1.473242, 1.470352, 1.467494, 1.464668, 1.461872, 1.459107, 1.456372, 1.453666, 1.450989, 1.448341, 1.445720, 1.443128, 1.440562, 1.438024, 1.435511, 1.433025, 1.430565, 1.428129, 1.425719, 1.423333, 1.420971, 1.418633, 1.416319, 1.414028, 1.411759, 1.409513, 1.407289, 1.405087, 1.402906, 1.400746, 1.398608, 1.396490, 1.394392, 1.392315, 1.390257, 1.388219, 1.386200, 1.384199, 1.382218, 1.380255, 1.378310, 1.376384, 1.374475, 1.372583, 1.370709, 1.368852, 1.367011, 1.365187, 1.363380, 1.361589, 1.359813, 1.358054, 1.356310, 1.354581, 1.352868, 1.351169, 1.349485, 1.347816, 1.346161, 1.344521, 1.342894, 1.341282, 1.339683, 1.338098, 1.336526, 1.334967, 1.333421, 1.331888, 1.330368, 1.328861, 1.327366, 1.325883, 1.324412, 1.322953, 1.321507, 1.320072, 1.318648, 1.317236, 1.315835, 1.314446, 1.313067, 1.311700, 1.310343, 1.308997, 1.307661, 1.306336, 1.305021, 1.303717, 1.302422, 1.301138, 1.299863, 1.298598, 1.297343, 1.296097, 1.294861, 1.293634, 1.292416, 1.291208, 1.290008, 1.288818, 1.287636, 1.286463, 1.285298, 1.284142, 1.282995, 1.281856, 1.280725, 1.279603, 1.278489, 1.277382, 1.276284, 1.275193, 1.274111, 1.273036, 1.271968, 1.270908, 1.269856, 1.268811, 1.267773, 1.266743, 1.265720, 1.264703, 1.263694, 1.262692, 1.261697, 1.260708, 1.259727, 1.258752, 1.257783, 1.256821, 1.255866, 1.254917, 1.253975, 1.253038, 1.252108, 1.251184, 1.250267, 1.249355, 1.248449, 1.247550, 1.246656, 1.245768, 1.244886, 1.244009, 1.243139, 1.242273, 1.241414, 1.240560, 1.239711, 1.238868, 1.238030, 1.237198, 1.236371, 1.235548, 1.234732, 1.233920, 1.233113, 1.232312, 1.231515, 1.230723, 1.229937, 1.229155, 1.228378, 1.227605, 1.226838, 1.226075, 1.225316, 1.224563, 1.223814, 1.223069, 1.222329, 1.221593, 1.220862, 1.220135, 1.219412, 1.218694, 1.217980, 1.217270, 1.216565, 1.215863, 1.215166, 1.214473, 1.213783, 1.213098, 1.212417, 1.211740, 1.211066, 1.210397, 1.209731, 1.209069, 1.208411, 1.207757, 1.207106, 1.206459, 1.205816, 1.205176, 1.204540, 1.203907, 1.203278, 1.202653, 1.202031, 1.201412, 1.200797, 1.200186, 1.199577, 1.198972, 1.198371, 1.197772, 1.197177, 1.196586, 1.195997, 1.195411, 1.194829, 1.194250, 1.193674, 1.193101, 1.192531, 1.191964, 1.191401, 1.190840, 1.190282, 1.189727, 1.189175, 1.188626, 1.188080, 1.187537, 1.186996, 1.186459, 1.185924, 1.185392, 1.184862, 1.184336, 1.183812, 1.183291, 1.182772, 1.182256, 1.181743, 1.181233, 1.180725, 1.180219, 1.179716, 1.179216, 1.178718, 1.178223, 1.177730, 1.177240, 1.176752, 1.176267, 1.175784, 1.175303, 1.174825, 1.174349, 1.173875, 1.173404, 1.172935, 1.172468, 1.172004, 1.171542, 1.171082, 1.170625, 1.170169, 1.169716, 1.169265, 1.168816, 1.168370, 1.167925, 1.167483, 1.167042, 1.166604, 1.166168, 1.165734, 1.165302, 1.164872, 1.164444, 1.164018, 1.163594, 1.163172, 1.162752, 1.162334, 1.161918, 1.161504, 1.161092, 1.160681, 1.160273, 1.159867, 1.159462, 1.159059, 1.158658, 1.158259, 1.157862, 1.157466, 1.157072, 1.156680, 1.156290, 1.155902, 1.155515, 1.155130, 1.154747, 1.154365, 1.153986, 1.153607, 1.153231, 1.152856, 1.152483, 1.152112, 1.151742, 1.151374, 1.151007, 1.150642, 1.150279, 1.149917, 1.149557, 1.149198, 1.148841, 1.148486, 1.148132, 1.147779, 1.147428, 1.147079, 1.146731, 1.146385, 1.146040, 1.145696, 1.145354, 1.145014, 1.144675, 1.144337, 1.144001, 1.143666, 1.143332, 1.143000, 1.142670, 1.142340, 1.142012, 1.141686, 1.141361, 1.141037, 1.140715, 1.140393, 1.140074, 1.139755, 1.139438, 1.139122, 1.138808, 1.138494, 1.138182, 1.137872, 1.137562, 1.137254, 1.136947, 1.136641, 1.136337, 1.136034, 1.135732, 1.135431, 1.135131, 1.134833, 1.134536, 1.134240, 1.133945, 1.133651, 1.133359, 1.133067, 1.132777, 1.132488, 1.132200, 1.131913, 1.131628, 1.131343, 1.131060, 1.130778, 1.130496, 1.130216, 1.129937, 1.129659, 1.129382, 1.129107, 1.128832, 1.128558, 1.128286, 1.128014, 1.127744, 1.127474, 1.127206, 1.126938, 1.126672, 1.126407, 1.126142, 1.125879, 1.125616, 1.125355, 1.125095, 1.124835, 1.124577, 1.124319, 1.124063, 1.123807, 1.123552, 1.123299, 1.123046, 1.122794, 1.122543, 1.122293, 1.122044, 1.121796, 1.121549, 1.121303, 1.121058, 1.120813, 1.120570, 1.120327, 1.120085, 1.119844, 1.119604, 1.119365, 1.119127, 1.118889, 1.118653, 1.118417, 1.118182, 1.117948, 1.117715, 1.117482, 1.117251, 1.117020, 1.116790, 1.116561, 1.116333, 1.116106, 1.115879, 1.115653, 1.115428, 1.115204, 1.114980, 1.114758, 1.114536, 1.114315, 1.114094, 1.113875, 1.113656, 1.113438, 1.113221, 1.113004, 1.112788, 1.112573, 1.112359, 1.112146, 1.111933, 1.111721, 1.111509, 1.111299, 1.111089, 1.110880, 1.110671, 1.110463, 1.110256, 1.110050, 1.109844, 1.109639, 1.109435, 1.109231, 1.109029, 1.108826, 1.108625, 1.108424, 1.108224, 1.108024, 1.107825, 1.107627, 1.107430, 1.107233, 1.107037, 1.106841, 1.106646, 1.106452, 1.106258, 1.106065, 1.105873, 1.105681, 1.105490, 1.105300, 1.105110, 1.104921, 1.104732, 1.104544, 1.104357, 1.104170, 1.103984, 1.103798, 1.103613, 1.103429, 1.103245, 1.103062, 1.102879, 1.102697, 1.102516, 1.102335, 1.102155, 1.101975, 1.101796, 1.101617, 1.101439, 1.101262, 1.101085, 1.100909, 1.100733, 1.100558, 1.100383, 1.100209, 1.100036, 1.099862, 1.099690, 1.099518, 1.099347, 1.099176, 1.099006, 1.098836, 1.098667, 1.098498, 1.098330, 1.098162, 1.097995, 1.097828, 1.097662, 1.097496, 1.097331, 1.097167, 1.097003, 1.096839, 1.096676, 1.096513, 1.096351, 1.096190, 1.096028, 1.095868, 1.095708, 1.095548, 1.095389, 1.095230, 1.095072, 1.094914, 1.094757, 1.094600, 1.094444, 1.094288, 1.094133, 1.093978, 1.093823, 1.093669, 1.093516, 1.093362, 1.093210, 1.093058, 1.092906, 1.092755, 1.092604, 1.092453, 1.092303, 1.092154, 1.092005, 1.091856, 1.091708, 1.091560, 1.091413, 1.091266, 1.091120, 1.090973, 1.090828, 1.090683, 1.090538, 1.090393, 1.090250, 1.090106, 1.089963, 1.089820, 1.089678, 1.089536, 1.089394, 1.089253, 1.089113, 1.088972, 1.088833, 1.088693, 1.088554, 1.088415, 1.088277, 1.088139, 1.088002, 1.087865, 1.087728, 1.087591, 1.087456, 1.087320, 1.087185, 1.087050, 1.086915, 1.086781, 1.086648, 1.086514, 1.086381, 1.086249, 1.086117, 1.085985, 1.085853, 1.085722, 1.085591, 1.085461, 1.085331, 1.085201, 1.085072, 1.084943, 1.084815, 1.084686, 1.084558, 1.084431, 1.084304, 1.084177, 1.084050, 1.083924, 1.083798, 1.083673, 1.083548, 1.083423, 1.083298, 1.083174, 1.083050, 1.082927, 1.082804, 1.082681, 1.082559, 1.082437, 1.082315, 1.082193, 1.082072, 1.081951, 1.081831, 1.081711, 1.081591, 1.081471, 1.081352, 1.081233, 1.081114, 1.080996, 1.080878, 1.080760, 1.080643, 1.080526, 1.080409, 1.080293, 1.080177, 1.080061, 1.079946, 1.079830, 1.079715, 1.079601, 1.079486, 1.079373, 1.079259, 1.079145, 1.079032, 1.078919, 1.078807, 1.078695, 1.078583, 1.078471, 1.078360, 1.078249, 1.078138, 1.078027, 1.077917, 1.077807, 1.077697, 1.077588, 1.077479, 1.077370, 1.077261, 1.077153, 1.077045, 1.076937, 1.076830, 1.076723, 1.076616, 1.076509, 1.076403, 1.076296, 1.076191, 1.076085, 1.075980, 1.075875, 1.075770, 1.075665, 1.075561, 1.075457, 1.075353, 1.075250, 1.075147, 1.075044, 1.074941, 1.074838, 1.074736, 1.074634, 1.074533, 1.074431, 1.074330, 1.074229, 1.074128, 1.074028, 1.073928, 1.073828, 1.073728, 1.073628, 1.073529, 1.073430, 1.073331, 1.073233, 1.073135, 1.073037, 1.072939, 1.072841, 1.072744, 1.072647, 1.072550, 1.072453, 1.072357, 1.072261, 1.072165, 1.072069, 1.071974, 1.071878, 1.071783, 1.071689, 1.071594, 1.071500, 1.071406, 1.071312, 1.071218, 1.071125, 1.071032, 1.070939, 1.070846, 1.070753, 1.070661, 1.070569, 1.070477, 1.070385, 1.070294, 1.070203, 1.070112, 1.070021, 1.069930, 1.069840, 1.069750, 1.069660, 1.069570, 1.069480, 1.069391, 1.069302, 1.069213, 1.069124, 1.069036, 1.068948, 1.068859, 1.068771, 1.068684, 1.068596, 1.068509, 1.068422, 1.068335, 1.068248, 1.068162, 1.068076, 1.067990, 1.067904, 1.067818, 1.067733, 1.067647, 1.067562, 1.067477, 1.067392, 1.067308, 1.067224, 1.067140, 1.067056, 1.066972, 1.066888, 1.066805, 1.066722, 1.066639, 1.066556, 1.066473, 1.066391, 1.066308, 1.066226, 1.066145, 1.066063, 1.065981, 1.065900, 1.065819, 1.065738, 1.065657, 1.065576, 1.065496, 1.065416, 1.065336, 1.065256, 1.065176, 1.065096, 1.065017, 1.064938, 1.064859, 1.064780, 1.064701, 1.064623, 1.064545, 1.064466, 1.064388, 1.064311, 1.064233, 1.064155, 1.064078, 1.064001, 1.063924, 1.063847, 1.063771, 1.063694, 1.063618, 1.063542, 1.063466, 1.063390, 1.063314, 1.063239, 1.063164, 1.063088, 1.063013, 1.062939, 1.062864, 1.062789, 1.062715, 1.062641, 1.062567, 1.062493, 1.062419, 1.062346, 1.062272, 1.062199, 1.062126, 1.062053, 1.061980, 1.061908, 1.061835, 1.061763, 1.061691, 1.061619, 1.061547, 1.061475, 1.061404, 1.061332, 1.061261, 1.061190, 1.061119, 1.061048, 1.060977, 1.060907, 1.060837, 1.060766, 1.060696, 1.060626, 1.060557, 1.060487, 1.060418, 1.060348, 1.060279, 1.060210, 1.060141, 1.060072, 1.060004, 1.059935, 1.059867, 1.059799, 1.059731, 1.059663, 1.059595, 1.059527, 1.059460, 1.059393, 1.059325, 1.059258, 1.059191, 1.059124, 1.059058, 1.058991, 1.058925, 1.058859, 1.058792, 1.058727, 1.058661, 1.058595, 1.058529, 1.058464, 1.058399, 1.058333, 1.058268, 1.058203, 1.058139, 1.058074, 1.058010, 1.057945, 1.057881, 1.057817, 1.057753, 1.057689, 1.057625, 1.057561, 1.057498, 1.057434, 1.057371, 1.057308, 1.057245, 1.057182, 1.057119, 1.057057, 1.056994, 1.056932, 1.056870, 1.056808, 1.056746, 1.056684, 1.056622, 1.056560, 1.056499, 1.056437, 1.056376, 1.056315, 1.056254, 1.056193, 1.056132, 1.056072, 1.056011, 1.055951, 1.055890, 1.055830, 1.055770, 1.055710, 1.055650, 1.055591, 1.055531, 1.055471, 1.055412, 1.055353, 1.055294, 1.055235, 1.055176, 1.055117, 1.055058, 1.054999, 1.054941, 1.054883, 1.054824, 1.054766, 1.054708, 1.054650, 1.054593, 1.054535, 1.054477, 1.054420, 1.054363, 1.054305, 1.054248, 1.054191, 1.054134, 1.054077, 1.054021, 1.053964, 1.053908, 1.053851, 1.053795, 1.053739, 1.053683, 1.053627, 1.053571, 1.053515, 1.053460, 1.053404, 1.053349, 1.053293, 1.053238, 1.053183, 1.053128, 1.053073, 1.053018, 1.052963, 1.052909, 1.052854, 1.052800, 1.052746, 1.052691, 1.052637, 1.052583, 1.052529, 1.052476, 1.052422, 1.052368, 1.052315, 1.052261, 1.052208, 1.052155, 1.052102, 1.052049, 1.051996, 1.051943, 1.051890, 1.051838, 1.051785, 1.051733, 1.051681, 1.051628, 1.051576, 1.051524, 1.051472, 1.051420, 1.051369, 1.051317, 1.051265, 1.051214, 1.051163, 1.051111, 1.051060, 1.051009, 1.050958, 1.050907, 1.050856, 1.050805, 1.050755, 1.050704, 1.050654, 1.050604, 1.050553, 1.050503, 1.050453, 1.050403, 1.050353, 1.050303, 1.050254, 1.050204, 1.050154, 1.050105, 1.050056, 1.050006, 1.049957, 1.049908, 1.049859, 1.049810, 1.049761, 1.049712, 1.049664, 1.049615, 1.049567, 1.049518, 1.049470, 1.049422, 1.049373, 1.049325, 1.049277, 1.049229, 1.049182, 1.049134, 1.049086, 1.049039, 1.048991, 1.048944, 1.048896, 1.048849, 1.048802, 1.048755, 1.048708, 1.048661, 1.048614, 1.048567, 1.048521, 1.048474, 1.048428, 1.048381, 1.048335, 1.048289, 1.048242, 1.048196, 1.048150, 1.048104, 1.048059, 1.048013, 1.047967, 1.047922, 1.047876, 1.047830, 1.047785, 1.047740, 1.047695, 1.047649, 1.047604, 1.047559, 1.047514, 1.047469, 1.047425, 1.047380, 1.047336, 1.047291, 1.047246, 1.047202, 1.047158, 1.047114, 1.047069, 1.047025, 1.046981, 1.046937, 1.046893, 1.046850, 1.046806, 1.046762, 1.046719, 1.046675, 1.046632, 1.046589, 1.046545, 1.046502, 1.046459, 1.046416, 1.046373, 1.046330, 1.046287, 1.046244, 1.046202, 1.046159, 1.046116, 1.046074, 1.046032, 1.045989, 1.045947, 1.045905, 1.045863, 1.045821, 1.045779, 1.045737, 1.045695, 1.045653, 1.045611, 1.045570, 1.045528, 1.045487, 1.045445, 1.045404, 1.045363, 1.045321, 1.045280, 1.045239, 1.045198, 1.045157, 1.045116, 1.045075, 1.045035, 1.044994, 1.044953, 1.044913, 1.044872, 1.044832, 1.044791, 1.044751, 1.044711, 1.044671, 1.044631, 1.044591, 1.044551, 1.044511, 1.044471, 1.044431, 1.044392, 1.044352, 1.044312, 1.044273, 1.044233, 1.044194, 1.044155, 1.044115, 1.044076, 1.044037, 1.043998, 1.043959, 1.043920, 1.043881, 1.043842, 1.043804, 1.043765, 1.043726, 1.043688, 1.043649, 1.043611, 1.043572, 1.043534, 1.043496, 1.043458, 1.043419, 1.043381, 1.043343, 1.043305, 1.043267, 1.043230, 1.043192, 1.043154, 1.043116, 1.043079, 1.043041, 1.043004, 1.042966, 1.042929, 1.042892, 1.042854, 1.042817, 1.042780, 1.042743, 1.042706, 1.042669, 1.042632, 1.042595, 1.042559, 1.042522, 1.042485, 1.042449, 1.042412, 1.042376, 1.042339, 1.042303, 1.042266, 1.042230, 1.042194, 1.042158, 1.042122, 1.042086, 1.042050, 1.042014, 1.041978, 1.041942, 1.041906, 1.041870, 1.041835, 1.041799, 1.041764, 1.041728, 1.041693, 1.041657, 1.041622, 1.041587, 1.041552, 1.041516, 1.041481, 1.041446, 1.041411, 1.041376, 1.041341, 1.041307, 1.041272, 1.041237, 1.041202, 1.041168, 1.041133, 1.041099, 1.041064, 1.041030, 1.040995, 1.040961, 1.040927, 1.040893, 1.040859, 1.040824, 1.040790, 1.040756, 1.040722, 1.040688, 1.040655, 1.040621, 1.040587, 1.040553, 1.040520, 1.040486, 1.040453, 1.040419, 1.040386, 1.040352, 1.040319, 1.040286, 1.040252, 1.040219, 1.040186, 1.040153, 1.040120, 1.040087, 1.040054, 1.040021, 1.039988, 1.039955, 1.039923, 1.039890, 1.039857, 1.039825, 1.039792, 1.039760, 1.039727, 1.039695, 1.039662, 1.039630, 1.039598, 1.039566, 1.039533, 1.039501, 1.039469, 1.039437, 1.039405, 1.039373, 1.039341, 1.039310, 1.039278, 1.039246, 1.039214, 1.039183, 1.039151, 1.039119, 1.039088, 1.039056, 1.039025, 1.038994, 1.038962, 1.038931, 1.038900, 1.038869, 1.038837, 1.038806, 1.038775, 1.038744, 1.038713, 1.038682, 1.038651, 1.038621, 1.038590, 1.038559, 1.038528, 1.038498, 1.038467, 1.038436, 1.038406, 1.038375, 1.038345, 1.038315, 1.038284, 1.038254, 1.038224, 1.038193, 1.038163, 1.038133, 1.038103, 1.038073, 1.038043, 1.038013, 1.037983, 1.037953, 1.037923, 1.037894, 1.037864, 1.037834, 1.037804, 1.037775, 1.037745, 1.037716, 1.037686, 1.037657, 1.037627, 1.037598, 1.037569, 1.037539, 1.037510, 1.037481, 1.037452, 1.037423, 1.037394, 1.037364, 1.037336, 1.037307, 1.037278, 1.037249, 1.037220, 1.037191, 1.037162, 1.037134, 1.037105, 1.037076, 1.037048, 1.037019, 1.036991, 1.036962, 1.036934, 1.036905, 1.036877, 1.036849, 1.036820, 1.036792, 1.036764, 1.036736, 1.036708, 1.036680, 1.036651, 1.036623, 1.036596, 1.036568, 1.036540, 1.036512, 1.036484, 1.036456, 1.036429, 1.036401, 1.036373, 1.036346, 1.036318, 1.036291, 1.036263, 1.036235, 1.036208, 1.036181, 1.036153, 1.036126, 1.036099, 1.036071, 1.036044, 1.036017, 1.035990, 1.035963, 1.035936, 1.035909, 1.035882, 1.035855, 1.035828, 1.035801, 1.035774, 1.035747, 1.035721, 1.035694, 1.035667, 1.035641, 1.035614, 1.035587, 1.035561, 1.035534, 1.035508, 1.035481, 1.035455, 1.035429, 1.035402, 1.035376, 1.035350, 1.035324, 1.035297, 1.035271, 1.035245, 1.035219, 1.035193, 1.035167, 1.035141, 1.035115, 1.035089, 1.035063, 1.035037, 1.035012, 1.034986, 1.034960, 1.034934, 1.034909, 1.034883, 1.034857, 1.034832, 1.034806, 1.034781, 1.034755, 1.034730, 1.034704, 1.034679, 1.034654, 1.034629, 1.034603, 1.034578, 1.034553, 1.034528, 1.034503, 1.034477, 1.034452, 1.034427, 1.034402, 1.034377, 1.034352, 1.034328, 1.034303, 1.034278, 1.034253, 1.034228, 1.034204, 1.034179, 1.034154, 1.034130, 1.034105, 1.034080, 1.034056, 1.034031, 1.034007, 1.033982, 1.033958, 1.033934, 1.033909, 1.033885, 1.033861, 1.033836, 1.033812, 1.033788, 1.033764, 1.033740, 1.033715, 1.033691, 1.033667, 1.033643, 1.033619, 1.033595, 1.033571, 1.033548, 1.033524, 1.033500, 1.033476, 1.033452, 1.033429, 1.033405, 1.033381, 1.033358, 1.033334, 1.033310, 1.033287, 1.033263, 1.033240, 1.033216, 1.033193, 1.033170, 1.033146, 1.033123, 1.033100, 1.033076, 1.033053, 1.033030, 1.033007, 1.032984, 1.032960, 1.032937, 1.032914, 1.032891, 1.032868, 1.032845, 1.032822, 1.032799, 1.032776, 1.032754, 1.032731, 1.032708, 1.032685, 1.032662, 1.032640, 1.032617, 1.032594, 1.032572, 1.032549, 1.032526, 1.032504, 1.032481, 1.032459, 1.032436, 1.032414, 1.032392, 1.032369, 1.032347, 1.032324, 1.032302, 1.032280, 1.032258, 1.032236, 1.032213, 1.032191, 1.032169, 1.032147, 1.032125, 1.032103, 1.032081, 1.032059, 1.032037, 1.032015, 1.031993, 1.031971, 1.031949, 1.031927, 1.031906, 1.031884, 1.031862, 1.031840, 1.031819, 1.031797, 1.031775, 1.031754, 1.031732, 1.031711, 1.031689, 1.031667, 1.031646, 1.031625, 1.031603, 1.031582, 1.031560, 1.031539, 1.031518, 1.031496, 1.031475, 1.031454, 1.031433, 1.031411, 1.031390, 1.031369, 1.031348, 1.031327, 1.031306, 1.031285, 1.031264, 1.031243, 1.031222, 1.031201, 1.031180, 1.031159, 1.031138, 1.031117, 1.031097, 1.031076, 1.031055, 1.031034, 1.031013, 1.030993, 1.030972, 1.030952, 1.030931, 1.030910, 1.030890, 1.030869, 1.030849, 1.030828, 1.030808, 1.030787, 1.030767, 1.030747, 1.030726, 1.030706, 1.030686, 1.030665, 1.030645, 1.030625, 1.030604, 1.030584, 1.030564, 1.030544, 1.030524, 1.030504, 1.030484, 1.030464, 1.030444, 1.030424, 1.030404, 1.030384, 1.030364, 1.030344, 1.030324, 1.030304, 1.030284, 1.030264, 1.030245, 1.030225, 1.030205, 1.030185, 1.030166, 1.030146, 1.030126, 1.030107, 1.030087, 1.030068, 1.030048, 1.030029, 1.030009, 1.029990, 1.029970, 1.029951, 1.029931, 1.029912, 1.029892, 1.029873, 1.029854, 1.029835, 1.029815, 1.029796, 1.029777, 1.029757, 1.029738, 1.029719, 1.029700, 1.029681, 1.029662, 1.029643, 1.029624, 1.029605, 1.029586, 1.029567, 1.029548, 1.029529, 1.029510, 1.029491, 1.029472, 1.029453, 1.029434, 1.029415, 1.029397, 1.029378, 1.029359, 1.029341, 1.029322, 1.029303, 1.029284, 1.029266, 1.029247, 1.029229, 1.029210, 1.029191, 1.029173, 1.029154, 1.029136, 1.029117, 1.029099, 1.029081, 1.029062, 1.029044, 1.029025, 1.029007, 1.028989, 1.028970, 1.028952, 1.028934, 1.028916, 1.028897, 1.028879, 1.028861, 1.028843, 1.028825, 1.028807, 1.028789, 1.028770, 1.028752, 1.028734, 1.028716, 1.028698, 1.028680, 1.028662, 1.028645, 1.028627, 1.028609, 1.028591, 1.028573, 1.028555, 1.028537, 1.028520, 1.028502, 1.028484, 1.028466, 1.028449, 1.028431, 1.028413, 1.028396, 1.028378, 1.028360, 1.028343, 1.028325, 1.028308, 1.028290, 1.028273, 1.028255, 1.028238, 1.028220, 1.028203, 1.028185, 1.028168, 1.028151, 1.028133, 1.028116, 1.028099, 1.028081, 1.028064, 1.028047, 1.028030, 1.028012, 1.027995, 1.027978, 1.027961, 1.027944, 1.027927, 1.027909, 1.027892, 1.027875, 1.027858, 1.027841, 1.027824, 1.027807, 1.027790, 1.027773, 1.027756, 1.027739, 1.027723, 1.027706, 1.027689, 1.027672, 1.027655, 1.027638, 1.027622, 1.027605, 1.027588, 1.027571, 1.027555, 1.027538, 1.027521, 1.027505, 1.027488, 1.027471, 1.027455, 1.027438, 1.027422, 1.027405, 1.027389, 1.027372, 1.027356, 1.027339, 1.027323, 1.027306, 1.027290, 1.027273, 1.027257, 1.027241, 1.027224, 1.027208, 1.027192, 1.027175, 1.027159, 1.027143, 1.027127, 1.027110, 1.027094, 1.027078, 1.027062, 1.027046, 1.027029, 1.027013, 1.026997, 1.026981, 1.026965, 1.026949, 1.026933, 1.026917, 1.026901, 1.026885, 1.026869, 1.026853, 1.026837, 1.026821, 1.026805, 1.026789, 1.026774, 1.026758, 1.026742, 1.026726, 1.026710, 1.026695, 1.026679, 1.026663, 1.026647, 1.026632, 1.026616, 1.026600, 1.026585, 1.026569, 1.026553, 1.026538, 1.026522, 1.026507, 1.026491, 1.026475, 1.026460, 1.026444, 1.026429, 1.026413, 1.026398, 1.026383, 1.026367, 1.026352, 1.026336, 1.026321, 1.026306, 1.026290, 1.026275, 1.026260, 1.026244, 1.026229, 1.026214, 1.026199, 1.026183, 1.026168, 1.026153, 1.026138, 1.026122, 1.026107, 1.026092, 1.026077, 1.026062, 1.026047, 1.026032, 1.026017, 1.026002, 1.025987, 1.025972, 1.025957, 1.025942, 1.025927, 1.025912, 1.025897, 1.025882, 1.025867, 1.025852, 1.025837, 1.025822, 1.025808, 1.025793, 1.025778, 1.025763, 1.025748, 1.025734, 1.025719, 1.025704, 1.025689, 1.025675, 1.025660, 1.025645, 1.025631, 1.025616, 1.025602, 1.025587, 1.025572, 1.025558, 1.025543, 1.025529, 1.025514, 1.025500, 1.025485, 1.025471, 1.025456, 1.025442, 1.025427, 1.025413, 1.025398, 1.025384, 1.025370, 1.025355, 1.025341, 1.025326, 1.025312, 1.025298, 1.025284, 1.025269, 1.025255, 1.025241, 1.025226, 1.025212, 1.025198, 1.025184, 1.025170, 1.025156, 1.025141, 1.025127, 1.025113, 1.025099, 1.025085, 1.025071, 1.025057, 1.025043, 1.025029, 1.025015, 1.025001, 1.024987, 1.024973, 1.024959, 1.024945, 1.024931, 1.024917, 1.024903, 1.024889, 1.024875, 1.024861, 1.024848, 1.024834, 1.024820, 1.024806, 1.024792, 1.024778, 1.024765, 1.024751, 1.024737, 1.024724, 1.024710, 1.024696, 1.024682, 1.024669, 1.024655, 1.024642, 1.024628, 1.024614, 1.024601, 1.024587, 1.024573, 1.024560, 1.024546, 1.024533, 1.024519, 1.024506, 1.024492, 1.024479, 1.024465, 1.024452, 1.024439, 1.024425, 1.024412, 1.024398, 1.024385, 1.024371, 1.024358, 1.024345, 1.024331, 1.024318, 1.024305, 1.024292, 1.024278, 1.024265, 1.024252, 1.024238, 1.024225, 1.024212, 1.024199, 1.024186, 1.024172, 1.024159, 1.024146, 1.024133, 1.024120, 1.024107, 1.024094, 1.024081, 1.024068, 1.024054, 1.024041, 1.024028, 1.024015, 1.024002, 1.023989, 1.023976, 1.023963, 1.023950, 1.023937, 1.023925, 1.023912, 1.023899, 1.023886, 1.023873, 1.023860, 1.023847, 1.023834, 1.023822, 1.023809, 1.023796, 1.023783, 1.023770, 1.023758, 1.023745, 1.023732, 1.023719, 1.023707, 1.023694, 1.023681, 1.023669, 1.023656, 1.023643, 1.023631, 1.023618, 1.023605, 1.023593, 1.023580, 1.023568, 1.023555, 1.023542, 1.023530, 1.023517, 1.023505, 1.023492, 1.023480, 1.023467, 1.023455, 1.023442, 1.023430, 1.023417, 1.023405, 1.023393, 1.023380, 1.023368, 1.023355, 1.023343, 1.023331, 1.023318, 1.023306, 1.023294, 1.023281, 1.023269, 1.023257, 1.023245, 1.023232, 1.023220, 1.023208, 1.023196, 1.023183, 1.023171, 1.023159, 1.023147, 1.023135, 1.023123, 1.023110, 1.023098, 1.023086, 1.023074, 1.023062, 1.023050, 1.023038, 1.023026, 1.023014, 1.023002, 1.022990, 1.022977, 1.022966, 1.022954, 1.022942, 1.022930, 1.022918, 1.022906, 1.022894, 1.022882, 1.022870, 1.022858, 1.022846, 1.022834, 1.022822, 1.022810, 1.022799, 1.022787, 1.022775, 1.022763, 1.022751, 1.022740, 1.022728, 1.022716, 1.022704, 1.022692, 1.022681, 1.022669, 1.022657, 1.022646, 1.022634, 1.022622, 1.022611, 1.022599, 1.022587, 1.022576, 1.022564, 1.022552, 1.022541, 1.022529, 1.022518, 1.022506, 1.022494, 1.022483, 1.022471, 1.022460, 1.022448, 1.022437, 1.022425, 1.022414, 1.022402, 1.022391, 1.022379, 1.022368, 1.022357, 1.022345, 1.022334, 1.022322, 1.022311, 1.022300, 1.022288, 1.022277, 1.022265, 1.022254, 1.022243, 1.022231, 1.022220, 1.022209, 1.022197, 1.022186, 1.022175, 1.022164, 1.022153, 1.022141, 1.022130, 1.022119, 1.022108, 1.022096, 1.022085, 1.022074, 1.022063, 1.022052, 1.022041, 1.022029, 1.022018, 1.022007, 1.021996, 1.021985, 1.021974, 1.021963, 1.021952, 1.021941, 1.021930, 1.021919, 1.021908, 1.021897, 1.021886, 1.021875, 1.021864, 1.021853, 1.021842, 1.021831, 1.021820, 1.021809, 1.021798, 1.021787, 1.021776, 1.021765, 1.021754, 1.021744, 1.021733, 1.021722, 1.021711, 1.021700, 1.021689, 1.021679, 1.021668, 1.021657, 1.021646, 1.021635, 1.021625, 1.021614, 1.021603, 1.021592, 1.021582, 1.021571, 1.021560, 1.021549, 1.021539, 1.021528, 1.021518, 1.021507, 1.021496, 1.021486, 1.021475, 1.021464, 1.021454, 1.021443, 1.021433, 1.021422, 1.021411, 1.021401, 1.021390, 1.021380, 1.021369, 1.021359, 1.021348, 1.021338, 1.021327, 1.021317, 1.021306, 1.021296, 1.021285, 1.021275, 1.021264, 1.021254, 1.021243, 1.021233, 1.021223, 1.021212, 1.021202, 1.021191, 1.021181, 1.021171, 1.021160, 1.021150, 1.021140, 1.021129, 1.021119, 1.021109, 1.021098, 1.021088, 1.021078, 1.021068, 1.021057, 1.021047, 1.021037, 1.021027, 1.021016, 1.021006, 1.020996, 1.020986, 1.020976, 1.020965, 1.020955, 1.020945, 1.020935, 1.020925, 1.020915, 1.020905, 1.020895, 1.020884, 1.020874, 1.020864, 1.020854, 1.020844, 1.020834, 1.020824, 1.020814, 1.020804, 1.020794, 1.020784, 1.020774, 1.020764, 1.020754, 1.020744, 1.020734, 1.020724, 1.020714, 1.020704, 1.020694, 1.020684, 1.020674, 1.020664, 1.020654, 1.020645, 1.020635, 1.020625, 1.020615, 1.020605, 1.020595, 1.020585, 1.020576, 1.020566, 1.020556, 1.020546, 1.020536, 1.020526, 1.020517, 1.020507, 1.020497, 1.020487, 1.020478, 1.020468, 1.020458, 1.020448, 1.020439, 1.020429, 1.020419, 1.020410, 1.020400, 1.020390, 1.020380, 1.020371, 1.020361, 1.020352, 1.020342, 1.020332, 1.020323, 1.020313, 1.020303, 1.020294, 1.020284, 1.020275, 1.020265, 1.020256, 1.020246, 1.020236, 1.020227, 1.020217, 1.020208, 1.020198, 1.020189, 1.020179, 1.020170, 1.020160, 1.020151, 1.020141, 1.020132, 1.020123, 1.020113, 1.020104, 1.020094, 1.020085, 1.020076, 1.020066, 1.020057, 1.020047, 1.020038, 1.020029, 1.020019, 1.020010, 1.020001, 1.019991, 1.019982, 1.019973, 1.019963, 1.019954, 1.019945, 1.019935, 1.019926, 1.019917, 1.019908, 1.019898, 1.019889, 1.019880, 1.019871, 1.019861, 1.019852, 1.019843, 1.019834, 1.019825, 1.019815, 1.019806, 1.019797, 1.019788, 1.019779, 1.019770, 1.019760, 1.019751, 1.019742, 1.019733, 1.019724, 1.019715, 1.019706, 1.019697, 1.019688, 1.019678, 1.019669, 1.019660, 1.019651, 1.019642, 1.019633, 1.019624, 1.019615, 1.019606, 1.019597, 1.019588, 1.019579, 1.019570, 1.019561, 1.019552, 1.019543, 1.019534, 1.019525, 1.019516, 1.019508, 1.019499, 1.019490, 1.019481, 1.019472, 1.019463, 1.019454, 1.019445, 1.019436, 1.019428, 1.019419, 1.019410, 1.019401, 1.019392, 1.019383, 1.019374, 1.019366, 1.019357, 1.019348, 1.019339, 1.019331, 1.019322, 1.019313, 1.019304, 1.019295, 1.019287, 1.019278, 1.019269, 1.019261, 1.019252, 1.019243, 1.019234, 1.019226, 1.019217, 1.019208, 1.019200, 1.019191, 1.019182, 1.019174, 1.019165, 1.019156, 1.019148, 1.019139, 1.019130, 1.019122, 1.019113, 1.019105, 1.019096, 1.019088, 1.019079, 1.019070, 1.019062, 1.019053, 1.019045, 1.019036, 1.019028, 1.019019, 1.019011, 1.019002, 1.018994, 1.018985, 1.018977, 1.018968, 1.018960, 1.018951, 1.018943, 1.018934, 1.018926, 1.018917, 1.018909, 1.018900, 1.018892, 1.018884, 1.018875, 1.018867, 1.018858, 1.018850, 1.018842, 1.018833, 1.018825, 1.018816, 1.018808, 1.018800, 1.018791, 1.018783, 1.018775, 1.018767, 1.018758, 1.018750, 1.018741, 1.018733, 1.018725, 1.018717, 1.018708, 1.018700, 1.018692, 1.018684, 1.018675, 1.018667, 1.018659, 1.018651, 1.018642, 1.018634, 1.018626, 1.018618, 1.018610, 1.018601, 1.018593, 1.018585, 1.018577, 1.018569, 1.018561, 1.018552, 1.018544, 1.018536, 1.018528, 1.018520, 1.018512, 1.018504, 1.018496, 1.018487, 1.018479, 1.018471, 1.018463, 1.018455, 1.018447, 1.018439, 1.018431, 1.018423, 1.018415, 1.018407, 1.018399, 1.018391, 1.018383, 1.018375, 1.018367, 1.018359, 1.018351, 1.018343, 1.018335, 1.018327, 1.018319, 1.018311, 1.018303, 1.018295, 1.018287, 1.018279, 1.018271, 1.018263, 1.018255, 1.018247, 1.018239, 1.018232, 1.018224, 1.018216, 1.018208, 1.018200, 1.018192, 1.018184, 1.018176, 1.018169, 1.018161, 1.018153, 1.018145, 1.018137, 1.018129, 1.018122, 1.018114, 1.018106, 1.018098, 1.018090, 1.018083, 1.018075, 1.018067, 1.018059, 1.018052, 1.018044, 1.018036, 1.018028, 1.018021, 1.018013, 1.018005, 1.017998, 1.017990, 1.017982, 1.017974, 1.017967, 1.017959, 1.017951, 1.017944, 1.017936, 1.017928, 1.017921, 1.017913, 1.017905, 1.017898, 1.017890, 1.017882, 1.017875, 1.017867, 1.017860, 1.017852, 1.017844, 1.017837, 1.017829, 1.017822, 1.017814, 1.017807, 1.017799, 1.017792, 1.017784, 1.017776, 1.017769, 1.017761, 1.017754, 1.017746, 1.017739, 1.017731, 1.017724, 1.017716, 1.017709, 1.017701, 1.017694, 1.017686, 1.017679, 1.017671, 1.017664, 1.017656, 1.017649, 1.017642, 1.017634, 1.017627, 1.017619, 1.017612, 1.017604, 1.017597, 1.017590, 1.017582, 1.017575, 1.017568, 1.017560, 1.017553, 1.017545, 1.017538, 1.017531, 1.017523, 1.017516, 1.017509, 1.017501, 1.017494, 1.017487, 1.017479, 1.017472, 1.017465, 1.017457, 1.017450, 1.017443, 1.017436, 1.017428, 1.017421, 1.017414, 1.017406, 1.017399, 1.017392, 1.017385, 1.017377, 1.017370, 1.017363, 1.017356, 1.017349, 1.017341, 1.017334, 1.017327, 1.017320, 1.017313, 1.017305, 1.017298, 1.017291, 1.017284, 1.017277, 1.017270, 1.017262, 1.017255, 1.017248, 1.017241, 1.017234, 1.017227, 1.017220, 1.017213, 1.017205, 1.017198, 1.017191, 1.017184, 1.017177, 1.017170, 1.017163, 1.017156, 1.017149, 1.017142, 1.017135, 1.017128, 1.017120, 1.017113, 1.017106, 1.017099, 1.017092, 1.017085, 1.017078, 1.017071, 1.017064, 1.017057, 1.017050, 1.017043, 1.017036, 1.017029, 1.017022, 1.017015, 1.017008, 1.017002, 1.016995, 1.016988, 1.016981, 1.016974, 1.016967, 1.016960, 1.016953, 1.016946, 1.016939, 1.016932, 1.016925, 1.016918, 1.016912, 1.016905, 1.016898, 1.016891, 1.016884, 1.016877, 1.016870, 1.016863, 1.016857, 1.016850, 1.016843, 1.016836, 1.016829, 1.016822, 1.016816, 1.016809, 1.016802, 1.016795, 1.016788, 1.016782, 1.016775, 1.016768, 1.016761, 1.016754, 1.016748, 1.016741, 1.016734, 1.016727, 1.016721, 1.016714, 1.016707, 1.016700, 1.016694, 1.016687, 1.016680, 1.016674, 1.016667, 1.016660, 1.016653, 1.016647, 1.016640, 1.016633, 1.016627, 1.016620, 1.016613, 1.016607, 1.016600, 1.016593, 1.016587, 1.016580, 1.016573, 1.016567, 1.016560, 1.016554, 1.016547, 1.016540, 1.016534, 1.016527, 1.016520, 1.016514, 1.016507, 1.016501, 1.016494, 1.016487, 1.016481, 1.016474, 1.016468, 1.016461, 1.016455, 1.016448, 1.016442, 1.016435, 1.016428, 1.016422, 1.016415, 1.016409, 1.016402, 1.016396, 1.016389, 1.016383, 1.016376, 1.016370, 1.016363, 1.016357, 1.016350, 1.016344, 1.016337, 1.016331, 1.016325, 1.016318, 1.016312, 1.016305, 1.016299, 1.016292, 1.016286, 1.016279, 1.016273, 1.016267, 1.016260, 1.016254, 1.016247, 1.016241, 1.016235, 1.016228, 1.016222, 1.016215, 1.016209, 1.016203, 1.016196, 1.016190, 1.016183, 1.016177, 1.016171, 1.016164, 1.016158, 1.016152, 1.016145, 1.016139, 1.016133, 1.016126, 1.016120, 1.016114, 1.016107, 1.016101, 1.016095, 1.016089, 1.016082, 1.016076, 1.016070, 1.016063, 1.016057, 1.016051, 1.016045, 1.016038, 1.016032, 1.016026, 1.016020, 1.016013, 1.016007, 1.016001, 1.015995, 1.015988, 1.015982, 1.015976, 1.015970, 1.015964, 1.015957, 1.015951, 1.015945, 1.015939, 1.015933, 1.015926, 1.015920, 1.015914, 1.015908, 1.015902, 1.015896, 1.015890, 1.015883, 1.015877, 1.015871, 1.015865, 1.015859, 1.015853, 1.015847, 1.015840, 1.015834, 1.015828, 1.015822, 1.015816, 1.015810, 1.015804, 1.015798, 1.015792, 1.015786, 1.015779, 1.015773, 1.015767, 1.015761, 1.015755, 1.015749, 1.015743, 1.015737, 1.015731, 1.015725, 1.015719, 1.015713, 1.015707, 1.015701, 1.015695, 1.015689, 1.015683, 1.015677, 1.015671, 1.015665, 1.015659, 1.015653, 1.015647, 1.015641, 1.015635, 1.015629, 1.015623, 1.015617, 1.015611, 1.015605, 1.015599, 1.015593, 1.015587, 1.015581, 1.015575, 1.015569, 1.015563, 1.015558, 1.015552, 1.015546, 1.015540, 1.015534, 1.015528, 1.015522, 1.015516, 1.015510, 1.015504, 1.015499, 1.015493, 1.015487, 1.015481, 1.015475, 1.015469, 1.015463, 1.015458, 1.015452, 1.015446, 1.015440, 1.015434, 1.015428, 1.015422, 1.015417, 1.015411, 1.015405, 1.015399, 1.015393, 1.015388, 1.015382, 1.015376, 1.015370, 1.015364, 1.015359, 1.015353, 1.015347, 1.015341, 1.015336, 1.015330, 1.015324, 1.015318, 1.015313, 1.015307, 1.015301, 1.015295, 1.015290, 1.015284, 1.015278, 1.015272, 1.015267, 1.015261, 1.015255, 1.015249, 1.015244, 1.015238, 1.015232, 1.015227, 1.015221, 1.015215, 1.015210, 1.015204, 1.015198, 1.015193, 1.015187, 1.015181, 1.015176, 1.015170, 1.015164, 1.015159, 1.015153, 1.015147, 1.015142, 1.015136, 1.015130, 1.015125, 1.015119, 1.015114, 1.015108, 1.015102, 1.015097, 1.015091, 1.015085, 1.015080, 1.015074, 1.015069, 1.015063, 1.015058, 1.015052, 1.015046, 1.015041, 1.015035, 1.015030, 1.015024, 1.015019, 1.015013, 1.015007, 1.015002, 1.014996, 1.014991, 1.014985, 1.014980, 1.014974, 1.014969, 1.014963, 1.014958, 1.014952, 1.014947, 1.014941, 1.014936, 1.014930, 1.014925, 1.014919, 1.014914, 1.014908, 1.014903, 1.014897, 1.014892, 1.014886, 1.014881, 1.014875, 1.014870, 1.014865, 1.014859, 1.014854, 1.014848, 1.014843, 1.014837, 1.014832, 1.014827, 1.014821, 1.014816, 1.014810, 1.014805, 1.014799, 1.014794, 1.014789, 1.014783, 1.014778, 1.014772, 1.014767, 1.014762, 1.014756, 1.014751, 1.014745, 1.014740, 1.014735, 1.014729, 1.014724, 1.014719, 1.014713, 1.014708, 1.014703, 1.014697, 1.014692, 1.014687, 1.014681, 1.014676, 1.014671, 1.014665, 1.014660, 1.014655, 1.014649, 1.014644, 1.014639, 1.014634, 1.014628, 1.014623, 1.014618, 1.014612, 1.014607, 1.014602, 1.014596, 1.014591, 1.014586, 1.014581, 1.014575, 1.014570, 1.014565, 1.014560, 1.014554, 1.014549, 1.014544, 1.014539, 1.014533, 1.014528, 1.014523, 1.014518, 1.014513, 1.014507, 1.014502, 1.014497, 1.014492, 1.014487, 1.014481, 1.014476, 1.014471, 1.014466, 1.014461, 1.014455, 1.014450, 1.014445, 1.014440, 1.014435, 1.014429, 1.014424, 1.014419, 1.014414, 1.014409, 1.014404, 1.014399, 1.014393, 1.014388, 1.014383, 1.014378, 1.014373, 1.014368, 1.014363, 1.014358, 1.014352, 1.014347, 1.014342, 1.014337, 1.014332, 1.014327, 1.014322, 1.014317, 1.014312, 1.014307, 1.014301, 1.014296, 1.014291, 1.014286, 1.014281, 1.014276, 1.014271, 1.014266, 1.014261, 1.014256, 1.014251, 1.014246, 1.014241, 1.014236, 1.014231, 1.014226, 1.014220, 1.014215, 1.014210, 1.014205, 1.014200, 1.014195, 1.014190, 1.014185, 1.014180, 1.014175, 1.014170, 1.014165, 1.014160, 1.014155, 1.014150, 1.014145, 1.014140, 1.014135, 1.014130, 1.014126, 1.014121, 1.014116, 1.014111, 1.014106, 1.014101, 1.014096, 1.014091, 1.014086, 1.014081, 1.014076, 1.014071, 1.014066, 1.014061, 1.014056, 1.014051, 1.014046, 1.014042, 1.014037, 1.014032, 1.014027, 1.014022, 1.014017, 1.014012, 1.014007, 1.014002, 1.013997, 1.013993, 1.013988, 1.013983, 1.013978, 1.013973, 1.013968, 1.013963, 1.013958, 1.013954, 1.013949, 1.013944, 1.013939, 1.013934, 1.013929, 1.013924, 1.013920, 1.013915, 1.013910, 1.013905, 1.013900, 1.013896, 1.013891, 1.013886, 1.013881, 1.013876, 1.013871, 1.013867, 1.013862, 1.013857, 1.013852, 1.013847, 1.013843, 1.013838, 1.013833, 1.013828, 1.013824, 1.013819, 1.013814, 1.013809, 1.013804, 1.013800, 1.013795, 1.013790, 1.013785, 1.013781, 1.013776, 1.013771, 1.013766, 1.013762, 1.013757, 1.013752, 1.013747, 1.013743, 1.013738, 1.013733, 1.013728, 1.013724, 1.013719, 1.013714, 1.013710, 1.013705, 1.013700, 1.013696, 1.013691, 1.013686, 1.013681, 1.013677, 1.013672, 1.013667, 1.013663, 1.013658, 1.013653, 1.013649, 1.013644, 1.013639, 1.013635, 1.013630, 1.013625, 1.013621, 1.013616, 1.013611, 1.013607, 1.013602, 1.013597, 1.013593, 1.013588, 1.013584, 1.013579, 1.013574, 1.013570, 1.013565, 1.013561, 1.013556, 1.013551, 1.013547, 1.013542, 1.013538, 1.013533, 1.013528, 1.013524, 1.013519, 1.013515, 1.013510, 1.013505, 1.013501, 1.013496, 1.013492, 1.013487, 1.013482, 1.013478, 1.013473, 1.013469, 1.013464, 1.013460, 1.013455, 1.013451, 1.013446, 1.013442, 1.013437, 1.013432, 1.013428, 1.013423, 1.013419, 1.013414, 1.013410, 1.013405, 1.013401, 1.013396, 1.013392, 1.013387, 1.013383, 1.013378, 1.013374, 1.013369, 1.013365, 1.013360, 1.013356, 1.013351, 1.013347, 1.013342, 1.013338, 1.013333, 1.013329, 1.013324, 1.013320, 1.013315, 1.013311, 1.013306, 1.013302, 1.013298, 1.013293, 1.013289, 1.013284, 1.013280, 1.013275, 1.013271, 1.013267, 1.013262, 1.013258, 1.013253, 1.013249, 1.013244, 1.013240, 1.013236, 1.013231, 1.013227, 1.013222, 1.013218, 1.013214, 1.013209, 1.013205, 1.013200, 1.013196, 1.013191, 1.013187, 1.013183, 1.013178, 1.013174, 1.013170, 1.013165, 1.013161, 1.013157, 1.013152, 1.013148, 1.013143, 1.013139, 1.013135, 1.013130, 1.013126, 1.013122, 1.013117, 1.013113, 1.013109, 1.013104, 1.013100, 1.013096, 1.013091, 1.013087, 1.013083, 1.013078, 1.013074, 1.013070, 1.013065, 1.013061, 1.013057, 1.013052, 1.013048, 1.013044, 1.013039, 1.013035, 1.013031, 1.013027, 1.013022, 1.013018, 1.013014, 1.013010, 1.013005, 1.013001, 1.012997, 1.012992, 1.012988, 1.012984, 1.012980, 1.012975, 1.012971, 1.012967, 1.012963, 1.012958, 1.012954, 1.012950, 1.012946, 1.012941, 1.012937, 1.012933, 1.012929, 1.012924, 1.012920, 1.012916, 1.012912, 1.012908, 1.012903, 1.012899, 1.012895, 1.012891, 1.012886, 1.012882, 1.012878, 1.012874, 1.012870, 1.012865, 1.012861, 1.012857, 1.012853, 1.012849, 1.012845, 1.012840, 1.012836, 1.012832, 1.012828, 1.012824, 1.012819, 1.012815, 1.012811, 1.012807, 1.012803, 1.012799, 1.012794, 1.012790, 1.012786, 1.012782, 1.012778, 1.012774, 1.012770, 1.012766, 1.012761, 1.012757, 1.012753, 1.012749, 1.012745, 1.012741, 1.012737, 1.012733, 1.012728, 1.012724, 1.012720, 1.012716, 1.012712, 1.012708, 1.012704, 1.012700, 1.012696, 1.012691, 1.012687, 1.012683, 1.012679, 1.012675, 1.012671, 1.012667, 1.012663, 1.012659, 1.012655, 1.012651, 1.012647, 1.012643, 1.012638, 1.012634, 1.012630, 1.012626, 1.012622, 1.012618, 1.012614, 1.012610, 1.012606, 1.012602, 1.012598, 1.012594, 1.012590, 1.012586, 1.012582, 1.012578, 1.012574, 1.012570, 1.012566, 1.012562, 1.012558, 1.012554, 1.012550, 1.012546, 1.012542, 1.012538, 1.012534, 1.012530, 1.012526, 1.012522, 1.012518, 1.012514, 1.012510, 1.012506, 1.012502, 1.012498, 1.012494, 1.012490, 1.012486, 1.012482, 1.012478, 1.012474, 1.012470, 1.012466, 1.012462, 1.012458, 1.012454, 1.012450, 1.012446, 1.012442, 1.012438, 1.012434, 1.012430, 1.012426, 1.012423, 1.012419, 1.012415, 1.012411, 1.012407, 1.012403, 1.012399, 1.012395, 1.012391, 1.012387, 1.012383, 1.012379, 1.012375, 1.012372, 1.012368, 1.012364, 1.012360, 1.012356, 1.012352, 1.012348, 1.012344, 1.012340, 1.012337, 1.012333, 1.012329, 1.012325, 1.012321, 1.012317, 1.012313, 1.012309, 1.012305, 1.012302, 1.012298, 1.012294, 1.012290, 1.012286, 1.012282, 1.012279, 1.012275, 1.012271, 1.012267, 1.012263, 1.012259, 1.012255, 1.012252, 1.012248, 1.012244, 1.012240, 1.012236, 1.012232, 1.012229, 1.012225, 1.012221, 1.012217, 1.012213, 1.012210, 1.012206, 1.012202, 1.012198, 1.012194, 1.012190, 1.012187, 1.012183, 1.012179, 1.012175, 1.012172, 1.012168, 1.012164, 1.012160, 1.012156, 1.012153, 1.012149, 1.012145, 1.012141, 1.012137, 1.012134, 1.012130, 1.012126, 1.012122, 1.012119, 1.012115, 1.012111, 1.012107, 1.012104, 1.012100, 1.012096, 1.012092, 1.012089, 1.012085, 1.012081, 1.012077, 1.012074, 1.012070, 1.012066, 1.012062, 1.012059, 1.012055, 1.012051, 1.012048, 1.012044, 1.012040, 1.012036, 1.012033, 1.012029, 1.012025, 1.012021, 1.012018, 1.012014, 1.012010, 1.012007, 1.012003, 1.011999, 1.011996, 1.011992, 1.011988, 1.011984, 1.011981, 1.011977, 1.011973, 1.011970, 1.011966, 1.011962, 1.011959, 1.011955, 1.011951, 1.011948, 1.011944, 1.011940, 1.011937, 1.011933, 1.011929, 1.011926, 1.011922, 1.011918, 1.011915, 1.011911, 1.011907, 1.011904, 1.011900, 1.011897, 1.011893, 1.011889, 1.011886, 1.011882, 1.011878, 1.011875, 1.011871, 1.011868, 1.011864, 1.011860, 1.011857, 1.011853, 1.011849, 1.011846, 1.011842, 1.011839, 1.011835, 1.011831, 1.011828, 1.011824, 1.011821, 1.011817, 1.011813, 1.011810, 1.011806, 1.011803, 1.011799, 1.011796, 1.011792, 1.011788, 1.011785, 1.011781, 1.011778, 1.011774, 1.011770, 1.011767, 1.011763, 1.011760, 1.011756, 1.011753, 1.011749, 1.011745, 1.011742, 1.011738, 1.011735, 1.011731, 1.011728, 1.011724, 1.011721, 1.011717, 1.011714, 1.011710, 1.011706, 1.011703, 1.011699, 1.011696, 1.011692, 1.011689, 1.011685, 1.011682, 1.011678, 1.011675, 1.011671, 1.011668, 1.011664, 1.011661, 1.011657, 1.011654, 1.011650, 1.011647, 1.011643, 1.011640, 1.011636, 1.011633, 1.011629, 1.011626, 1.011622, 1.011619, 1.011615, 1.011612, 1.011608, 1.011605, 1.011601, 1.011598, 1.011594, 1.011591, 1.011588, 1.011584, 1.011581, 1.011577, 1.011574, 1.011570, 1.011567, 1.011563, 1.011560, 1.011556, 1.011553, 1.011549, 1.011546, 1.011543, 1.011539, 1.011536, 1.011532, 1.011529, 1.011525, 1.011522, 1.011518, 1.011515, 1.011512, 1.011508, 1.011505, 1.011501, 1.011498, 1.011495, 1.011491, 1.011488, 1.011484, 1.011481, 1.011477, 1.011474, 1.011471, 1.011467, 1.011464, 1.011460, 1.011457, 1.011454, 1.011450, 1.011447, 1.011443, 1.011440, 1.011437, 1.011433, 1.011430, 1.011426, 1.011423, 1.011420, 1.011416, 1.011413, 1.011410, 1.011406, 1.011403, 1.011400, 1.011396, 1.011393, 1.011389, 1.011386, 1.011383, 1.011379, 1.011376, 1.011373, 1.011369, 1.011366, 1.011363, 1.011359, 1.011356, 1.011353, 1.011349, 1.011346, 1.011343, 1.011339, 1.011336, 1.011333, 1.011329, 1.011326, 1.011322, 1.011319, 1.011316, 1.011313, 1.011309, 1.011306, 1.011303, 1.011299, 1.011296, 1.011293, 1.011289, 1.011286, 1.011283, 1.011279, 1.011276, 1.011273, 1.011270, 1.011266, 1.011263, 1.011260, 1.011256, 1.011253, 1.011250, 1.011246, 1.011243, 1.011240, 1.011237, 1.011233, 1.011230, 1.011227, 1.011224, 1.011220, 1.011217, 1.011214, 1.011210, 1.011207, 1.011204, 1.011201, 1.011197, 1.011194, 1.011191, 1.011188, 1.011184, 1.011181, 1.011178, 1.011175, 1.011171, 1.011168, 1.011165, 1.011162, 1.011158, 1.011155, 1.011152, 1.011149, 1.011145, 1.011142, 1.011139, 1.011136, 1.011132, 1.011129, 1.011126, 1.011123, 1.011120, 1.011116, 1.011113, 1.011110, 1.011107, 1.011104, 1.011100, 1.011097, 1.011094, 1.011091, 1.011088, 1.011084, 1.011081, 1.011078, 1.011075, 1.011072, 1.011068, 1.011065, 1.011062, 1.011059, 1.011056, 1.011052, 1.011049, 1.011046, 1.011043, 1.011040, 1.011037, 1.011033, 1.011030, 1.011027, 1.011024, 1.011021, 1.011017, 1.011014, 1.011011, 1.011008, 1.011005, 1.011002, 1.010998, 1.010995, 1.010992, 1.010989, 1.010986, 1.010983, 1.010980, 1.010976, 1.010973, 1.010970, 1.010967, 1.010964, 1.010961, 1.010958, 1.010954, 1.010951, 1.010948, 1.010945, 1.010942, 1.010939, 1.010936, 1.010933, 1.010929, 1.010926, 1.010923, 1.010920, 1.010917, 1.010914, 1.010911, 1.010908, 1.010905, 1.010901, 1.010898, 1.010895, 1.010892, 1.010889, 1.010886, 1.010883, 1.010880, 1.010877, 1.010874, 1.010871, 1.010867, 1.010864, 1.010861, 1.010858, 1.010855, 1.010852, 1.010849, 1.010846, 1.010843, 1.010840, 1.010837, 1.010834, 1.010831, 1.010827, 1.010824, 1.010821, 1.010818, 1.010815, 1.010812, 1.010809, 1.010806, 1.010803, 1.010800, 1.010797, 1.010794, 1.010791, 1.010788, 1.010785, 1.010782, 1.010779, 1.010776, 1.010772, 1.010769, 1.010766, 1.010763, 1.010760, 1.010757, 1.010754, 1.010751, 1.010748, 1.010745, 1.010742, 1.010739, 1.010736, 1.010733, 1.010730, 1.010727, 1.010724, 1.010721, 1.010718, 1.010715, 1.010712, 1.010709, 1.010706, 1.010703, 1.010700, 1.010697, 1.010694, 1.010691, 1.010688, 1.010685, 1.010682, 1.010679, 1.010676, 1.010673, 1.010670, 1.010667, 1.010664, 1.010661, 1.010658, 1.010655, 1.010652, 1.010649, 1.010646, 1.010643, 1.010640, 1.010637, 1.010634, 1.010631, 1.010628, 1.010625, 1.010622, 1.010620, 1.010617, 1.010614, 1.010611, 1.010608, 1.010605, 1.010602, 1.010599, 1.010596, 1.010593, 1.010590, 1.010587, 1.010584, 1.010581, 1.010578, 1.010575, 1.010572, 1.010569, 1.010566, 1.010563, 1.010561, 1.010558, 1.010555, 1.010552, 1.010549, 1.010546, 1.010543, 1.010540, 1.010537, 1.010534, 1.010531, 1.010528, 1.010525, 1.010523, 1.010520, 1.010517, 1.010514, 1.010511, 1.010508, 1.010505, 1.010502, 1.010499, 1.010496, 1.010494, 1.010491, 1.010488, 1.010485, 1.010482, 1.010479, 1.010476, 1.010473, 1.010470, 1.010468, 1.010465, 1.010462, 1.010459, 1.010456, 1.010453, 1.010450, 1.010447, 1.010445, 1.010442, 1.010439, 1.010436, 1.010433, 1.010430, 1.010427, 1.010424, 1.010422, 1.010419, 1.010416, 1.010413, 1.010410, 1.010407, 1.010404, 1.010401, 1.010399, 1.010396, 1.010393, 1.010390, 1.010387, 1.010384, 1.010382, 1.010379, 1.010376, 1.010373, 1.010370, 1.010367, 1.010365, 1.010362, 1.010359, 1.010356, 1.010353, 1.010350, 1.010348, 1.010345, 1.010342, 1.010339, 1.010336, 1.010334, 1.010331, 1.010328, 1.010325, 1.010322, 1.010319, 1.010317, 1.010314, 1.010311, 1.010308, 1.010305, 1.010303, 1.010300, 1.010297, 1.010294, 1.010291, 1.010289, 1.010286, 1.010283, 1.010280, 1.010277, 1.010275, 1.010272, 1.010269, 1.010266, 1.010263, 1.010261, 1.010258, 1.010255, 1.010252, 1.010250, 1.010247, 1.010244, 1.010241, 1.010239, 1.010236, 1.010233, 1.010230, 1.010227, 1.010225, 1.010222, 1.010219, 1.010216, 1.010214, 1.010211, 1.010208, 1.010205, 1.010203, 1.010200, 1.010197, 1.010194, 1.010192, 1.010189, 1.010186, 1.010183, 1.010181, 1.010178, 1.010175, 1.010172, 1.010170, 1.010167, 1.010164, 1.010161, 1.010159, 1.010156, 1.010153, 1.010151, 1.010148, 1.010145, 1.010142, 1.010140, 1.010137, 1.010134, 1.010131, 1.010129, 1.010126, 1.010123, 1.010121, 1.010118, 1.010115, 1.010112, 1.010110, 1.010107, 1.010104, 1.010102, 1.010099, 1.010096, 1.010093, 1.010091, 1.010088, 1.010085, 1.010083, 1.010080, 1.010077, 1.010075, 1.010072, 1.010069, 1.010067, 1.010064, 1.010061, 1.010059, 1.010056, 1.010053, 1.010050, 1.010048, 1.010045, 1.010042, 1.010040, 1.010037, 1.010034, 1.010032, 1.010029, 1.010026, 1.010024, 1.010021, 1.010018, 1.010016, 1.010013, 1.010010, 1.010008, 1.010005, 1.010002, 1.010000, 1.009997, 1.009995, 1.009992, 1.009989, 1.009987, 1.009984, 1.009981, 1.009979, 1.009976, 1.009973, 1.009971, 1.009968, 1.009965, 1.009963, 1.009960, 1.009957, 1.009955, 1.009952, 1.009950, 1.009947, 1.009944, 1.009942, 1.009939, 1.009936, 1.009934, 1.009931, 1.009929, 1.009926, 1.009923, 1.009921, 1.009918, 1.009915, 1.009913, 1.009910, 1.009908, 1.009905, 1.009902, 1.009900, 1.009897, 1.009895, 1.009892, 1.009889, 1.009887, 1.009884, 1.009882, 1.009879, 1.009876, 1.009874, 1.009871, 1.009869, 1.009866, 1.009863, 1.009861, 1.009858, 1.009856, 1.009853, 1.009851, 1.009848, 1.009845, 1.009843, 1.009840, 1.009838, 1.009835, 1.009832, 1.009830, 1.009827, 1.009825, 1.009822, 1.009820, 1.009817, 1.009815, 1.009812, 1.009809, 1.009807, 1.009804, 1.009802, 1.009799, 1.009797, 1.009794, 1.009791, 1.009789, 1.009786, 1.009784, 1.009781, 1.009779, 1.009776, 1.009773, 1.009771, 1.009768, 1.009766, 1.009763, 1.009761, 1.009758, 1.009756, 1.009753, 1.009751, 1.009748, 1.009746, 1.009743, 1.009740, 1.009738, 1.009735, 1.009733, 1.009730, 1.009728, 1.009725, 1.009723, 1.009720, 1.009718, 1.009715, 1.009713, 1.009710, 1.009708, 1.009705, 1.009703, 1.009700, 1.009698, 1.009695, 1.009693, 1.009690, 1.009688, 1.009685, 1.009683, 1.009680, 1.009678, 1.009675, 1.009673, 1.009670, 1.009668, 1.009665, 1.009663, 1.009660, 1.009658, 1.009655, 1.009653, 1.009650, 1.009648, 1.009645, 1.009643, 1.009640, 1.009638, 1.009635, 1.009633, 1.009630, 1.009628, 1.009625, 1.009623, 1.009620, 1.009618, 1.009615, 1.009613, 1.009611, 1.009608, 1.009606, 1.009603, 1.009601, 1.009598, 1.009596, 1.009593, 1.009591, 1.009588, 1.009586, 1.009583, 1.009581, 1.009578, 1.009576, 1.009574, 1.009571, 1.009569, 1.009566, 1.009564, 1.009561, 1.009559, 1.009556, 1.009554, 1.009552, 1.009549, 1.009547, 1.009544, 1.009542, 1.009539, 1.009537, 1.009534, 1.009532, 1.009530, 1.009527, 1.009525, 1.009522, 1.009520, 1.009517, 1.009515, 1.009513, 1.009510, 1.009508, 1.009505, 1.009503, 1.009501, 1.009498, 1.009496, 1.009493, 1.009491, 1.009488, 1.009486, 1.009484, 1.009481, 1.009479, 1.009476, 1.009474, 1.009472, 1.009469, 1.009467, 1.009464, 1.009462, 1.009459, 1.009457, 1.009455, 1.009452, 1.009450, 1.009447, 1.009445, 1.009443, 1.009440, 1.009438, 1.009436, 1.009433, 1.009431, 1.009428, 1.009426, 1.009424, 1.009421, 1.009419, 1.009416, 1.009414, 1.009412, 1.009409, 1.009407, 1.009405, 1.009402, 1.009400, 1.009398, 1.009395, 1.009393, 1.009390, 1.009388, 1.009386, 1.009383, 1.009381, 1.009379, 1.009376, 1.009374, 1.009371, 1.009369, 1.009367, 1.009364, 1.009362, 1.009360, 1.009357, 1.009355, 1.009353, 1.009350, 1.009348, 1.009346, 1.009343, 1.009341, 1.009338, 1.009336, 1.009334, 1.009331, 1.009329, 1.009327, 1.009324, 1.009322, 1.009320, 1.009317, 1.009315, 1.009313, 1.009310, 1.009308, 1.009306, 1.009303, 1.009301, 1.009299, 1.009296, 1.009294, 1.009292, 1.009290, 1.009287, 1.009285, 1.009282, 1.009280, 1.009278, 1.009276, 1.009273, 1.009271, 1.009269, 1.009266, 1.009264, 1.009262, 1.009259, 1.009257, 1.009255, 1.009252, 1.009250, 1.009248, 1.009246, 1.009243, 1.009241, 1.009239, 1.009236, 1.009234, 1.009232, 1.009229, 1.009227, 1.009225, 1.009223, 1.009220, 1.009218, 1.009216, 1.009213, 1.009211, 1.009209, 1.009207, 1.009204, 1.009202, 1.009200, 1.009197, 1.009195, 1.009193, 1.009191, 1.009188, 1.009186, 1.009184, 1.009181, 1.009179, 1.009177, 1.009175, 1.009172, 1.009170, 1.009168, 1.009166, 1.009163, 1.009161, 1.009159, 1.009156, 1.009154, 1.009152, 1.009150, 1.009147, 1.009145, 1.009143, 1.009141, 1.009138, 1.009136, 1.009134, 1.009132, 1.009129, 1.009127, 1.009125, 1.009123, 1.009120, 1.009118, 1.009116, 1.009114, 1.009111, 1.009109, 1.009107, 1.009105, 1.009102, 1.009100, 1.009098, 1.009096, 1.009094, 1.009091, 1.009089, 1.009087, 1.009085, 1.009082, 1.009080, 1.009078, 1.009076, 1.009073, 1.009071, 1.009069, 1.009067, 1.009065, 1.009062, 1.009060, 1.009058, 1.009056, 1.009053, 1.009051, 1.009049, 1.009047, 1.009045, 1.009042, 1.009040, 1.009038, 1.009036, 1.009034, 1.009031, 1.009029, 1.009027, 1.009025, 1.009022, 1.009020, 1.009018, 1.009016, 1.009014, 1.009012, 1.009009, 1.009007, 1.009005, 1.009003, 1.009001, 1.008998 };
        float SlowBlocksLimit[5040] = { 0.003147, 0.007340, 0.012020, 0.017026, 0.022274, 0.027709, 0.033291, 0.038992, 0.044788, 0.050661, 0.056595, 0.062578, 0.068598, 0.074646, 0.080713, 0.086792, 0.092877, 0.098962, 0.105042, 0.111113, 0.117170, 0.123209, 0.129229, 0.135224, 0.141194, 0.147136, 0.153047, 0.158927, 0.164772, 0.170581, 0.176354, 0.182089, 0.187784, 0.193440, 0.199054, 0.204627, 0.210157, 0.215645, 0.221089, 0.226490, 0.231846, 0.237158, 0.242425, 0.247647, 0.252825, 0.257957, 0.263045, 0.268087, 0.273084, 0.278037, 0.282945, 0.287807, 0.292626, 0.297400, 0.302129, 0.306815, 0.311457, 0.316055, 0.320610, 0.325122, 0.329592, 0.334019, 0.338404, 0.342747, 0.347049, 0.351309, 0.355529, 0.359709, 0.363848, 0.367947, 0.372008, 0.376029, 0.380011, 0.383956, 0.387862, 0.391731, 0.395562, 0.399357, 0.403115, 0.406837, 0.410524, 0.414175, 0.417791, 0.421373, 0.424920, 0.428433, 0.431913, 0.435360, 0.438774, 0.442156, 0.445505, 0.448823, 0.452109, 0.455365, 0.458589, 0.461783, 0.464948, 0.468082, 0.471188, 0.474264, 0.477311, 0.480331, 0.483322, 0.486285, 0.489221, 0.492130, 0.495013, 0.497868, 0.500698, 0.503501, 0.506279, 0.509032, 0.511760, 0.514463, 0.517141, 0.519796, 0.522426, 0.525033, 0.527617, 0.530177, 0.532715, 0.535230, 0.537723, 0.540193, 0.542642, 0.545070, 0.547476, 0.549861, 0.552225, 0.554569, 0.556892, 0.559195, 0.561478, 0.563742, 0.565986, 0.568211, 0.570417, 0.572604, 0.574772, 0.576922, 0.579054, 0.581168, 0.583264, 0.585343, 0.587404, 0.589448, 0.591475, 0.593485, 0.595479, 0.597456, 0.599417, 0.601361, 0.603290, 0.605203, 0.607101, 0.608983, 0.610850, 0.612702, 0.614538, 0.616361, 0.618168, 0.619961, 0.621740, 0.623505, 0.625256, 0.626993, 0.628716, 0.630426, 0.632122, 0.633805, 0.635475, 0.637132, 0.638777, 0.640408, 0.642027, 0.643634, 0.645228, 0.646810, 0.648380, 0.649938, 0.651484, 0.653019, 0.654542, 0.656054, 0.657554, 0.659043, 0.660521, 0.661988, 0.663444, 0.664890, 0.666324, 0.667749, 0.669162, 0.670566, 0.671959, 0.673342, 0.674715, 0.676078, 0.677432, 0.678775, 0.680109, 0.681434, 0.682749, 0.684054, 0.685351, 0.686638, 0.687916, 0.689185, 0.690445, 0.691697, 0.692939, 0.694173, 0.695399, 0.696616, 0.697824, 0.699025, 0.700217, 0.701400, 0.702576, 0.703744, 0.704904, 0.706056, 0.707200, 0.708336, 0.709465, 0.710586, 0.711700, 0.712806, 0.713905, 0.714997, 0.716081, 0.717158, 0.718228, 0.719292, 0.720348, 0.721397, 0.722439, 0.723475, 0.724504, 0.725526, 0.726542, 0.727551, 0.728553, 0.729550, 0.730539, 0.731523, 0.732500, 0.733471, 0.734436, 0.735395, 0.736348, 0.737295, 0.738236, 0.739171, 0.740100, 0.741023, 0.741941, 0.742853, 0.743759, 0.744660, 0.745556, 0.746445, 0.747330, 0.748209, 0.749082, 0.749951, 0.750814, 0.751672, 0.752524, 0.753372, 0.754214, 0.755052, 0.755884, 0.756712, 0.757535, 0.758352, 0.759165, 0.759974, 0.760777, 0.761576, 0.762370, 0.763159, 0.763944, 0.764724, 0.765500, 0.766271, 0.767038, 0.767800, 0.768558, 0.769312, 0.770061, 0.770806, 0.771547, 0.772284, 0.773016, 0.773745, 0.774469, 0.775189, 0.775905, 0.776617, 0.777325, 0.778030, 0.778730, 0.779426, 0.780119, 0.780807, 0.781492, 0.782174, 0.782851, 0.783525, 0.784195, 0.784861, 0.785524, 0.786183, 0.786839, 0.787491, 0.788140, 0.788785, 0.789426, 0.790064, 0.790699, 0.791331, 0.791959, 0.792583, 0.793205, 0.793823, 0.794438, 0.795050, 0.795658, 0.796263, 0.796865, 0.797464, 0.798060, 0.798653, 0.799243, 0.799829, 0.800413, 0.800994, 0.801571, 0.802146, 0.802718, 0.803287, 0.803852, 0.804416, 0.804976, 0.805533, 0.806088, 0.806639, 0.807189, 0.807735, 0.808278, 0.808819, 0.809357, 0.809893, 0.810425, 0.810956, 0.811483, 0.812008, 0.812530, 0.813050, 0.813567, 0.814082, 0.814594, 0.815104, 0.815611, 0.816116, 0.816618, 0.817118, 0.817615, 0.818110, 0.818603, 0.819093, 0.819581, 0.820067, 0.820550, 0.821031, 0.821510, 0.821987, 0.822461, 0.822933, 0.823403, 0.823870, 0.824336, 0.824799, 0.825260, 0.825719, 0.826176, 0.826630, 0.827083, 0.827533, 0.827981, 0.828428, 0.828872, 0.829314, 0.829754, 0.830193, 0.830629, 0.831063, 0.831495, 0.831925, 0.832354, 0.832780, 0.833204, 0.833627, 0.834047, 0.834466, 0.834883, 0.835298, 0.835711, 0.836123, 0.836532, 0.836940, 0.837346, 0.837750, 0.838152, 0.838552, 0.838951, 0.839348, 0.839744, 0.840137, 0.840529, 0.840919, 0.841307, 0.841694, 0.842079, 0.842463, 0.842844, 0.843225, 0.843603, 0.843980, 0.844355, 0.844729, 0.845101, 0.845471, 0.845840, 0.846207, 0.846573, 0.846937, 0.847300, 0.847661, 0.848021, 0.848379, 0.848736, 0.849091, 0.849445, 0.849797, 0.850147, 0.850497, 0.850844, 0.851191, 0.851536, 0.851879, 0.852221, 0.852562, 0.852901, 0.853239, 0.853576, 0.853911, 0.854245, 0.854577, 0.854908, 0.855238, 0.855567, 0.855894, 0.856219, 0.856544, 0.856867, 0.857189, 0.857509, 0.857829, 0.858147, 0.858463, 0.858779, 0.859093, 0.859406, 0.859718, 0.860028, 0.860338, 0.860646, 0.860953, 0.861258, 0.861563, 0.861866, 0.862168, 0.862469, 0.862769, 0.863067, 0.863365, 0.863661, 0.863956, 0.864250, 0.864543, 0.864835, 0.865125, 0.865415, 0.865703, 0.865991, 0.866277, 0.866562, 0.866846, 0.867129, 0.867411, 0.867692, 0.867971, 0.868250, 0.868528, 0.868804, 0.869080, 0.869354, 0.869628, 0.869900, 0.870172, 0.870442, 0.870712, 0.870980, 0.871248, 0.871514, 0.871780, 0.872044, 0.872308, 0.872570, 0.872832, 0.873092, 0.873352, 0.873611, 0.873869, 0.874125, 0.874381, 0.874636, 0.874890, 0.875144, 0.875396, 0.875647, 0.875898, 0.876147, 0.876396, 0.876643, 0.876890, 0.877136, 0.877381, 0.877626, 0.877869, 0.878111, 0.878353, 0.878594, 0.878834, 0.879073, 0.879311, 0.879548, 0.879785, 0.880021, 0.880256, 0.880490, 0.880723, 0.880955, 0.881187, 0.881418, 0.881648, 0.881877, 0.882106, 0.882333, 0.882560, 0.882786, 0.883011, 0.883236, 0.883460, 0.883683, 0.883905, 0.884126, 0.884347, 0.884567, 0.884786, 0.885005, 0.885223, 0.885440, 0.885656, 0.885871, 0.886086, 0.886300, 0.886514, 0.886726, 0.886938, 0.887150, 0.887360, 0.887570, 0.887779, 0.887987, 0.888195, 0.888402, 0.888609, 0.888814, 0.889019, 0.889224, 0.889427, 0.889630, 0.889832, 0.890034, 0.890235, 0.890435, 0.890635, 0.890834, 0.891033, 0.891230, 0.891427, 0.891624, 0.891820, 0.892015, 0.892209, 0.892403, 0.892597, 0.892789, 0.892981, 0.893173, 0.893364, 0.893554, 0.893744, 0.893933, 0.894121, 0.894309, 0.894496, 0.894683, 0.894869, 0.895054, 0.895239, 0.895423, 0.895607, 0.895790, 0.895972, 0.896155, 0.896336, 0.896517, 0.896697, 0.896877, 0.897056, 0.897234, 0.897412, 0.897590, 0.897767, 0.897943, 0.898119, 0.898294, 0.898469, 0.898643, 0.898817, 0.898990, 0.899163, 0.899335, 0.899507, 0.899678, 0.899848, 0.900018, 0.900188, 0.900356, 0.900525, 0.900693, 0.900860, 0.901027, 0.901194, 0.901360, 0.901525, 0.901690, 0.901854, 0.902018, 0.902182, 0.902345, 0.902507, 0.902669, 0.902831, 0.902992, 0.903152, 0.903313, 0.903472, 0.903631, 0.903790, 0.903948, 0.904106, 0.904263, 0.904420, 0.904576, 0.904732, 0.904887, 0.905042, 0.905197, 0.905351, 0.905505, 0.905658, 0.905811, 0.905963, 0.906115, 0.906266, 0.906417, 0.906568, 0.906718, 0.906867, 0.907017, 0.907165, 0.907314, 0.907462, 0.907609, 0.907756, 0.907903, 0.908049, 0.908195, 0.908341, 0.908486, 0.908630, 0.908774, 0.908918, 0.909062, 0.909205, 0.909347, 0.909489, 0.909631, 0.909772, 0.909913, 0.910054, 0.910194, 0.910334, 0.910473, 0.910612, 0.910751, 0.910889, 0.911027, 0.911165, 0.911302, 0.911439, 0.911575, 0.911711, 0.911846, 0.911982, 0.912117, 0.912251, 0.912385, 0.912519, 0.912652, 0.912785, 0.912918, 0.913050, 0.913182, 0.913314, 0.913445, 0.913576, 0.913706, 0.913836, 0.913966, 0.914096, 0.914225, 0.914353, 0.914482, 0.914610, 0.914737, 0.914865, 0.914992, 0.915119, 0.915245, 0.915371, 0.915497, 0.915622, 0.915747, 0.915872, 0.915996, 0.916120, 0.916243, 0.916367, 0.916490, 0.916613, 0.916735, 0.916857, 0.916979, 0.917100, 0.917221, 0.917342, 0.917462, 0.917583, 0.917702, 0.917822, 0.917941, 0.918060, 0.918179, 0.918297, 0.918415, 0.918532, 0.918650, 0.918767, 0.918884, 0.919000, 0.919116, 0.919232, 0.919348, 0.919463, 0.919578, 0.919693, 0.919807, 0.919921, 0.920035, 0.920148, 0.920262, 0.920374, 0.920487, 0.920599, 0.920711, 0.920823, 0.920935, 0.921046, 0.921157, 0.921268, 0.921378, 0.921488, 0.921598, 0.921707, 0.921817, 0.921926, 0.922034, 0.922143, 0.922251, 0.922359, 0.922466, 0.922574, 0.922681, 0.922788, 0.922894, 0.923001, 0.923107, 0.923213, 0.923318, 0.923423, 0.923528, 0.923633, 0.923737, 0.923842, 0.923946, 0.924049, 0.924153, 0.924256, 0.924359, 0.924462, 0.924564, 0.924666, 0.924768, 0.924870, 0.924971, 0.925073, 0.925174, 0.925274, 0.925375, 0.925475, 0.925575, 0.925675, 0.925774, 0.925874, 0.925973, 0.926071, 0.926170, 0.926268, 0.926366, 0.926464, 0.926562, 0.926659, 0.926756, 0.926853, 0.926950, 0.927046, 0.927143, 0.927239, 0.927334, 0.927430, 0.927525, 0.927620, 0.927715, 0.927810, 0.927904, 0.927999, 0.928093, 0.928186, 0.928280, 0.928373, 0.928466, 0.928559, 0.928652, 0.928744, 0.928837, 0.928929, 0.929020, 0.929112, 0.929203, 0.929295, 0.929386, 0.929476, 0.929567, 0.929657, 0.929747, 0.929837, 0.929927, 0.930016, 0.930106, 0.930195, 0.930284, 0.930372, 0.930461, 0.930549, 0.930637, 0.930725, 0.930813, 0.930900, 0.930988, 0.931075, 0.931162, 0.931248, 0.931335, 0.931421, 0.931507, 0.931593, 0.931679, 0.931764, 0.931850, 0.931935, 0.932020, 0.932104, 0.932189, 0.932273, 0.932357, 0.932442, 0.932525, 0.932609, 0.932692, 0.932776, 0.932859, 0.932942, 0.933024, 0.933107, 0.933189, 0.933271, 0.933353, 0.933435, 0.933517, 0.933598, 0.933679, 0.933760, 0.933841, 0.933922, 0.934003, 0.934083, 0.934163, 0.934243, 0.934323, 0.934403, 0.934482, 0.934561, 0.934640, 0.934719, 0.934798, 0.934877, 0.934955, 0.935034, 0.935112, 0.935190, 0.935267, 0.935345, 0.935422, 0.935500, 0.935577, 0.935654, 0.935730, 0.935807, 0.935883, 0.935960, 0.936036, 0.936112, 0.936188, 0.936263, 0.936339, 0.936414, 0.936489, 0.936564, 0.936639, 0.936714, 0.936788, 0.936863, 0.936937, 0.937011, 0.937085, 0.937158, 0.937232, 0.937305, 0.937379, 0.937452, 0.937525, 0.937597, 0.937670, 0.937743, 0.937815, 0.937887, 0.937959, 0.938031, 0.938103, 0.938174, 0.938246, 0.938317, 0.938388, 0.938459, 0.938530, 0.938601, 0.938671, 0.938742, 0.938812, 0.938882, 0.938952, 0.939022, 0.939092, 0.939161, 0.939231, 0.939300, 0.939369, 0.939438, 0.939507, 0.939575, 0.939644, 0.939712, 0.939781, 0.939849, 0.939917, 0.939985, 0.940052, 0.940120, 0.940187, 0.940255, 0.940322, 0.940389, 0.940456, 0.940523, 0.940589, 0.940656, 0.940722, 0.940788, 0.940854, 0.940920, 0.940986, 0.941052, 0.941117, 0.941183, 0.941248, 0.941313, 0.941378, 0.941443, 0.941508, 0.941573, 0.941637, 0.941701, 0.941766, 0.941830, 0.941894, 0.941958, 0.942022, 0.942085, 0.942149, 0.942212, 0.942275, 0.942338, 0.942402, 0.942464, 0.942527, 0.942590, 0.942652, 0.942715, 0.942777, 0.942839, 0.942901, 0.942963, 0.943025, 0.943086, 0.943148, 0.943209, 0.943271, 0.943332, 0.943393, 0.943454, 0.943515, 0.943575, 0.943636, 0.943696, 0.943757, 0.943817, 0.943877, 0.943937, 0.943997, 0.944057, 0.944116, 0.944176, 0.944236, 0.944295, 0.944354, 0.944413, 0.944472, 0.944531, 0.944590, 0.944648, 0.944707, 0.944765, 0.944824, 0.944882, 0.944940, 0.944998, 0.945056, 0.945113, 0.945171, 0.945229, 0.945286, 0.945343, 0.945401, 0.945458, 0.945515, 0.945572, 0.945628, 0.945685, 0.945742, 0.945798, 0.945854, 0.945911, 0.945967, 0.946023, 0.946079, 0.946135, 0.946190, 0.946246, 0.946302, 0.946357, 0.946412, 0.946467, 0.946523, 0.946578, 0.946632, 0.946687, 0.946742, 0.946797, 0.946851, 0.946905, 0.946960, 0.947014, 0.947068, 0.947122, 0.947176, 0.947230, 0.947283, 0.947337, 0.947391, 0.947444, 0.947497, 0.947551, 0.947604, 0.947657, 0.947710, 0.947762, 0.947815, 0.947868, 0.947920, 0.947973, 0.948025, 0.948077, 0.948129, 0.948181, 0.948233, 0.948285, 0.948337, 0.948389, 0.948440, 0.948492, 0.948543, 0.948595, 0.948646, 0.948697, 0.948748, 0.948799, 0.948850, 0.948901, 0.948951, 0.949002, 0.949052, 0.949103, 0.949153, 0.949203, 0.949253, 0.949303, 0.949353, 0.949403, 0.949453, 0.949503, 0.949552, 0.949602, 0.949651, 0.949701, 0.949750, 0.949799, 0.949848, 0.949897, 0.949946, 0.949995, 0.950044, 0.950092, 0.950141, 0.950189, 0.950238, 0.950286, 0.950334, 0.950382, 0.950430, 0.950478, 0.950526, 0.950574, 0.950622, 0.950669, 0.950717, 0.950764, 0.950812, 0.950859, 0.950906, 0.950953, 0.951001, 0.951047, 0.951094, 0.951141, 0.951188, 0.951235, 0.951281, 0.951328, 0.951374, 0.951420, 0.951467, 0.951513, 0.951559, 0.951605, 0.951651, 0.951697, 0.951743, 0.951788, 0.951834, 0.951879, 0.951925, 0.951970, 0.952016, 0.952061, 0.952106, 0.952151, 0.952196, 0.952241, 0.952286, 0.952331, 0.952375, 0.952420, 0.952464, 0.952509, 0.952553, 0.952598, 0.952642, 0.952686, 0.952730, 0.952774, 0.952818, 0.952862, 0.952906, 0.952950, 0.952993, 0.953037, 0.953080, 0.953124, 0.953167, 0.953211, 0.953254, 0.953297, 0.953340, 0.953383, 0.953426, 0.953469, 0.953512, 0.953554, 0.953597, 0.953640, 0.953682, 0.953725, 0.953767, 0.953809, 0.953851, 0.953894, 0.953936, 0.953978, 0.954020, 0.954062, 0.954103, 0.954145, 0.954187, 0.954228, 0.954270, 0.954311, 0.954353, 0.954394, 0.954435, 0.954477, 0.954518, 0.954559, 0.954600, 0.954641, 0.954682, 0.954722, 0.954763, 0.954804, 0.954845, 0.954885, 0.954925, 0.954966, 0.955006, 0.955047, 0.955087, 0.955127, 0.955167, 0.955207, 0.955247, 0.955287, 0.955327, 0.955366, 0.955406, 0.955446, 0.955485, 0.955525, 0.955564, 0.955604, 0.955643, 0.955682, 0.955721, 0.955761, 0.955800, 0.955839, 0.955878, 0.955917, 0.955955, 0.955994, 0.956033, 0.956071, 0.956110, 0.956148, 0.956187, 0.956225, 0.956264, 0.956302, 0.956340, 0.956378, 0.956416, 0.956454, 0.956492, 0.956530, 0.956568, 0.956606, 0.956644, 0.956681, 0.956719, 0.956756, 0.956794, 0.956831, 0.956869, 0.956906, 0.956943, 0.956981, 0.957018, 0.957055, 0.957092, 0.957129, 0.957166, 0.957202, 0.957239, 0.957276, 0.957313, 0.957349, 0.957386, 0.957422, 0.957459, 0.957495, 0.957532, 0.957568, 0.957604, 0.957640, 0.957676, 0.957713, 0.957749, 0.957784, 0.957820, 0.957856, 0.957892, 0.957928, 0.957963, 0.957999, 0.958035, 0.958070, 0.958106, 0.958141, 0.958176, 0.958212, 0.958247, 0.958282, 0.958317, 0.958352, 0.958387, 0.958422, 0.958457, 0.958492, 0.958527, 0.958562, 0.958597, 0.958631, 0.958666, 0.958700, 0.958735, 0.958769, 0.958804, 0.958838, 0.958872, 0.958907, 0.958941, 0.958975, 0.959009, 0.959043, 0.959077, 0.959111, 0.959145, 0.959179, 0.959213, 0.959246, 0.959280, 0.959314, 0.959347, 0.959381, 0.959414, 0.959448, 0.959481, 0.959514, 0.959548, 0.959581, 0.959614, 0.959647, 0.959680, 0.959713, 0.959746, 0.959779, 0.959812, 0.959845, 0.959878, 0.959911, 0.959943, 0.959976, 0.960009, 0.960041, 0.960074, 0.960106, 0.960138, 0.960171, 0.960203, 0.960235, 0.960268, 0.960300, 0.960332, 0.960364, 0.960396, 0.960428, 0.960460, 0.960492, 0.960524, 0.960556, 0.960587, 0.960619, 0.960651, 0.960682, 0.960714, 0.960745, 0.960777, 0.960808, 0.960840, 0.960871, 0.960902, 0.960934, 0.960965, 0.960996, 0.961027, 0.961058, 0.961089, 0.961120, 0.961151, 0.961182, 0.961213, 0.961244, 0.961275, 0.961305, 0.961336, 0.961366, 0.961397, 0.961428, 0.961458, 0.961489, 0.961519, 0.961549, 0.961580, 0.961610, 0.961640, 0.961670, 0.961701, 0.961731, 0.961761, 0.961791, 0.961821, 0.961851, 0.961881, 0.961910, 0.961940, 0.961970, 0.962000, 0.962030, 0.962059, 0.962089, 0.962118, 0.962148, 0.962177, 0.962207, 0.962236, 0.962265, 0.962295, 0.962324, 0.962353, 0.962382, 0.962412, 0.962441, 0.962470, 0.962499, 0.962528, 0.962557, 0.962586, 0.962615, 0.962643, 0.962672, 0.962701, 0.962730, 0.962758, 0.962787, 0.962816, 0.962844, 0.962873, 0.962901, 0.962929, 0.962958, 0.962986, 0.963015, 0.963043, 0.963071, 0.963099, 0.963127, 0.963156, 0.963184, 0.963212, 0.963240, 0.963268, 0.963295, 0.963323, 0.963351, 0.963379, 0.963407, 0.963435, 0.963462, 0.963490, 0.963517, 0.963545, 0.963573, 0.963600, 0.963628, 0.963655, 0.963682, 0.963710, 0.963737, 0.963764, 0.963792, 0.963819, 0.963846, 0.963873, 0.963900, 0.963927, 0.963954, 0.963981, 0.964008, 0.964035, 0.964062, 0.964089, 0.964116, 0.964142, 0.964169, 0.964196, 0.964223, 0.964249, 0.964276, 0.964302, 0.964329, 0.964355, 0.964382, 0.964408, 0.964435, 0.964461, 0.964487, 0.964513, 0.964540, 0.964566, 0.964592, 0.964618, 0.964644, 0.964670, 0.964696, 0.964722, 0.964748, 0.964774, 0.964800, 0.964826, 0.964852, 0.964878, 0.964903, 0.964929, 0.964955, 0.964980, 0.965006, 0.965032, 0.965057, 0.965083, 0.965108, 0.965134, 0.965159, 0.965184, 0.965210, 0.965235, 0.965260, 0.965286, 0.965311, 0.965336, 0.965361, 0.965386, 0.965411, 0.965436, 0.965461, 0.965486, 0.965511, 0.965536, 0.965561, 0.965586, 0.965611, 0.965636, 0.965660, 0.965685, 0.965710, 0.965734, 0.965759, 0.965784, 0.965808, 0.965833, 0.965857, 0.965882, 0.965906, 0.965931, 0.965955, 0.965979, 0.966004, 0.966028, 0.966052, 0.966076, 0.966101, 0.966125, 0.966149, 0.966173, 0.966197, 0.966221, 0.966245, 0.966269, 0.966293, 0.966317, 0.966341, 0.966365, 0.966388, 0.966412, 0.966436, 0.966460, 0.966483, 0.966507, 0.966531, 0.966554, 0.966578, 0.966601, 0.966625, 0.966648, 0.966672, 0.966695, 0.966718, 0.966742, 0.966765, 0.966788, 0.966812, 0.966835, 0.966858, 0.966881, 0.966905, 0.966928, 0.966951, 0.966974, 0.966997, 0.967020, 0.967043, 0.967066, 0.967089, 0.967112, 0.967135, 0.967157, 0.967180, 0.967203, 0.967226, 0.967248, 0.967271, 0.967294, 0.967316, 0.967339, 0.967362, 0.967384, 0.967407, 0.967429, 0.967452, 0.967474, 0.967497, 0.967519, 0.967541, 0.967564, 0.967586, 0.967608, 0.967630, 0.967653, 0.967675, 0.967697, 0.967719, 0.967741, 0.967763, 0.967785, 0.967807, 0.967829, 0.967851, 0.967873, 0.967895, 0.967917, 0.967939, 0.967961, 0.967983, 0.968005, 0.968026, 0.968048, 0.968070, 0.968091, 0.968113, 0.968135, 0.968156, 0.968178, 0.968199, 0.968221, 0.968242, 0.968264, 0.968285, 0.968307, 0.968328, 0.968349, 0.968371, 0.968392, 0.968413, 0.968435, 0.968456, 0.968477, 0.968498, 0.968519, 0.968541, 0.968562, 0.968583, 0.968604, 0.968625, 0.968646, 0.968667, 0.968688, 0.968709, 0.968729, 0.968750, 0.968771, 0.968792, 0.968813, 0.968834, 0.968854, 0.968875, 0.968896, 0.968916, 0.968937, 0.968958, 0.968978, 0.968999, 0.969019, 0.969040, 0.969060, 0.969081, 0.969101, 0.969122, 0.969142, 0.969163, 0.969183, 0.969203, 0.969224, 0.969244, 0.969264, 0.969284, 0.969305, 0.969325, 0.969345, 0.969365, 0.969385, 0.969405, 0.969425, 0.969445, 0.969465, 0.969485, 0.969505, 0.969525, 0.969545, 0.969565, 0.969585, 0.969605, 0.969625, 0.969644, 0.969664, 0.969684, 0.969704, 0.969724, 0.969743, 0.969763, 0.969783, 0.969802, 0.969822, 0.969841, 0.969861, 0.969880, 0.969900, 0.969919, 0.969939, 0.969958, 0.969978, 0.969997, 0.970016, 0.970036, 0.970055, 0.970074, 0.970094, 0.970113, 0.970132, 0.970151, 0.970171, 0.970190, 0.970209, 0.970228, 0.970247, 0.970266, 0.970285, 0.970304, 0.970323, 0.970342, 0.970361, 0.970380, 0.970399, 0.970418, 0.970437, 0.970456, 0.970475, 0.970493, 0.970512, 0.970531, 0.970550, 0.970568, 0.970587, 0.970606, 0.970625, 0.970643, 0.970662, 0.970680, 0.970699, 0.970718, 0.970736, 0.970755, 0.970773, 0.970792, 0.970810, 0.970828, 0.970847, 0.970865, 0.970884, 0.970902, 0.970920, 0.970939, 0.970957, 0.970975, 0.970993, 0.971012, 0.971030, 0.971048, 0.971066, 0.971084, 0.971102, 0.971121, 0.971139, 0.971157, 0.971175, 0.971193, 0.971211, 0.971229, 0.971247, 0.971265, 0.971282, 0.971300, 0.971318, 0.971336, 0.971354, 0.971372, 0.971390, 0.971407, 0.971425, 0.971443, 0.971461, 0.971478, 0.971496, 0.971514, 0.971531, 0.971549, 0.971566, 0.971584, 0.971601, 0.971619, 0.971637, 0.971654, 0.971672, 0.971689, 0.971707, 0.971724, 0.971741, 0.971759, 0.971776, 0.971793, 0.971811, 0.971828, 0.971845, 0.971862, 0.971880, 0.971897, 0.971914, 0.971931, 0.971949, 0.971966, 0.971983, 0.972000, 0.972017, 0.972034, 0.972051, 0.972068, 0.972085, 0.972102, 0.972119, 0.972136, 0.972153, 0.972170, 0.972187, 0.972204, 0.972221, 0.972238, 0.972254, 0.972271, 0.972288, 0.972305, 0.972322, 0.972338, 0.972355, 0.972372, 0.972388, 0.972405, 0.972422, 0.972438, 0.972455, 0.972472, 0.972488, 0.972505, 0.972521, 0.972538, 0.972554, 0.972571, 0.972587, 0.972604, 0.972620, 0.972637, 0.972653, 0.972669, 0.972686, 0.972702, 0.972718, 0.972735, 0.972751, 0.972767, 0.972784, 0.972800, 0.972816, 0.972832, 0.972848, 0.972865, 0.972881, 0.972897, 0.972913, 0.972929, 0.972945, 0.972961, 0.972977, 0.972993, 0.973009, 0.973025, 0.973041, 0.973057, 0.973073, 0.973089, 0.973105, 0.973121, 0.973137, 0.973153, 0.973168, 0.973184, 0.973200, 0.973216, 0.973232, 0.973247, 0.973263, 0.973279, 0.973295, 0.973310, 0.973326, 0.973342, 0.973357, 0.973373, 0.973388, 0.973404, 0.973420, 0.973435, 0.973451, 0.973466, 0.973482, 0.973497, 0.973513, 0.973528, 0.973544, 0.973559, 0.973575, 0.973590, 0.973605, 0.973621, 0.973636, 0.973651, 0.973667, 0.973682, 0.973697, 0.973713, 0.973728, 0.973743, 0.973758, 0.973773, 0.973789, 0.973804, 0.973819, 0.973834, 0.973849, 0.973864, 0.973879, 0.973894, 0.973909, 0.973925, 0.973940, 0.973955, 0.973970, 0.973985, 0.974000, 0.974014, 0.974029, 0.974044, 0.974059, 0.974074, 0.974089, 0.974104, 0.974119, 0.974134, 0.974148, 0.974163, 0.974178, 0.974193, 0.974207, 0.974222, 0.974237, 0.974252, 0.974266, 0.974281, 0.974296, 0.974310, 0.974325, 0.974339, 0.974354, 0.974369, 0.974383, 0.974398, 0.974412, 0.974427, 0.974441, 0.974456, 0.974470, 0.974485, 0.974499, 0.974514, 0.974528, 0.974543, 0.974557, 0.974571, 0.974586, 0.974600, 0.974614, 0.974629, 0.974643, 0.974657, 0.974672, 0.974686, 0.974700, 0.974714, 0.974729, 0.974743, 0.974757, 0.974771, 0.974785, 0.974799, 0.974813, 0.974828, 0.974842, 0.974856, 0.974870, 0.974884, 0.974898, 0.974912, 0.974926, 0.974940, 0.974954, 0.974968, 0.974982, 0.974996, 0.975010, 0.975024, 0.975038, 0.975052, 0.975065, 0.975079, 0.975093, 0.975107, 0.975121, 0.975134, 0.975148, 0.975162, 0.975176, 0.975190, 0.975203, 0.975217, 0.975231, 0.975244, 0.975258, 0.975272, 0.975285, 0.975299, 0.975313, 0.975326, 0.975340, 0.975354, 0.975367, 0.975381, 0.975394, 0.975408, 0.975421, 0.975435, 0.975448, 0.975462, 0.975475, 0.975489, 0.975502, 0.975515, 0.975529, 0.975542, 0.975556, 0.975569, 0.975582, 0.975596, 0.975609, 0.975622, 0.975636, 0.975649, 0.975662, 0.975676, 0.975689, 0.975702, 0.975715, 0.975728, 0.975742, 0.975755, 0.975768, 0.975781, 0.975794, 0.975807, 0.975821, 0.975834, 0.975847, 0.975860, 0.975873, 0.975886, 0.975899, 0.975912, 0.975925, 0.975938, 0.975951, 0.975964, 0.975977, 0.975990, 0.976003, 0.976016, 0.976029, 0.976042, 0.976055, 0.976067, 0.976080, 0.976093, 0.976106, 0.976119, 0.976132, 0.976144, 0.976157, 0.976170, 0.976183, 0.976196, 0.976208, 0.976221, 0.976234, 0.976247, 0.976259, 0.976272, 0.976285, 0.976297, 0.976310, 0.976323, 0.976335, 0.976348, 0.976360, 0.976373, 0.976385, 0.976398, 0.976411, 0.976423, 0.976436, 0.976448, 0.976461, 0.976473, 0.976486, 0.976498, 0.976511, 0.976523, 0.976535, 0.976548, 0.976560, 0.976573, 0.976585, 0.976597, 0.976610, 0.976622, 0.976634, 0.976647, 0.976659, 0.976671, 0.976684, 0.976696, 0.976708, 0.976721, 0.976733, 0.976745, 0.976757, 0.976769, 0.976782, 0.976794, 0.976806, 0.976818, 0.976830, 0.976842, 0.976855, 0.976867, 0.976879, 0.976891, 0.976903, 0.976915, 0.976927, 0.976939, 0.976951, 0.976963, 0.976975, 0.976987, 0.976999, 0.977011, 0.977023, 0.977035, 0.977047, 0.977059, 0.977071, 0.977083, 0.977095, 0.977107, 0.977118, 0.977130, 0.977142, 0.977154, 0.977166, 0.977178, 0.977189, 0.977201, 0.977213, 0.977225, 0.977237, 0.977248, 0.977260, 0.977272, 0.977283, 0.977295, 0.977307, 0.977319, 0.977330, 0.977342, 0.977354, 0.977365, 0.977377, 0.977388, 0.977400, 0.977412, 0.977423, 0.977435, 0.977446, 0.977458, 0.977470, 0.977481, 0.977493, 0.977504, 0.977516, 0.977527, 0.977539, 0.977550, 0.977562, 0.977573, 0.977584, 0.977596, 0.977607, 0.977619, 0.977630, 0.977642, 0.977653, 0.977664, 0.977676, 0.977687, 0.977698, 0.977710, 0.977721, 0.977732, 0.977744, 0.977755, 0.977766, 0.977777, 0.977789, 0.977800, 0.977811, 0.977822, 0.977833, 0.977845, 0.977856, 0.977867, 0.977878, 0.977889, 0.977901, 0.977912, 0.977923, 0.977934, 0.977945, 0.977956, 0.977967, 0.977978, 0.977989, 0.978000, 0.978011, 0.978023, 0.978033, 0.978045, 0.978056, 0.978067, 0.978078, 0.978089, 0.978100, 0.978110, 0.978121, 0.978132, 0.978143, 0.978154, 0.978165, 0.978176, 0.978187, 0.978198, 0.978209, 0.978220, 0.978230, 0.978241, 0.978252, 0.978263, 0.978274, 0.978285, 0.978295, 0.978306, 0.978317, 0.978328, 0.978338, 0.978349, 0.978360, 0.978371, 0.978381, 0.978392, 0.978403, 0.978413, 0.978424, 0.978435, 0.978445, 0.978456, 0.978467, 0.978477, 0.978488, 0.978499, 0.978509, 0.978520, 0.978530, 0.978541, 0.978551, 0.978562, 0.978573, 0.978583, 0.978594, 0.978604, 0.978615, 0.978625, 0.978636, 0.978646, 0.978657, 0.978667, 0.978678, 0.978688, 0.978698, 0.978709, 0.978719, 0.978730, 0.978740, 0.978750, 0.978761, 0.978771, 0.978781, 0.978792, 0.978802, 0.978812, 0.978823, 0.978833, 0.978843, 0.978854, 0.978864, 0.978874, 0.978885, 0.978895, 0.978905, 0.978915, 0.978926, 0.978936, 0.978946, 0.978956, 0.978966, 0.978977, 0.978987, 0.978997, 0.979007, 0.979017, 0.979027, 0.979038, 0.979048, 0.979058, 0.979068, 0.979078, 0.979088, 0.979098, 0.979108, 0.979118, 0.979128, 0.979138, 0.979148, 0.979158, 0.979168, 0.979178, 0.979189, 0.979198, 0.979209, 0.979218, 0.979228, 0.979238, 0.979248, 0.979258, 0.979268, 0.979278, 0.979288, 0.979298, 0.979308, 0.979318, 0.979328, 0.979337, 0.979347, 0.979357, 0.979367, 0.979377, 0.979387, 0.979397, 0.979406, 0.979416, 0.979426, 0.979436, 0.979446, 0.979455, 0.979465, 0.979475, 0.979484, 0.979494, 0.979504, 0.979514, 0.979523, 0.979533, 0.979543, 0.979553, 0.979562, 0.979572, 0.979581, 0.979591, 0.979601, 0.979610, 0.979620, 0.979630, 0.979639, 0.979649, 0.979658, 0.979668, 0.979678, 0.979687, 0.979697, 0.979706, 0.979716, 0.979726, 0.979735, 0.979745, 0.979754, 0.979764, 0.979773, 0.979783, 0.979792, 0.979802, 0.979811, 0.979820, 0.979830, 0.979839, 0.979849, 0.979858, 0.979868, 0.979877, 0.979886, 0.979896, 0.979905, 0.979915, 0.979924, 0.979933, 0.979943, 0.979952, 0.979961, 0.979971, 0.979980, 0.979989, 0.979999, 0.980008, 0.980017, 0.980027, 0.980036, 0.980045, 0.980054, 0.980064, 0.980073, 0.980082, 0.980091, 0.980101, 0.980110, 0.980119, 0.980128, 0.980137, 0.980147, 0.980156, 0.980165, 0.980174, 0.980183, 0.980192, 0.980201, 0.980211, 0.980220, 0.980229, 0.980238, 0.980247, 0.980256, 0.980265, 0.980274, 0.980283, 0.980292, 0.980301, 0.980310, 0.980320, 0.980329, 0.980338, 0.980347, 0.980356, 0.980365, 0.980374, 0.980383, 0.980392, 0.980401, 0.980410, 0.980419, 0.980427, 0.980436, 0.980445, 0.980454, 0.980463, 0.980472, 0.980481, 0.980490, 0.980499, 0.980508, 0.980516, 0.980525, 0.980534, 0.980543, 0.980552, 0.980561, 0.980570, 0.980579, 0.980587, 0.980596, 0.980605, 0.980614, 0.980623, 0.980631, 0.980640, 0.980649, 0.980658, 0.980666, 0.980675, 0.980684, 0.980693, 0.980701, 0.980710, 0.980719, 0.980727, 0.980736, 0.980745, 0.980753, 0.980762, 0.980771, 0.980779, 0.980788, 0.980797, 0.980805, 0.980814, 0.980823, 0.980831, 0.980840, 0.980849, 0.980857, 0.980866, 0.980874, 0.980883, 0.980891, 0.980900, 0.980909, 0.980917, 0.980926, 0.980934, 0.980943, 0.980951, 0.980960, 0.980968, 0.980977, 0.980985, 0.980994, 0.981002, 0.981011, 0.981019, 0.981028, 0.981036, 0.981044, 0.981053, 0.981061, 0.981070, 0.981078, 0.981087, 0.981095, 0.981103, 0.981112, 0.981120, 0.981129, 0.981137, 0.981145, 0.981154, 0.981162, 0.981170, 0.981179, 0.981187, 0.981195, 0.981204, 0.981212, 0.981220, 0.981229, 0.981237, 0.981245, 0.981253, 0.981262, 0.981270, 0.981278, 0.981286, 0.981295, 0.981303, 0.981311, 0.981319, 0.981328, 0.981336, 0.981344, 0.981352, 0.981360, 0.981369, 0.981377, 0.981385, 0.981393, 0.981401, 0.981409, 0.981418, 0.981426, 0.981434, 0.981442, 0.981450, 0.981458, 0.981466, 0.981474, 0.981483, 0.981491, 0.981499, 0.981507, 0.981515, 0.981523, 0.981531, 0.981539, 0.981547, 0.981555, 0.981563, 0.981571, 0.981579, 0.981587, 0.981595, 0.981603, 0.981611, 0.981619, 0.981627, 0.981635, 0.981643, 0.981651, 0.981659, 0.981667, 0.981675, 0.981683, 0.981691, 0.981699, 0.981707, 0.981715, 0.981723, 0.981730, 0.981738, 0.981746, 0.981754, 0.981762, 0.981770, 0.981778, 0.981785, 0.981793, 0.981801, 0.981809, 0.981817, 0.981825, 0.981833, 0.981840, 0.981848, 0.981856, 0.981864, 0.981872, 0.981879, 0.981887, 0.981895, 0.981903, 0.981910, 0.981918, 0.981926, 0.981934, 0.981941, 0.981949, 0.981957, 0.981965, 0.981972, 0.981980, 0.981988, 0.981995, 0.982003, 0.982011, 0.982018, 0.982026, 0.982034, 0.982041, 0.982049, 0.982057, 0.982064, 0.982072, 0.982080, 0.982087, 0.982095, 0.982103, 0.982110, 0.982118, 0.982125, 0.982133, 0.982140, 0.982148, 0.982156, 0.982163, 0.982171, 0.982178, 0.982186, 0.982193, 0.982201, 0.982208, 0.982216, 0.982224, 0.982231, 0.982238, 0.982246, 0.982253, 0.982261, 0.982268, 0.982276, 0.982283, 0.982291, 0.982298, 0.982306, 0.982313, 0.982321, 0.982328, 0.982336, 0.982343, 0.982350, 0.982358, 0.982365, 0.982373, 0.982380, 0.982387, 0.982395, 0.982402, 0.982410, 0.982417, 0.982424, 0.982432, 0.982439, 0.982446, 0.982454, 0.982461, 0.982468, 0.982476, 0.982483, 0.982490, 0.982498, 0.982505, 0.982512, 0.982520, 0.982527, 0.982534, 0.982541, 0.982549, 0.982556, 0.982563, 0.982570, 0.982578, 0.982585, 0.982592, 0.982599, 0.982607, 0.982614, 0.982621, 0.982628, 0.982635, 0.982643, 0.982650, 0.982657, 0.982664, 0.982671, 0.982679, 0.982686, 0.982693, 0.982700, 0.982707, 0.982714, 0.982722, 0.982729, 0.982736, 0.982743, 0.982750, 0.982757, 0.982764, 0.982771, 0.982778, 0.982786, 0.982793, 0.982800, 0.982807, 0.982814, 0.982821, 0.982828, 0.982835, 0.982842, 0.982849, 0.982856, 0.982863, 0.982870, 0.982877, 0.982884, 0.982891, 0.982898, 0.982905, 0.982912, 0.982919, 0.982926, 0.982933, 0.982940, 0.982947, 0.982954, 0.982961, 0.982968, 0.982975, 0.982982, 0.982989, 0.982996, 0.983003, 0.983010, 0.983017, 0.983024, 0.983030, 0.983037, 0.983044, 0.983051, 0.983058, 0.983065, 0.983072, 0.983079, 0.983086, 0.983092, 0.983099, 0.983106, 0.983113, 0.983120, 0.983127, 0.983134, 0.983140, 0.983147, 0.983154, 0.983161, 0.983168, 0.983175, 0.983181, 0.983188, 0.983195, 0.983202, 0.983208, 0.983215, 0.983222, 0.983229, 0.983236, 0.983242, 0.983249, 0.983256, 0.983263, 0.983269, 0.983276, 0.983283, 0.983289, 0.983296, 0.983303, 0.983310, 0.983316, 0.983323, 0.983330, 0.983336, 0.983343, 0.983350, 0.983356, 0.983363, 0.983370, 0.983376, 0.983383, 0.983390, 0.983396, 0.983403, 0.983410, 0.983416, 0.983423, 0.983429, 0.983436, 0.983443, 0.983449, 0.983456, 0.983463, 0.983469, 0.983476, 0.983482, 0.983489, 0.983495, 0.983502, 0.983509, 0.983515, 0.983522, 0.983528, 0.983535, 0.983541, 0.983548, 0.983554, 0.983561, 0.983567, 0.983574, 0.983580, 0.983587, 0.983593, 0.983600, 0.983606, 0.983613, 0.983619, 0.983626, 0.983632, 0.983639, 0.983645, 0.983652, 0.983658, 0.983665, 0.983671, 0.983678, 0.983684, 0.983690, 0.983697, 0.983703, 0.983710, 0.983716, 0.983723, 0.983729, 0.983735, 0.983742, 0.983748, 0.983754, 0.983761, 0.983767, 0.983774, 0.983780, 0.983786, 0.983793, 0.983799, 0.983805, 0.983812, 0.983818, 0.983824, 0.983831, 0.983837, 0.983843, 0.983850, 0.983856, 0.983862, 0.983869, 0.983875, 0.983881, 0.983887, 0.983894, 0.983900, 0.983906, 0.983913, 0.983919, 0.983925, 0.983931, 0.983938, 0.983944, 0.983950, 0.983957, 0.983963, 0.983969, 0.983975, 0.983981, 0.983988, 0.983994, 0.984000, 0.984006, 0.984012, 0.984019, 0.984025, 0.984031, 0.984037, 0.984043, 0.984050, 0.984056, 0.984062, 0.984068, 0.984074, 0.984080, 0.984087, 0.984093, 0.984099, 0.984105, 0.984111, 0.984117, 0.984123, 0.984130, 0.984136, 0.984142, 0.984148, 0.984154, 0.984160, 0.984166, 0.984172, 0.984178, 0.984184, 0.984191, 0.984197, 0.984203, 0.984209, 0.984215, 0.984221, 0.984227, 0.984233, 0.984239, 0.984245, 0.984251, 0.984257, 0.984263, 0.984269, 0.984275, 0.984281, 0.984287, 0.984293, 0.984299, 0.984305, 0.984311, 0.984317, 0.984323, 0.984329, 0.984335, 0.984341, 0.984347, 0.984353, 0.984359, 0.984365, 0.984371, 0.984377, 0.984383, 0.984389, 0.984395, 0.984401, 0.984407, 0.984412, 0.984418, 0.984424, 0.984430, 0.984436, 0.984442, 0.984448, 0.984454, 0.984460, 0.984466, 0.984471, 0.984477, 0.984483, 0.984489, 0.984495, 0.984501, 0.984507, 0.984513, 0.984518, 0.984524, 0.984530, 0.984536, 0.984542, 0.984548, 0.984553, 0.984559, 0.984565, 0.984571, 0.984577, 0.984583, 0.984588, 0.984594, 0.984600, 0.984606, 0.984612, 0.984617, 0.984623, 0.984629, 0.984635, 0.984640, 0.984646, 0.984652, 0.984658, 0.984663, 0.984669, 0.984675, 0.984681, 0.984686, 0.984692, 0.984698, 0.984704, 0.984709, 0.984715, 0.984721, 0.984727, 0.984732, 0.984738, 0.984744, 0.984749, 0.984755, 0.984761, 0.984766, 0.984772, 0.984778, 0.984783, 0.984789, 0.984795, 0.984800, 0.984806, 0.984812, 0.984817, 0.984823, 0.984829, 0.984834, 0.984840, 0.984846, 0.984851, 0.984857, 0.984862, 0.984868, 0.984874, 0.984879, 0.984885, 0.984891, 0.984896, 0.984902, 0.984907, 0.984913, 0.984918, 0.984924, 0.984930, 0.984935, 0.984941, 0.984946, 0.984952, 0.984957, 0.984963, 0.984968, 0.984974, 0.984980, 0.984985, 0.984991, 0.984996, 0.985002, 0.985007, 0.985013, 0.985018, 0.985024, 0.985029, 0.985035, 0.985040, 0.985046, 0.985051, 0.985057, 0.985062, 0.985068, 0.985073, 0.985079, 0.985084, 0.985090, 0.985095, 0.985101, 0.985106, 0.985111, 0.985117, 0.985122, 0.985128, 0.985133, 0.985139, 0.985144, 0.985150, 0.985155, 0.985160, 0.985166, 0.985171, 0.985177, 0.985182, 0.985187, 0.985193, 0.985198, 0.985204, 0.985209, 0.985214, 0.985220, 0.985225, 0.985231, 0.985236, 0.985241, 0.985247, 0.985252, 0.985257, 0.985263, 0.985268, 0.985273, 0.985279, 0.985284, 0.985289, 0.985295, 0.985300, 0.985305, 0.985311, 0.985316, 0.985321, 0.985327, 0.985332, 0.985337, 0.985343, 0.985348, 0.985353, 0.985358, 0.985364, 0.985369, 0.985374, 0.985380, 0.985385, 0.985390, 0.985395, 0.985401, 0.985406, 0.985411, 0.985416, 0.985422, 0.985427, 0.985432, 0.985437, 0.985443, 0.985448, 0.985453, 0.985458, 0.985464, 0.985469, 0.985474, 0.985479, 0.985484, 0.985490, 0.985495, 0.985500, 0.985505, 0.985510, 0.985516, 0.985521, 0.985526, 0.985531, 0.985536, 0.985542, 0.985547, 0.985552, 0.985557, 0.985562, 0.985567, 0.985572, 0.985578, 0.985583, 0.985588, 0.985593, 0.985598, 0.985603, 0.985608, 0.985614, 0.985619, 0.985624, 0.985629, 0.985634, 0.985639, 0.985644, 0.985649, 0.985654, 0.985659, 0.985665, 0.985670, 0.985675, 0.985680, 0.985685, 0.985690, 0.985695, 0.985700, 0.985705, 0.985710, 0.985715, 0.985720, 0.985725, 0.985731, 0.985735, 0.985741, 0.985746, 0.985751, 0.985756, 0.985761, 0.985766, 0.985771, 0.985776, 0.985781, 0.985786, 0.985791, 0.985796, 0.985801, 0.985806, 0.985811, 0.985816, 0.985821, 0.985826, 0.985831, 0.985836, 0.985841, 0.985846, 0.985851, 0.985856, 0.985861, 0.985866, 0.985871, 0.985876, 0.985880, 0.985885, 0.985890, 0.985895, 0.985900, 0.985905, 0.985910, 0.985915, 0.985920, 0.985925, 0.985930, 0.985935, 0.985940, 0.985945, 0.985949, 0.985954, 0.985959, 0.985964, 0.985969, 0.985974, 0.985979, 0.985984, 0.985989, 0.985994, 0.985998, 0.986003, 0.986008, 0.986013, 0.986018, 0.986023, 0.986028, 0.986032, 0.986037, 0.986042, 0.986047, 0.986052, 0.986057, 0.986062, 0.986066, 0.986071, 0.986076, 0.986081, 0.986086, 0.986091, 0.986095, 0.986100, 0.986105, 0.986110, 0.986115, 0.986119, 0.986124, 0.986129, 0.986134, 0.986139, 0.986143, 0.986148, 0.986153, 0.986158, 0.986162, 0.986167, 0.986172, 0.986177, 0.986182, 0.986186, 0.986191, 0.986196, 0.986201, 0.986205, 0.986210, 0.986215, 0.986220, 0.986224, 0.986229, 0.986234, 0.986238, 0.986243, 0.986248, 0.986253, 0.986257, 0.986262, 0.986267, 0.986272, 0.986276, 0.986281, 0.986286, 0.986290, 0.986295, 0.986300, 0.986304, 0.986309, 0.986314, 0.986318, 0.986323, 0.986328, 0.986332, 0.986337, 0.986342, 0.986346, 0.986351, 0.986356, 0.986360, 0.986365, 0.986370, 0.986374, 0.986379, 0.986384, 0.986388, 0.986393, 0.986397, 0.986402, 0.986407, 0.986411, 0.986416, 0.986421, 0.986425, 0.986430, 0.986434, 0.986439, 0.986444, 0.986448, 0.986453, 0.986457, 0.986462, 0.986467, 0.986471, 0.986476, 0.986480, 0.986485, 0.986489, 0.986494, 0.986499, 0.986503, 0.986508, 0.986512, 0.986517, 0.986521, 0.986526, 0.986530, 0.986535, 0.986540, 0.986544, 0.986549, 0.986553, 0.986558, 0.986562, 0.986567, 0.986571, 0.986576, 0.986580, 0.986585, 0.986589, 0.986594, 0.986598, 0.986603, 0.986607, 0.986612, 0.986616, 0.986621, 0.986625, 0.986630, 0.986634, 0.986639, 0.986643, 0.986648, 0.986652, 0.986657, 0.986661, 0.986666, 0.986670, 0.986675, 0.986679, 0.986684, 0.986688, 0.986692, 0.986697, 0.986701, 0.986706, 0.986710, 0.986715, 0.986719, 0.986723, 0.986728, 0.986732, 0.986737, 0.986741, 0.986746, 0.986750, 0.986754, 0.986759, 0.986763, 0.986768, 0.986772, 0.986776, 0.986781, 0.986785, 0.986790, 0.986794, 0.986798, 0.986803, 0.986807, 0.986812, 0.986816, 0.986820, 0.986825, 0.986829, 0.986833, 0.986838, 0.986842, 0.986846, 0.986851, 0.986855, 0.986860, 0.986864, 0.986868, 0.986873, 0.986877, 0.986881, 0.986886, 0.986890, 0.986894, 0.986899, 0.986903, 0.986907, 0.986912, 0.986916, 0.986920, 0.986924, 0.986929, 0.986933, 0.986937, 0.986942, 0.986946, 0.986950, 0.986955, 0.986959, 0.986963, 0.986967, 0.986972, 0.986976, 0.986980, 0.986984, 0.986989, 0.986993, 0.986997, 0.987002, 0.987006, 0.987010, 0.987014, 0.987019, 0.987023, 0.987027, 0.987031, 0.987036, 0.987040, 0.987044, 0.987048, 0.987053, 0.987057, 0.987061, 0.987065, 0.987069, 0.987074, 0.987078, 0.987082, 0.987086, 0.987091, 0.987095, 0.987099, 0.987103, 0.987107, 0.987112, 0.987116, 0.987120, 0.987124, 0.987128, 0.987133, 0.987137, 0.987141, 0.987145, 0.987149, 0.987153, 0.987158, 0.987162, 0.987166, 0.987170, 0.987174, 0.987178, 0.987182, 0.987187, 0.987191, 0.987195, 0.987199, 0.987203, 0.987207, 0.987212, 0.987216, 0.987220, 0.987224, 0.987228, 0.987232, 0.987236, 0.987240, 0.987245, 0.987249, 0.987253, 0.987257, 0.987261, 0.987265, 0.987269, 0.987273, 0.987278, 0.987282, 0.987286, 0.987290, 0.987294, 0.987298, 0.987302, 0.987306, 0.987310, 0.987314, 0.987318, 0.987322, 0.987327, 0.987331, 0.987335, 0.987339, 0.987343, 0.987347, 0.987351, 0.987355, 0.987359, 0.987363, 0.987367, 0.987371, 0.987375, 0.987379, 0.987383, 0.987387, 0.987391, 0.987395, 0.987399, 0.987404, 0.987407, 0.987412, 0.987415, 0.987420, 0.987423, 0.987428, 0.987432, 0.987436, 0.987440, 0.987444, 0.987448, 0.987452, 0.987456, 0.987460, 0.987464, 0.987468, 0.987472, 0.987476, 0.987480, 0.987484, 0.987488, 0.987491, 0.987495, 0.987499, 0.987503, 0.987507, 0.987511, 0.987515, 0.987519, 0.987523, 0.987527, 0.987531, 0.987535, 0.987539, 0.987543, 0.987547, 0.987551, 0.987555, 0.987559, 0.987563, 0.987567, 0.987571, 0.987575, 0.987578, 0.987582, 0.987586, 0.987590, 0.987594, 0.987598, 0.987602, 0.987606, 0.987610, 0.987614, 0.987618, 0.987621, 0.987625, 0.987629, 0.987633, 0.987637, 0.987641, 0.987645, 0.987649, 0.987653, 0.987656, 0.987660, 0.987664, 0.987668, 0.987672, 0.987676, 0.987680, 0.987684, 0.987687, 0.987691, 0.987695, 0.987699, 0.987703, 0.987707, 0.987711, 0.987714, 0.987718, 0.987722, 0.987726, 0.987730, 0.987734, 0.987738, 0.987741, 0.987745, 0.987749, 0.987753, 0.987757, 0.987760, 0.987764, 0.987768, 0.987772, 0.987776, 0.987780, 0.987783, 0.987787, 0.987791, 0.987795, 0.987799, 0.987802, 0.987806, 0.987810, 0.987814, 0.987818, 0.987821, 0.987825, 0.987829, 0.987833, 0.987837, 0.987840, 0.987844, 0.987848, 0.987852, 0.987855, 0.987859, 0.987863, 0.987867, 0.987870, 0.987874, 0.987878, 0.987882, 0.987885, 0.987889, 0.987893, 0.987897, 0.987900, 0.987904, 0.987908, 0.987912, 0.987915, 0.987919, 0.987923, 0.987927, 0.987930, 0.987934, 0.987938, 0.987941, 0.987945, 0.987949, 0.987953, 0.987956, 0.987960, 0.987964, 0.987968, 0.987971, 0.987975, 0.987979, 0.987982, 0.987986, 0.987990, 0.987993, 0.987997, 0.988001, 0.988004, 0.988008, 0.988012, 0.988015, 0.988019, 0.988023, 0.988026, 0.988030, 0.988034, 0.988038, 0.988041, 0.988045, 0.988048, 0.988052, 0.988056, 0.988059, 0.988063, 0.988067, 0.988070, 0.988074, 0.988078, 0.988081, 0.988085, 0.988089, 0.988092, 0.988096, 0.988100, 0.988103, 0.988107, 0.988110, 0.988114, 0.988118, 0.988121, 0.988125, 0.988129, 0.988132, 0.988136, 0.988139, 0.988143, 0.988147, 0.988150, 0.988154, 0.988157, 0.988161, 0.988165, 0.988168, 0.988172, 0.988175, 0.988179, 0.988183, 0.988186, 0.988190, 0.988193, 0.988197, 0.988201, 0.988204, 0.988208, 0.988211, 0.988215, 0.988218, 0.988222, 0.988225, 0.988229, 0.988233, 0.988236, 0.988240, 0.988243, 0.988247, 0.988250, 0.988254, 0.988257, 0.988261, 0.988265, 0.988268, 0.988272, 0.988275, 0.988279, 0.988282, 0.988286, 0.988289, 0.988293, 0.988296, 0.988300, 0.988303, 0.988307, 0.988310, 0.988314, 0.988318, 0.988321, 0.988325, 0.988328, 0.988331, 0.988335, 0.988338, 0.988342, 0.988346, 0.988349, 0.988352, 0.988356, 0.988359, 0.988363, 0.988366, 0.988370, 0.988373, 0.988377, 0.988380, 0.988384, 0.988387, 0.988391, 0.988394, 0.988398, 0.988401, 0.988405, 0.988408, 0.988412, 0.988415, 0.988419, 0.988422, 0.988425, 0.988429, 0.988432, 0.988436, 0.988439, 0.988443, 0.988446, 0.988450, 0.988453, 0.988456, 0.988460, 0.988463, 0.988467, 0.988470, 0.988474, 0.988477, 0.988480, 0.988484, 0.988487, 0.988491, 0.988494, 0.988498, 0.988501, 0.988504, 0.988508, 0.988511, 0.988515, 0.988518, 0.988521, 0.988525, 0.988528, 0.988532, 0.988535, 0.988539, 0.988542, 0.988545, 0.988549, 0.988552, 0.988555, 0.988559, 0.988562, 0.988566, 0.988569, 0.988572, 0.988576, 0.988579, 0.988582, 0.988586, 0.988589, 0.988593, 0.988596, 0.988599, 0.988603, 0.988606, 0.988609, 0.988613, 0.988616, 0.988619, 0.988623, 0.988626, 0.988629, 0.988633, 0.988636, 0.988639, 0.988643, 0.988646, 0.988649, 0.988653, 0.988656, 0.988659, 0.988663, 0.988666, 0.988669, 0.988673, 0.988676, 0.988679, 0.988683, 0.988686, 0.988689, 0.988693, 0.988696, 0.988699, 0.988703, 0.988706, 0.988709, 0.988713, 0.988716, 0.988719, 0.988722, 0.988726, 0.988729, 0.988732, 0.988736, 0.988739, 0.988742, 0.988745, 0.988749, 0.988752, 0.988755, 0.988759, 0.988762, 0.988765, 0.988768, 0.988772, 0.988775, 0.988778, 0.988781, 0.988785, 0.988788, 0.988791, 0.988794, 0.988798, 0.988801, 0.988804, 0.988807, 0.988811, 0.988814, 0.988817, 0.988820, 0.988824, 0.988827, 0.988830, 0.988833, 0.988837, 0.988840, 0.988843, 0.988846, 0.988850, 0.988853, 0.988856, 0.988859, 0.988863, 0.988866, 0.988869, 0.988872, 0.988875, 0.988879, 0.988882, 0.988885, 0.988888, 0.988891, 0.988895, 0.988898, 0.988901, 0.988904, 0.988907, 0.988911, 0.988914, 0.988917, 0.988920, 0.988923, 0.988927, 0.988930, 0.988933, 0.988936, 0.988939, 0.988943, 0.988946, 0.988949, 0.988952, 0.988955, 0.988958, 0.988962, 0.988965, 0.988968, 0.988971, 0.988974, 0.988977, 0.988981, 0.988984, 0.988987, 0.988990, 0.988993, 0.988996, 0.989000, 0.989003, 0.989006, 0.989009, 0.989012, 0.989015, 0.989018, 0.989021, 0.989025, 0.989028, 0.989031, 0.989034, 0.989037, 0.989040, 0.989043, 0.989047, 0.989050, 0.989053, 0.989056, 0.989059, 0.989062, 0.989065, 0.989068, 0.989071, 0.989075, 0.989078, 0.989081, 0.989084, 0.989087, 0.989090, 0.989093, 0.989096, 0.989099, 0.989103, 0.989106, 0.989109, 0.989112, 0.989115, 0.989118, 0.989121, 0.989124, 0.989127, 0.989130, 0.989133, 0.989137, 0.989140, 0.989143, 0.989146, 0.989149, 0.989152, 0.989155, 0.989158, 0.989161, 0.989164, 0.989167, 0.989170, 0.989173, 0.989176, 0.989179, 0.989183, 0.989186, 0.989189, 0.989192, 0.989195, 0.989198, 0.989201, 0.989204, 0.989207, 0.989210, 0.989213, 0.989216, 0.989219, 0.989222, 0.989225, 0.989228, 0.989231, 0.989234, 0.989237, 0.989240, 0.989243, 0.989246, 0.989249, 0.989252, 0.989255, 0.989258, 0.989262, 0.989265, 0.989268, 0.989270, 0.989273, 0.989277, 0.989280, 0.989283, 0.989286, 0.989289, 0.989292, 0.989295, 0.989298, 0.989301, 0.989303, 0.989307, 0.989310, 0.989312, 0.989316, 0.989319, 0.989321, 0.989325, 0.989327, 0.989330, 0.989333, 0.989336, 0.989339, 0.989342, 0.989345, 0.989348, 0.989351, 0.989354, 0.989357, 0.989360, 0.989363, 0.989366, 0.989369, 0.989372, 0.989375, 0.989378, 0.989381, 0.989384, 0.989387, 0.989390, 0.989393, 0.989396, 0.989399, 0.989402, 0.989404, 0.989407, 0.989410, 0.989413, 0.989416, 0.989419, 0.989422, 0.989425, 0.989428, 0.989431, 0.989434, 0.989437, 0.989440, 0.989443, 0.989446, 0.989448, 0.989451, 0.989454, 0.989457, 0.989460, 0.989463, 0.989466, 0.989469, 0.989472, 0.989475, 0.989478, 0.989480, 0.989483, 0.989486, 0.989489, 0.989492, 0.989495, 0.989498, 0.989501, 0.989504, 0.989507, 0.989509, 0.989512, 0.989515, 0.989518, 0.989521, 0.989524, 0.989527, 0.989530, 0.989533, 0.989536, 0.989538, 0.989541, 0.989544, 0.989547, 0.989550, 0.989553, 0.989556, 0.989558, 0.989561, 0.989564, 0.989567, 0.989570, 0.989573, 0.989576, 0.989578, 0.989581, 0.989584, 0.989587, 0.989590, 0.989593, 0.989596, 0.989598, 0.989601, 0.989604, 0.989607, 0.989610, 0.989613, 0.989615, 0.989618, 0.989621, 0.989624, 0.989627, 0.989630, 0.989632, 0.989635, 0.989638, 0.989641, 0.989644, 0.989647, 0.989649, 0.989652, 0.989655, 0.989658, 0.989661, 0.989663, 0.989666, 0.989669, 0.989672, 0.989675, 0.989678, 0.989680, 0.989683, 0.989686, 0.989689, 0.989692, 0.989694, 0.989697, 0.989700, 0.989703, 0.989706, 0.989708, 0.989711, 0.989714, 0.989717, 0.989720, 0.989722, 0.989725, 0.989728, 0.989731, 0.989733, 0.989736, 0.989739, 0.989742, 0.989744, 0.989747, 0.989750, 0.989753, 0.989756, 0.989758, 0.989761, 0.989764, 0.989767, 0.989769, 0.989772, 0.989775, 0.989778, 0.989780, 0.989783, 0.989786, 0.989789, 0.989792, 0.989794, 0.989797, 0.989800, 0.989803, 0.989805, 0.989808, 0.989811, 0.989814, 0.989816, 0.989819, 0.989822, 0.989824, 0.989827, 0.989830, 0.989833, 0.989835, 0.989838, 0.989841, 0.989843, 0.989846, 0.989849, 0.989852, 0.989854, 0.989857, 0.989860, 0.989863, 0.989865, 0.989868, 0.989871, 0.989873, 0.989876, 0.989879, 0.989882, 0.989884, 0.989887, 0.989890, 0.989892, 0.989895, 0.989898, 0.989901, 0.989903, 0.989906, 0.989909, 0.989911, 0.989914, 0.989917, 0.989919, 0.989922, 0.989925, 0.989927, 0.989930, 0.989933, 0.989935, 0.989938, 0.989941, 0.989944, 0.989946, 0.989949, 0.989951, 0.989954, 0.989957, 0.989959, 0.989962, 0.989965, 0.989968, 0.989970, 0.989973, 0.989976, 0.989978, 0.989981, 0.989983, 0.989986, 0.989989, 0.989991, 0.989994, 0.989997, 0.989999, 0.990002, 0.990005, 0.990007, 0.990010, 0.990013, 0.990015, 0.990018, 0.990021, 0.990023, 0.990026, 0.990028, 0.990031, 0.990034, 0.990036, 0.990039, 0.990042, 0.990044, 0.990047, 0.990050, 0.990052, 0.990055, 0.990057, 0.990060, 0.990063, 0.990065, 0.990068, 0.990071, 0.990073, 0.990076, 0.990078, 0.990081, 0.990084, 0.990086, 0.990089, 0.990091, 0.990094, 0.990097, 0.990099, 0.990102, 0.990104, 0.990107, 0.990110, 0.990112, 0.990115, 0.990117, 0.990120, 0.990123, 0.990125, 0.990128, 0.990130, 0.990133, 0.990136, 0.990138, 0.990141, 0.990143, 0.990146, 0.990148, 0.990151, 0.990154, 0.990156, 0.990159, 0.990161, 0.990164, 0.990166, 0.990169, 0.990172, 0.990174, 0.990177, 0.990179, 0.990182, 0.990184, 0.990187, 0.990190, 0.990192, 0.990195, 0.990197, 0.990200, 0.990202, 0.990205, 0.990207, 0.990210, 0.990213, 0.990215, 0.990218, 0.990220, 0.990223, 0.990225, 0.990228, 0.990230, 0.990233, 0.990235, 0.990238, 0.990241, 0.990243, 0.990246, 0.990248, 0.990251, 0.990253, 0.990256, 0.990258, 0.990261, 0.990263, 0.990266, 0.990268, 0.990271, 0.990273, 0.990276, 0.990278, 0.990281, 0.990283, 0.990286, 0.990288, 0.990291, 0.990294, 0.990296, 0.990299, 0.990301, 0.990304, 0.990306, 0.990309, 0.990311, 0.990314, 0.990316, 0.990319, 0.990321, 0.990324, 0.990326, 0.990329, 0.990331, 0.990333, 0.990336, 0.990339, 0.990341, 0.990344, 0.990346, 0.990348, 0.990351, 0.990353, 0.990356, 0.990358, 0.990361, 0.990363, 0.990366, 0.990368, 0.990371, 0.990373, 0.990376, 0.990378, 0.990381, 0.990383, 0.990386, 0.990388, 0.990391, 0.990393, 0.990395, 0.990398, 0.990400, 0.990403, 0.990405, 0.990408, 0.990410, 0.990413, 0.990415, 0.990418, 0.990420, 0.990422, 0.990425, 0.990427, 0.990430, 0.990432, 0.990435, 0.990437, 0.990440, 0.990442, 0.990445, 0.990447, 0.990449, 0.990452, 0.990454, 0.990457, 0.990459, 0.990462, 0.990464, 0.990466, 0.990469, 0.990471, 0.990474, 0.990476, 0.990479, 0.990481, 0.990483, 0.990486, 0.990488, 0.990491, 0.990493, 0.990496, 0.990498, 0.990500, 0.990503, 0.990505, 0.990508, 0.990510, 0.990512, 0.990515, 0.990517, 0.990520, 0.990522, 0.990524, 0.990527, 0.990529, 0.990532, 0.990534, 0.990537, 0.990539, 0.990541, 0.990544, 0.990546, 0.990548, 0.990551, 0.990553, 0.990556, 0.990558, 0.990560, 0.990563, 0.990565, 0.990568, 0.990570, 0.990572, 0.990575, 0.990577, 0.990579, 0.990582, 0.990584, 0.990587, 0.990589, 0.990591, 0.990594, 0.990596, 0.990598, 0.990601, 0.990603, 0.990606, 0.990608, 0.990610, 0.990613, 0.990615, 0.990617, 0.990620, 0.990622, 0.990624, 0.990627, 0.990629, 0.990631, 0.990634, 0.990636, 0.990639, 0.990641, 0.990643, 0.990646, 0.990648, 0.990650, 0.990653, 0.990655, 0.990657, 0.990660, 0.990662, 0.990664, 0.990667, 0.990669, 0.990671, 0.990674, 0.990676, 0.990678, 0.990681, 0.990683, 0.990685, 0.990688, 0.990690, 0.990692, 0.990695, 0.990697, 0.990699, 0.990702, 0.990704, 0.990706, 0.990709, 0.990711, 0.990713, 0.990716, 0.990718, 0.990720, 0.990723, 0.990725, 0.990727, 0.990729, 0.990732, 0.990734, 0.990736, 0.990739, 0.990741, 0.990743, 0.990746, 0.990748, 0.990750, 0.990752, 0.990755, 0.990757, 0.990759, 0.990762, 0.990764, 0.990766, 0.990769, 0.990771, 0.990773, 0.990776, 0.990778, 0.990780, 0.990782, 0.990785, 0.990787, 0.990789, 0.990791, 0.990794, 0.990796, 0.990798, 0.990801, 0.990803, 0.990805, 0.990807, 0.990810, 0.990812, 0.990814, 0.990816, 0.990819, 0.990821, 0.990823, 0.990826, 0.990828, 0.990830, 0.990832, 0.990835, 0.990837, 0.990839, 0.990841, 0.990844, 0.990846, 0.990848, 0.990850, 0.990853, 0.990855, 0.990857, 0.990859, 0.990862, 0.990864, 0.990866, 0.990868, 0.990871, 0.990873, 0.990875, 0.990877, 0.990880, 0.990882, 0.990884, 0.990886, 0.990889, 0.990891, 0.990893, 0.990895, 0.990898, 0.990900, 0.990902, 0.990904, 0.990907, 0.990909, 0.990911, 0.990913, 0.990915, 0.990918, 0.990920, 0.990922, 0.990924, 0.990927, 0.990929, 0.990931, 0.990933, 0.990936, 0.990938, 0.990940, 0.990942, 0.990944, 0.990947, 0.990949, 0.990951, 0.990953, 0.990955, 0.990958, 0.990960, 0.990962, 0.990964, 0.990966, 0.990969, 0.990971, 0.990973, 0.990975, 0.990977, 0.990980, 0.990982, 0.990984, 0.990986, 0.990988, 0.990991, 0.990993, 0.990995, 0.990997, 0.990999, 0.991002, 0.991004, 0.991006, 0.991008, 0.991010, 0.991013, 0.991015, 0.991017, 0.991019, 0.991021, 0.991023, 0.991026, 0.991028, 0.991030, 0.991032, 0.991034, 0.991036, 0.991039, 0.991041, 0.991043, 0.991045, 0.991047, 0.991050, 0.991052, 0.991054, 0.991056, 0.991058, 0.991060, 0.991062, 0.991065, 0.991067, 0.991069, 0.991071, 0.991073, 0.991075, 0.991078, 0.991080, 0.991082 };
       
        if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || (uint64)BlockLastSolved->nHeight < PastBlocksMin) { return pindexLast->nBits; }
        

        for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++)
    	{
             	
    	        if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
                nPastBlocks++;
                
                if (i == 1)        { bnPastTargetAverage.SetCompact(BlockReading->nBits); }
                
                else                { bnPastTargetAverage = ((CBigNum().SetCompact(BlockReading->nBits) - bnPastTargetAveragePrev) / i) + bnPastTargetAveragePrev; }
                bnPastTargetAveragePrev = bnPastTargetAverage;
            
                nActualSeconds                        = BlockLastSolved->GetBlockTime() - BlockReading->GetBlockTime();
                nTargetSeconds                        = TargetBlocksSpacingSeconds * nPastBlocks;
                nBlockTimeRatio                            = 1;
                if (nActualSeconds < 1) { nActualSeconds = 1; }
                
                if (nActualSeconds != 0 && nTargetSeconds != 0) 
                {
                	nBlockTimeRatio= double(nTargetSeconds) / nActualSeconds;
                }
            
            
            if (nPastBlocks >= PastBlocksMin) 
            {
                    if ((nBlockTimeRatio <= SlowBlocksLimit[nPastBlocks-1]) || (nBlockTimeRatio >= FastBlocksLimit[nPastBlocks-1])) { assert(BlockReading); break; }
            }
            
            
            if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
            BlockReading = BlockReading->pprev;
       }
        
    // Limit range of bnPastTargetAverage to a halving or doubling from most recent block target
    if (bnPastTargetAverage < (CBigNum().SetCompact(BlockLastSolved->nBits) / 2)) { bnPastTargetAverage = CBigNum().SetCompact(BlockLastSolved->nBits) / 2; }
    if (bnPastTargetAverage > (CBigNum().SetCompact(BlockLastSolved->nBits) * 2)) { bnPastTargetAverage = CBigNum().SetCompact(BlockLastSolved->nBits) * 2; }
    
    CBigNum bnNew(bnPastTargetAverage);
    
    if (nActualSeconds != 0 && nTargetSeconds != 0) 
    {
       	
    	if ( nActualSeconds > 3 * nTargetSeconds ) { nActualSeconds = 3 * nTargetSeconds; } // Maximal difficulty decrease of /3 from constrained past average     	
    	if ( nActualSeconds < nTargetSeconds / 3 ) { nActualSeconds = nTargetSeconds / 3; } // Maximal difficulty increase of x3 from constrained past average
        
    	bnNew *= nActualSeconds;
        bnNew /= nTargetSeconds;
    }
           
    
    if (bnNew > bnProofOfWorkLimit) { bnNew = bnProofOfWorkLimit; }
    
      
// debug print
    printf("Difficulty Retarget - Boris's Ridiculously Named Difficulty Function\n");
    printf("nHeight = %i\n", pindexLast->nHeight);
    printf("nPastBlocks = %u\n", nPastBlocks);
    printf("nBlockTimeRatio Target/Actual = %.4f\n", nBlockTimeRatio.to_float());
    printf("Mean blocktime = %.1fs\n", TargetBlocksSpacingSeconds / nBlockTimeRatio.to_float());
    printf("SlowBlocksLimit = %.4f\n", SlowBlocksLimit[nPastBlocks-1]);
    printf("FastBlocksLimit = %.4f\n", FastBlocksLimit[nPastBlocks-1]);
    printf("Before: %08x %.8f\n", BlockLastSolved->nBits, GetDifficultyHelper(BlockLastSolved->nBits));
    printf("After: %08x %.8f\n", bnNew.GetCompact(), GetDifficultyHelper(bnNew.GetCompact()));
    printf("Ratio After/Before: %.8f\n", GetDifficultyHelper(bnNew.GetCompact()) / GetDifficultyHelper(BlockLastSolved->nBits));

    return bnNew.GetCompact();
}

unsigned int static GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock)
{

    if(pindexLast == NULL)
    {
        return bnProofOfWorkLimit.GetCompact();
    }

   static const uint32_t        BlocksTargetSpacing                        = 55; // 55 Seconds
        unsigned int                TimeDaySeconds                                = 60 * 60 * 24; // 86400 Seconds
        int64                                PastSecondsMin                                = TimeDaySeconds * .0005; // 7 minutes
        int64                                PastSecondsMax                                = TimeDaySeconds * .007; // 1.7 Hours
        uint32_t                                PastBlocksMin                                = PastSecondsMin / BlocksTargetSpacing; // 36 blocks
        uint32_t                                PastBlocksMax                                = PastSecondsMax / BlocksTargetSpacing; // 1008 blocks

  	if ((pindexLast->nHeight+1) % nInterval != 0) // Retarget every nInterval blocks
    {
        return pindexLast->nBits;
    }
 
    return BorisRidiculouslyNamedDifficultyFunction(pindexLast, BlocksTargetSpacing, PastBlocksMin, PastBlocksMax);
}


bool CheckProofOfWork(uint256 hash, unsigned int nBits)
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Check range
    if (bnTarget <= 0 || bnTarget > bnProofOfWorkLimit)
        return error("CheckProofOfWork() : nBits below minimum work");

    // Check proof of work matches claimed amount
    if (hash > bnTarget.getuint256())
        return error("CheckProofOfWork() : hash doesn't match nBits");

    return true;
}

// Return maximum amount of blocks that other nodes claim to have
int GetNumBlocksOfPeers()
{
    return std::max(cPeerBlockCounts.median(), Checkpoints::GetTotalBlocksEstimate());
}

bool IsInitialBlockDownload()
{
    if (pindexBest == NULL || fImporting || fReindex || nBestHeight < Checkpoints::GetTotalBlocksEstimate())
        return true;
    static int64 nLastUpdate;
    static CBlockIndex* pindexLastBest;
    if (pindexBest != pindexLastBest)
    {
        pindexLastBest = pindexBest;
        nLastUpdate = GetTime();
    }
    return (GetTime() - nLastUpdate < 10 &&
            pindexBest->GetBlockTime() < GetTime() - 24 * 60 * 60);
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (pindexNew->nChainWork > nBestInvalidWork)
    {
        nBestInvalidWork = pindexNew->nChainWork;
        pblocktree->WriteBestInvalidWork(CBigNum(nBestInvalidWork));
        uiInterface.NotifyBlocksChanged();
    }
    printf("InvalidChainFound: invalid block=%s  height=%d  log2_work=%.8g  date=%s\n",
      pindexNew->GetBlockHash().ToString().c_str(), pindexNew->nHeight,
      log(pindexNew->nChainWork.getdouble())/log(2.0), DateTimeStrFormat("%Y-%m-%d %H:%M:%S",
      pindexNew->GetBlockTime()).c_str());
    printf("InvalidChainFound:  current best=%s  height=%d  log2_work=%.8g  date=%s\n",
      hashBestChain.ToString().c_str(), nBestHeight, log(nBestChainWork.getdouble())/log(2.0),
      DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexBest->GetBlockTime()).c_str());
    if (pindexBest && nBestInvalidWork > nBestChainWork + (pindexBest->GetBlockWork() * 6).getuint256())
        printf("InvalidChainFound: Warning: Displayed transactions may not be correct! You may need to upgrade, or other nodes may need to upgrade.\n");
}

void static InvalidBlockFound(CBlockIndex *pindex) {
    pindex->nStatus |= BLOCK_FAILED_VALID;
    pblocktree->WriteBlockIndex(*pindex);
    setBlockIndexValid.erase(pindex);
    InvalidChainFound(pindex);
    if (pindex->pnext) {
        CValidationState stateDummy;
        ConnectBestBlock(stateDummy); // reorganise away from the failed block
    }
}

bool ConnectBestBlock(CValidationState &state) {
    do {
        CBlockIndex *pindexNewBest;

        {
            std::set<CBlockIndex*,CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexValid.rbegin();
            if (it == setBlockIndexValid.rend())
                return true;
            pindexNewBest = *it;
        }

        if (pindexNewBest == pindexBest || (pindexBest && pindexNewBest->nChainWork == pindexBest->nChainWork))
            return true; // nothing to do

        // check ancestry
        CBlockIndex *pindexTest = pindexNewBest;
        std::vector<CBlockIndex*> vAttach;
        do {
            if (pindexTest->nStatus & BLOCK_FAILED_MASK) {
                // mark descendants failed
                CBlockIndex *pindexFailed = pindexNewBest;
                while (pindexTest != pindexFailed) {
                    pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
                    setBlockIndexValid.erase(pindexFailed);
                    pblocktree->WriteBlockIndex(*pindexFailed);
                    pindexFailed = pindexFailed->pprev;
                }
                InvalidChainFound(pindexNewBest);
                break;
            }

            if (pindexBest == NULL || pindexTest->nChainWork > pindexBest->nChainWork)
                vAttach.push_back(pindexTest);

            if (pindexTest->pprev == NULL || pindexTest->pnext != NULL) {
                reverse(vAttach.begin(), vAttach.end());
                BOOST_FOREACH(CBlockIndex *pindexSwitch, vAttach) {
                    boost::this_thread::interruption_point();
                    try {
                        if (!SetBestChain(state, pindexSwitch))
                            return false;
                    } catch(std::runtime_error &e) {
                        return state.Abort(_("System error: ") + e.what());
                    }
                }
                return true;
            }
            pindexTest = pindexTest->pprev;
        } while(true);
    } while(true);
}

void CBlockHeader::UpdateTime(const CBlockIndex* pindexPrev)
{
    nTime = max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    // Updating time can change work required on testnet:
    if (fTestNet)
        nBits = GetNextWorkRequired(pindexPrev, this);
}

const CTxOut &CTransaction::GetOutputFor(const CTxIn& input, CCoinsViewCache& view)
{
    const CCoins &coins = view.GetCoins(input.prevout.hash);
    assert(coins.IsAvailable(input.prevout.n));
    return coins.vout[input.prevout.n];
}

int64 CTransaction::GetValueIn(CCoinsViewCache& inputs) const
{
    if (IsCoinBase() || IsZerocoinSpend())
        return 0;

    int64 nResult = 0;
    for (unsigned int i = 0; i < vin.size(); i++)
        nResult += GetOutputFor(vin[i], inputs).nValue;

    return nResult;
}

unsigned int CTransaction::GetP2SHSigOpCount(CCoinsViewCache& inputs) const
{
    if (IsCoinBase() || IsZerocoinSpend())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const CTxOut &prevout = GetOutputFor(vin[i], inputs);
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(vin[i].scriptSig);
    }
    return nSigOps;
}

void CTransaction::UpdateCoins(CValidationState &state, CCoinsViewCache &inputs, CTxUndo &txundo, int nHeight, const uint256 &txhash) const
{
    bool ret;
    // mark inputs spent
    if (!IsCoinBase() && !IsZerocoinSpend()) {
        BOOST_FOREACH(const CTxIn &txin, vin) {
            CCoins &coins = inputs.GetCoins(txin.prevout.hash);
            CTxInUndo undo;
            ret = coins.Spend(txin.prevout, undo);
            assert(ret);
            txundo.vprevout.push_back(undo);
        }
    }

    // add outputs
    assert(inputs.SetCoins(txhash, CCoins(*this, nHeight)));
}

bool CTransaction::HaveInputs(CCoinsViewCache &inputs) const
{
    if (!IsCoinBase() && !IsZerocoinSpend()) {
        // first check whether information about the prevout hash is available
        for (unsigned int i = 0; i < vin.size(); i++) {
            const COutPoint &prevout = vin[i].prevout;
            if (!inputs.HaveCoins(prevout.hash))
                return false;
        }

        // then check whether the actual outputs are available
        for (unsigned int i = 0; i < vin.size(); i++) {
            const COutPoint &prevout = vin[i].prevout;
            const CCoins &coins = inputs.GetCoins(prevout.hash);
            if (!coins.IsAvailable(prevout.n))
                return false;
        }
    }
    return true;
}

bool CScriptCheck::operator()() const {
    const CScript &scriptSig = ptxTo->vin[nIn].scriptSig;
    if (!VerifyScript(scriptSig, scriptPubKey, *ptxTo, nIn, nFlags, nHashType))
        return error("CScriptCheck() : %s VerifySignature failed", ptxTo->GetHash().ToString().c_str());
    return true;
}

bool VerifySignature(const CCoins& txFrom, const CTransaction& txTo, unsigned int nIn, unsigned int flags, int nHashType)
{
    return CScriptCheck(txFrom, txTo, nIn, flags, nHashType)();
}

bool CTransaction::CheckInputs(CValidationState &state, CCoinsViewCache &inputs, bool fScriptChecks, unsigned int flags, std::vector<CScriptCheck> *pvChecks) const
{
    if (!IsCoinBase() && !IsZerocoinSpend())
    {
        if (pvChecks)
            pvChecks->reserve(vin.size());

        // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
        // for an attacker to attempt to split the network.
        if (!HaveInputs(inputs))
            return state.Invalid(error("CheckInputs() : %s inputs unavailable", GetHash().ToString().c_str()));

        // While checking, GetBestBlock() refers to the parent block.
        // This is also true for mempool checks.
        int nSpendHeight = inputs.GetBestBlock()->nHeight + 1;
        int64 nValueIn = 0;
        int64 nFees = 0;
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            const COutPoint &prevout = vin[i].prevout;
            const CCoins &coins = inputs.GetCoins(prevout.hash);

            // If prev is coinbase, check that it's matured
            if (coins.IsCoinBase()) {
                if (nSpendHeight - coins.nHeight < COINBASE_MATURITY)
                    return state.Invalid(error("CheckInputs() : tried to spend coinbase at depth %d", nSpendHeight - coins.nHeight));
            }

            // Check for negative or overflow input values
            nValueIn += coins.vout[prevout.n].nValue;
            if (!MoneyRange(coins.vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                return state.DoS(100, error("CheckInputs() : txin values out of range"));

        }

        if (nValueIn < GetValueOut())
            return state.DoS(100, error("CheckInputs() : %s value in < value out", GetHash().ToString().c_str()));

        // Tally transaction fees
        int64 nTxFee = nValueIn - GetValueOut();
        if (nTxFee < 0)
            return state.DoS(100, error("CheckInputs() : %s nTxFee < 0", GetHash().ToString().c_str()));
        nFees += nTxFee;
        if (!MoneyRange(nFees))
            return state.DoS(100, error("CheckInputs() : nFees out of range"));

        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.

        // Skip ECDSA signature verification when connecting blocks
        // before the last block chain checkpoint. This is safe because block merkle hashes are
        // still computed and checked, and any change will be caught at the next checkpoint.
        if (fScriptChecks) {
            for (unsigned int i = 0; i < vin.size(); i++) {
                const COutPoint &prevout = vin[i].prevout;
                const CCoins &coins = inputs.GetCoins(prevout.hash);

                // Verify signature
                CScriptCheck check(coins, *this, i, flags, 0);
                if (pvChecks) {
                    pvChecks->push_back(CScriptCheck());
                    check.swap(pvChecks->back());
                } else if (!check()) {
                    if (flags & SCRIPT_VERIFY_STRICTENC) {
                        // For now, check whether the failure was caused by non-canonical
                        // encodings or not; if so, don't trigger DoS protection.
                        CScriptCheck check(coins, *this, i, flags & (~SCRIPT_VERIFY_STRICTENC), 0);
                        if (check())
                            return state.Invalid();
                    }
                    return state.DoS(100,false);
                }
            }
        }
    }

    return true;
}




bool CBlock::DisconnectBlock(CValidationState &state, CBlockIndex *pindex, CCoinsViewCache &view, bool *pfClean)
{
    assert(pindex == view.GetBestBlock());

    if (pfClean)
        *pfClean = false;

    bool fClean = true;

    CBlockUndo blockUndo;
    CDiskBlockPos pos = pindex->GetUndoPos();
    if (pos.IsNull())
        return error("DisconnectBlock() : no undo data available");
    if (!blockUndo.ReadFromDisk(pos, pindex->pprev->GetBlockHash()))
        return error("DisconnectBlock() : failure reading undo data");

    if (blockUndo.vtxundo.size() + 1 != vtx.size())
        return error("DisconnectBlock() : block and undo data inconsistent");

    // undo transactions in reverse order
    for (int i = vtx.size() - 1; i >= 0; i--) {
        const CTransaction &tx = vtx[i];
        uint256 hash = tx.GetHash();

        // check that all outputs are available
        if (!view.HaveCoins(hash)) {
            fClean = fClean && error("DisconnectBlock() : outputs still spent? database corrupted");
            view.SetCoins(hash, CCoins());
        }
        CCoins &outs = view.GetCoins(hash);

        CCoins outsBlock = CCoins(tx, pindex->nHeight);
        // The CCoins serialization does not serialize negative numbers.
        // No network rules currently depend on the version here, so an inconsistency is harmless
        // but it must be corrected before txout nversion ever influences a network rule.
        if (outsBlock.nVersion < 0)
            outs.nVersion = outsBlock.nVersion;
        if (outs != outsBlock)
            fClean = fClean && error("DisconnectBlock() : added transaction mismatch? database corrupted");

        // remove outputs
        outs = CCoins();

        // restore inputs
        //if (i > 0) { // not coinbases
        if (!tx.IsCoinBase() && !tx.IsZerocoinSpend()) {
            const CTxUndo &txundo = blockUndo.vtxundo[i-1];
            if (txundo.vprevout.size() != tx.vin.size())
                return error("DisconnectBlock() : transaction and undo data inconsistent");
            for (unsigned int j = tx.vin.size(); j-- > 0;) {
                const COutPoint &out = tx.vin[j].prevout;
                const CTxInUndo &undo = txundo.vprevout[j];
                CCoins coins;
                view.GetCoins(out.hash, coins); // this can fail if the prevout was already entirely spent
                if (undo.nHeight != 0) {
                    // undo data contains height: this is the last output of the prevout tx being spent
                    if (!coins.IsPruned())
                        fClean = fClean && error("DisconnectBlock() : undo data overwriting existing transaction");
                    coins = CCoins();
                    coins.fCoinBase = undo.fCoinBase;
                    coins.nHeight = undo.nHeight;
                    coins.nVersion = undo.nVersion;
                } else {
                    if (coins.IsPruned())
                        fClean = fClean && error("DisconnectBlock() : undo data adding output to missing transaction");
                }
                if (coins.IsAvailable(out.n))
                    fClean = fClean && error("DisconnectBlock() : undo data overwriting existing output");
                if (coins.vout.size() < out.n+1)
                    coins.vout.resize(out.n+1);
                coins.vout[out.n] = undo.txout;
                if (!view.SetCoins(out.hash, coins))
                    return error("DisconnectBlock() : cannot restore coin inputs");
            }
        }
    }

    // move best block pointer to prevout block
    view.SetBestBlock(pindex->pprev);

    if (pfClean) {
        *pfClean = fClean;
        return true;
    } else {
        return fClean;
    }
}

void static FlushBlockFile(bool fFinalize = false)
{
    LOCK(cs_LastBlockFile);

    CDiskBlockPos posOld(nLastBlockFile, 0);

    FILE *fileOld = OpenBlockFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, infoLastBlockFile.nSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }

    fileOld = OpenUndoFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, infoLastBlockFile.nUndoSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize);

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);

void ThreadScriptCheck() {
    RenameThread("bitcoin-scriptch");
    scriptcheckqueue.Thread();
}

bool CBlock::ConnectBlock(CValidationState &state, CBlockIndex* pindex, CCoinsViewCache &view, bool fJustCheck)
{

    // Check it again in case a previous version let a bad block in
    if (!CheckBlock(state, pindex->nHeight, !fJustCheck, !fJustCheck, false))
        return false;

    // verify that the view's current state corresponds to the previous block
    assert(pindex->pprev == view.GetBestBlock());

    // Special case for the genesis block, skipping connection of its transactions
    // (its coinbase is unspendable)
    if (GetHash() == hashGenesisBlock) {
        view.SetBestBlock(pindex);
        pindexGenesisBlock = pindex;
        return true;
    }

    bool fScriptChecks = pindex->nHeight >= Checkpoints::GetTotalBlocksEstimate();

    // Do not allow blocks that contain transactions which 'overwrite' older transactions,
    // unless those are already completely spent.
    // If such overwrites are allowed, coinbases and transactions depending upon those
    // can be duplicated to remove the ability to spend the first instance -- even after
    // being sent to another address.
    // See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
    // This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
    // already refuses previously-known transaction ids entirely.
    // This rule was originally applied all blocks whose timestamp was after October 1, 2012, 0:00 UTC.
    // Now that the whole chain is irreversibly beyond that time it is applied to all blocks,
    // this prevents exploiting the issue against nodes in their initial block download.
    bool fEnforceBIP30 = true;

    if (fEnforceBIP30) {
        for (unsigned int i=0; i<vtx.size(); i++) {
            uint256 hash = GetTxHash(i);
            if (view.HaveCoins(hash) && !view.GetCoins(hash).IsPruned())
                return state.DoS(100, error("ConnectBlock() : tried to overwrite transaction"));
        }
    }

    // BIP16 didn't become active until Oct 1 2012
    int64 nBIP16SwitchTime = 1349049600;
    bool fStrictPayToScriptHash = (pindex->nTime >= nBIP16SwitchTime);

    unsigned int flags = SCRIPT_VERIFY_NOCACHE |
                         (fStrictPayToScriptHash ? SCRIPT_VERIFY_P2SH : SCRIPT_VERIFY_NONE);

    CBlockUndo blockundo;

    CCheckQueueControl<CScriptCheck> control(fScriptChecks && nScriptCheckThreads ? &scriptcheckqueue : NULL);

    int64 nStart = GetTimeMicros();
    int64 nFees = 0;
    int nInputs = 0;
    unsigned int nSigOps = 0;
    CDiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos> > vPos;
    vPos.reserve(vtx.size());
    for (unsigned int i=0; i<vtx.size(); i++)
    {
        const CTransaction &tx = vtx[i];

        nInputs += tx.vin.size();
        nSigOps += tx.GetLegacySigOpCount();
        if (nSigOps > MAX_BLOCK_SIGOPS)
            return state.DoS(100, error("ConnectBlock() : too many sigops"));

        if (!tx.IsCoinBase() && !tx.IsZerocoinSpend())
        {
            if (!tx.HaveInputs(view))
                return state.DoS(100, error("ConnectBlock() : inputs missing/spent"));

            if (fStrictPayToScriptHash)
            {
                // Add in sigops done by pay-to-script-hash inputs;
                // this is to prevent a "rogue miner" from creating
                // an incredibly-expensive-to-validate block.
                nSigOps += tx.GetP2SHSigOpCount(view);
                if (nSigOps > MAX_BLOCK_SIGOPS)
                     return state.DoS(100, error("ConnectBlock() : too many sigops"));
            }

            nFees += tx.GetValueIn(view)-tx.GetValueOut();

            std::vector<CScriptCheck> vChecks;
            if (!tx.CheckInputs(state, view, fScriptChecks, flags, nScriptCheckThreads ? &vChecks : NULL))
                return false;
            control.Add(vChecks);
        }

        CTxUndo txundo;
        tx.UpdateCoins(state, view, txundo, pindex->nHeight, GetTxHash(i));
        if (!tx.IsCoinBase())
            blockundo.vtxundo.push_back(txundo);

        vPos.push_back(std::make_pair(GetTxHash(i), pos));
        pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
    }
    int64 nTime = GetTimeMicros() - nStart;
    if (fBenchmark)
        printf("- Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin)\n", (unsigned)vtx.size(), 0.001 * nTime, 0.001 * nTime / vtx.size(), nInputs <= 1 ? 0 : 0.001 * nTime / (nInputs-1));

    if (vtx[0].GetValueOut() > GetBlockValue(pindex->nHeight, nFees, pindex->nTime))
        return state.DoS(100, error("ConnectBlock() : coinbase pays too much (actual=%" PRI64d" vs limit=%" PRI64d")", vtx[0].GetValueOut(), GetBlockValue(pindex->nHeight, nFees, pindex->nTime)));

    if (!control.Wait())
        return state.DoS(100, false);
    int64 nTime2 = GetTimeMicros() - nStart;
    if (fBenchmark)
        printf("- Verify %u txins: %.2fms (%.3fms/txin)\n", nInputs - 1, 0.001 * nTime2, nInputs <= 1 ? 0 : 0.001 * nTime2 / (nInputs-1));

    if (fJustCheck)
        return true;

    // Write undo information to disk
    if (pindex->GetUndoPos().IsNull() || (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS)
    {
        if (pindex->GetUndoPos().IsNull()) {
            CDiskBlockPos pos;
            if (!FindUndoPos(state, pindex->nFile, pos, ::GetSerializeSize(blockundo, SER_DISK, CLIENT_VERSION) + 40))
                return error("ConnectBlock() : FindUndoPos failed");
            if (!blockundo.WriteToDisk(pos, pindex->pprev->GetBlockHash()))
                return state.Abort(_("Failed to write undo data"));

            // update nUndoPos in block index
            pindex->nUndoPos = pos.nPos;
            pindex->nStatus |= BLOCK_HAVE_UNDO;
        }

        pindex->nStatus = (pindex->nStatus & ~BLOCK_VALID_MASK) | BLOCK_VALID_SCRIPTS;

        if (!pblocktree->WriteBlockIndex(*pindex))
            return state.Abort(_("Failed to write block index"));
    }

    if (fTxIndex)
        if (!pblocktree->WriteTxIndex(vPos))
            return state.Abort(_("Failed to write transaction index"));

    // add this block to the view's block chain
    assert(view.SetBestBlock(pindex));

    // Watch for transactions paying to me
    for (unsigned int i=0; i<vtx.size(); i++)
        SyncWithWallets(GetTxHash(i), vtx[i], this, true);

    return true;
}

bool SetBestChain(CValidationState &state, CBlockIndex* pindexNew)
{
    // All modifications to the coin state will be done in this cache.
    // Only when all have succeeded, we push it to pcoinsTip.
    CCoinsViewCache view(*pcoinsTip, true);

    // Find the fork (typically, there is none)
    CBlockIndex* pfork = view.GetBestBlock();
    CBlockIndex* plonger = pindexNew;
    while (pfork && pfork != plonger)
    {
        while (plonger->nHeight > pfork->nHeight) {
            plonger = plonger->pprev;
            assert(plonger != NULL);
        }
        if (pfork == plonger)
            break;
        pfork = pfork->pprev;
        assert(pfork != NULL);
    }

    // List of what to disconnect (typically nothing)
    vector<CBlockIndex*> vDisconnect;
    for (CBlockIndex* pindex = view.GetBestBlock(); pindex != pfork; pindex = pindex->pprev)
        vDisconnect.push_back(pindex);

    // List of what to connect (typically only pindexNew)
    vector<CBlockIndex*> vConnect;
    vector<CBlockIndex*> vConnectZC;
    for (CBlockIndex* pindex = pindexNew; pindex != pfork; pindex = pindex->pprev){
        vConnect.push_back(pindex);
        vConnectZC.push_back(pindex);
    }
    reverse(vConnect.begin(), vConnect.end());
    //reverse(vConnectZC.begin(), vConnectZC.end());

    if (vDisconnect.size() > 0) {
        printf("REORGANIZE: Disconnect %" PRIszu" blocks; %s..\n", vDisconnect.size(), pfork->GetBlockHash().ToString().c_str());
        printf("REORGANIZE: Connect %" PRIszu" blocks; ..%s\n", vConnect.size(), pindexNew->GetBlockHash().ToString().c_str());
    }

    // Disconnect shorter branch
    vector<CTransaction> vResurrect;
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect) {
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return state.Abort(_("Failed to read block"));
        int64 nStart = GetTimeMicros();
        if (!block.DisconnectBlock(state, pindex, view))
            return error("SetBestBlock() : DisconnectBlock %s failed", pindex->GetBlockHash().ToString().c_str());
        if (fBenchmark)
            printf("- Disconnect: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);

        // Queue memory transactions to resurrect.
        // We only do this for blocks after the last checkpoint (reorganisation before that
        // point should only happen with -reindex/-loadblock, or a misbehaving peer.
        BOOST_FOREACH(const CTransaction& tx, block.vtx)
            if (!tx.IsCoinBase() && pindex->nHeight > Checkpoints::GetTotalBlocksEstimate())
                vResurrect.push_back(tx);



        // Zerocoin reorg, set mint to height -1, id -1
        list<CZerocoinEntry> listPubCoin = list<CZerocoinEntry>();
        CWalletDB walletdb(pwalletMain->strWalletFile);
        walletdb.ListPubCoin(listPubCoin);

        list<CZerocoinSpendEntry> listCoinSpendSerial;
        walletdb.ListCoinSpendSerial(listCoinSpendSerial);

        BOOST_FOREACH(const CTransaction& tx, block.vtx){

            // Check Spend Zerocoin Transaction
            if (tx.IsZerocoinSpend())
            {
                BOOST_FOREACH(const CZerocoinSpendEntry& item, listCoinSpendSerial) {
                    if (item.hashTx == tx.GetHash()) {
                        BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                            if (pubCoinItem.value == item.pubCoin) {
                                CZerocoinEntry pubCoinTx;
                                pubCoinTx.nHeight = pubCoinItem.nHeight;
                                pubCoinTx.denomination = pubCoinItem.denomination;
                                // UPDATE FOR INDICATE IT HAS BEEN RESET
                                pubCoinTx.IsUsed = false;
                                pubCoinTx.randomness = pubCoinItem.randomness;
                                pubCoinTx.serialNumber = pubCoinItem.serialNumber;
                                pubCoinTx.value = pubCoinItem.value;
                                pubCoinTx.id = pubCoinItem.id;
                                walletdb.WriteZerocoinEntry(pubCoinTx);

                                pwalletMain->NotifyZerocoinChanged(pwalletMain, pubCoinItem.value.GetHex(), "New", CT_UPDATED);
                                walletdb.EarseCoinSpendSerialEntry(item);
                                pwalletMain->EraseFromWallet(item.hashTx);
                             }
                        }
                     }
                }
            }

            // Check Mint Zerocoin Transaction
            BOOST_FOREACH(const CTxOut txout, tx.vout) {
                if (!txout.scriptPubKey.empty() && txout.scriptPubKey.IsZerocoinMint()) {
                    vector<unsigned char> vchZeroMint;
                    vchZeroMint.insert(vchZeroMint.end(), txout.scriptPubKey.begin() + 6, txout.scriptPubKey.begin() + txout.scriptPubKey.size());

                    CBigNum pubCoin;
                    pubCoin.setvch(vchZeroMint);
                    int zercoinMintHeight = -1;

                    BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                        if (pubCoinItem.value == pubCoin) {
                            zercoinMintHeight = pubCoinItem.nHeight;
                            CZerocoinEntry pubCoinTx;
                            pubCoinTx.id = -1;
                            pubCoinTx.IsUsed = pubCoinItem.IsUsed;
                            pubCoinTx.randomness = pubCoinItem.randomness;
                            pubCoinTx.denomination = pubCoinItem.denomination;
                            pubCoinTx.serialNumber = pubCoinItem.serialNumber;
                            pubCoinTx.value = pubCoin;
                            pubCoinTx.nHeight = -1;
                            printf("- Disconnect Reset Pubcoin Id: %d Height: %d\n", pubCoinTx.id, pindex->nHeight);
                            walletdb.WriteZerocoinEntry(pubCoinTx);
                        }

                    }

                    BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                        if (pubCoinItem.nHeight > zercoinMintHeight) {
                            CZerocoinEntry pubCoinTx;
                            pubCoinTx.id = -1;
                            pubCoinTx.IsUsed = pubCoinItem.IsUsed;
                            pubCoinTx.randomness = pubCoinItem.randomness;
                            pubCoinTx.denomination = pubCoinItem.denomination;
                            pubCoinTx.serialNumber = pubCoinItem.serialNumber;
                            pubCoinTx.value = pubCoin;
                            pubCoinTx.nHeight = -1;
                            printf("- Disconnect Reset Pubcoin Id: %d Height: %d\n", pubCoinTx.id, pindex->nHeight);
                            walletdb.WriteZerocoinEntry(pubCoinTx);
                        }

                    }
                }
            }
        }
    }



    // Connect longer branch
    vector<CTransaction> vDelete;
    BOOST_FOREACH(CBlockIndex *pindex, vConnect) {
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return state.Abort(_("Failed to read block"));
        int64 nStart = GetTimeMicros();
        if (!block.ConnectBlock(state, pindex, view)) {
            if (state.IsInvalid()) {
                InvalidChainFound(pindexNew);
                InvalidBlockFound(pindex);
            }
            return error("SetBestBlock() : ConnectBlock %s failed", pindex->GetBlockHash().ToString().c_str());
        }
        if (fBenchmark)
            printf("- Connect: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);

        // Queue memory transactions to delete
        BOOST_FOREACH(const CTransaction& tx, block.vtx)
            vDelete.push_back(tx);

    }

    BOOST_FOREACH(CBlockIndex *pindex, vConnect) {
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return state.Abort(_("Failed to read block"));
        /*
        int64 nStart = GetTimeMicros();
        if (!block.ConnectBlock(state, pindex, view)) {
            if (state.IsInvalid()) {
                InvalidChainFound(pindexNew);
                InvalidBlockFound(pindex);
            }
            return error("SetBestBlock() : ConnectBlock %s failed", pindex->GetBlockHash().ToString().c_str());
        }
        if (fBenchmark)
            printf("- Connect: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
        */


        // Zerocoin reorg, calculate new height and id
        list<CZerocoinEntry> listPubCoin = list<CZerocoinEntry>();
        CWalletDB walletdb(pwalletMain->strWalletFile);
        walletdb.ListPubCoin(listPubCoin);

        BOOST_FOREACH(const CTransaction& tx, block.vtx){
            // Check Mint Zerocoin Transaction
            BOOST_FOREACH(const CTxOut txout, tx.vout) {
                if (!txout.scriptPubKey.empty() && txout.scriptPubKey.IsZerocoinMint()) {
                    vector<unsigned char> vchZeroMint;
                    vchZeroMint.insert(vchZeroMint.end(), txout.scriptPubKey.begin() + 6, txout.scriptPubKey.begin() + txout.scriptPubKey.size());

                    CBigNum pubCoin;
                    pubCoin.setvch(vchZeroMint);
                    int zercoinMintHeight = -1;

                    BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                        if (pubCoinItem.value == pubCoin) {

                            CZerocoinEntry pubCoinTx;
                            pubCoinTx.id = -1;
                            pubCoinTx.IsUsed = pubCoinItem.IsUsed;
                            pubCoinTx.randomness = pubCoinItem.randomness;
                            pubCoinTx.denomination = pubCoinItem.denomination;
                            pubCoinTx.serialNumber = pubCoinItem.serialNumber;
                            pubCoinTx.value = pubCoinItem.value;
                            pubCoinTx.nHeight = -1;
                            walletdb.WriteZerocoinEntry(pubCoinTx);
                            printf("- Connect Reset Pubcoin Denomination: %d Pubcoin Id: %d Height: %d\n", pubCoinTx.denomination, pubCoinTx.id, pindex->nHeight);
                            zercoinMintHeight = pindex->nHeight;
                        }
                    }


                    BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                        if (pubCoinItem.nHeight > zercoinMintHeight) {
                            CZerocoinEntry pubCoinTx;
                            pubCoinTx.id = -1;
                            pubCoinTx.IsUsed = pubCoinItem.IsUsed;
                            pubCoinTx.randomness = pubCoinItem.randomness;
                            pubCoinTx.denomination = pubCoinItem.denomination;
                            pubCoinTx.serialNumber = pubCoinItem.serialNumber;
                            pubCoinTx.value = pubCoin;
                            pubCoinTx.nHeight = -1;
                            printf("- Connect Reset Pubcoin Denomination: %d Pubcoin Id: %d Height: %d\n", pubCoinTx.denomination, pubCoinTx.id, pindex->nHeight);
                            walletdb.WriteZerocoinEntry(pubCoinTx);
                        }

                    }


                }
            }
        }
    }


    // Rearrange Zerocoin Mint
    BOOST_FOREACH(CBlockIndex *pindex, vConnectZC) {
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return state.Abort(_("Failed to read block"));
        /*
        int64 nStart = GetTimeMicros();
        if (!block.ConnectBlock(state, pindex, view)) {
            if (state.IsInvalid()) {
                InvalidChainFound(pindexNew);
                InvalidBlockFound(pindex);
            }
            return error("SetBestBlock() : ConnectBlock %s failed", pindex->GetBlockHash().ToString().c_str());
        }
        if (fBenchmark)
            printf("- Connect: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
        */

        // Zerocoin reorg, calculate new height and id
        list<CZerocoinEntry> listPubCoin = list<CZerocoinEntry>();
        CWalletDB walletdb(pwalletMain->strWalletFile);
        walletdb.ListPubCoin(listPubCoin);

        BOOST_FOREACH(const CTransaction& tx, block.vtx){
            // Check Mint Zerocoin Transaction
            BOOST_FOREACH(const CTxOut txout, tx.vout) {
                if (!txout.scriptPubKey.empty() && txout.scriptPubKey.IsZerocoinMint()) {
                    vector<unsigned char> vchZeroMint;
                    vchZeroMint.insert(vchZeroMint.end(), txout.scriptPubKey.begin() + 6, txout.scriptPubKey.begin() + txout.scriptPubKey.size());

                    CBigNum pubCoin;
                    pubCoin.setvch(vchZeroMint);

                    BOOST_FOREACH(const CZerocoinEntry& pubCoinItem, listPubCoin) {
                        if (pubCoinItem.value == pubCoin) {

                            CZerocoinEntry pubCoinTx;

                            // PUBCOIN IS IN DB, BUT NOT UPDATE ID
                            printf("UPDATING\n");
                            // GET MAX ID
                            int currentId = 1;
                            BOOST_FOREACH(const CZerocoinEntry& maxIdPubcoin, listPubCoin) {
                                if (maxIdPubcoin.id > currentId && maxIdPubcoin.denomination == pubCoinItem.denomination && maxIdPubcoin.id > 0) {
                                    currentId = maxIdPubcoin.id;
                                }
                            }

                            // FIND HOW MANY OF MAX ID
                            unsigned int countExistingItems = 0;
                            BOOST_FOREACH(const CZerocoinEntry& countItemPubcoin, listPubCoin) {
                                if (currentId == countItemPubcoin.id && countItemPubcoin.denomination == pubCoinItem.denomination && countItemPubcoin.id > 0) {
                                    countExistingItems++;
                                }
                                printf("pubCoinItem.id = %d\n", countItemPubcoin.id);
                            }

                            // IF IT IS NOT 10 -> ADD MORE
                            if (countExistingItems < 10) {
                                pubCoinTx.id = currentId;
                            }
                            else {// ELSE INCREASE 1 -> ADD
                                currentId += 1;
                                pubCoinTx.id = currentId;
                            }

                            pubCoinTx.IsUsed = pubCoinItem.IsUsed;
                            pubCoinTx.randomness = pubCoinItem.randomness;
                            pubCoinTx.denomination = pubCoinItem.denomination;
                            pubCoinTx.serialNumber = pubCoinItem.serialNumber;
                            pubCoinTx.value = pubCoinItem.value;
                            pubCoinTx.nHeight = pindex->nHeight;
                            printf("REORG PUBCOIN DENOMINATION: %d PUBCOIN ID: %d HEIGHT: %d\n", pubCoinTx.denomination, pubCoinTx.id, pubCoinTx.nHeight);
                            walletdb.WriteZerocoinEntry(pubCoinTx);
                        }
                    }
                }
            }

        }

        walletdb.WriteCalculatedZCBlock(pindex->nHeight);
    }

    // Flush changes to global coin state
    int64 nStart = GetTimeMicros();
    int nModified = view.GetCacheSize();
    assert(view.Flush());
    int64 nTime = GetTimeMicros() - nStart;
    if (fBenchmark)
        printf("- Flush %i transactions: %.2fms (%.4fms/tx)\n", nModified, 0.001 * nTime, 0.001 * nTime / nModified);

    // Make sure it's successfully written to disk before changing memory structure
    bool fIsInitialDownload = IsInitialBlockDownload();
    if (!fIsInitialDownload || pcoinsTip->GetCacheSize() > nCoinCacheSize) {
        // Typical CCoins structures on disk are around 100 bytes in size.
        // Pushing a new one to the database can cause it to be written
        // twice (once in the log, and once in the tables). This is already
        // an overestimation, as most will delete an existing entry or
        // overwrite one. Still, use a conservative safety factor of 2.
        if (!CheckDiskSpace(100 * 2 * 2 * pcoinsTip->GetCacheSize()))
            return state.Error();
        FlushBlockFile();
        pblocktree->Sync();
        if (!pcoinsTip->Flush())
            return state.Abort(_("Failed to write to coin database"));
    }

    // At this point, all changes have been done to the database.
    // Proceed by updating the memory structures.

    // Disconnect shorter branch
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
        if (pindex->pprev)
            pindex->pprev->pnext = NULL;

    // Connect longer branch
    BOOST_FOREACH(CBlockIndex* pindex, vConnect)
        if (pindex->pprev)
            pindex->pprev->pnext = pindex;

    // Resurrect memory transactions that were in the disconnected branch
    BOOST_FOREACH(CTransaction& tx, vResurrect) {
        // ignore validation errors in resurrected transactions
        CValidationState stateDummy;
        if (!tx.AcceptToMemoryPool(stateDummy, true, false))
            mempool.remove(tx, true);
    }

    // Delete redundant memory transactions that are in the connected branch
    BOOST_FOREACH(CTransaction& tx, vDelete) {
        mempool.remove(tx);
        mempool.removeConflicts(tx);
    }

    // Update best block in wallet (so we can detect restored wallets)
    if ((pindexNew->nHeight % 20160) == 0 || (!fIsInitialDownload && (pindexNew->nHeight % 144) == 0))
    {
        const CBlockLocator locator(pindexNew);
        ::SetBestChain(locator);
    }

    // New best block
    hashBestChain = pindexNew->GetBlockHash();
    pindexBest = pindexNew;
    pblockindexFBBHLast = NULL;
    nBestHeight = pindexBest->nHeight;
    nBestChainWork = pindexNew->nChainWork;
    nTimeBestReceived = GetTime();
    nTransactionsUpdated++;
    printf("SetBestChain: new best=%s  height=%d  log2_work=%.8g  tx=%lu  date=%s progress=%f\n",
      hashBestChain.ToString().c_str(), nBestHeight, log(nBestChainWork.getdouble())/log(2.0), (unsigned long)pindexNew->nChainTx,
      DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexBest->GetBlockTime()).c_str(),
      Checkpoints::GuessVerificationProgress(pindexBest));

    // Check the version of the last 100 blocks to see if we need to upgrade:
    if (!fIsInitialDownload)
    {
        int nUpgraded = 0;
        const CBlockIndex* pindex = pindexBest;
        for (int i = 0; i < 100 && pindex != NULL; i++)
        {
            // mask out the high bits of nVersion;
            // since they indicate merged mining information
            if ((pindex->nVersion&0xff) > CBlock::CURRENT_VERSION)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            printf("SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, CBlock::CURRENT_VERSION);
        if (nUpgraded > 100/2)
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: This version is obsolete, upgrade required!");
    }

    std::string strCmd = GetArg("-blocknotify", "");

    if (!fIsInitialDownload && !strCmd.empty())
    {
        boost::replace_all(strCmd, "%s", hashBestChain.GetHex());
        boost::thread t(runCommand, strCmd); // thread runs free
    }

    return true;
}


bool CBlock::AddToBlockIndex(CValidationState &state, const CDiskBlockPos &pos)
{
    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return state.Invalid(error("AddToBlockIndex() : %s already exists", hash.ToString().c_str()));

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(*this);
    assert(pindexNew);
    BlockMap::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = mapBlockIndex.find(hashPrevBlock);
    if (miPrev != mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
    }
    pindexNew->nTx = vtx.size();
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + pindexNew->GetBlockWork().getuint256();
    pindexNew->nChainTx = (pindexNew->pprev ? pindexNew->pprev->nChainTx : 0) + pindexNew->nTx;
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus = BLOCK_VALID_TRANSACTIONS | BLOCK_HAVE_DATA;
    setBlockIndexValid.insert(pindexNew);

    /* write both the immutible data (CDiskBlockIndex) and the mutable data (BlockIndex) */
    if (!pblocktree->WriteDiskBlockIndex(CDiskBlockIndex(pindexNew)) || !pblocktree->WriteBlockIndex(*pindexNew))
        return state.Abort(_("Failed to write block index"));

    // New best?
    if (!ConnectBestBlock(state))
        return false;

    if (pindexNew == pindexBest)
    {
        // Notify UI to display prev block's coinbase if it was ours
        static uint256 hashPrevBestCoinBase;
        UpdatedTransaction(hashPrevBestCoinBase);
        hashPrevBestCoinBase = GetTxHash(0);
    }

    if (!pblocktree->Flush())
        return state.Abort(_("Failed to sync block index"));

    uiInterface.NotifyBlocksChanged();
    return true;
}

bool CBlockHeader::CheckProofOfWork(int nHeight) const
{
    return true;
}

bool FindBlockPos(CValidationState &state, CDiskBlockPos &pos, unsigned int nAddSize, unsigned int nHeight, uint64 nTime, bool fKnown = false)
{
    bool fUpdatedLast = false;

    LOCK(cs_LastBlockFile);

    if (fKnown) {
        if (nLastBlockFile != pos.nFile) {
            nLastBlockFile = pos.nFile;
            infoLastBlockFile.SetNull();
            pblocktree->ReadBlockFileInfo(nLastBlockFile, infoLastBlockFile);
            fUpdatedLast = true;
        }
    } else {
        while (infoLastBlockFile.nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            printf("Leaving block file %i: %s\n", nLastBlockFile, infoLastBlockFile.ToString().c_str());
            FlushBlockFile(true);
            nLastBlockFile++;
            infoLastBlockFile.SetNull();
            pblocktree->ReadBlockFileInfo(nLastBlockFile, infoLastBlockFile); // check whether data for the new file somehow already exist; can fail just fine
            fUpdatedLast = true;
        }
        pos.nFile = nLastBlockFile;
        pos.nPos = infoLastBlockFile.nSize;
    }

    infoLastBlockFile.nSize += nAddSize;
    infoLastBlockFile.AddBlock(nHeight, nTime);

    if (!fKnown) {
        unsigned int nOldChunks = (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks = (infoLastBlockFile.nSize + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        if (nNewChunks > nOldChunks) {
            if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos)) {
                FILE *file = OpenBlockFile(pos);
                if (file) {
                    printf("Pre-allocating up to position 0x%x in blk%05u.dat\n", nNewChunks * BLOCKFILE_CHUNK_SIZE, pos.nFile);
                    AllocateFileRange(file, pos.nPos, nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos);
                    fclose(file);
                }
            }
            else
                return state.Error();
        }
    }

    if (!pblocktree->WriteBlockFileInfo(nLastBlockFile, infoLastBlockFile))
        return state.Abort(_("Failed to write file info"));
    if (fUpdatedLast)
        pblocktree->WriteLastBlockFile(nLastBlockFile);

    return true;
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    unsigned int nNewSize;
    if (nFile == nLastBlockFile) {
        pos.nPos = infoLastBlockFile.nUndoSize;
        nNewSize = (infoLastBlockFile.nUndoSize += nAddSize);
        if (!pblocktree->WriteBlockFileInfo(nLastBlockFile, infoLastBlockFile))
            return state.Abort(_("Failed to write block info"));
    } else {
        CBlockFileInfo info;
        if (!pblocktree->ReadBlockFileInfo(nFile, info))
            return state.Abort(_("Failed to read block info"));
        pos.nPos = info.nUndoSize;
        nNewSize = (info.nUndoSize += nAddSize);
        if (!pblocktree->WriteBlockFileInfo(nFile, info))
            return state.Abort(_("Failed to write block info"));
    }

    unsigned int nOldChunks = (pos.nPos + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    unsigned int nNewChunks = (nNewSize + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    if (nNewChunks > nOldChunks) {
        if (CheckDiskSpace(nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos)) {
            FILE *file = OpenUndoFile(pos);
            if (file) {
                printf("Pre-allocating up to position 0x%x in rev%05u.dat\n", nNewChunks * UNDOFILE_CHUNK_SIZE, pos.nFile);
                AllocateFileRange(file, pos.nPos, nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos);
                fclose(file);
            }
        }
        else
            return state.Error();
    }

    return true;
}


bool CBlock::CheckBlock(CValidationState &state, int nHeight, bool fCheckPOW, bool fCheckMerkleRoot, bool isVerifyDB) const
{
    // These are checks that are independent of context
    // that can be verified before saving an orphan block.

    // Size limits
    if (vtx.empty() || vtx.size() > MAX_BLOCK_SIZE || ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return state.DoS(100, error("CheckBlock() : size limits failed"));

    // SmartCash: Special short-term limits to avoid 10,000 BDB lock limit:
    if (GetBlockTime() < 1376568000)  // stop enforcing 15 August 2013 00:00:00
    {
        // Rule is: #unique txids referenced <= 4,500
        // ... to prevent 10,000 BDB lock exhaustion on old clients
        set<uint256> setTxIn;
        for (size_t i = 0; i < vtx.size(); i++)
        {
            setTxIn.insert(vtx[i].GetHash());
            if (i == 0) continue; // skip coinbase txin
            BOOST_FOREACH(const CTxIn& txin, vtx[i].vin)
                setTxIn.insert(txin.prevout.hash);
        }
        size_t nTxids = setTxIn.size();
        if (nTxids > 4500)
            return error("CheckBlock() : 15 August maxlocks violation");
    }

    CBlockIndex* pindexPrev = NULL;
    if (GetHash() != hashGenesisBlock)
    {
        int nHeightCheckPoW = 0;
        BlockMap::iterator mi = mapBlockIndex.find(hashPrevBlock);
        pindexPrev = (*mi).second;
        if (mi != mapBlockIndex.end())
        {
            if (pindexPrev != NULL)
            {
                nHeightCheckPoW = pindexPrev->nHeight+1;
                // Check proof of work matches claimed amount
                if (fCheckPOW && !CheckProofOfWork(nHeightCheckPoW))
                    return state.DoS(50, error("CheckBlock() : proof of work failed"));
            }
        }
    }


    // Check timestamp
    if (GetBlockTime() > GetAdjustedTime() + 2 * 60 * 60)
        return state.Invalid(error("CheckBlock() : block timestamp too far in the future"));

    // First transaction must be coinbase, the rest must not be
    if (vtx.empty() || !vtx[0].IsCoinBase())
        return state.DoS(100, error("CheckBlock() : first tx is not coinbase"));
    for (unsigned int i = 1; i < vtx.size(); i++)
        if (vtx[i].IsCoinBase())
            return state.DoS(100, error("CheckBlock() : more than one coinbase"));


    // Check transactions
    BOOST_FOREACH(const CTransaction& tx, vtx){
        if (!tx.CheckTransaction(state, tx.GetHash(), isVerifyDB, nHeight))
            return error("CheckBlock() : CheckTransaction failed");
    }

    // Build the merkle tree already. We need it anyway later, and it makes the
    // block cache the transaction hashes, which means they don't need to be
    // recalculated many times during this block's validation.
    BuildMerkleTree();

    // Check for duplicate txids. This is caught by ConnectInputs(),
    // but catching it earlier avoids a potential DoS attack:
    set<uint256> uniqueTx;
    for (unsigned int i=0; i<vtx.size(); i++) {
        uniqueTx.insert(GetTxHash(i));
    }
    if (uniqueTx.size() != vtx.size())
        return state.DoS(100, error("CheckBlock() : duplicate transaction"), true);

    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        nSigOps += tx.GetLegacySigOpCount();
    }
    if (nSigOps > MAX_BLOCK_SIGOPS)
        return state.DoS(100, error("CheckBlock() : out-of-bounds SigOpCount"));

    // Check merkle root
    if (fCheckMerkleRoot && hashMerkleRoot != BuildMerkleTree())
        return state.DoS(100, error("CheckBlock() : hashMerkleRoot mismatch"));

    return true;
}

bool CBlock::AcceptBlock(CValidationState &state, CDiskBlockPos *dbp)
{
    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return state.Invalid(error("AcceptBlock() : block already in mapBlockIndex"));

    // Get prev block index
    CBlockIndex* pindexPrev = NULL;
    int nHeight = 0;
    if (hash != hashGenesisBlock) {
        BlockMap::iterator mi = mapBlockIndex.find(hashPrevBlock);
        if (mi == mapBlockIndex.end())
            return state.DoS(10, error("AcceptBlock() : prev block not found"));
        pindexPrev = (*mi).second;
        nHeight = pindexPrev->nHeight+1;
//        LastHeight = pindexPrev->nHeight;

        // Check proof of work
        if (nBits != GetNextWorkRequired(pindexPrev, this))
            return state.DoS(100, error("AcceptBlock() : incorrect proof of work"));

        // Check timestamp against prev
        if (GetBlockTime() <= pindexPrev->GetMedianTimePast())
            return state.Invalid(error("AcceptBlock() : block's timestamp is too early"));

        // Check that all transactions are finalized
        BOOST_FOREACH(const CTransaction& tx, vtx)
            if (!tx.IsFinal(nHeight, GetBlockTime()))
                return state.DoS(10, error("AcceptBlock() : contains a non-final transaction"));

        // Check that the block chain matches the known block chain up to a checkpoint
        if (!Checkpoints::CheckBlock(nHeight, hash))
            return state.DoS(100, error("AcceptBlock() : rejected by checkpoint lock-in at %d", nHeight));

        // Don't accept any forks from the main chain prior to last checkpoint
        CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(mapBlockIndex);
        if (pcheckpoint && nHeight < pcheckpoint->nHeight)
            return state.DoS(100, error("AcceptBlock() : forked chain older than last checkpoint (height %d)", nHeight));

        // Reject block.nVersion=1 blocks when 95% (75% on testnet) of the network has upgraded:
        if ((nVersion&0xff) < 2)
        {
            if ((!fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 950, 1000)) ||
                (fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 75, 100)))
            {
                return state.Invalid(error("AcceptBlock() : rejected nVersion=1 block"));
            }
        }
        // Enforce block.nVersion=2 rule that the coinbase starts with serialized block height
        if ((nVersion&0xff) >= 2)
        {
            // if 750 of the last 1,000 blocks are version 2 or greater (51/100 if testnet):
            if ((!fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 750, 1000)) ||
                (fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 51, 100)))
            {
                CScript expect = CScript() << nHeight;
                if (vtx[0].vin[0].scriptSig.size() < expect.size() ||
                    !std::equal(expect.begin(), expect.end(), vtx[0].vin[0].scriptSig.begin()))
                    return state.DoS(100, error("AcceptBlock() : block height mismatch in coinbase"));
            }
        }
    }

    // Write block to history file
    try {
        unsigned int nBlockSize = ::GetSerializeSize(*this, SER_DISK, CLIENT_VERSION);
        CDiskBlockPos blockPos;
        if (dbp != NULL)
            blockPos = *dbp;
        if (!FindBlockPos(state, blockPos, nBlockSize+8, nHeight, nTime, dbp != NULL))
            return error("AcceptBlock() : FindBlockPos failed");
        if (dbp == NULL)
            if (!WriteToDisk(blockPos))
                return state.Abort(_("Failed to write block"));
        if (!AddToBlockIndex(state, blockPos))
            return error("AcceptBlock() : AddToBlockIndex failed");
    } catch(std::runtime_error &e) {
        return state.Abort(_("System error: ") + e.what());
    }

    // Relay inventory, but don't relay old inventory during initial block download
    int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate();
    if (hashBestChain == hash)
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            if (nBestHeight > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate))
                pnode->PushInventory(CInv(MSG_BLOCK, hash));
    }

    return true;
}

bool CBlockIndex::IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck)
{
    // SmartCash: temporarily disable v2 block lockin until we are ready for v2 transition
    return false;
    unsigned int nFound = 0;
    for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; i++)
    {
        if ((pstart->nVersion&0xff) >= minVersion)
            ++nFound;
        pstart = pstart->pprev;
    }
    return (nFound >= nRequired);
}

bool ProcessBlock(CValidationState &state, CNode* pfrom, CBlock* pblock, CDiskBlockPos *dbp)
{
    // Check for duplicate
    uint256 hash = pblock->GetHash();
    if (mapBlockIndex.count(hash))
        return state.Invalid(error("ProcessBlock() : already have block %d %s", mapBlockIndex[hash]->nHeight, hash.ToString().c_str()));
    if (mapOrphanBlocks.count(hash))
        return state.Invalid(error("ProcessBlock() : already have block (orphan) %s", hash.ToString().c_str()));

    // Preliminary checks
    if (!pblock->CheckBlock(state, INT_MAX, true, true, false))
        return error("ProcessBlock() : CheckBlock FAILED");


    CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(mapBlockIndex);
    if (pcheckpoint && pblock->hashPrevBlock != hashBestChain)
    {
        // Extra checks to prevent "fill up memory by spamming with bogus blocks"
        int64 deltaTime = pblock->GetBlockTime() - pcheckpoint->nTime;
        if (deltaTime < 0)
        {
            return state.DoS(100, error("ProcessBlock() : block with timestamp before last checkpoint"));
        }
        CBigNum bnNewBlock;
        bnNewBlock.SetCompact(pblock->nBits);
        CBigNum bnRequired;
        bnRequired.SetCompact(ComputeMinWork(pcheckpoint->nBits, deltaTime));
        if (bnNewBlock > bnRequired)
        {
            return state.DoS(100, error("ProcessBlock() : block with too little proof-of-work"));
        }
    }


    // If we don't already have its previous block, shunt it off to holding area until we get it
    if (pblock->hashPrevBlock != 0 && !mapBlockIndex.count(pblock->hashPrevBlock))
    {
        printf("ProcessBlock: ORPHAN BLOCK, prev=%s\n", pblock->hashPrevBlock.ToString().c_str());

        // Accept orphans as long as there is a node to request its parents from
        if (pfrom) {
            CBlock* pblock2 = new CBlock(*pblock);
            mapOrphanBlocks.insert(make_pair(hash, pblock2));
            mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrevBlock, pblock2));

            // Ask this guy to fill in what we're missing
            pfrom->PushGetBlocks(pindexBest, GetOrphanRoot(pblock2));
        }
        return true;
    }

    // Store to disk
    if (!pblock->AcceptBlock(state, dbp))
        return error("ProcessBlock() : AcceptBlock FAILED");

    // Recursively process any orphan blocks that depended on this one
    vector<uint256> vWorkQueue;
    vWorkQueue.push_back(hash);
    for (unsigned int i = 0; i < vWorkQueue.size(); i++)
    {
        uint256 hashPrev = vWorkQueue[i];
        for (multimap<uint256, CBlock*>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
             mi != mapOrphanBlocksByPrev.upper_bound(hashPrev);
             ++mi)
        {
            CBlock* pblockOrphan = (*mi).second;
            // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan resolution (that is, feeding people an invalid block based on LegitBlockX in order to get anyone relaying LegitBlockX banned)
            CValidationState stateDummy;
            if (pblockOrphan->AcceptBlock(stateDummy))
                vWorkQueue.push_back(pblockOrphan->GetHash());
            mapOrphanBlocks.erase(pblockOrphan->GetHash());
            delete pblockOrphan;
        }
        mapOrphanBlocksByPrev.erase(hashPrev);
    }

    printf("ProcessBlock: ACCEPTED\n");
    return true;
}








CMerkleBlock::CMerkleBlock(const CBlock& block, CBloomFilter& filter)
{
    header = block.GetBlockHeader();

    vector<bool> vMatch;
    vector<uint256> vHashes;

    vMatch.reserve(block.vtx.size());
    vHashes.reserve(block.vtx.size());

    for (unsigned int i = 0; i < block.vtx.size(); i++)
    {
        uint256 hash = block.vtx[i].GetHash();
        if (filter.IsRelevantAndUpdate(block.vtx[i], hash))
        {
            vMatch.push_back(true);
            vMatchedTxn.push_back(make_pair(i, hash));
        }
        else
            vMatch.push_back(false);
        vHashes.push_back(hash);
    }

    txn = CPartialMerkleTree(vHashes, vMatch);
}








uint256 CPartialMerkleTree::CalcHash(int height, unsigned int pos, const std::vector<uint256> &vTxid) {
    if (height == 0) {
        // hash at height 0 is the txids themself
        return vTxid[pos];
    } else {
        // calculate left hash
        uint256 left = CalcHash(height-1, pos*2, vTxid), right;
        // calculate right hash if not beyong the end of the array - copy left hash otherwise1
        if (pos*2+1 < CalcTreeWidth(height-1))
            right = CalcHash(height-1, pos*2+1, vTxid);
        else
            right = left;
        // combine subhashes
//        return Hash(BEGIN(left), END(left), BEGIN(right), END(right));
        return Hash4(BEGIN(left), END(left), BEGIN(right), END(right));
    }
}

void CPartialMerkleTree::TraverseAndBuild(int height, unsigned int pos, const std::vector<uint256> &vTxid, const std::vector<bool> &vMatch) {
    // determine whether this node is the parent of at least one matched txid
    bool fParentOfMatch = false;
    for (unsigned int p = pos << height; p < (pos+1) << height && p < nTransactions; p++)
        fParentOfMatch |= vMatch[p];
    // store as flag bit
    vBits.push_back(fParentOfMatch);
    if (height==0 || !fParentOfMatch) {
        // if at height 0, or nothing interesting below, store hash and stop
        vHash.push_back(CalcHash(height, pos, vTxid));
    } else {
        // otherwise, don't store any hash, but descend into the subtrees
        TraverseAndBuild(height-1, pos*2, vTxid, vMatch);
        if (pos*2+1 < CalcTreeWidth(height-1))
            TraverseAndBuild(height-1, pos*2+1, vTxid, vMatch);
    }
}

uint256 CPartialMerkleTree::TraverseAndExtract(int height, unsigned int pos, unsigned int &nBitsUsed, unsigned int &nHashUsed, std::vector<uint256> &vMatch) {
    if (nBitsUsed >= vBits.size()) {
        // overflowed the bits array - failure
        fBad = true;
        return 0;
    }
    bool fParentOfMatch = vBits[nBitsUsed++];
    if (height==0 || !fParentOfMatch) {
        // if at height 0, or nothing interesting below, use stored hash and do not descend
        if (nHashUsed >= vHash.size()) {
            // overflowed the hash array - failure
            fBad = true;
            return 0;
        }
        const uint256 &hash = vHash[nHashUsed++];
        if (height==0 && fParentOfMatch) // in case of height 0, we have a matched txid
            vMatch.push_back(hash);
        return hash;
    } else {
        // otherwise, descend into the subtrees to extract matched txids and hashes
        uint256 left = TraverseAndExtract(height-1, pos*2, nBitsUsed, nHashUsed, vMatch), right;
        if (pos*2+1 < CalcTreeWidth(height-1))
            right = TraverseAndExtract(height-1, pos*2+1, nBitsUsed, nHashUsed, vMatch);
        else
            right = left;
        // and combine them before returning
//        return Hash(BEGIN(left), END(left), BEGIN(right), END(right));
        return Hash4(BEGIN(left), END(left), BEGIN(right), END(right));
    }
}

CPartialMerkleTree::CPartialMerkleTree(const std::vector<uint256> &vTxid, const std::vector<bool> &vMatch) : nTransactions(vTxid.size()), fBad(false) {
    // reset state
    vBits.clear();
    vHash.clear();

    // calculate height of tree
    int nHeight = 0;
    while (CalcTreeWidth(nHeight) > 1)
        nHeight++;

    // traverse the partial tree
    TraverseAndBuild(nHeight, 0, vTxid, vMatch);
}

CPartialMerkleTree::CPartialMerkleTree() : nTransactions(0), fBad(true) {}

uint256 CPartialMerkleTree::ExtractMatches(std::vector<uint256> &vMatch) {
    vMatch.clear();
    // An empty set will not work
    if (nTransactions == 0)
        return 0;
    // check for excessively high numbers of transactions
    if (nTransactions > MAX_BLOCK_SIZE / 60) // 60 is the lower bound for the size of a serialized CTransaction
        return 0;
    // there can never be more hashes provided than one for every txid
    if (vHash.size() > nTransactions)
        return 0;
    // there must be at least one bit per node in the partial tree, and at least one node per hash
    if (vBits.size() < vHash.size())
        return 0;
    // calculate height of tree
    int nHeight = 0;
    while (CalcTreeWidth(nHeight) > 1)
        nHeight++;
    // traverse the partial tree
    unsigned int nBitsUsed = 0, nHashUsed = 0;
    uint256 hashMerkleRoot = TraverseAndExtract(nHeight, 0, nBitsUsed, nHashUsed, vMatch);
    // verify that no problems occured during the tree traversal
    if (fBad)
        return 0;
    // verify that all bits were consumed (except for the padding caused by serializing it as a byte sequence)
    if ((nBitsUsed+7)/8 != (vBits.size()+7)/8)
        return 0;
    // verify that all hashes were consumed
    if (nHashUsed != vHash.size())
        return 0;
    return hashMerkleRoot;
}







bool AbortNode(const std::string &strMessage) {
    strMiscWarning = strMessage;
    printf("*** %s\n", strMessage.c_str());
    uiInterface.ThreadSafeMessageBox(strMessage, "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

bool CheckDiskSpace(uint64 nAdditionalBytes)
{
    uint64 nFreeBytesAvailable = filesystem::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
        return AbortNode(_("Error: Disk space is low!"));

    return true;
}

CCriticalSection cs_LastBlockFile;
CBlockFileInfo infoLastBlockFile;
int nLastBlockFile = 0;

FILE* OpenDiskFile(const CDiskBlockPos &pos, const char *prefix, bool fReadOnly)
{
    if (pos.IsNull())
        return NULL;
    boost::filesystem::path path = GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, pos.nFile);
    boost::filesystem::create_directories(path.parent_path());
    FILE* file = fopen(path.string().c_str(), "rb+");
    if (!file && !fReadOnly)
        file = fopen(path.string().c_str(), "wb+");
    if (!file) {
        printf("Unable to open file %s\n", path.string().c_str());
        return NULL;
    }
    if (pos.nPos) {
        if (fseek(file, pos.nPos, SEEK_SET)) {
            printf("Unable to seek to position %u of %s\n", pos.nPos, path.string().c_str());
            fclose(file);
            return NULL;
        }
    }
    return file;
}

FILE* OpenBlockFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "blk", fReadOnly);
}

FILE* OpenUndoFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "rev", fReadOnly);
}

CBlockIndex * InsertBlockIndex(uint256 hash)
{
    if (hash == 0)
        return NULL;

    // Return existing
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw runtime_error("LoadBlockIndex() : new CBlockIndex failed");
    mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

bool static LoadBlockIndexDB()
{
    if (!pblocktree->LoadBlockIndexGuts())
        return false;

    boost::this_thread::interruption_point();

    // Calculate nChainWork
    vector<pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    BOOST_FOREACH(const PAIRTYPE(uint256, CBlockIndex*)& item, mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    BOOST_FOREACH(const PAIRTYPE(int, CBlockIndex*)& item, vSortedByHeight)
    {
        CBlockIndex* pindex = item.second;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + pindex->GetBlockWork().getuint256();
        pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TRANSACTIONS && !(pindex->nStatus & BLOCK_FAILED_MASK))
            setBlockIndexValid.insert(pindex);
    }

    // Load block file info
    pblocktree->ReadLastBlockFile(nLastBlockFile);
    printf("LoadBlockIndexDB(): last block file = %i\n", nLastBlockFile);
    if (pblocktree->ReadBlockFileInfo(nLastBlockFile, infoLastBlockFile))
        printf("LoadBlockIndexDB(): last block file info: %s\n", infoLastBlockFile.ToString().c_str());

    // Load nBestInvalidWork, OK if it doesn't exist
    CBigNum bnBestInvalidWork;
    pblocktree->ReadBestInvalidWork(bnBestInvalidWork);
    nBestInvalidWork = bnBestInvalidWork.getuint256();

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    pblocktree->ReadReindexing(fReindexing);
    fReindex |= fReindexing;

    // Check whether we have a transaction index
    pblocktree->ReadFlag("txindex", fTxIndex);
    printf("LoadBlockIndexDB(): transaction index %s\n", fTxIndex ? "enabled" : "disabled");

    // Load hashBestChain pointer to end of best chain
    pindexBest = pcoinsTip->GetBestBlock();
    if (pindexBest == NULL)
        return true;
    hashBestChain = pindexBest->GetBlockHash();
    nBestHeight = pindexBest->nHeight;
    nBestChainWork = pindexBest->nChainWork;

    // set 'next' pointers in best chain
    CBlockIndex *pindex = pindexBest;
    while(pindex != NULL && pindex->pprev != NULL) {
         CBlockIndex *pindexPrev = pindex->pprev;
         pindexPrev->pnext = pindex;
         pindex = pindexPrev;
    }
    printf("LoadBlockIndexDB(): hashBestChain=%s  height=%d date=%s\n",
        hashBestChain.ToString().c_str(), nBestHeight,
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexBest->GetBlockTime()).c_str());

    return true;
}

bool VerifyDB(int nCheckLevel, int nCheckDepth)
{
    if (pindexBest == NULL || pindexBest->pprev == NULL)
        return true;

    // Verify blocks in the best chain
    if (nCheckDepth <= 0)
        nCheckDepth = 1000000000; // suffices until the year 19000
    if (nCheckDepth > nBestHeight)
        nCheckDepth = nBestHeight;
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    printf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
    CCoinsViewCache coins(*pcoinsTip, true);
    CBlockIndex* pindexState = pindexBest;
    CBlockIndex* pindexFailure = NULL;
    int nGoodTransactions = 0;
    CValidationState state;
    for (CBlockIndex* pindex = pindexBest; pindex && pindex->pprev; pindex = pindex->pprev)
    {
        boost::this_thread::interruption_point();
        if (pindex->nHeight < nBestHeight-nCheckDepth)
            break;
        CBlock block;
        // check level 0: read from disk
        if (!block.ReadFromDisk(pindex))
            return error("VerifyDB() : *** block.ReadFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
        // check level 1: verify block validity
        if (nCheckLevel >= 1 && !block.CheckBlock(state, pindex->nHeight, true, true, true))
            return error("VerifyDB() : *** found bad block at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
        // check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            CDiskBlockPos pos = pindex->GetUndoPos();
            if (!pos.IsNull()) {
                if (!undo.ReadFromDisk(pos, pindex->pprev->GetBlockHash()))
                    return error("VerifyDB() : *** found bad undo data at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
            }
        }
        // check level 3: check for inconsistencies during memory-only disconnect of tip blocks
        if (nCheckLevel >= 3 && pindex == pindexState && (coins.GetCacheSize() + pcoinsTip->GetCacheSize()) <= 2*nCoinCacheSize + 32000) {
            bool fClean = true;
            if (!block.DisconnectBlock(state, pindex, coins, &fClean))
                return error("VerifyDB() : *** irrecoverable inconsistency in block data at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
            pindexState = pindex->pprev;
            if (!fClean) {
                nGoodTransactions = 0;
                pindexFailure = pindex;
            } else
                nGoodTransactions += block.vtx.size();
        }
    }
    if (pindexFailure)
        return error("VerifyDB() : *** coin database inconsistencies found (last %i blocks, %i good transactions before that)\n", pindexBest->nHeight - pindexFailure->nHeight + 1, nGoodTransactions);

    // check level 4: try reconnecting blocks
    if (nCheckLevel >= 4) {
        CBlockIndex *pindex = pindexState;
        while (pindex != pindexBest) {
            boost::this_thread::interruption_point();
            pindex = pindex->pnext;
            CBlock block;
            if (!block.ReadFromDisk(pindex))
                return error("VerifyDB() : *** block.ReadFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
            if (!block.ConnectBlock(state, pindex, coins))
                return error("VerifyDB() : *** found unconnectable block at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
        }
    }

    printf("No coin database inconsistencies in last %i blocks (%i transactions)\n", pindexBest->nHeight - pindexState->nHeight, nGoodTransactions);

    return true;
}

void UnloadBlockIndex()
{
    mapBlockIndex.clear();
    setBlockIndexValid.clear();
    pindexGenesisBlock = NULL;
    nBestHeight = 0;
    nBestChainWork = 0;
    nBestInvalidWork = 0;
    hashBestChain = 0;
    pindexBest = NULL;
    BOOST_FOREACH(BlockMap::value_type& entry, mapBlockIndex) {
        delete entry.second;
    }
    mapBlockIndex.clear();
}

bool LoadBlockIndex()
{
    if (fTestNet)
    {
        pchMessageStart[0] = 0xcf;
        pchMessageStart[1] = 0xfc;
        pchMessageStart[2] = 0xbe;
        pchMessageStart[3] = 0xea;
        hashGenesisBlock = uint256("0x0000027235b5679bcd28c90d03d4bf1a9ba4c07c4efcc1c87d6c68cce25e6e5d");
//        hashGenesisBlock = uint256("0x6c88eb567e5ea876b7082c16a07041bf6c1eb0cb42389323f73a908f9f32b2af");
    }

    //
    // Load block index from databases
    //
    if (!fReindex && !LoadBlockIndexDB())
        return false;

    return true;
}


bool InitBlockIndex() {
    // Check whether we're already initialized
    if (pindexGenesisBlock != NULL)
        return true;

    // Use the provided setting for -txindex in the new database
    fTxIndex = GetBoolArg("-txindex", false);
    pblocktree->WriteFlag("txindex", fTxIndex);
    printf("Initializing databases...\n");

    // Only add the genesis block if not reindexing (in which case we reuse the one already on disk)
    if (!fReindex) {

        // Genesis block
        const char* pszTimestamp = "SmartCash, Communinty Driven Cash";
        CTransaction txNew;
        vector<unsigned char> extraNonce(4);
        unsigned int startBits;
        startBits = 0x1e0ffff0;

        if(fTestNet) {
            extraNonce[0] = 0x09;
            extraNonce[1] = 0x00;
            extraNonce[2] = 0x00;
            extraNonce[3] = 0x00;

        } else {
            extraNonce[0] = 0x83;
            extraNonce[1] = 0x3e;
            extraNonce[2] = 0x00;
            extraNonce[3] = 0x00;
        }

        txNew.vin.resize(1);
        txNew.vout.resize(1);
	txNew.vin[0].scriptSig = CScript() << bnProofOfWorkLimit.GetCompact() << CBigNum(4) << vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp)) << extraNonce;
        txNew.vout[0].nValue = 0 * COIN;

        txNew.vout[0].scriptPubKey = CScript();
        CBlock block;
        block.vtx.push_back(txNew);
        block.hashPrevBlock = 0;
        block.hashMerkleRoot = block.BuildMerkleTree();
        block.nVersion = 2;
        block.nTime    = 1496467978; // Mon, 10 Apr 2017 00:33:25 GMT
        block.nBits    = startBits;
        block.nNonce   = 245887;

        if (fTestNet)
        {
            block.nTime    = 1496467978; // Mon, 10 Apr 2017 00:33:25 GMT
            block.nNonce   = 420977;
        }

        //// debug print
        uint256 hash = block.GetHash();
        printf("block.GetHash = %s\n", hash.ToString().c_str());
        printf("hashGenesisBlock = %s\n", hashGenesisBlock.ToString().c_str());
        printf("block.hashMerkleRoot = %s\n", block.hashMerkleRoot.ToString().c_str());

        uint256 genMerkleRoot;
        if(fTestNet)
            genMerkleRoot.SetHex("0xb344094bc70d6a82c2c33f6d21005b78d83524b4f976b8eacf0e71ccc6488aee");
        else
            genMerkleRoot.SetHex("0xb79187d8ce4d5ec398730dd34276248f1e7b09d98ca29b829e5e5e67ff21d462");
        

        assert(block.hashMerkleRoot == genMerkleRoot);
        block.print();
//        assert(hash == hashGenesisBlock);

    // If genesis block hash does not match, then generate new genesis hash.
    if (block.GetHash() != hashGenesisBlock)
    {
        printf("Searching for genesis block...\n");
        // This will figure out a valid hash and Nonce if you're
        // creating a different genesis block:
        uint256 hashTarget = CBigNum().SetCompact(block.nBits).getuint256();
        uint256 thash;

        while(true)
        {
	    thash = block.GetHash();
            if (thash <= hashTarget)
                break;
            if ((block.nNonce & 0xFFF) == 0)
            {
                printf("nonce %08X: hash = %s (target = %s)\n", block.nNonce, thash.ToString().c_str(), hashTarget.ToString().c_str());
            }
            ++block.nNonce;
            if (block.nNonce == 0)
            {
                printf("NONCE WRAPPED, incrementing time\n");
                ++block.nTime;
            }
        }
        printf("block.nTime = %u \n", block.nTime);
        printf("block.nNonce = %u \n", block.nNonce);
        printf("block.GetHash = %s\n", block.GetHash().ToString().c_str());
    }

        // Start new block file
        try {
            unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
            CDiskBlockPos blockPos;
            CValidationState state;
            if (!FindBlockPos(state, blockPos, nBlockSize+8, 0, block.nTime))
                return error("LoadBlockIndex() : FindBlockPos failed");
            if (!block.WriteToDisk(blockPos))
                return error("LoadBlockIndex() : writing genesis block to disk failed");
            if (!block.AddToBlockIndex(state, blockPos))
                return error("LoadBlockIndex() : genesis block not accepted");
        } catch(std::runtime_error &e) {
            return error("LoadBlockIndex() : failed to initialize block database: %s", e.what());
        }
    }

    return true;
}



void PrintBlockTree()
{
    // pre-compute tree structure
    map<CBlockIndex*, vector<CBlockIndex*> > mapNext;
    for (BlockMap::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex* pindex = (*mi).second;
        mapNext[pindex->pprev].push_back(pindex);
    }

    vector<pair<int, CBlockIndex*> > vStack;
    vStack.push_back(make_pair(0, pindexGenesisBlock));

    int nPrevCol = 0;
    while (!vStack.empty())
    {
        int nCol = vStack.back().first;
        CBlockIndex* pindex = vStack.back().second;
        vStack.pop_back();

        // print split or gap
        if (nCol > nPrevCol)
        {
            for (int i = 0; i < nCol-1; i++)
                printf("| ");
            printf("|\\\n");
        }
        else if (nCol < nPrevCol)
        {
            for (int i = 0; i < nCol; i++)
                printf("| ");
            printf("|\n");
       }
        nPrevCol = nCol;

        // print columns
        for (int i = 0; i < nCol; i++)
            printf("| ");

        // print item
        CBlock block;
        block.ReadFromDisk(pindex);
        printf("%d (blk%05u.dat:0x%x)  %s  tx %" PRIszu"",
            pindex->nHeight,
            pindex->GetBlockPos().nFile, pindex->GetBlockPos().nPos,
            DateTimeStrFormat("%Y-%m-%d %H:%M:%S", block.GetBlockTime()).c_str(),
            block.vtx.size());

        PrintWallets(block);

        // put the main time-chain first
        vector<CBlockIndex*>& vNext = mapNext[pindex];
        for (unsigned int i = 0; i < vNext.size(); i++)
        {
            if (vNext[i]->pnext)
            {
                swap(vNext[0], vNext[i]);
                break;
            }
        }

        // iterate children
        for (unsigned int i = 0; i < vNext.size(); i++)
            vStack.push_back(make_pair(nCol+i, vNext[i]));
    }
}

bool LoadExternalBlockFile(FILE* fileIn, CDiskBlockPos *dbp)
{
    int64 nStart = GetTimeMillis();

    int nLoaded = 0;
    try {
        CBufferedFile blkdat(fileIn, 2*MAX_BLOCK_SIZE, MAX_BLOCK_SIZE+8, SER_DISK, CLIENT_VERSION);
        uint64 nStartByte = 0;
        if (dbp) {
            // (try to) skip already indexed part
            CBlockFileInfo info;
            if (pblocktree->ReadBlockFileInfo(dbp->nFile, info)) {
                nStartByte = info.nSize;
                blkdat.Seek(info.nSize);
            }
        }
        uint64 nRewind = blkdat.GetPos();
        while (blkdat.good() && !blkdat.eof()) {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            nRewind++; // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try {
                // locate a header
                unsigned char buf[4];
                blkdat.FindByte(pchMessageStart[0]);
                nRewind = blkdat.GetPos()+1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, pchMessageStart, 4))
                    continue;
                // read size
                blkdat >> nSize;
                if (nSize < 80 || nSize > MAX_BLOCK_SIZE)
                    continue;
            } catch (std::exception &e) {
                // no valid block header found; don't complain
                break;
            }
            try {
                // read block
                uint64 nBlockPos = blkdat.GetPos();
                blkdat.SetLimit(nBlockPos + nSize);
                CBlock block;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // process block
                if (nBlockPos >= nStartByte) {
                    LOCK(cs_main);
                    if (dbp)
                        dbp->nPos = nBlockPos;
                    CValidationState state;
                    if (ProcessBlock(state, NULL, &block, dbp))
                        nLoaded++;
                    if (state.IsError())
                        break;
                }
            } catch (std::exception &e) {
                printf("%s() : Deserialize or I/O error caught during load\n", __PRETTY_FUNCTION__);
            }
        }
        fclose(fileIn);
    } catch(std::runtime_error &e) {
        AbortNode(_("Error: system error: ") + e.what());
    }
    if (nLoaded > 0)
        printf("Loaded %i blocks from external file in %" PRI64d"ms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

extern map<uint256, CAlert> mapAlerts;
extern CCriticalSection cs_mapAlerts;

string GetWarnings(string strFor)
{
    int nPriority = 0;
    string strStatusBar;
    string strRPC;

    if (GetBoolArg("-testsafemode"))
        strRPC = "test";

    if (!CLIENT_VERSION_IS_RELEASE)
        strStatusBar = _("This is a pre-release test build - use at your own risk - do not use for mining or merchant applications");

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "")
    {
        nPriority = 1000;
        strStatusBar = strMiscWarning;
    }

    // Longer invalid proof-of-work chain
    if (pindexBest && nBestInvalidWork > nBestChainWork + (pindexBest->GetBlockWork() * 6).getuint256())
    {
        nPriority = 2000;
        strStatusBar = strRPC = _("Warning: Displayed transactions may not be correct! You may need to upgrade, or other nodes may need to upgrade.");
    }

    // Alerts
    {
        LOCK(cs_mapAlerts);
        BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
        {
            const CAlert& alert = item.second;
            if (alert.AppliesToMe() && alert.nPriority > nPriority)
            {
                nPriority = alert.nPriority;
                strStatusBar = alert.strStatusBar;
            }
        }
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings() : invalid parameter");
    return "error";
}

//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static AlreadyHave(const CInv& inv)
{
    switch (inv.type)
    {
    case MSG_TX:
        {
            bool txInMap = false;
            {
                LOCK(mempool.cs);
                txInMap = mempool.exists(inv.hash);
            }
            return txInMap || mapOrphanTransactions.count(inv.hash) ||
                pcoinsTip->HaveCoins(inv.hash);
        }
    case MSG_BLOCK:
        return mapBlockIndex.count(inv.hash) ||
               mapOrphanBlocks.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}




// The message start string is designed to be unlikely to occur in normal data.
// The characters are rarely used upper ASCII, not valid as UTF-8, and produce
// a large 4-byte int at any alignment.
unsigned char pchMessageStart[4] = { 0x5c, 0xa1, 0xab, 0x1e };


void static ProcessGetData(CNode* pfrom)
{
    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();

    vector<CInv> vNotFound;

    while (it != pfrom->vRecvGetData.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        // Don't waste work on slow peers until they catch up on the blocks we
        // give them. 80 bytes is just the size of a block header - obviously
        // the minimum we might return.
        if (pfrom->nBlocksRequested * 80 > pfrom->nSendBytes)
            break;

        const CInv &inv = *it;
        {
            boost::this_thread::interruption_point();
            it++;

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
            {
                bool send = true;
                BlockMap::iterator mi = mapBlockIndex.find(inv.hash);
                pfrom->nBlocksRequested++;
                if (mi != mapBlockIndex.end())
                {
                    // If the requested block is at a height below our last
                    // checkpoint, only serve it if it's in the checkpointed chain
                    int nHeight = ((*mi).second)->nHeight;
                    CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(mapBlockIndex);
                    if (pcheckpoint && nHeight < pcheckpoint->nHeight) {
                       if (!((*mi).second)->IsInMainChain())
                       {
                         printf("ProcessGetData(): ignoring request for old block that isn't in the main chain\n");
                         send = false;
                       }
                    }
                } else {
                    send = false;
                }
                if (send)
                {
                    // Send block from disk
                    CBlock block;
                    block.ReadFromDisk((*mi).second);
                    if (inv.type == MSG_BLOCK)
                        pfrom->PushMessage("block", block);
                    else // MSG_FILTERED_BLOCK)
                    {
                        LOCK(pfrom->cs_filter);
                        if (pfrom->pfilter)
                        {
                            CMerkleBlock merkleBlock(block, *pfrom->pfilter);
                            pfrom->PushMessage("merkleblock", merkleBlock);
                            // CMerkleBlock just contains hashes, so also push any transactions in the block the client did not see
                            // This avoids hurting performance by pointlessly requiring a round-trip
                            // Note that there is currently no way for a node to request any single transactions we didnt send here -
                            // they must either disconnect and retry or request the full block.
                            // Thus, the protocol spec specified allows for us to provide duplicate txn here,
                            // however we MUST always provide at least what the remote peer needs
                            typedef std::pair<unsigned int, uint256> PairType;
                            BOOST_FOREACH(PairType& pair, merkleBlock.vMatchedTxn)
                                if (!pfrom->setInventoryKnown.count(CInv(MSG_TX, pair.second)))
                                    pfrom->PushMessage("tx", block.vtx[pair.first]);
                        }
                        // else
                            // no response
                    }

                    // Trigger them to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue)
                    {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, hashBestChain));
                        pfrom->PushMessage("inv", vInv);
                        pfrom->hashContinue = 0;
                    }
                }
            }
            else if (inv.IsKnownType())
            {
                // Send stream from relay memory
                bool pushed = false;
                {
                    LOCK(cs_mapRelay);
                    map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    if (mi != mapRelay.end()) {
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TX) {
                    LOCK(mempool.cs);
                    if (mempool.exists(inv.hash)) {
                        CTransaction tx = mempool.lookup(inv.hash);
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << tx;
                        pfrom->PushMessage("tx", ss);
                        pushed = true;
                    }
                }
                if (!pushed) {
                    vNotFound.push_back(inv);
                }
            }

            // Track requests for our stuff.
            Inventory(inv.hash);

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
                break;
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        pfrom->PushMessage("notfound", vNotFound);
    }
}

bool static ProcessMessage(CNode* pfrom, string strCommand, CDataStream& vRecv)
{
    RandAddSeedPerfmon();
    if (fDebug)
        printf("received: %s (%" PRIszu" bytes)\n", strCommand.c_str(), vRecv.size());
    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        printf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }





    if (strCommand == "version")
    {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
        {
            pfrom->Misbehaving(1);
            return false;
        }

        int64 nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64 nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;
        if (pfrom->nVersion < MIN_PEER_PROTO_VERSION)
        {
            // disconnect from peers older than this proto version
            printf("partner %s using obsolete version %i; disconnecting\n", pfrom->addr.ToString().c_str(), pfrom->nVersion);
            pfrom->fDisconnect = true;
            return false;
        }

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty()) {
            vRecv >> pfrom->strSubVer;
            pfrom->cleanSubVer = SanitizeString(pfrom->strSubVer);
        }
        if (!vRecv.empty())
            vRecv >> pfrom->nStartingHeight;
        if (!vRecv.empty())
            vRecv >> pfrom->fRelayTxes; // set to true after we get the first filter* message
        else
            pfrom->fRelayTxes = true;

        if (pfrom->fInbound && addrMe.IsRoutable())
        {
            pfrom->addrLocal = addrMe;
            SeenLocal(addrMe);
        }

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1)
        {
            printf("connected to self at %s, disconnecting\n", pfrom->addr.ToString().c_str());
            pfrom->fDisconnect = true;
            return true;
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        AddTimeData(pfrom->addr, nTime);

        // Change version
        pfrom->PushMessage("verack");
        pfrom->ssSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (!fNoListen && !IsInitialBlockDownload())
            {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                if (addr.IsRoutable())
                    pfrom->PushAddress(addr);
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000)
            {
                pfrom->PushMessage("getaddr");
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        } else {
            if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom)
            {
                addrman.Add(addrFrom, addrFrom);
                addrman.Good(addrFrom);
            }
        }

        // Relay alerts
        {
            LOCK(cs_mapAlerts);
            BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
                item.second.RelayTo(pfrom);
        }

        pfrom->fSuccessfullyConnected = true;

        printf("receive version message: %s: version %d, blocks=%d, us=%s, them=%s, peer=%s\n", pfrom->cleanSubVer.c_str(), pfrom->nVersion, pfrom->nStartingHeight, addrMe.ToString().c_str(), addrFrom.ToString().c_str(), pfrom->addr.ToString().c_str());

        cPeerBlockCounts.input(pfrom->nStartingHeight);
    }


    else if (pfrom->nVersion == 0)
    {
        // Must have a version message before anything else
        pfrom->Misbehaving(1);
        return false;
    }


    else if (strCommand == "verack")
    {
        pfrom->SetRecvVersion(min(pfrom->nVersion, PROTOCOL_VERSION));
    }


    else if (strCommand == "addr")
    {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
            return true;
        if (vAddr.size() > 1000)
        {
            pfrom->Misbehaving(20);
            return error("message addr size() = %" PRIszu"", vAddr.size());
        }

        // Store the new addresses
        vector<CAddress> vAddrOk;
        int64 nNow = GetAdjustedTime();
        int64 nSince = nNow - 10 * 60;
        BOOST_FOREACH(CAddress& addr, vAddr)
        {
            boost::this_thread::interruption_point();

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the setAddrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint64 hashAddr = addr.GetHash();
                    uint256 hashRand = hashSalt ^ (hashAddr<<32) ^ ((GetTime()+hashAddr)/(24*60*60));
                    hashRand = HashKeccak(BEGIN(hashRand), END(hashRand));
                    multimap<uint256, CNode*> mapMix;
                    BOOST_FOREACH(CNode* pnode, vNodes)
                    {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = HashKeccak(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    for (multimap<uint256, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot)
            pfrom->fDisconnect = true;
    }


    else if (strCommand == "inv")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            pfrom->Misbehaving(20);
            return error("message inv size() = %" PRIszu"", vInv.size());
        }

        // find last block in inv vector
        unsigned int nLastBlock = (unsigned int)(-1);
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
            if (vInv[vInv.size() - 1 - nInv].type == MSG_BLOCK) {
                nLastBlock = vInv.size() - 1 - nInv;
                break;
            }
        }
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            const CInv &inv = vInv[nInv];

            boost::this_thread::interruption_point();
            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = AlreadyHave(inv);
            if (fDebug)
                printf("  got inventory: %s  %s\n", inv.ToString().c_str(), fAlreadyHave ? "have" : "new");

            if (!fAlreadyHave) {
                if (!fImporting && !fReindex)
                    pfrom->AskFor(inv);
            } else if (inv.type == MSG_BLOCK && mapOrphanBlocks.count(inv.hash)) {
                pfrom->PushGetBlocks(pindexBest, GetOrphanRoot(mapOrphanBlocks[inv.hash]));
            } else if (nInv == nLastBlock) {
                // In case we are on a very long side-chain, it is possible that we already have
                // the last block in an inv bundle sent in response to getblocks. Try to detect
                // this situation and push another getblocks to continue.
                pfrom->PushGetBlocks(mapBlockIndex[inv.hash], uint256(0));
                if (fDebug)
                    printf("force request: %s\n", inv.ToString().c_str());
            }

            // Track requests for our stuff
            Inventory(inv.hash);
        }
    }


    else if (strCommand == "getdata")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            pfrom->Misbehaving(20);
            return error("message getdata size() = %" PRIszu"", vInv.size());
        }

        if (fDebugNet || (vInv.size() != 1))
            printf("received getdata (%" PRIszu" invsz)\n", vInv.size());

        if ((fDebugNet && vInv.size() > 0) || (vInv.size() == 1))
            printf("received getdata for: %s\n", vInv[0].ToString().c_str());

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        ProcessGetData(pfrom);
    }


    else if (strCommand == "getblocks")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        // Find the last block the caller has in the main chain
        CBlockIndex* pindex = locator.GetBlockIndex();

        // Send the rest of the chain
        if (pindex)
            pindex = pindex->pnext;
        int nLimit = 500;
        printf("getblocks %d to %s limit %d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString().c_str(), nLimit);
        for (; pindex; pindex = pindex->pnext)
        {
            if (pindex->GetBlockHash() == hashStop)
            {
                printf("  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0)
            {
                // When this block is requested, we'll send an inv that'll make them
                // getblocks the next batch of inventory.
                printf("  getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }


    else if (strCommand == "getheaders")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        CBlockIndex* pindex = NULL;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            BlockMap::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = locator.GetBlockIndex();
            if (pindex)
                pindex = pindex->pnext;
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
        vector<CBlock> vHeaders;
        int nLimit = 2000;
        printf("getheaders %d to %s\n", (pindex ? pindex->nHeight : -1), hashStop.ToString().c_str());
        for (; pindex; pindex = pindex->pnext)
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        pfrom->PushMessage("headers", vHeaders);
    }


    else if (strCommand == "tx")
    {
        vector<uint256> vWorkQueue;
        vector<uint256> vEraseQueue;
        CDataStream vMsg(vRecv);
        CTransaction tx;
        vRecv >> tx;

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        bool fMissingInputs = false;
        bool fMissingInputsZerocoin = false;
        CValidationState state;
        if ((!tx.IsZerocoinSpend()) &&
                (tx.AcceptToMemoryPool(state, true, true, &fMissingInputs)))
        {
            RelayTransaction(tx, inv.hash);
            mapAlreadyAskedFor.erase(inv);
            vWorkQueue.push_back(inv.hash);
            vEraseQueue.push_back(inv.hash);

            printf("AcceptToMemoryPool: %s %s : accepted %s (poolsz %" PRIszu")\n",
                pfrom->addr.ToString().c_str(), pfrom->cleanSubVer.c_str(),
                tx.GetHash().ToString().c_str(),
                mempool.mapTx.size());

            // Recursively process any orphan transactions that depended on this one
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                uint256 hashPrev = vWorkQueue[i];
                for (set<uint256>::iterator mi = mapOrphanTransactionsByPrev[hashPrev].begin();
                     mi != mapOrphanTransactionsByPrev[hashPrev].end();
                     ++mi)
                {
                    const uint256& orphanHash = *mi;
                    const CTransaction& orphanTx = mapOrphanTransactions[orphanHash];
                    bool fMissingInputs2 = false;
                    // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan
                    // resolution (that is, feeding people an invalid transaction based on LegitTxX in order to get
                    // anyone relaying LegitTxX banned)
                    CValidationState stateDummy;

                    if (tx.AcceptToMemoryPool(stateDummy, true, true, &fMissingInputs2))
                    {
                        printf("   accepted orphan tx %s\n", orphanHash.ToString().c_str());
                        RelayTransaction(orphanTx, orphanHash);
                        mapAlreadyAskedFor.erase(CInv(MSG_TX, orphanHash));
                        vWorkQueue.push_back(orphanHash);
                        vEraseQueue.push_back(orphanHash);
                    }
                    else if (!fMissingInputs2)
                    {
                        // invalid or too-little-fee orphan
                        vEraseQueue.push_back(orphanHash);
                        printf("   removed orphan tx %s\n", orphanHash.ToString().c_str());
                    }
                }
            }

            BOOST_FOREACH(uint256 hash, vEraseQueue)
                EraseOrphanTx(hash);
        }
        else if ((tx.IsZerocoinSpend()) &&
                 (tx.AcceptToMemoryPool(state, false, true, &fMissingInputsZerocoin)))
        {
            RelayTransaction(tx, inv.hash);
            //mapAlreadyAskedFor.erase(inv);
            //vWorkQueue.push_back(inv.hash);
            //vEraseQueue.push_back(inv.hash);

            printf("AcceptToMemoryPool: %s %s : accepted %s (poolsz %" PRIszu")\n",
                   pfrom->addr.ToString().c_str(), pfrom->cleanSubVer.c_str(),
                   tx.GetHash().ToString().c_str(),
                   mempool.mapTx.size());

            /*
            // Recursively process any orphan transactions that depended on this one
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                uint256 hashPrev = vWorkQueue[i];
                for (set<uint256>::iterator mi = mapOrphanTransactionsByPrev[hashPrev].begin();
                     mi != mapOrphanTransactionsByPrev[hashPrev].end();
                     ++mi)
                {
                    const uint256& orphanHash = *mi;
                    const CTransaction& orphanTx = mapOrphanTransactions[orphanHash];
                    bool fMissingInputs2 = false;
                    // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan
                    // resolution (that is, feeding people an invalid transaction based on LegitTxX in order to get
                    // anyone relaying LegitTxX banned)
                    CValidationState stateDummy;

                    if (tx.AcceptToMemoryPool(stateDummy, true, true, &fMissingInputs2))
                    {
                        printf("   accepted orphan tx %s\n", orphanHash.ToString().c_str());
                        RelayTransaction(orphanTx, orphanHash);
                        mapAlreadyAskedFor.erase(CInv(MSG_TX, orphanHash));
                        vWorkQueue.push_back(orphanHash);
                        vEraseQueue.push_back(orphanHash);
                    }
                    else if (!fMissingInputs2)
                    {
                        // invalid or too-little-fee orphan
                        vEraseQueue.push_back(orphanHash);
                        printf("   removed orphan tx %s\n", orphanHash.ToString().c_str());
                    }
                }
            }

            BOOST_FOREACH(uint256 hash, vEraseQueue)
                    EraseOrphanTx(hash);
            */
        }else if (fMissingInputs){

            AddOrphanTx(tx);

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nEvicted = LimitOrphanTxSize(MAX_ORPHAN_TRANSACTIONS);
            if (nEvicted > 0)
                printf("mapOrphan overflow, removed %u tx\n", nEvicted);
        }
        int nDoS = 0;
        if (state.IsInvalid(nDoS))
        {
            printf("%s from %s %s was not accepted into the memory pool\n", tx.GetHash().ToString().c_str(),
                pfrom->addr.ToString().c_str(), pfrom->cleanSubVer.c_str());
            if (nDoS > 0)
                pfrom->Misbehaving(nDoS);
        }
    }


    else if (strCommand == "block" && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        CBlock block;
        vRecv >> block;

        printf("received block %s\n", block.GetHash().ToString().c_str());
        // block.print();


        CInv inv(MSG_BLOCK, block.GetHash());
        pfrom->AddInventoryKnown(inv);

        CValidationState state;
        if (ProcessBlock(state, pfrom, &block) || state.CorruptionPossible())
            mapAlreadyAskedFor.erase(inv);
        int nDoS = 0;
        if (state.IsInvalid(nDoS))
            if (nDoS > 0)
                pfrom->Misbehaving(nDoS);
    }


    else if (strCommand == "getaddr")
    {
        pfrom->vAddrToSend.clear();
        vector<CAddress> vAddr = addrman.GetAddr();
        BOOST_FOREACH(const CAddress &addr, vAddr)
            pfrom->PushAddress(addr);
    }


    else if (strCommand == "mempool")
    {
        std::vector<uint256> vtxid;
        LOCK2(mempool.cs, pfrom->cs_filter);
        mempool.queryHashes(vtxid);
        vector<CInv> vInv;
        BOOST_FOREACH(uint256& hash, vtxid) {
            CInv inv(MSG_TX, hash);
            if ((pfrom->pfilter && pfrom->pfilter->IsRelevantAndUpdate(mempool.lookup(hash), hash)) ||
               (!pfrom->pfilter))
                vInv.push_back(inv);
            if (vInv.size() == MAX_INV_SZ)
                break;
        }
        if (vInv.size() > 0)
            pfrom->PushMessage("inv", vInv);
    }


    else if (strCommand == "ping")
    {
        if (pfrom->nVersion > BIP0031_VERSION)
        {
            uint64 nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            pfrom->PushMessage("pong", nonce);
        }
    }


    else if (strCommand == "alert")
    {
        CAlert alert;
        vRecv >> alert;

        uint256 alertHash = alert.GetHash();
        if (pfrom->setKnown.count(alertHash) == 0)
        {
            if (alert.ProcessAlert())
            {
                // Relay
                pfrom->setKnown.insert(alertHash);
                {
                    LOCK(cs_vNodes);
                    BOOST_FOREACH(CNode* pnode, vNodes)
                        alert.RelayTo(pnode);
                }
            }
            else {
                // Small DoS penalty so peers that send us lots of
                // duplicate/expired/invalid-signature/whatever alerts
                // eventually get banned.
                // This isn't a Misbehaving(100) (immediate ban) because the
                // peer might be an older or different implementation with
                // a different signature key, etc.
                pfrom->Misbehaving(10);
            }
        }
    }


    else if (!fBloomFilters &&
             (strCommand == "filterload" ||
              strCommand == "filteradd" ||
              strCommand == "filterclear"))
    {
        pfrom->CloseSocketDisconnect();
        return error("peer %s attempted to set a bloom filter even though we do not advertise that service",
                     pfrom->addr.ToString().c_str());
    }

    else if (strCommand == "filterload")
    {
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints())
            // There is no excuse for sending a too-large filter
            pfrom->Misbehaving(100);
        else
        {
            LOCK(pfrom->cs_filter);
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter(filter);
            pfrom->pfilter->UpdateEmptyFull();
        }
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == "filteradd")
    {
        vector<unsigned char> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
        // and thus, the maximum size any matched object can have) in a filteradd message
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE)
        {
            pfrom->Misbehaving(100);
        } else {
            LOCK(pfrom->cs_filter);
            if (pfrom->pfilter)
                pfrom->pfilter->insert(vData);
            else
                pfrom->Misbehaving(100);
        }
    }


    else if (strCommand == "filterclear")
    {
        LOCK(pfrom->cs_filter);
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter();
        pfrom->fRelayTxes = true;
    }


    else
    {
        // Ignore unknown commands for extensibility
    }


    // Update the last seen time for this node's address
    if (pfrom->fNetworkNode)
        if (strCommand == "version" || strCommand == "addr" || strCommand == "inv" || strCommand == "getdata" || strCommand == "ping")
            AddressCurrentlyConnected(pfrom->addr);


    return true;
}

// requires LOCK(cs_vRecvMsg)
bool ProcessMessages(CNode* pfrom)
{
    //if (fDebug)
    //    printf("ProcessMessages(%zu messages)\n", pfrom->vRecvMsg.size());

    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fOk = true;

    if (!pfrom->vRecvGetData.empty())
        ProcessGetData(pfrom);

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty()) return fOk;

    std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
    while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        // get next message
        CNetMessage& msg = *it;

        //if (fDebug)
        //    printf("ProcessMessages(message %u msgsz, %zu bytes, complete:%s)\n",
        //            msg.hdr.nMessageSize, msg.vRecv.size(),
        //            msg.complete() ? "Y" : "N");

        // end, if an incomplete message is found
        if (!msg.complete())
            break;

        // at this point, any failure means we can delete the current message
        it++;

        // Scan for message start
        if (memcmp(msg.hdr.pchMessageStart, pchMessageStart, sizeof(pchMessageStart)) != 0) {
            printf("\n\nPROCESSMESSAGE: INVALID MESSAGESTART\n\n");
            fOk = false;
            break;
        }

        // Read header
        CMessageHeader& hdr = msg.hdr;
        if (!hdr.IsValid())
        {
            printf("\n\nPROCESSMESSAGE: ERRORS IN HEADER %s\n\n\n", hdr.GetCommand().c_str());
            continue;
        }
        string strCommand = hdr.GetCommand();

        // Message size
        unsigned int nMessageSize = hdr.nMessageSize;

        // Checksum
        CDataStream& vRecv = msg.vRecv;
        uint256 hash = HashKeccak(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        if (nChecksum != hdr.nChecksum)
        {
            printf("ProcessMessages(%s, %u bytes) : CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n",
               strCommand.c_str(), nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        // Process message
        bool fRet = false;
        try
        {
            {
                LOCK(cs_main);
                fRet = ProcessMessage(pfrom, strCommand, vRecv);
            }
            boost::this_thread::interruption_point();
        }
        catch (std::ios_base::failure& e)
        {
            if (strstr(e.what(), "end of data"))
            {
                // Allow exceptions from under-length message on vRecv
                printf("ProcessMessages(%s, %u bytes) : Exception '%s' caught, normally caused by a message being shorter than its stated length\n", strCommand.c_str(), nMessageSize, e.what());
            }
            else if (strstr(e.what(), "size too large"))
            {
                // Allow exceptions from over-long size
                printf("ProcessMessages(%s, %u bytes) : Exception '%s' caught\n", strCommand.c_str(), nMessageSize, e.what());
            }
            else
            {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        }
        catch (boost::thread_interrupted) {
            throw;
        }
        catch (std::exception& e) {
            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
            printf("ProcessMessage(%s, %u bytes) FAILED\n", strCommand.c_str(), nMessageSize);

        break;
    }

    // In case the connection got shut down, its receive buffer was wiped
    if (!pfrom->fDisconnect)
        pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);

    return fOk;
}


bool SendMessages(CNode* pto, bool fSendTrickle)
{
    TRY_LOCK(cs_main, lockMain);
    if (lockMain) {
        // Don't send anything until we get their version message
        if (pto->nVersion == 0)
            return true;

        // Keep-alive ping. We send a nonce of zero because we don't use it anywhere
        // right now.
        if (pto->nLastSend && GetTime() - pto->nLastSend > 30 * 60 && pto->vSendMsg.empty()) {
            uint64 nonce = 0;
            if (pto->nVersion > BIP0031_VERSION)
                pto->PushMessage("ping", nonce);
            else
                pto->PushMessage("ping");
        }

        // Start block sync
        if (pto->fStartSync && !fImporting && !fReindex) {
            pto->fStartSync = false;
            pto->PushGetBlocks(pindexBest, uint256(0));
        }

        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!fReindex && !fImporting && !IsInitialBlockDownload())
        {
            ResendWalletTransactions();
        }

        // Address refresh broadcast
        static int64 nLastRebroadcast;
        if (!IsInitialBlockDownload() && (GetTime() - nLastRebroadcast > 24 * 60 * 60))
        {
            {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    // Periodically clear setAddrKnown to allow refresh broadcasts
                    if (nLastRebroadcast)
                        pnode->setAddrKnown.clear();

                    // Rebroadcast our address
                    if (!fNoListen)
                    {
                        CAddress addr = GetLocalAddress(&pnode->addr);
                        if (addr.IsRoutable())
                            pnode->PushAddress(addr);
                    }
                }
            }
            nLastRebroadcast = GetTime();
        }

        //
        // Message: addr
        //
        if (fSendTrickle)
        {
            vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            BOOST_FOREACH(const CAddress& addr, pto->vAddrToSend)
            {
                // returns true if wasn't already contained in the set
                if (pto->setAddrKnown.insert(addr).second)
                {
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        pto->PushMessage("addr", vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage("addr", vAddr);
        }


        //
        // Message: inventory
        //
        vector<CInv> vInv;
        vector<CInv> vInvWait;
        {
            LOCK(pto->cs_inventory);
            vInv.reserve(pto->vInventoryToSend.size());
            vInvWait.reserve(pto->vInventoryToSend.size());
            BOOST_FOREACH(const CInv& inv, pto->vInventoryToSend)
            {
                if (pto->setInventoryKnown.count(inv))
                    continue;

                // trickle out tx inv to protect privacy
                if (inv.type == MSG_TX && !fSendTrickle)
                {
                    // 1/4 of tx invs blast to all immediately
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint256 hashRand = inv.hash ^ hashSalt;
                    hashRand = HashKeccak(BEGIN(hashRand), END(hashRand));
                    bool fTrickleWait = ((hashRand & 3) != 0);

                    // always trickle our own transactions
                    if (!fTrickleWait)
                    {
                        CWalletTx wtx;
                        if (GetTransaction(inv.hash, wtx))
                            if (wtx.fFromMe)
                                fTrickleWait = true;
                    }

                    if (fTrickleWait)
                    {
                        vInvWait.push_back(inv);
                        continue;
                    }
                }

                // returns true if wasn't already contained in the set
                if (pto->setInventoryKnown.insert(inv).second)
                {
                    vInv.push_back(inv);
                    if (vInv.size() >= 1000)
                    {
                        pto->PushMessage("inv", vInv);
                        vInv.clear();
                    }
                }
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
            pto->PushMessage("inv", vInv);


        //
        // Message: getdata
        //
        vector<CInv> vGetData;
        int64 nNow = GetTime() * 1000000;
        while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(inv))
            {
                if (fDebugNet)
                    printf("sending getdata: %s\n", inv.ToString().c_str());
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000)
                {
                    pto->PushMessage("getdata", vGetData);
                    vGetData.clear();
                }
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            pto->PushMessage("getdata", vGetData);

    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// SmartCashMiner
//

int static FormatHashBlocks(void* pbuffer, unsigned int len)
{
    unsigned char* pdata = (unsigned char*)pbuffer;
    unsigned int blocks = 1 + ((len + 8) / 64);
    unsigned char* pend = pdata + 64 * blocks;
    memset(pdata + len, 0, 64 * blocks - len);
    pdata[len] = 0x80;
    unsigned int bits = len * 8;
    pend[-1] = (bits >> 0) & 0xff;
    pend[-2] = (bits >> 8) & 0xff;
    pend[-3] = (bits >> 16) & 0xff;
    pend[-4] = (bits >> 24) & 0xff;
    return blocks;
}

static const unsigned int pSHA256InitState[8] =
{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

void SHA256Transform(void* pstate, void* pinput, const void* pinit)
{
    SHA256_CTX ctx;
    unsigned char data[64];

    SHA256_Init(&ctx);

    for (int i = 0; i < 16; i++)
        ((uint32_t*)data)[i] = ByteReverse(((uint32_t*)pinput)[i]);

    for (int i = 0; i < 8; i++)
        ctx.h[i] = ((uint32_t*)pinit)[i];

    SHA256_Update(&ctx, data, sizeof(data));
    for (int i = 0; i < 8; i++)
        ((uint32_t*)pstate)[i] = ctx.h[i];
}

// Some explaining would be appreciated
class COrphan
{
public:
    CTransaction* ptx;
    set<uint256> setDependsOn;
    double dPriority;
    double dFeePerKb;

    COrphan(CTransaction* ptxIn)
    {
        ptx = ptxIn;
        dPriority = dFeePerKb = 0;
    }

    void print() const
    {
        printf("COrphan(hash=%s, dPriority=%.1f, dFeePerKb=%.1f)\n",
               ptx->GetHash().ToString().c_str(), dPriority, dFeePerKb);
        BOOST_FOREACH(uint256 hash, setDependsOn)
            printf("   setDependsOn %s\n", hash.ToString().c_str());
    }
};


uint64 nLastBlockTx = 0;
uint64 nLastBlockSize = 0;

// We want to sort transactions by priority and fee, so:
typedef boost::tuple<double, double, CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;
public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) { }
    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee)
        {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        }
        else
        {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();
            return a.get<0>() < b.get<0>();
        }
    }
};

CBlockTemplate* CreateNewBlock(const CScript& scriptPubKeyIn)
{
    // Create new block
    auto_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if(!pblocktemplate.get())
        return NULL;
    CBlock *pblock = &pblocktemplate->block; // pointer for convenience

    // Create coinbase tx
    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);
    txNew.vout[0].scriptPubKey = scriptPubKeyIn;
    txNew.vout[0].nValue = 0;

    CBlock lastBLock;
    lastBLock.ReadFromDisk(pindexBest);

/*    int64 nFees = 0;
    CCoinsViewCache view(*pcoinsTip, true);

    for (unsigned int i=0; i<lastBLock.vtx.size(); i++)
    {
        const CTransaction &tx = lastBLock.vtx[i];
        nFees += tx.GetValueIn(view)-tx.GetValueOut();
    }
*/
    // To SmartHive Teams, SmartHive Budget, and SmartDeposits
    if ((pindexBest->nHeight+1 > 0) && (pindexBest->nHeight+1 < 717499999)) {
	// Take out amounts for budgets.
	txNew.vout[0].nValue =-((int64)(0.95 * (GetBlockValue(pindexBest->nHeight+1, 0, pindexBest->nTime))));

         CScript FOUNDER_1_SCRIPT;
         CScript FOUNDER_2_SCRIPT;
         CScript FOUNDER_3_SCRIPT;
         CScript FOUNDER_4_SCRIPT;
         CScript FOUNDER_5_SCRIPT;

         if(!fTestNet && (GetAdjustedTime() > nStartRewardTime)){
                FOUNDER_1_SCRIPT.SetDestination(CBitcoinAddress("Siim7T5zMH3he8xxtQzhmHs4CQSuMrCV1M").Get());
                FOUNDER_2_SCRIPT.SetDestination(CBitcoinAddress("SW2FbVaBhU1Www855V37auQzGQd8fuLR9x").Get());
                FOUNDER_3_SCRIPT.SetDestination(CBitcoinAddress("SPusYr5tUdUyRXevJg7pnCc9Sm4HEzaYZF").Get());
                FOUNDER_4_SCRIPT.SetDestination(CBitcoinAddress("SU5bKb35xUV8aHG5dNarWHB3HBVjcCRjYo").Get());
                FOUNDER_5_SCRIPT.SetDestination(CBitcoinAddress("SXun9XDHLdBhG4Yd1ueZfLfRpC9kZgwT1b").Get());
         }else if(!fTestNet && (GetAdjustedTime() <= nStartRewardTime)){
             throw std::runtime_error("CreateNewBlock() : Create new block too early");
         }else{
                FOUNDER_1_SCRIPT.SetDestination(CBitcoinAddress("TBizCRSozKpCbheftmzs75fZnc7h6HocJ3").Get());
                FOUNDER_2_SCRIPT.SetDestination(CBitcoinAddress("THc8faox1kKZ3aegLdU4cwCJwgehLHSe9M").Get());
                FOUNDER_3_SCRIPT.SetDestination(CBitcoinAddress("TK7CPJ2BS2UxAc7KBbUYySCBczww97Qr7p").Get());
                FOUNDER_4_SCRIPT.SetDestination(CBitcoinAddress("TUPAY3ziYY7znMLxRJJNuvfuFWS1snrjiM").Get());
                FOUNDER_5_SCRIPT.SetDestination(CBitcoinAddress("TMtxkvmAMyL5siHX1n3zKAvAKnev8if8KA").Get());
         }

         // And pay the budgets
         txNew.vout.push_back(CTxOut((int64)(0.08 * (GetBlockValue(pindexBest->nHeight+1, 0, pindexBest->nTime))), CScript(FOUNDER_1_SCRIPT.begin(), FOUNDER_1_SCRIPT.end())));
         txNew.vout.push_back(CTxOut((int64)(0.08 * (GetBlockValue(pindexBest->nHeight+1, 0, pindexBest->nTime))), CScript(FOUNDER_2_SCRIPT.begin(), FOUNDER_2_SCRIPT.end())));
         txNew.vout.push_back(CTxOut((int64)(0.08 * (GetBlockValue(pindexBest->nHeight+1, 0, pindexBest->nTime))), CScript(FOUNDER_3_SCRIPT.begin(), FOUNDER_3_SCRIPT.end())));
         txNew.vout.push_back(CTxOut((int64)(0.15 * (GetBlockValue(pindexBest->nHeight+1, 0, pindexBest->nTime))), CScript(FOUNDER_4_SCRIPT.begin(), FOUNDER_4_SCRIPT.end())));
         txNew.vout.push_back(CTxOut((int64)(0.56 * (GetBlockValue(pindexBest->nHeight+1, 0, pindexBest->nTime))), CScript(FOUNDER_5_SCRIPT.begin(), FOUNDER_5_SCRIPT.end())));
    }
    // Add our coinbase tx as first transaction
    pblock->vtx.push_back(txNew);
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE-1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", 0);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    unsigned int COUNT_SPEND_ZC_TX = 0;
    unsigned int MAX_SPEND_ZC_TX_PER_BLOCK = 1;

    // Collect memory pool transactions into the block
//    nFees = 0;
    int64 nFees = 0;
    {
        LOCK2(cs_main, mempool.cs);
        CBlockIndex* pindexPrev = pindexBest;
        CCoinsViewCache view(*pcoinsTip, true);

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;
        bool fPrintPriority = GetBoolArg("-printpriority");        

        // Collect transactions into block
        uint64 nBlockSize = 1000;
        uint64 nBlockTx = 0;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());

        // printf("mempool.mapTx.size() = %d\n", mempool.mapTx.size());

        for (map<uint256, CTransaction>::iterator mi = mempool.mapTx.begin(); mi != mempool.mapTx.end(); ++mi)
        {
            CTransaction& tx = (*mi).second;
            if (tx.IsCoinBase() || !tx.IsFinal())
                continue;

            if (tx.IsZerocoinSpend())
            {

                if (COUNT_SPEND_ZC_TX >= MAX_SPEND_ZC_TX_PER_BLOCK) {
                    continue;
                }

                //mempool.countZCSpend--;
                // Size limits
                unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);

                printf("\n\n\n\n######################################\n");
                printf("nBlockMaxSize = %d\n", nBlockMaxSize);
                printf("nBlockSize = %d\n", nBlockSize);
                printf("nTxSize = %d\n", nTxSize);
                printf("nBlockSize + nTxSize  = %d\n", nBlockSize + nTxSize );
                printf("######################################\n\n\n\n\n");

                if (nBlockSize + nTxSize >= nBlockMaxSize)
                    continue;

                // Legacy limits on sigOps:
                unsigned int nTxSigOps = tx.GetLegacySigOpCount();
                if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                    continue;

                int64 nTxFees = 0;

                pblock->vtx.push_back(tx);
                pblocktemplate->vTxFees.push_back(nTxFees);
                pblocktemplate->vTxSigOps.push_back(nTxSigOps);
                nBlockSize += nTxSize;
                ++nBlockTx;
                nBlockSigOps += nTxSigOps;
                nFees += nTxFees;
                COUNT_SPEND_ZC_TX++;
                continue;
            }

            COrphan* porphan = NULL;
            double dPriority = 0;
            int64 nTotalIn = 0;
            bool fMissingInputs = false;
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
            {
                // Read prev transaction
                if (!view.HaveCoins(txin.prevout.hash))
                {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if (!mempool.mapTx.count(txin.prevout.hash))
                    {
                        printf("ERROR: mempool transaction missing input\n");
                        if (fDebug) assert("mempool transaction missing input" == 0);
                        fMissingInputs = true;
                        if (porphan)
                            vOrphan.pop_back();
                        break;
                    }

                    // Has to wait for dependencies
                    if (!porphan)
                    {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }
                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].vout[txin.prevout.n].nValue;
                    continue;
                }

                const CCoins &coins = view.GetCoins(txin.prevout.hash);

                int64 nValueIn = coins.vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = pindexPrev->nHeight - coins.nHeight + 1;

                dPriority += (double)nValueIn * nConf;


            }
            if (fMissingInputs) continue;

            // Priority is sum(valuein * age) / txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority /= nTxSize;

            // This is a more accurate fee-per-kilobyte than is used by the client code, because the
            // client code rounds up the size to the nearest 1K. That's good, because it gives an
            // incentive to create smaller transactions.
            double dFeePerKb =  double(nTotalIn-tx.GetValueOut()) / (double(nTxSize)/1000.0);

            if (porphan)
            {
                porphan->dPriority = dPriority;
                porphan->dFeePerKb = dFeePerKb;
            }
            else
                vecPriority.push_back(TxPriority(dPriority, dFeePerKb, &(*mi).second));
        }

        printf("mempool.countZCSpend = %d\n", mempool.countZCSpend);
        //if(mempool.countZCSpend != 0) return NULL;

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty())
        {                        
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            double dFeePerKb = vecPriority.front().get<1>();
            CTransaction& tx = *(vecPriority.front().get<2>());


            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
                continue;

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = tx.GetLegacySigOpCount();
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            // Skip free transactions if we're past the minimum block size:
            if (fSortedByFee && (dFeePerKb < CTransaction::nMinTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
                continue;

            // Prioritize by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || (dPriority < COIN * 144 / 250)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            if (!tx.HaveInputs(view))
                continue;

            int64 nTxFees = tx.GetValueIn(view)-tx.GetValueOut();

            nTxSigOps += tx.GetP2SHSigOpCount(view);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            CValidationState state;
            if (!tx.CheckInputs(state, view, true, SCRIPT_VERIFY_P2SH))
                continue;

            CTxUndo txundo;
            uint256 hash = tx.GetHash();
            tx.UpdateCoins(state, view, txundo, pindexPrev->nHeight+1, hash);

            // Added
            pblock->vtx.push_back(tx);
            pblocktemplate->vTxFees.push_back(nTxFees);
            pblocktemplate->vTxSigOps.push_back(nTxSigOps);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fPrintPriority)
            {
                printf("priority %.1f feeperkb %.1f txid %s\n",
                       dPriority, dFeePerKb, tx.GetHash().ToString().c_str());
            }

            // Add transactions that depend on this one to the priority queue
            if (mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->dFeePerKb, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;
        printf("CreateNewBlock(): total size %" PRI64u"\n", nBlockSize);



        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
//        pblock->LastHeight = pindexPrev->nHeight;
        pblock->UpdateTime(pindexPrev);
        pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock);
        pblock->nNonce         = 0;
        pblock->vtx[0].vin[0].scriptSig = CScript() << OP_0 << OP_0;
        pblocktemplate->vTxSigOps[0] = pblock->vtx[0].GetLegacySigOpCount();

        pblock->vtx[0].vout[0].nValue += GetBlockValue(pindexPrev->nHeight+1, nFees, pblock->nTime);
        pblocktemplate->vTxFees[0] = -nFees;

        CBlockIndex indexDummy(*pblock);
        indexDummy.pprev = pindexPrev;
        indexDummy.nHeight = pindexPrev->nHeight + 1;
        CCoinsViewCache viewNew(*pcoinsTip, true);
        CValidationState state;
        if (!pblock->ConnectBlock(state, &indexDummy, viewNew, true))
            throw std::runtime_error("CreateNewBlock() : ConnectBlock failed");
    }

    return pblocktemplate.release();
}

CBlockTemplate* CreateNewBlockWithKey(CReserveKey& reservekey)
{
    CPubKey pubkey;
    if (!reservekey.GetReservedKey(pubkey))
        return NULL;

    CScript scriptPubKey = CScript() << pubkey << OP_CHECKSIG;
    return CreateNewBlock(scriptPubKey);
}

void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    pblock->vtx[0].vin[0].scriptSig = (CScript() << nHeight << CBigNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(pblock->vtx[0].vin[0].scriptSig.size() <= 100);

    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}


void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1)
{
    //
    // Pre-build hash buffers
    //
    struct
    {
        struct unnamed2
        {
            int nVersion;
            uint256 hashPrevBlock;
            uint256 hashMerkleRoot;
            unsigned int nTime;
            unsigned int nBits;
            unsigned int nNonce;
        }
        block;
        unsigned char pchPadding0[64];
        uint256 hash1;
        unsigned char pchPadding1[64];
    }
    tmp;
    memset(&tmp, 0, sizeof(tmp));

    tmp.block.nVersion       = pblock->nVersion;
    tmp.block.hashPrevBlock  = pblock->hashPrevBlock;
    tmp.block.hashMerkleRoot = pblock->hashMerkleRoot;
    tmp.block.nTime          = pblock->nTime;
    tmp.block.nBits          = pblock->nBits;
    tmp.block.nNonce         = pblock->nNonce;

    FormatHashBlocks(&tmp.block, sizeof(tmp.block));
    FormatHashBlocks(&tmp.hash1, sizeof(tmp.hash1));

    // Byte swap all the input buffer
    for (unsigned int i = 0; i < sizeof(tmp)/4; i++)
        ((unsigned int*)&tmp)[i] = ByteReverse(((unsigned int*)&tmp)[i]);

    // Precalc the first half of the first hash, which stays constant
    SHA256Transform(pmidstate, &tmp.block, pSHA256InitState);

    memcpy(pdata, &tmp.block, 128);
    memcpy(phash1, &tmp.hash1, 64);
}


bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    CBlockIndex* pindexPrev = NULL;
    int nHeight = 0;
    if (pblock->GetHash() != hashGenesisBlock)
    {
        BlockMap::iterator mi = mapBlockIndex.find(pblock->hashPrevBlock);
        pindexPrev = (*mi).second;
        nHeight = pindexPrev->nHeight+1;
    }

    uint256 hash = pblock->GetPoWHash();
//    if (hash == 0)
//        return error("CheckWork() : Out of memory");

    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
    if (hash > hashTarget)
        return false;

/*    CAuxPow *auxpow = pblock->auxpow.get();

    if (auxpow != NULL) {
        if (!auxpow->Check(pblock->GetHash(), pblock->GetChainID()))
            return error("AUX POW is not valid");

        if (auxpow->GetParentBlockHash(nHeight) > hashTarget)
            return error("AUX POW parent hash %s is not under target %s", auxpow->GetParentBlockHash(nHeight).GetHex().c_str(), hashTarget.GetHex().c_str());
*/
        //// debug print
        printf("SmartCashMiner:\n");
        printf("proof-of-work found  \n  hash: %s  \target: %s\n", hash.GetHex().c_str(), hashTarget.GetHex().c_str());

/*	hash.GetHex().c_str(),
                auxpow->GetParentBlockHash(nHeight).GetHex().c_str(),
                hashTarget.GetHex().c_str());

    }
    else
    {
        if (hash > hashTarget)
            return false;
*/
        //// debug print
        printf("SmartCashMiner:\n");
        printf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", hash.GetHex().c_str(), hashTarget.GetHex().c_str());
//    }
    
    //// debug print    
    pblock->print();
    printf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue).c_str());

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != hashBestChain)
            return error("SmartCashMiner : generated block is stale");

        // Remove key from key pool
        reservekey.KeepKey();

        // Track how many getdata requests this block gets
        {
            LOCK(wallet.cs_wallet);
            wallet.mapRequestCount[pblock->GetHash()] = 0;
        }

        // Process this block the same as if we had received it from another node
        CValidationState state;
        if (!ProcessBlock(state, NULL, pblock))
            return error("SmartCashMiner : ProcessBlock, block not accepted");
    }

    return true;
}
/*
std::string CBlockIndex::ToString() const
{
    return strprintf("CBlockIndex(pprev=%p, pnext=%p, nHeight=%d, merkle=%s, hashBlock=%s)",
            pprev, pnext, nHeight,
            hashMerkleRoot.ToString().substr(0,10).c_str(),
            GetBlockHash().ToString().c_str());
}

std::string CDiskBlockIndex::ToString() const
{
    std::string str = "CDiskBlockIndex(";
    str += CBlockIndex::ToString();
    str += strprintf("\n                hashBlock=%s, hashPrev=%s, hashParentBlock=%s)",
        GetBlockHash().ToString().c_str(),
        hashPrev.ToString().c_str(),
        (auxpow.get() != NULL) ? auxpow->GetParentBlockHash(nHeight).ToString().substr(0,20).c_str() : "-");
    return str;
}
*/
CBlockHeader CBlockIndex::GetBlockHeader() const
{
    CBlockHeader block;
/*
    if (nVersion & BLOCK_VERSION_AUXPOW) {
        CDiskBlockIndex diskblockindex;
        // auxpow is not in memory, load CDiskBlockHeader
        // from database to get it

        pblocktree->ReadDiskBlockIndex(*phashBlock, diskblockindex);
        block.auxpow = diskblockindex.auxpow;
    }
*/
    block.nVersion       = nVersion;
    if (pprev)
    block.hashPrevBlock = pprev->GetBlockHash();
    block.hashMerkleRoot = hashMerkleRoot;
    block.nTime          = nTime;
    block.nBits          = nBits;
    block.nNonce         = nNonce;
    return block;
}

void static SmartcashMiner(CWallet *pwallet)
{
    printf("SmartCashMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("SmartCash-Miner");

    // Each thread has its own key and counter
    CReserveKey reservekey(pwallet);
    unsigned int nExtraNonce = 0;

    try { while(true) {

            while (vNodes.empty())
                MilliSleep(1000);

            //
            // Create new block
            //
            unsigned int nTransactionsUpdatedLast = nTransactionsUpdated;
            CBlockIndex* pindexPrev = pindexBest;

            auto_ptr<CBlockTemplate> pblocktemplate(CreateNewBlockWithKey(reservekey));
            if (!pblocktemplate.get())
                return;
            CBlock *pblock = &pblocktemplate->block;
            IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

            printf("Running SmartcashMiner with %" PRIszu" transactions in block (%u bytes)\n", pblock->vtx.size(),
                   ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));
                //
                // Pre-build hash buffers
                //
                char pmidstatebuf[32+16]; char* pmidstate = alignup<16>(pmidstatebuf);
                char pdatabuf[128+16];    char* pdata     = alignup<16>(pdatabuf);
                char phash1buf[64+16];    char* phash1    = alignup<16>(phash1buf);

                FormatHashBuffers(pblock, pmidstate, pdata, phash1);

                unsigned int& nBlockTime = *(unsigned int*)(pdata + 64 + 4);
                unsigned int& nBlockBits = *(unsigned int*)(pdata + 64 + 8);
                //unsigned int& nBlockNonce = *(unsigned int*)(pdata + 64 + 12);


                //
                // Search
                //
                uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
                int64 nStart = GetTime();
                while(true)
                {
                    unsigned int nHashesDone = 0;
                    uint256 thash;

                    while(true)
		    {
                        //    lyra2z_hash(BEGIN(pblock->nVersion), BEGIN(thash));
//			hash = block.GetHash();
//        		Hash(BEGIN(nVersion), END(nNonce));
            		thash = pblock->GetHash();
/*
                    {
                        if ( (!fTestNet && pindexPrev->nHeight + 1 >= 20500) ) {
                            lyra2z_hash(BEGIN(pblock->nVersion), BEGIN(thash));
                        } else if( !fTestNet && pindexPrev->nHeight + 1 >= 8192){
                            LYRA2(BEGIN(thash), 32, BEGIN(pblock->nVersion), 80, BEGIN(pblock->nVersion), 80, 2, 8192, 256);
                        } else if( !fTestNet && pindexPrev->nHeight + 1 >= 500){
                            LYRA2(BEGIN(thash), 32, BEGIN(pblock->nVersion), 80, BEGIN(pblock->nVersion), 80, 2, pindexPrev->nHeight + 1, 256);
                        } else if(fTestNet && pindexPrev->nHeight + 1 >= 90) { // testnet
                            lyra2z_hash(BEGIN(pblock->nVersion), BEGIN(thash));
                        } else if(fTestNet && pindexPrev->nHeight + 1 >= 80){ // testnet
                            LYRA2(BEGIN(thash), 32, BEGIN(pblock->nVersion), 80, BEGIN(pblock->nVersion), 80, 2, 8192, 256);
                        } else {
                            unsigned long int scrypt_scratpad_size_current_block = ((1 << (GetNfactor(pblock->nTime) + 1)) * 128 ) + 63;
                            char scratchpad[scrypt_scratpad_size_current_block];
                            scrypt_N_1_1_256_sp_generic(BEGIN(pblock->nVersion), BEGIN(thash), scratchpad, GetNfactor(pblock->nTime));
                            //printf("scrypt thash: %s\n", thash.ToString().c_str());
                            //printf("hashTarget: %s\n", hashTarget.ToString().c_str());
                        }
*/
                        if (thash <= hashTarget)
                        {
                            // Found a solution
//                            printf("Found a solution. Hash: %s", thash.GetHex().c_str());
                            SetThreadPriority(THREAD_PRIORITY_NORMAL);
                            CheckWork(pblock, *pwallet, reservekey);
                            SetThreadPriority(THREAD_PRIORITY_LOWEST);
                            break;
                        }
                        pblock->nNonce += 1;
                        nHashesDone += 1;
                        if ((pblock->nNonce & 0xFF) == 0)
                            break;
                    }

                    // Meter hashes/sec
                    static int64 nHashCounter;
                    if (nHPSTimerStart == 0)
                    {
                        nHPSTimerStart = GetTimeMillis();
                        nHashCounter = 0;
                    }
                    else
                        nHashCounter += nHashesDone;
                    if (GetTimeMillis() - nHPSTimerStart > 4000)
                    {
                        static CCriticalSection cs;
                        {
                            LOCK(cs);
                            if (GetTimeMillis() - nHPSTimerStart > 4000)
                            {
                                dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                                nHPSTimerStart = GetTimeMillis();
                                nHashCounter = 0;
                                static int64 nLogTime;
                                if (GetTime() - nLogTime > 2 * 60)
                                {
                                    nLogTime = GetTime();
                                    printf("hashmeter %f hash/s\n", dHashesPerSec);
                                }
                            }
                        }
                    }

                    // Check for stop or if block needs to be rebuilt
                    boost::this_thread::interruption_point();
                    if (vNodes.empty())
                        break;
                    if (pblock->nNonce >= 0xffff0000)
                        break;
                    if (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                        break;
                    if (pindexPrev != pindexBest)
                        break;

                    // Update nTime every few seconds
                    pblock->UpdateTime(pindexPrev);
                    nBlockTime = ByteReverse(pblock->nTime);
                    if (fTestNet)
                    {
                        // Changing pblock->nTime can change work required on testnet:
                        nBlockBits = ByteReverse(pblock->nBits);
                        hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
                    }

            }
        }
    }
    catch (boost::thread_interrupted)
    {
        printf("SmartcashMiner terminated\n");
        throw;
    }
}

void GenerateBitcoins(bool fGenerate, CWallet* pwallet)
{

    static boost::thread_group* minerThreads = NULL;

    int nThreads = GetArg("-genproclimit", -1);
    if (nThreads < 0)
        nThreads = boost::thread::hardware_concurrency();

    if (minerThreads != NULL)
    {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++)
        minerThreads->create_thread(boost::bind(&SmartcashMiner, pwallet));

}



// Amount compression:
// * If the amount is 0, output 0
// * first, divide the amount (in base units) by the largest power of 10 possible; call the exponent e (e is max 9)
// * if e<9, the last digit of the resulting number cannot be 0; store it as d, and drop it (divide by 10)
//   * call the result n
//   * output 1 + 10*(9*n + d - 1) + e
// * if e==9, we only know the resulting number is not zero, so output 1 + 10*(n - 1) + 9
// (this is decodable, as d is in [1-9] and e is in [0-9])

uint64 CTxOutCompressor::CompressAmount(uint64 n)
{
    if (n == 0)
        return 0;
    int e = 0;
    while (((n % 10) == 0) && e < 9) {
        n /= 10;
        e++;
    }
    if (e < 9) {
        int d = (n % 10);
        assert(d >= 1 && d <= 9);
        n /= 10;
        return 1 + (n*9 + d - 1)*10 + e;
    } else {
        return 1 + (n - 1)*10 + 9;
    }
}

uint64 CTxOutCompressor::DecompressAmount(uint64 x)
{
    // x = 0  OR  x = 1+10*(9*n + d - 1) + e  OR  x = 1+10*(n - 1) + 9
    if (x == 0)
        return 0;
    x--;
    // x = 10*(9*n + d - 1) + e
    int e = x % 10;
    x /= 10;
    uint64 n = 0;
    if (e < 9) {
        // x = 9*n + d - 1
        int d = (x % 9) + 1;
        x /= 9;
        // x = n
        n = x*10 + d;
    } else {
        n = x+1;
    }
    while (e) {
        n *= 10;
        e--;
    }
    return n;
}


class CMainCleanup
{
public:
    CMainCleanup() {}
    ~CMainCleanup() {
        // block headers
        BlockMap::iterator it1 = mapBlockIndex.begin();
        for (; it1 != mapBlockIndex.end(); it1++)
            delete (*it1).second;
        mapBlockIndex.clear();

        // orphan blocks
        std::map<uint256, CBlock*>::iterator it2 = mapOrphanBlocks.begin();
        for (; it2 != mapOrphanBlocks.end(); it2++)
            delete (*it2).second;
        mapOrphanBlocks.clear();

        // orphan transactions
        mapOrphanTransactions.clear();
        mapOrphanTransactionsByPrev.clear();
    }
} instance_of_cmaincleanup;
