// Copyright (c) 2017 - 2020 - The SmartCash Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "smartrewards/rewards.h"
#include "consensus/consensus.h"
#include "init.h"
#include "rewards.h"
#include "script/standard.h"
#include "smarthive/hive.h"
#include "smartnode/smartnodepayments.h"
#include "smartnode/spork.h"
#include "smartrewards/rewardspayments.h"
#include "ui_interface.h"
#include "validation.h"

#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/range/irange.hpp>
#include <boost/thread.hpp>

#define REWARDS_MAX_CACHE        400000000UL     // 400MB
#define SUPER_REWARDS_MIN_BALANCE_1_3 (999999 * COIN) // Reduce by 1 to allow for activation fee

CSmartRewards* prewards = NULL;

CCriticalSection cs_rewardsdb;
CCriticalSection cs_rewardscache;

size_t nCacheRewardEntries;

// Used for time conversions.
boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));

struct CompareRewardScore {
    bool operator()(const std::pair<arith_uint256, CSmartRewardResultEntry*>& s1,
        const std::pair<arith_uint256, CSmartRewardResultEntry*>& s2) const
    {
        return (s1.first != s2.first) ? (s1.first < s2.first) : (s1.second->entry.balance < s2.second->entry.balance);
    }
};

struct ComparePaymentPrtList {
    bool operator()(const CSmartRewardResultEntry* p1,
        const CSmartRewardResultEntry* p2) const
    {
        return *p1 < *p2;
    }
};

// Estimate or return the current block height.
int GetBlockHeight(const CBlockIndex* index)
{
    int64_t syncDiff = std::time(0) - index->GetBlockTime();
    int64_t firstTxDiff;
    if (MainNet())
        firstTxDiff = std::time(0) - nStartRewardTime; // Diff from the reward blocks start till now on mainnet.
    else
        firstTxDiff = std::time(0) - nFirstTxTimestamp_Testnet; // Diff from the first transaction till now on testnet.
    return syncDiff > 1200 ? firstTxDiff / 55 : index->nHeight; // If we are 20 minutes near now use the current height.
}

bool ExtractDestination(const CScript& scriptPubKey, CSmartAddress& idRet)
{
    vector<vector<unsigned char>> vSolutions;
    txnouttype whichType;
    if (!Solver(scriptPubKey, whichType, vSolutions))
        return false;

    if (whichType == TX_PUBKEY) {
        CPubKey pubKey(vSolutions[0]);
        if (!pubKey.IsValid())
            return false;

        idRet = CSmartAddress(pubKey.GetID());
        return true;
    } else if (whichType == TX_PUBKEYHASH) {
        idRet = CSmartAddress(CKeyID(uint160(vSolutions[0])));
        return true;
    } else if (whichType == TX_SCRIPTHASH) {
        idRet = CSmartAddress(CScriptID(uint160(vSolutions[0])));
        return true;
    } else if (whichType == TX_PUBKEYHASHLOCKED) {
        idRet = CSmartAddress(CKeyID(uint160(vSolutions[0])));
        return true;
    } else if (whichType == TX_SCRIPTHASHLOCKED) {
        idRet = CSmartAddress(CScriptID(uint160(vSolutions[0])));
        return true;
    }
    // Multisig txns have more than one address...
    return false;
}

bool CSmartRewards::Verify()
{
    LOCK(cs_rewardsdb);
    int nHeight = cache.GetCurrentBlock()->nHeight;
    return pdb->Verify(nHeight);
}

bool CSmartRewards::NeedsCacheWrite()
{
    return cache.NeedsSync();
}

void CSmartRewards::UpdateRoundPayoutParameter()
{
    AssertLockHeld(cs_rewardscache);

    const CSmartRewardRound* round = cache.GetCurrentRound();
    int64_t nBlockPayees = Params().GetConsensus().nRewardsPayouts_1_3_BlockPayees;
    int64_t nBlockInterval = Params().GetConsensus().nRewardsPayouts_1_3_BlockStretch / 20;
    int nFirst_1_3_Round = Params().GetConsensus().nRewardsFirst_1_3_Round;

    if ( round->number < (nFirst_1_3_Round + 3) ) {
        nBlockPayees = Params().GetConsensus().nRewardsPayouts_1_2_BlockPayees;
        nBlockInterval = Params().GetConsensus().nRewardsPayouts_1_2_BlockInterval;
    }
    int64_t nPayeeCount = round->eligibleEntries - round->disqualifiedEntries;
    if ( !nPayeeCount || nPayeeCount <= 0 ) {
        nBlockPayees = 0;
    }

    cache.UpdateRoundPayoutParameter(nBlockPayees, nBlockInterval);
}

void CSmartRewards::UpdatePercentage()
{
    AssertLockHeld(cs_rewardscache);

    const CSmartRewardRound *round = cache.GetCurrentRound();

    double nPercent = 0.0;
    if ( (round->eligibleSmart - round->disqualifiedSmart) > 0 ) {
        nPercent = double(round->rewards) / (round->eligibleSmart - round->disqualifiedSmart);
    }else{
        nPercent = 0;
    }

    cache.UpdateRoundPercent(nPercent);
}

bool CSmartRewards::Is_1_3(uint16_t round)
{
    if (round >= Params().GetConsensus().nRewardsFirst_1_3_Round) {
        return 1;
    } else {
        return 0;
    }
}

void CSmartRewards::EvaluateRound(CSmartRewardRound &next)
{
    LOCK(cs_rewardscache);

    UpdateRoundPayoutParameter();

    const CSmartRewardRound *round = cache.GetCurrentRound();

    int nFirst_1_3_Round = Params().GetConsensus().nRewardsFirst_1_3_Round;
    CAmount nMinBalance = next.number < nFirst_1_3_Round ? SMART_REWARDS_MIN_BALANCE_1_2 : SMART_REWARDS_MIN_BALANCE_1_3;

    CSmartRewardsRoundResult *pResult = new CSmartRewardsRoundResult();

    pResult->round = *round;
    pResult->round.UpdatePayoutParameter();

    CAmount nReward;
    CSmartRewardEntryMap tmpEntries;
    pdb->ReadRewardEntries(tmpEntries);

    for( auto itDb = tmpEntries.begin(); itDb != tmpEntries.end(); ++itDb){

        auto itCache = cache.GetEntries()->find(itDb->first);

        if( itCache == cache.GetEntries()->end() ){
            cache.AddEntry(itDb->second);
        }else{
            delete itDb->second;
        }
    }

    if( round->number >= nFirst_1_3_Round ) {
        int64_t nTime = GetTime();
        double dBlockReward = 0.60;

        // Calculate rewards for next cycle
        next.rewards = 0;
        int64_t nStartHeight = next.startBlockHeight;
        while( nStartHeight <= next.endBlockHeight) next.rewards += GetBlockValue(nStartHeight++, 0, nTime) * dBlockReward;

        // Compute payouts for current ending round
        auto entry = cache.GetEntries()->begin();
        while(entry != cache.GetEntries()->end() ) {
            nReward = entry->second->IsEligible() ? CAmount(entry->second->balanceEligible * round->percent) : 0;
            pResult->results.push_back(new CSmartRewardResultEntry(entry->second, nReward));
            if( nReward ){
                pResult->payouts.push_back(pResult->results.back());
            }
            if (round->number < Params().GetConsensus().nRewardsFirst_2_0_Round ) {
                // Reset outgoing transaction with every cycle.
                entry->second->disqualifyingTx.SetNull();
                entry->second->fDisqualifyingTx = false;

                // Reset SmartNode payment tx with every cycle in case a node was shut down during the cycle.
                entry->second->smartnodePaymentTx.SetNull();
                entry->second->fSmartnodePaymentTx = false;
            }
            // Reset the vote proof tx 2 cycles before the first 1.3 round.
            ++entry;
        }

        if( pResult->payouts.size() ){
            uint256 blockHash;
            if(!GetBlockHash(blockHash, pResult->round.startBlockHeight)) {
                throw std::runtime_error(strprintf("CSmartRewards::EvaluateRound -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", round->startBlockHeight));
            }

            std::vector<std::pair<arith_uint256, CSmartRewardResultEntry*>> vecScores;
            // Since we use payouts stretched out over a week better to have some "random" sort here
            // based on a score calculated with the round start's blockhash.
            CSmartRewardResultEntryPtrList::iterator it = pResult->payouts.begin();

            while( it != pResult->payouts.end() ){
                arith_uint256 nScore = (*it)->CalculateScore(blockHash);
                vecScores.push_back(std::make_pair(nScore,*it));
                ++it;
            }

            std::sort(vecScores.begin(), vecScores.end(), CompareRewardScore());

            pResult->payouts.clear();

            for(auto s : vecScores)
                pResult->payouts.push_back(s.second);
        }

        // Look for entries eligible to the next round
        std::list<CSmartAddress> eligibleAddresses;
        next.eligibleSmart = 0;
        next.eligibleEntries = 0;
        next.disqualifiedEntries = 0;
        next.disqualifiedSmart = 0;

        entry = cache.GetEntries()->begin();
        while(entry != cache.GetEntries()->end() ) {
            if( entry->second->balance >= nMinBalance && !SmartHive::IsHive(entry->second->id) && !entry->second->fDisqualifyingTx && entry->second->fActivated ) {
                entry->second->balanceEligible = entry->second->balance;
                next.eligibleSmart += entry->second->balanceEligible;
                ++next.eligibleEntries;
                eligibleAddresses.push_back(entry->first);
            } else {
                entry->second->balanceEligible = 0;
            }
            entry->second->balanceAtStart = entry->second->balance;
            ++entry;
        }

        // Check back all the 4 previous rounds for adding weighted balance if applicable
        if (next.number - 1 >= nFirst_1_3_Round && !eligibleAddresses.empty()) {
            int roundNumber = next.number - 1;
            while ((roundNumber >= nFirst_1_3_Round) && (roundNumber >= next.number - 4)) {
                CSmartRewardResultEntryList results;
                if (roundNumber == round->number) {
                    // Need to convert pointers list into objects list
                    results.reserve(pResult->results.size());
                    std::for_each(pResult->results.begin(), pResult->results.end(),
                            [&results] (const CSmartRewardResultEntry *e) {
                          results.push_back(*e);
                    });
                } else {
                    if (!GetRewardRoundResults(roundNumber, results)) {
                        break;
                    }
                }

                // Iterate over all still eligible addresses
                auto address = eligibleAddresses.begin();
                while (address != eligibleAddresses.end()) {
                    // Look for address in the round results
                    auto addressResult = std::find_if(results.begin(), results.end(),
                        [&address](const CSmartRewardResultEntry& e) -> bool {
                            return *address == e.entry.id;
                    });

                    // If address was not in last round result => remove from list
                    if (addressResult == results.end()) {
                        address = eligibleAddresses.erase(address);
                        continue;
                    }

                    // If the address balance was not eligible => remove from list
                    if (!addressResult->entry.balanceEligible) {
                        address = eligibleAddresses.erase(address);
                        continue;
                    }

                    // Calculate bonus based on current round eligibility
                    auto &cacheEntry = cache.GetEntries()->at(*address);
                    if (roundNumber == next.number - 1) {
                        if (addressResult->entry.balance > SUPER_REWARDS_MIN_BALANCE_1_3) {
                            next.eligibleSmart += addressResult->entry.balance;
                            cacheEntry->balanceEligible += addressResult->entry.balance;
                            cacheEntry->bonusLevel = CSmartRewardEntry::SuperBonus;
                        } else {
                            cacheEntry->bonusLevel = CSmartRewardEntry::NoBonus;
                        }
                    } else if (roundNumber == next.number - 2) {
                        next.eligibleSmart += 0.2 * addressResult->entry.balance;
                        cacheEntry->balanceEligible += 0.2 * addressResult->entry.balance;
                        cacheEntry->bonusLevel++;
                    } else if (roundNumber == next.number - 3) {
                        next.eligibleSmart += 0.2 * addressResult->entry.balance;
                        cacheEntry->balanceEligible += 0.2 * addressResult->entry.balance;
                        cacheEntry->bonusLevel++;
                    } else if (roundNumber == next.number - 4) {
                        next.eligibleSmart += 0.1 * addressResult->entry.balance;
                        cacheEntry->balanceEligible += 0.1 * addressResult->entry.balance;
                        cacheEntry->bonusLevel++;
                    }

                    address++;
                }

                // If there are no more eligible addresses to check, exit the loop
                if (eligibleAddresses.empty()) {
                    break;
                }

                roundNumber--;
            }
        }

        if( pResult->round.number ){
            cache.AddFinishedRound(pResult->round);
        }

        cache.SetResult(pResult);
        cache.SetCurrentRound(next);

    } else if( round->number && ( round->number < nFirst_1_3_Round )){
        auto entry = cache.GetEntries()->begin();
        while(entry != cache.GetEntries()->end() ) {

            nReward = entry->second->balanceEligible > 0 && !entry->second->fDisqualifyingTx ? CAmount(entry->second->balanceEligible * round->percent) : 0;

            pResult->results.push_back(new CSmartRewardResultEntry(entry->second, nReward));

            if( nReward ){
                pResult->payouts.push_back(pResult->results.back());
            }

            entry->second->balanceAtStart = entry->second->balance;

            if( entry->second->balance >= nMinBalance && !SmartHive::IsHive(entry->second->id) ){
                entry->second->balanceEligible = entry->second->balance;
            }else{
                entry->second->balanceEligible = 0;
            }

            // Reset outgoing transaction with every cycle.
            entry->second->disqualifyingTx.SetNull();
            entry->second->fDisqualifyingTx = false;

            // Reset SmartNode payment tx with every cycle in case a node was shut down during the cycle.
            entry->second->smartnodePaymentTx.SetNull();
            entry->second->fSmartnodePaymentTx = false;

            if( entry->second->balanceEligible ){
                ++next.eligibleEntries;
                next.eligibleSmart += entry->second->balanceEligible;
            }

            // Reset activations and reset eligible before 1.3 round starts.
            if( next.number == (nFirst_1_3_Round) ){
                entry->second->activationTx.SetNull();
                entry->second->fActivated = false;
                entry->second->bonusLevel = CSmartRewardEntry::NotEligible;
                next.eligibleEntries = 0;
                next.eligibleSmart = 0;
            }
            ++entry;
        }

        if( pResult->payouts.size() ){
            // Sort it to make sure the slices are the same network wide.
            std::sort(pResult->payouts.begin(), pResult->payouts.end(), ComparePaymentPrtList() );
        }

        // Calculate the current rewards percentage
        int64_t nTime = GetTime();
        int64_t nStartHeight = next.startBlockHeight;
        double dBlockReward = next.number < nFirst_1_3_Round ? 0.15 : 0.60;
        next.rewards = 0;
        while( nStartHeight <= next.endBlockHeight) next.rewards += GetBlockValue(nStartHeight++, 0, nTime) * dBlockReward;

        if( pResult->round.number ){
           cache.AddFinishedRound(pResult->round);
        }
        cache.SetResult(pResult);
        cache.SetCurrentRound(next);
    } else {
        // Calculate the current total smartrewards amount
        int64_t nTime = GetTime();
        int64_t nStartHeight = next.startBlockHeight;
        double dBlockReward = 0.15;
        next.rewards = 0;
        while( nStartHeight <= next.endBlockHeight) next.rewards += GetBlockValue(nStartHeight++, 0, nTime) * dBlockReward;

        cache.SetResult(pResult);
        cache.SetCurrentRound(next);
    }
}

bool CSmartRewards::GetRewardRoundResults(const int16_t round, CSmartRewardResultEntryList& results)
{
    LOCK(cs_rewardsdb);
    return pdb->ReadRewardRoundResults(round, results);
}

bool CSmartRewards::GetRewardRoundResults(const int16_t round, CSmartRewardResultEntryPtrList& results)
{
    LOCK(cs_rewardsdb);
    return pdb->ReadRewardRoundResults(round, results);
}

const CSmartRewardsRoundResult* CSmartRewards::GetLastRoundResult()
{
    return cache.GetLastRoundResult();
}

bool CSmartRewards::GetRewardPayouts(const int16_t round, CSmartRewardResultEntryList& payouts)
{
    LOCK(cs_rewardsdb);
    return pdb->ReadRewardPayouts(round, payouts);
}

bool CSmartRewards::GetRewardPayouts(const int16_t round, CSmartRewardResultEntryPtrList& payouts)
{
    LOCK(cs_rewardsdb);
    return pdb->ReadRewardPayouts(round, payouts);
}

bool CSmartRewards::GetRewardEntry(const CSmartAddress& id, CSmartRewardEntry*& entry, bool fCreate)
{
    LOCK(cs_rewardscache);

    // Return the entry if its already in cache.
    auto it = cache.GetEntries()->find(id);

    if (it != cache.GetEntries()->end()) {
        entry = it->second;
        return true;
    }

    entry = new CSmartRewardEntry(id);

    // Return the entry if its already in db.
    if (pdb->ReadRewardEntry(id, *entry)) {
        cache.AddEntry(entry);
        return true;
    }

    if (fCreate) {
        cache.AddEntry(entry);
        return true;
    }

    delete entry;
    entry = nullptr;

    return false;
}

bool CSmartRewards::GetTermRewardEntry(const std::pair<CSmartAddress, uint256>& id, CTermRewardEntry*& entry,
    bool fCreate)
{
    LOCK(cs_rewardscache);

    // Return the entry if its already in cache.
    auto it = cache.GetTermRewardsEntries()->find(id);

    if (it != cache.GetTermRewardsEntries()->end()) {
        entry = it->second;
        return true;
    }

    entry = new CTermRewardEntry(id.first, id.second);

    // Return the entry if its already in db.
    if (pdb->ReadTermRewardEntry(id, *entry)) {
        cache.AddTermRewardEntry(entry);
        return true;
    }

    if (fCreate) {
        cache.AddTermRewardEntry(entry);
        return true;
    }

    delete entry;
    entry = nullptr;

    return false;
}

bool CSmartRewards::GetRewardEntries(CSmartRewardEntryMap& entries)
{
    LOCK(cs_rewardsdb);
    return pdb->ReadRewardEntries(entries);
}

bool CSmartRewards::GetTermRewardsEntries(CTermRewardEntryMap& entries)
{
    LOCK(cs_rewardsdb);
    return pdb->ReadTermRewardEntries(entries);
}

bool CSmartRewards::SyncCached()
{
    LOCK2(cs_rewardscache, cs_rewardsdb);

    int nTimeStart = GetTimeMicros();
    int nEntriesPre = cache.GetEntries()->size();
    int nSizePre = cache.EstimatedSize();

    bool ret = pdb->SyncCached(cache);

    cache.Clear();

    int nTimeDone = GetTimeMicros();

    int nEntriesPost = cache.GetEntries()->size();
    int nSizePost = cache.EstimatedSize();

    LogPrint("smartrewards-bench", "CSmartRewards::SyncCached size before/after %dMB/%dMB, entries before/after %d/%d, time %.2fms", nSizePre / 1000000, nSizePost / 1000000, nEntriesPre, nEntriesPost, (nTimeDone - nTimeStart) * 0.001);

    return ret;
}

bool CSmartRewards::IsSynced()
{
    static bool fSynced = false;

    if (fSynced)
        return true;

    fSynced = (GetTime() - cache.GetCurrentBlock()->nTime) <= nRewardsSyncDistance;

    return fSynced;
}

int CSmartRewards::GetBlocksPerRound(const int nRound)
{
    const CChainParams& chainparams = Params();

    if (nRound < chainparams.GetConsensus().nRewardsFirst_1_3_Round) {
        return chainparams.GetConsensus().nRewardsBlocksPerRound_1_2;
    } else {
        return chainparams.GetConsensus().nRewardsBlocksPerRound_1_3;
    }
}

CSmartRewards::CSmartRewards(CSmartRewardsDB* prewardsdb) : pdb(prewardsdb)
{
    LOCK2(cs_rewardscache, cs_rewardsdb);

    CSmartRewardBlock block;
    CSmartRewardRound round;
    CSmartRewardRoundMap rounds;
    CSmartRewardResultEntryList results;

    pdb->ReadLastBlock(block);
    pdb->ReadCurrentRound(round);
    pdb->ReadRounds(rounds);

    cache.Load(block, round, rounds);

    CSmartRewardsRoundResult* pResult = new CSmartRewardsRoundResult();

    if (round.number > 1) {
        pResult->fSynced = true;
        pResult->round = rounds[round.number - 1];
        pdb->ReadRewardRoundResults(round.number - 1, pResult->results);

        for (auto it : pResult->results) {
            if (it->reward) {
                pResult->payouts.push_back(it);
            }
        }
    }

    cache.SetResult(pResult);

    LogPrintf("CSmartRewards::CSmartRewards\n  Last block %s\n  Current Round %s\n  Rounds: %d", block.ToString(), round.ToString(), rounds.size());
}

bool CSmartRewards::GetLastBlock(CSmartRewardBlock& block)
{
    LOCK(cs_rewardsdb);
    // Read the last block stored in the rewards database.
    return pdb->ReadLastBlock(block);
}

bool CSmartRewards::GetTransaction(const uint256 nHash, CSmartRewardTransaction& transaction)
{
    LOCK2(cs_rewardscache, cs_rewardsdb);
    // If the transaction is already in the cache use this one.
    auto it = cache.GetAddedTransactions()->find(nHash);

    if (it != cache.GetAddedTransactions()->end()) {
        transaction = it->second;
        return true;
    }

    return pdb->ReadTransaction(nHash, transaction);
}

const CSmartRewardRound* CSmartRewards::GetCurrentRound()
{
    return cache.GetCurrentRound();
}

const CSmartRewardRoundMap* CSmartRewards::GetRewardRounds()
{
    return cache.GetRounds();
}

void CSmartRewards::ProcessInput(const CTransaction& tx, const CTxOut& in, int txHeight, uint16_t nCurrentRound, CSmartRewardsUpdateResult& result)
{
    uint16_t nFirst_1_3_Round = Params().GetConsensus().nRewardsFirst_1_3_Round;
    CSmartRewardEntry* rEntry = nullptr;
    CSmartAddress id;

    if (!ExtractDestination(in.scriptPubKey, id)) {
        LogPrint("smartrewards-tx", "CSmartRewards::ProcessInput - Could't parse CSmartAddress: %s\n", in.ToString());
        return;
    }

    if (!GetRewardEntry(id, rEntry, false)) {
        return;
    }

    if (nCurrentRound >= nFirst_1_3_Round && tx.IsActivationTx() && !rEntry->fActivated) {
        rEntry->activationTx = tx.GetHash();
        rEntry->fActivated = true;
        rEntry->bonusLevel = CSmartRewardEntry::NoBonus;
    }

    if (!in.GetLockTime() ||  ((txHeight < HF_V2_0_HEIGHT) && MainNet()) || ((txHeight < TESTNET_V2_0_HEIGHT) && TestNet()) ) {
        rEntry->balance -= in.nValue;
    }

    if (nCurrentRound >= nFirst_1_3_Round && !tx.IsActivationTx() && !rEntry->fDisqualifyingTx) {
        if( rEntry->IsEligible() ){
            result.disqualifiedEntries++;
            result.disqualifiedSmart += rEntry->balanceEligible;
        }
        rEntry->disqualifyingTx = tx.GetHash();
        rEntry->fDisqualifyingTx = true;
    }

    if (nCurrentRound >= nFirst_1_3_Round && rEntry->fActivated && !tx.IsActivationTx()) {
//        rEntry->activationTx.SetNull();
        rEntry->activationTx = tx.GetHash();
        rEntry->fActivated = false;
        rEntry->bonusLevel = CSmartRewardEntry::NotEligible;
    }

    if (nCurrentRound < nFirst_1_3_Round && !rEntry->fDisqualifyingTx) {

        rEntry->disqualifyingTx = tx.GetHash();
        rEntry->fDisqualifyingTx = true;

        if( rEntry->balanceEligible ){
            result.disqualifiedEntries++;
            result.disqualifiedSmart += rEntry->balanceEligible;
        }

    }

    if (rEntry->balance < 0) {
        rEntry->balance = 0;
    }
}

void CSmartRewards::ProcessOutput(const CTransaction& tx, const CTxOut& out, uint16_t nCurrentRound, int nHeight,
    unsigned int nTime, CSmartRewardsUpdateResult& result)
{
    CSmartRewardEntry* rEntry = nullptr;
    CTermRewardEntry* rTermEntry = nullptr;
    CSmartAddress id;

    if (!ExtractDestination(out.scriptPubKey, id)) {
        LogPrint("smartrewards-tx", "CSmartRewards::ProcessOutput - Could't parse CSmartAddress: %s\n", out.ToString());
        return;
    } else {
        if (GetRewardEntry(id, rEntry, true)) {
            if (tx.IsActivationTx() && Is_1_3(nCurrentRound) && !SmartHive::IsHive(rEntry->id)) {
                if (!rEntry->fActivated) {
                    rEntry->activationTx = tx.GetHash();
                    rEntry->fActivated = true;
                    rEntry->bonusLevel = CSmartRewardEntry::NoBonus;
                    if (nCurrentRound >= Params().GetConsensus().nRewardsFirst_2_0_Round ) {
		        // Reset outgoing transaction.
                        rEntry->disqualifyingTx.SetNull();
                        rEntry->fDisqualifyingTx = false;
                        // Reset SmartNode payment.
                        rEntry->smartnodePaymentTx.SetNull();
                        rEntry->fSmartnodePaymentTx = false;
                    }
                    if ( rEntry->IsEligible() ) {
                       result.qualifiedEntries++;
                       result.qualifiedSmart += rEntry->balanceEligible;
                    }
                }
            }
        }

        // Disable SmartRewards from locked outputs to allow payimg based on TermRewards
        if (!out.GetLockTime() ||
                ((nCurrentRound < Params().GetConsensus().nRewardsFirst_2_0_Round) && MainNet()) ||
                ((nCurrentRound < 20) && TestNet())) {  //Round 46 ends 1860599 10/24 (activates around 11/28)
            rEntry->balance += out.nValue;
        } else if ( (out.nValue >= 1000000 * COIN) && (nCurrentRound >= Params().GetConsensus().nRewardsFirst_2_0_Round) && MainNet() ) {
                if (out.GetLockTime() > (31500000 + nTime)) {
                    // At least one year then create an entry
                    if (GetTermRewardEntry({id, tx.GetHash()}, rTermEntry, true)) {
                        rTermEntry->level = CTermRewardEntry::OneYear;
                        rTermEntry->percent = 40;
                        rTermEntry->expires = out.GetLockTime();
                        if (out.GetLockTime() > (94500000 + nTime)) {
                            // If minimum 3 years
                            rTermEntry->level = CTermRewardEntry::ThreeYears;
                            rTermEntry->percent = 60;
                        } else if (out.GetLockTime() > (63000000 + nTime)) {
                            // Mininimum 2 years
                            rTermEntry->level = CTermRewardEntry::TwoYears;
                            rTermEntry->percent = 50;
                        }
                    }

                    rTermEntry->balance = out.nValue;
                    LogPrintf("CSmartRewards::ProcessOutput: Output qualifies for %s TermRewards\n",
                        rTermEntry->GetLevel());
                }
        } else if ( (out.nValue >= 1000 * COIN) && (nCurrentRound >= 20) && TestNet() ) {
                if (out.GetLockTime() > (31500000 + nTime)) {
                    // At least one year 31500000 94500000
                    if (GetTermRewardEntry({id, tx.GetHash()}, rTermEntry, true)) {
                        rTermEntry->level = CTermRewardEntry::OneYear;
                        rTermEntry->percent = 40;
                        rTermEntry->expires = out.GetLockTime();
                        if (out.GetLockTime() > (94500000 + nTime)) {
                            // If minimum 3 years
                            rTermEntry->level = CTermRewardEntry::ThreeYears;
                            rTermEntry->percent = 60;
                        } else if (out.GetLockTime() > (63000000 + nTime)) {
                            // Mininimum 2 years
                            rTermEntry->level = CTermRewardEntry::TwoYears;
                            rTermEntry->percent = 50;
                        }
                    }

                    rTermEntry->balance = out.nValue;
                    LogPrintf("CSmartRewards::ProcessOutput: Output qualifies for %s TermRewards\n",
                        rTermEntry->GetLevel());
                }
        }

        if (Is_1_3(nCurrentRound) && tx.IsCoinBase()) {
            int nInterval = SmartNodePayments::PayoutInterval(nHeight);
            int nPayoutsPerBlock = SmartNodePayments::PayoutsPerBlock(nHeight);
            // Just to avoid potential zero divisions
            nPayoutsPerBlock = std::max(1, nPayoutsPerBlock);

            CAmount nNodeReward = SmartNodePayments::Payment(nHeight) / nPayoutsPerBlock;
            // If we have an interval check if this is a node payout block
            if (nInterval && !(nHeight % nInterval)) {
                   // If the amount matches and the entry is not yet marked as node do it
                if (abs(out.nValue - nNodeReward) < 2) {
                    if (!rEntry->fSmartnodePaymentTx) {
                        // If it is currently eligible adjust the round's results
                        if (rEntry->IsEligible() ) {
                            ++result.disqualifiedEntries;
                            result.disqualifiedSmart += rEntry->balanceEligible;
//                            rEntry->activationTx.SetNull();
                            rEntry->activationTx = tx.GetHash();
                            rEntry->fActivated = false;
                            rEntry->bonusLevel = CSmartRewardEntry::NotEligible;
                            if (nCurrentRound < Params().GetConsensus().nRewardsFirst_2_0_Round ) {
                                rEntry->disqualifyingTx = tx.GetHash();
                                rEntry->fDisqualifyingTx = true;
                            }
                        }
                        rEntry->smartnodePaymentTx = tx.GetHash();
                        rEntry->fSmartnodePaymentTx = true;
                    }
                }
            }
        }
    }
}

bool CSmartRewards::ProcessTransaction(CBlockIndex* pIndex, const CTransaction& tx, int nCurrentRound)
{
    LogPrint("smartrewards-tx", "CSmartRewards::ProcessTransaction - %s", tx.GetHash().ToString());

    int nHeight = pIndex->nHeight;

    if (nHeight == 0 || nHeight > sporkManager.GetSporkValue(SPORK_15_SMARTREWARDS_BLOCKS_ENABLED)) {
        return false;
    }

    CSmartRewardTransaction testTx;

    // For the first 4 rounds we have zerocoin exploits and we don't want to add them to the rewards db.
    if (nCurrentRound <= 4) {
        // First check if the transaction hash did already come up in the past.
        if (GetTransaction(tx.GetHash(), testTx)) {
            // If yes we want to ignore it! There are some double appearing transactions in the history due to zerocoin exploits.
            LogPrint("smartrewards-tx", "CSmartRewards::ProcessTransaction - [%s] Double appearance! First in %d - Now in %d\n", testTx.hash.ToString(), testTx.blockHeight, pIndex->nHeight);
            return false;
        } else {
            // If not save add it to the cache.
            cache.AddTransaction(CSmartRewardTransaction(pIndex->nHeight, tx.GetHash()));
        }
    }

    return true;
}

void CSmartRewards::UndoInput(const CTransaction& tx, const CTxOut& in, int txHeight, uint16_t nCurrentRound, CSmartRewardsUpdateResult& result)
{
    uint16_t nFirst_1_3_Round = Params().GetConsensus().nRewardsFirst_1_3_Round;
    CSmartRewardEntry* rEntry = nullptr;
    CSmartAddress id;

    if (!ExtractDestination(in.scriptPubKey, id)) {
        LogPrint("smartrewards-tx", "CSmartRewards::UndoInput - Process Inputs: Could't parse CSmartAddress: %s\n", in.ToString());
        return;
    }

    if (!GetRewardEntry(id, rEntry, true)) {
        LogPrint("smartrewards-tx", "CSmartRewards::UndoInput - Spend without previous receive - %s", tx.ToString());
        return;
    }
    // 1.3.4 rules effective at the start of round 52 or 1915600 and round 10 block 5000
    if ( !in.GetLockTime() || ((txHeight < HF_V2_0_HEIGHT /*1915600*/) && MainNet()) || ((txHeight < TESTNET_V2_0_HEIGHT) && TestNet()) ) {
        rEntry->balance += in.nValue;
    }

    if ( nCurrentRound >= nFirst_1_3_Round && rEntry->disqualifyingTx == tx.GetHash()) {
        rEntry->disqualifyingTx.SetNull();
        rEntry->fDisqualifyingTx = false;
        if (rEntry->activationTx == tx.GetHash()) {
            rEntry->fActivated = true;
        }
        if (rEntry->IsEligible()) {
            --result.disqualifiedEntries;
            result.disqualifiedSmart -= rEntry->balanceEligible;
        }

    } else if (nCurrentRound < nFirst_1_3_Round && rEntry->disqualifyingTx == tx.GetHash()) {
        rEntry->disqualifyingTx.SetNull();
        rEntry->fDisqualifyingTx = false;

        if (rEntry->balanceEligible) {
            --result.disqualifiedEntries;
            result.disqualifiedSmart -= rEntry->balanceEligible;
        }
        if(rEntry->balance < 0 ){
            LogPrint("smartrewards-tx", "CSmartRewards::UndoInput - Negative amount - %s", rEntry->ToString());
            rEntry->balance = 0;
        }
    }
}

void CSmartRewards::UndoOutput(const CTransaction& tx, const CTxOut& out, uint16_t nCurrentRound, CSmartRewardsUpdateResult& result)
{
    CSmartRewardEntry* rEntry = nullptr;
    CSmartAddress id;

    if (!ExtractDestination(out.scriptPubKey, id)) {
        LogPrint("smartrewards-tx", "CSmartRewards::UndoOutput - Process Outputs: Could't parse CSmartAddress: %s\n", out.ToString());
        return;
    } else {
        GetRewardEntry(id, rEntry, true);
        if (!tx.IsActivationTx() && !rEntry->fActivated) {
            if (nCurrentRound < Params().GetConsensus().nRewardsFirst_2_0_Round ) {
                rEntry->activationTx.SetNull();
                rEntry->fActivated = false;
                rEntry->bonusLevel = CSmartRewardEntry::NotEligible;
            }
            if (rEntry->disqualifyingTx == tx.GetHash()) {
                rEntry->disqualifyingTx.SetNull();
                rEntry->fDisqualifyingTx = false;
                if (rEntry->activationTx == tx.GetHash()) {
                    rEntry->fActivated = true;
                if (rEntry->bonusLevel == CSmartRewardEntry::NotEligible) {
                    rEntry->bonusLevel = CSmartRewardEntry::NoBonus;
                }
                }
                if (rEntry->IsEligible() && !tx.IsCoinBase()) {
                    --result.disqualifiedEntries;
                    result.disqualifiedSmart -= rEntry->balanceEligible;
                }
            }
//        } else if (!tx.IsActivationTx() && rEntry->fActivated && !SmartHive::IsHive(rEntry->id)) {
        } else if (tx.IsActivationTx() && rEntry->fActivated && !SmartHive::IsHive(rEntry->id)) {
            if (rEntry->activationTx == tx.GetHash()) {
                rEntry->activationTx.SetNull();
                rEntry->fActivated = false;
                // Don't assume this isn't a duplicate activation transaciton
                if (rEntry->bonusLevel == CSmartRewardEntry::NoBonus) {
                    rEntry->bonusLevel = CSmartRewardEntry::NotEligible;
                }
                --result.qualifiedEntries;
                result.qualifiedSmart -= rEntry->balanceEligible;
            }
        }
        if (!out.GetLockTime() || ((nCurrentRound < Params().GetConsensus().nRewardsFirst_2_0_Round) && MainNet()) || ((nCurrentRound < 20) && TestNet()) ) {
            rEntry->balance -= out.nValue;
        }
        // If we are in the 1.3 cycles check for node rewards to remove node addresses from lists
        if (Is_1_3(nCurrentRound) && tx.IsCoinBase()) {
            if (rEntry->smartnodePaymentTx == tx.GetHash()) {
                rEntry->smartnodePaymentTx.SetNull();
                rEntry->fSmartnodePaymentTx = false;
                // If it is eligible now adjust the round's results
                if (rEntry->IsEligible()) {
                    --result.disqualifiedEntries;
                    result.disqualifiedSmart -= rEntry->balanceEligible;
                }
            }
        }
    }
    if (rEntry->balance < 0) {
        LogPrint("smartrewards-tx", "CSmartRewards::UndoOutput - Negative amount - %s", rEntry->ToString());
        rEntry->balance = 0;
    }
}

void CSmartRewards::UndoTransaction(CBlockIndex* pIndex, const CTransaction& tx, CCoinsViewCache& coins, const CChainParams& chainparams, CSmartRewardsUpdateResult& result)
{
    int nCurrentRound;

    {
        LOCK(cs_rewardscache);
        nCurrentRound = cache.GetCurrentRound()->number;
    }

    int nHeight = pIndex->nHeight;

    if (nHeight > sporkManager.GetSporkValue(SPORK_15_SMARTREWARDS_BLOCKS_ENABLED)) {
        return;
    }

    int nTime1 = GetTimeMicros();


    CSmartRewardTransaction testTx;

    if (GetTransaction(tx.GetHash(), testTx) && testTx.blockHeight == pIndex->nHeight) {
        cache.RemoveTransaction(testTx);
    } else if (nCurrentRound <= 4) {
        return;
    }

    BOOST_REVERSE_FOREACH (const CTxOut& out, tx.vout) {
        if (out.scriptPubKey.IsZerocoinMint())
            continue;

        UndoOutput(tx, out, nCurrentRound, result);
    }

    int nTime2 = GetTimeMicros();

    // No reason to check the input here for new coins.
    if (!tx.IsCoinBase()) {
        BOOST_REVERSE_FOREACH (const CTxIn& in, tx.vin) {
            if (in.scriptSig.IsZerocoinSpend())
                continue;

            const Coin& coin = coins.AccessCoin(in.prevout);
            const CTxOut& rOut = coin.out;

            UndoInput(tx, rOut, nHeight, nCurrentRound, result);
        }
    }

    int nTime3 = GetTimeMicros();
    double dProcessingTime = (nTime3 - nTime1) * 0.001;

    if (dProcessingTime > 10.0 && LogAcceptCategory("smartrewards-tx")) {
        LogPrint("smartrewards-tx", "CSmartRewards::UndoTransaction - TX %s - %.2fms\n", tx.GetHash().ToString(), dProcessingTime);
        LogPrint("smartrewards-tx", " outputs - %.2fms\n", (nTime2 - nTime1) * 0.001);
        LogPrint("smartrewards-tx", " inputs - %.2fms\n", (nTime3 - nTime2) * 0.001);
    }
}

bool CSmartRewards::CommitBlock(CBlockIndex* pIndex, const CSmartRewardsUpdateResult& result)
{
    int nTime1 = GetTimeMicros();

    if (pIndex && pIndex->nHeight > sporkManager.GetSporkValue(SPORK_15_SMARTREWARDS_BLOCKS_ENABLED)) {
        return true;
    }

    LOCK(cs_rewardscache);

    const CSmartRewardRound* round = cache.GetCurrentRound();

    if (!pIndex || pIndex->nHeight != cache.GetCurrentBlock()->nHeight + 1) {
        LogPrintf("CSmartRewards::CommitBlock - Invalid next block!");
        return false;
    }

    if (cache.GetLastRoundResult() &&
        cache.GetLastRoundResult()->fSynced &&
        pIndex->nHeight > cache.GetLastRoundResult()->round.GetLastRoundBlock()) {
        cache.ClearResult();
    }

    cache.ApplyRoundUpdateResult(result);

    // For the first round we have special parameter..
    if (!round->number) {
        if ((MainNet() && pIndex->GetBlockTime() > nFirstRoundStartTime) ||
            (TestNet() && pIndex->nHeight >= nFirstRoundStartBlock_Testnet)) {
            // Create the very first smartrewards round.
            CSmartRewardRound first;
            first.number = 1;
            first.startBlockTime = pIndex->GetBlockTime();
            first.startBlockHeight = MainNet() ? nFirstRoundStartBlock : nFirstRoundStartBlock_Testnet;
            first.endBlockTime = MainNet() ? nFirstRoundEndTime : nFirstRoundEndTime_Testnet;
            // Estimate the block, gets updated on the end of the round to the real one.
            first.endBlockHeight = MainNet() ? nFirstRoundEndBlock : nFirstRoundEndBlock_Testnet;

            // Evaluate the round and update the next rounds parameter.
            EvaluateRound(first);
        }
    }

    // If just hit the next round threshold
    if ((MainNet() && round->number < nRewardsFirstAutomatedRound - 1 && pIndex->GetBlockTime() > round->endBlockTime) ||
        ((TestNet() || round->number >= nRewardsFirstAutomatedRound - 1) && pIndex->nHeight == round->endBlockHeight)) {
        UpdatePercentage();

        cache.UpdateRoundEnd(pIndex->nHeight, pIndex->GetBlockTime());

        // Create the next round.
        CSmartRewardRound next;
        next.number = round->number + 1;
        next.startBlockTime = round->endBlockTime;
        next.startBlockHeight = round->endBlockHeight + 1;

        int nBlocksPerRound = GetBlocksPerRound(next.number);
        time_t startTime = (time_t)next.startBlockTime;

        if (MainNet()) {
            if (next.number == nRewardsFirstAutomatedRound - 1) {
                // Let the round 12 end at height 574099 so that round 13 starts at 574100
                next.endBlockHeight = HF_V1_2_SMARTREWARD_HEIGHT - 1;
                next.endBlockTime = startTime + ((next.endBlockHeight - next.startBlockHeight) * 55);
            } else if (next.number < nRewardsFirstAutomatedRound) {
                boost::gregorian::date endDate = boost::posix_time::from_time_t(startTime).date();

                endDate += boost::gregorian::months(1);
                // End date at 00:00:00 + 25200 seconds (7 hours) to match the date at 07:00 UTC
                next.endBlockTime = time_t((boost::posix_time::ptime(endDate, boost::posix_time::seconds(25200)) - epoch).total_seconds());
                next.endBlockHeight = next.startBlockHeight + ((next.endBlockTime - next.startBlockTime) / 55);
            } else {
                next.endBlockHeight = next.startBlockHeight + nBlocksPerRound - 1;
                next.endBlockTime = startTime + nBlocksPerRound * 55;
            }

        } else {
            next.endBlockHeight = next.startBlockHeight + nBlocksPerRound - 1;
            next.endBlockTime = startTime + nBlocksPerRound * 55;
        }

        // Evaluate the round and update the next rounds parameter.
        EvaluateRound(next);
    }

    UpdatePercentage();

    cache.UpdateHeights(GetBlockHeight(pIndex), cache.GetCurrentBlock()->nHeight);

    if (LogAcceptCategory("smartrewards-bench")) {
        int nTime2 = GetTimeMicros();
        double dProcessingTime = (nTime2 - nTime1) * 0.001;
        LogPrint("smartrewards-bench", "Round %d - Block: %d\n", round->number, cache.GetCurrentBlock()->nHeight);
        LogPrint("smartrewards-bench", "  Commit block: %.2fms\n", dProcessingTime);
    }

    // If we are synced notify the UI on each new block.
    // If not notify the UI every nRewardsUISyncUpdateRate blocks to let it update the
    // loading screen.
    if (IsSynced() || !(cache.GetCurrentBlock()->nHeight % nRewardsUISyncUpdateRate))
        uiInterface.NotifySmartRewardUpdate();

    return true;
}

bool CSmartRewards::CommitUndoBlock(CBlockIndex* pIndex, const CSmartRewardsUpdateResult& result)
{
    int nTime1 = GetTimeMicros();

    if (pIndex && pIndex->nHeight > sporkManager.GetSporkValue(SPORK_15_SMARTREWARDS_BLOCKS_ENABLED)) {
        return true;
    }

    LOCK2(cs_rewardscache, cs_rewardsdb);

    if (!pIndex || pIndex->nHeight != cache.GetCurrentBlock()->nHeight) {
        LogPrintf("CSmartRewards::CommitUndoBlock - Invalid next block!");
        return false;
    }

    const CSmartRewardRound* pRound = cache.GetCurrentRound();

    CSmartRewardsUpdateResult undoUpdate(pIndex->pprev->nHeight, pIndex->pprev->phashBlock, pIndex->pprev->GetBlockTime());

    undoUpdate.disqualifiedEntries = result.disqualifiedEntries;
    undoUpdate.disqualifiedSmart = result.disqualifiedSmart;
    undoUpdate.qualifiedEntries = result.qualifiedEntries;
    undoUpdate.qualifiedSmart = result.qualifiedSmart;

    cache.ApplyRoundUpdateResult(undoUpdate);

    // If just hit the last round's threshold
    if ((MainNet() && pRound->number < nRewardsFirstAutomatedRound - 1 && pIndex->GetBlockTime() < pRound->endBlockTime) ||
        (((TestNet() && pRound->number > 1) || pRound->number >= nRewardsFirstAutomatedRound - 1) && pIndex->nHeight == pRound->startBlockHeight)) {
        // Recover the last round from the history as current round
        CSmartRewardRound prevRound = cache.GetRounds()->at(pRound->number - 1);

        cache.SetCurrentRound(prevRound);
        cache.RemoveFinishedRound(prevRound.number);

        CSmartRewardsRoundResult* undoResult = new CSmartRewardsRoundResult();

        undoResult->round = prevRound;

        if (!GetRewardRoundResults(prevRound.number, undoResult->results)) {
            LogPrintf("CSmartRewards::CommitUndoBlock - Failed to read last round's results!");
            return false;
        }

        cache.SetUndoResult(undoResult);

        // Load all entries into the cache
        CSmartRewardEntryMap tmpEntries;
        pdb->ReadRewardEntries(tmpEntries);

        for (auto itDb = tmpEntries.begin(); itDb != tmpEntries.end(); ++itDb) {
            auto itCache = cache.GetEntries()->find(itDb->first);

            if (itCache == cache.GetEntries()->end()) {
                cache.AddEntry(itDb->second);
            } else {
                delete itDb->second;
            }
        }
    }

    UpdatePercentage();

    cache.UpdateHeights(GetBlockHeight(pIndex), cache.GetCurrentBlock()->nHeight);

    int nTime2 = GetTimeMicros();

    if (LogAcceptCategory("smartrewards-block")) {
        LogPrint("smartrewards-block", "Round %d - Block: %d\n", pRound->number, cache.GetCurrentBlock()->nHeight);
        LogPrint("smartrewards-block", "  Commit undo block: %.2fms\n", (nTime2 - nTime1) * 0.001);
    }

    return true;
}

CSmartRewardsCache::~CSmartRewardsCache()
{
    LOCK(cs_rewardscache);

    for (std::pair<CSmartAddress, CSmartRewardEntry*> it : entries) {
        delete it.second;
    }

    if (result) {
        result->Clear();
        delete result;
    }

    block = CSmartRewardBlock();
    round = CSmartRewardRound();
    rounds.clear();
    addTransactions.clear();
    removeTransactions.clear();
    entries.clear();
}

unsigned long CSmartRewardsCache::EstimatedSize()
{
    unsigned long nEntriesSize = entries.size() * (sizeof(CSmartAddress) + sizeof(CSmartRewardEntry));
    unsigned long nRoundsSize = (rounds.size() + 1) * sizeof(CSmartRewardRound);
    unsigned long nTransactionsSize = (addTransactions.size() + removeTransactions.size()) * (sizeof(uint256) + sizeof(CSmartRewardTransaction));
    unsigned long nBlockSize = sizeof(CSmartRewardBlock);
    return nEntriesSize + nRoundsSize + nTransactionsSize + nBlockSize;
}

void CSmartRewardsCache::Load(const CSmartRewardBlock& block, const CSmartRewardRound& round, const CSmartRewardRoundMap& rounds)
{
    this->block = block;
    this->round = round;
    this->rounds = rounds;
}

bool CSmartRewardsCache::NeedsSync()
{
    LOCK(cs_rewardscache);
    return (result != nullptr && !result->fSynced) ||
           (undoResults != nullptr && !undoResults->fSynced) ||
           EstimatedSize() > REWARDS_MAX_CACHE || entries.size() > nCacheRewardEntries;
}

void CSmartRewardsCache::Clear()
{
    LOCK(cs_rewardscache);

    if (result) {
        result->fSynced = true;
    }

    if (undoResults) {
        undoResults->fSynced = true;
    }

    for (auto it = entries.cbegin(); it != entries.cend(); it++) {
        delete it->second;
    }

    entries.clear();
    addTransactions.clear();
    removeTransactions.clear();
}

void CSmartRewardsCache::ClearResult()
{
    if (result) {
        auto it = result->results.begin();

        while (it != result->results.end()) {
            delete *it;
            ++it;
        }

        result->results.clear();
        result->payouts.clear();
        delete result;
        result = nullptr;
    }

    if (undoResults) {
        auto it = undoResults->results.begin();

        while (it != undoResults->results.end()) {
            delete *it;
            ++it;
        }

        undoResults->results.clear();
        undoResults->payouts.clear();
        delete undoResults;
        undoResults = nullptr;
    }
}

void CSmartRewardsCache::SetCurrentBlock(const CSmartRewardBlock& currentBlock)
{
    AssertLockHeld(cs_rewardscache);
    block = currentBlock;
}

void CSmartRewardsCache::SetCurrentRound(const CSmartRewardRound& currentRound)
{
    AssertLockHeld(cs_rewardscache);
    round = currentRound;
}

void CSmartRewardsCache::SetResult(CSmartRewardsRoundResult* pResult)
{
    AssertLockHeld(cs_rewardscache);

    if (result) {
        result->Clear();
        delete result;
    }

    result = pResult;
}

void CSmartRewardsCache::SetUndoResult(CSmartRewardsRoundResult* pResult)
{
    AssertLockHeld(cs_rewardscache);

    if (undoResults) {
        undoResults->Clear();
        delete undoResults;
    }

    undoResults = pResult;
}

void CSmartRewardsCache::ApplyRoundUpdateResult(const CSmartRewardsUpdateResult& result)
{
    AssertLockHeld(cs_rewardscache);

    if (result.IsValid()) {
        SetCurrentBlock(result.block);

        round.disqualifiedEntries += result.disqualifiedEntries;
        round.disqualifiedSmart += result.disqualifiedSmart;

        round.eligibleEntries += result.qualifiedEntries;
        round.eligibleSmart += result.qualifiedSmart;
    }
}

void CSmartRewardsCache::UpdateRoundPayoutParameter(int64_t nBlockPayees, int64_t nBlockInterval)
{
    AssertLockHeld(cs_rewardscache);

    round.nBlockPayees = nBlockPayees;
    round.nBlockInterval = nBlockInterval;
}

void CSmartRewardsCache::UpdateRoundEnd(int nBlockHeight, int64_t nBlockTime)
{
    AssertLockHeld(cs_rewardscache);

    round.endBlockHeight = nBlockHeight;
    round.endBlockTime = nBlockTime;
}

void CSmartRewardsCache::UpdateRoundPercent(double dPercent)
{
    AssertLockHeld(cs_rewardscache);

    round.percent = dPercent;
}

void CSmartRewardsCache::UpdateHeights(const int nHeight, const int nRewardHeight)
{
    AssertLockHeld(cs_rewardscache);

    chainHeight = nHeight;
    rewardHeight = nRewardHeight;
}

void CSmartRewardsCache::AddFinishedRound(const CSmartRewardRound& round)
{
    AssertLockHeld(cs_rewardscache);

    rounds[round.number] = round;
}

void CSmartRewardsCache::RemoveFinishedRound(const int& nNumber)
{
    AssertLockHeld(cs_rewardscache);

    auto it = rounds.find(nNumber);

    if (it != rounds.end()) {
        rounds.erase(it);
    }
}

void CSmartRewardsCache::AddTransaction(const CSmartRewardTransaction& transaction)
{
    LOCK(cs_rewardscache);
    auto it = removeTransactions.find(transaction.hash);

    if (it != removeTransactions.end()) {
        removeTransactions.erase(it);
    } else {
        addTransactions.insert(std::make_pair(transaction.hash, transaction));
    }
}

void CSmartRewardsCache::RemoveTransaction(const CSmartRewardTransaction& transaction)
{
    LOCK(cs_rewardscache);
    auto it = addTransactions.find(transaction.hash);

    if (it != addTransactions.end()) {
        addTransactions.erase(it);
    } else {
        removeTransactions.insert(std::make_pair(transaction.hash, transaction));
    }
}

void CSmartRewardsCache::AddEntry(CSmartRewardEntry* entry)
{
    LOCK(cs_rewardscache);
    entries[entry->id] = entry;
}

void CSmartRewardsCache::AddTermRewardEntry(CTermRewardEntry *entry)
{
    LOCK(cs_rewardscache);
    termRewardEntries[{entry->address, entry->txHash}] = entry;
}

void CSmartRewardsRoundResult::Clear()
{
    for (CSmartRewardResultEntry* resultEntry : results) {
        delete resultEntry;
    }

    results.clear();
    payouts.clear();
}
