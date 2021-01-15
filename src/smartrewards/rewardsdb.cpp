// Copyright (c) 2017 - 2020 - The SmartCash Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "smartrewards/rewardsdb.h"

#include "chainparams.h"
#include "hash.h"
#include "init.h"
#include "pow.h"
#include "rewards.h"
#include "rewardsdb.h"
#include "ui_interface.h"
#include "uint256.h"

#include <stdint.h>

#include "leveldb/include/leveldb/db.h"
#include <boost/functional/hash.hpp>
#include <boost/thread.hpp>

using namespace std;

static const char DB_ROUND_CURRENT = 'R';
static const char DB_ROUND = 'r';
static const char DB_ROUND_SNAPSHOT = 's';

static const char DB_REWARD_ENTRY = 'E';
static const char DB_TERMREWARD_ENTRY = 'T';
static const char DB_BLOCK = 'B';
static const char DB_BLOCK_LAST = 'b';
static const char DB_TX_HASH = 't';

static const char DB_VERSION = 'V';

size_t CSmartAddressHasher::operator()(const CSmartAddress& a) const {
    return a.GetHashSeed();
}

size_t CTermRewardDbKeyHasher::operator()(const CTermRewardDbKey& k) const {
    size_t seed = k.first.GetHashSeed();
    boost::hash_combine(seed, std::vector<uint8_t>(k.second.begin(), k.second.end()));
    return seed;
}

CSmartRewardsDB::CSmartRewardsDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "rewards", nCacheSize, fMemory, fWipe)
{
    if (!Exists(DB_VERSION)) {
        Write(DB_VERSION, REWARDS_DB_VERSION);
    }
}

bool CSmartRewardsDB::Verify(int& lastBlockHeight)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());
    CSmartRewardBlock last;
    uint8_t dbVersion;

    lastBlockHeight = 0;

    if (!Read(DB_VERSION, dbVersion)) {
        LogPrintf("CSmartRewards::Verify() Could't read DB_VERSION\n");
        return false;
    }

    if (dbVersion < REWARDS_DB_VERSION) {
        LogPrintf("CSmartRewards::Verify() DB_VERSION too old.\n");
        return false;
    }

    if (!ReadLastBlock(last)) {
        LogPrintf("CSmartRewards::Verify() No block here yet\n");
        return true;
    }

    lastBlockHeight = last.nHeight;

    return true;
}

bool CSmartRewardsDB::ReadBlock(const int nHeight, CSmartRewardBlock& block)
{
    return Read(make_pair(DB_BLOCK, nHeight), block);
}

bool CSmartRewardsDB::ReadLastBlock(CSmartRewardBlock& block)
{
    return Read(DB_BLOCK_LAST, block);
}

bool CSmartRewardsDB::ReadTransaction(const uint256 hash, CSmartRewardTransaction& transaction)
{
    return Read(make_pair(DB_TX_HASH, hash), transaction);
}

bool CSmartRewardsDB::ReadRound(const int16_t number, CSmartRewardRound& round)
{
    return Read(make_pair(DB_ROUND, number), round);
}

bool CSmartRewardsDB::ReadRounds(CSmartRewardRoundMap& rounds)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(DB_ROUND);

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint16_t> key;
        if (pcursor->GetKey(key) && key.first == DB_ROUND) {
            CSmartRewardRound nValue;
            if (pcursor->GetValue(nValue)) {
                rounds.insert(make_pair(nValue.number, nValue));
                pcursor->Next();
            } else {
                return error("failed to get reward round");
            }
        } else {
            break;
        }
    }

    return true;
}

bool CSmartRewardsDB::ReadCurrentRound(CSmartRewardRound& round)
{
    return Read(DB_ROUND_CURRENT, round);
}

bool CSmartRewardsDB::ReadRewardEntry(const CSmartAddress& id, CSmartRewardEntry& entry)
{
    return Read(make_pair(DB_REWARD_ENTRY, id), entry);
}

bool CSmartRewardsDB::ReadTermRewardEntry(const std::pair<CSmartAddress, uint256> &id, CTermRewardEntry &entry)
{
    return Read(make_pair(DB_TERMREWARD_ENTRY, id), entry);
}

bool CSmartRewardsDB::SyncCached(const CSmartRewardsCache& cache)
{
    CDBBatch batch(*this);

    if (cache.GetUndoResult() != nullptr && !cache.GetUndoResult()->fSynced) {
        CSmartRewardResultEntryPtrList tmpResults = cache.GetUndoResult()->results;

        auto entry = cache.GetEntries()->begin();

        while (entry != cache.GetEntries()->end()) {
            CSmartAddress searchAddress = entry->second->id;

            auto it = std::find_if(tmpResults.begin(),
                tmpResults.end(),
                [searchAddress](const CSmartRewardResultEntry* rEntry) -> bool {
                    return rEntry->entry.id == searchAddress;
                });

            if (it == tmpResults.end()) {
                batch.Erase(make_pair(DB_REWARD_ENTRY, entry->first));
            } else {
                CSmartRewardEntry rewardEntry = (*it)->entry;
                batch.Write(make_pair(DB_REWARD_ENTRY, rewardEntry.id), rewardEntry);
                batch.Erase(make_pair(DB_ROUND_SNAPSHOT, make_pair(cache.GetUndoResult()->round.number, rewardEntry.id)));
                tmpResults.erase(it);
            }

            ++entry;
        }

        auto it = tmpResults.begin();

        while (it != tmpResults.end()) {
            batch.Write(make_pair(DB_REWARD_ENTRY, (*it)->entry.id), (*it)->entry);
            batch.Erase(make_pair(DB_ROUND_SNAPSHOT, make_pair(cache.GetUndoResult()->round.number, (*it)->entry.id)));

            ++it;
        }

    } else {
        auto entry = cache.GetEntries()->begin();

        while (entry != cache.GetEntries()->end()) {
            if (entry->second->balance <= 0) {
                batch.Erase(make_pair(DB_REWARD_ENTRY, entry->first));
            } else {
                batch.Write(make_pair(DB_REWARD_ENTRY, entry->first), *entry->second);
            }

            ++entry;
        }
    }

    auto entry = cache.GetTermRewardsEntries()->begin();

    while (entry != cache.GetTermRewardsEntries()->end()) {
        batch.Write(make_pair(DB_TERMREWARD_ENTRY, entry->first), *entry->second);
        ++entry;
    }

    auto addTx = cache.GetAddedTransactions()->begin();

    while (addTx != cache.GetAddedTransactions()->end()) {
        batch.Write(make_pair(DB_TX_HASH, addTx->first), addTx->second);
        ++addTx;
    }

    auto removeTx = cache.GetRemovedTransactions()->begin();

    while (removeTx != cache.GetRemovedTransactions()->end()) {
        batch.Erase(make_pair(DB_TX_HASH, removeTx->first));
        ++removeTx;
    }

    auto round = cache.GetRounds()->begin();

    while (round != cache.GetRounds()->end()) {
        batch.Write(make_pair(DB_ROUND, round->first), round->second);
        ++round;
    }

    batch.Write(DB_BLOCK_LAST, *cache.GetCurrentBlock());

    if (cache.GetCurrentRound()->number) {
        batch.Write(DB_ROUND_CURRENT, *cache.GetCurrentRound());
    }

    if (cache.GetLastRoundResult() != nullptr && !cache.GetLastRoundResult()->fSynced) {
        BOOST_FOREACH (const CSmartRewardResultEntry* s, cache.GetLastRoundResult()->results) {
            batch.Write(make_pair(DB_ROUND_SNAPSHOT, make_pair(cache.GetLastRoundResult()->round.number, s->entry.id)), *s);
        }
    }

    return WriteBatch(batch, true);
}

bool CSmartRewardsDB::ReadRewardEntries(CSmartRewardEntryMap& entries)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(DB_REWARD_ENTRY);

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, CSmartAddress> key;
        if (pcursor->GetKey(key) && key.first == DB_REWARD_ENTRY) {
            CSmartRewardEntry entry;
            if (pcursor->GetValue(entry)) {
                entries.insert(std::make_pair(entry.id, new CSmartRewardEntry(entry)));
                pcursor->Next();
            } else {
                return error("failed to get reward entry");
            }
        } else {
            break;
        }
    }

    return true;
}

bool CSmartRewardsDB::ReadTermRewardEntries(CTermRewardEntryMap& entries)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(DB_TERMREWARD_ENTRY);

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, CSmartAddress> key;
        if (pcursor->GetKey(key) && key.first == DB_TERMREWARD_ENTRY) {
            CTermRewardEntry entry;
            if (pcursor->GetValue(entry)) {
                entries.insert(std::make_pair(std::make_pair(entry.address, entry.txHash), new CTermRewardEntry(entry)));
                pcursor->Next();
            } else {
                return error("failed to get term reward entry");
            }
        } else {
            break;
        }
    }

    return true;
}

bool CSmartRewardsDB::ReadRewardRoundResults(const int16_t round, CSmartRewardResultEntryList& results)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_ROUND_SNAPSHOT, round));

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, std::pair<int16_t, CSmartAddress> > key;
        if (pcursor->GetKey(key) && key.first == DB_ROUND_SNAPSHOT) {
            if (key.second.first != round)
                break;

            CSmartRewardResultEntry nValue;
            if (pcursor->GetValue(nValue)) {
                results.push_back(nValue);
                pcursor->Next();
            } else {
                return error("failed to get reward entry");
            }
        } else {
            break;
        }
    }

    return true;
}

bool CSmartRewardsDB::ReadRewardRoundResults(const int16_t round, CSmartRewardResultEntryPtrList& results)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_ROUND_SNAPSHOT, round));

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, std::pair<int16_t, CSmartAddress> > key;
        if (pcursor->GetKey(key) && key.first == DB_ROUND_SNAPSHOT) {
            if (key.second.first != round)
                break;

            CSmartRewardResultEntry nValue;
            if (pcursor->GetValue(nValue)) {
                results.push_back(new CSmartRewardResultEntry(nValue));
                pcursor->Next();
            } else {
                return error("failed to get reward entry");
            }
        } else {
            break;
        }
    }

    return true;
}

bool CSmartRewardsDB::ReadRewardPayouts(const int16_t round, CSmartRewardResultEntryList& payouts)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_ROUND_SNAPSHOT, round));

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, std::pair<int16_t, CSmartAddress> > key;
        if (pcursor->GetKey(key) && key.first == DB_ROUND_SNAPSHOT) {
            if (key.second.first != round)
                break;

            CSmartRewardResultEntry nValue;
            if (pcursor->GetValue(nValue)) {
                if (nValue.reward)
                    payouts.push_back(nValue);
                pcursor->Next();
            } else {
                return error("failed to get reward entry");
            }
        } else {
            break;
        }
    }

    return true;
}

bool CSmartRewardsDB::ReadRewardPayouts(const int16_t round, CSmartRewardResultEntryPtrList& payouts)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_ROUND_SNAPSHOT, round));

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, std::pair<int16_t, CSmartAddress> > key;
        if (pcursor->GetKey(key) && key.first == DB_ROUND_SNAPSHOT) {
            if (key.second.first != round)
                break;

            CSmartRewardResultEntry nValue;
            if (pcursor->GetValue(nValue)) {
                if (nValue.reward)
                    payouts.push_back(new CSmartRewardResultEntry(nValue));
                pcursor->Next();
            } else {
                // Delete everything if something fails
                for (auto it : payouts)
                    delete it;
                payouts.clear();
                return error("failed to get reward entry");
            }
        } else {
            break;
        }
    }

    return true;
}

string CSmartRewardEntry::GetAddress() const
{
    return id.ToString();
}

void CSmartRewardEntry::SetNull()
{
    id = CSmartAddress();
    balance = 0;
    balanceAtStart = 0;
    balanceEligible = 0;
    disqualifyingTx.SetNull();
    fDisqualifyingTx = false;
    smartnodePaymentTx.SetNull();
    fSmartnodePaymentTx = false;
    activationTx.SetNull();
    fActivated = false;
    bonusLevel = NoBonus;
}

string CSmartRewardEntry::ToString() const
{
    std::stringstream s;
    s << strprintf("CSmartRewardEntry(id=%s, balance=%d, balanceEligible=%d, isSmartNode=%b, activated=%b, bonus=%d)\n",
        GetAddress(),
        balance,
        balanceEligible,
        fSmartnodePaymentTx,
        fActivated,
        bonusLevel);
    return s.str();
}

bool CSmartRewardEntry::IsEligible()
{
    return fActivated && !fSmartnodePaymentTx && balanceEligible > 0 && !fDisqualifyingTx;
}

string CSmartRewardBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CSmartRewardBlock(height=%d, hash=%s, time=%d)\n",
        nHeight,
        nHash.ToString(),
        nTime);
    return s.str();
}

void CSmartRewardRound::UpdatePayoutParameter()
{
    nPayeeCount = ((eligibleEntries - disqualifiedEntries) > 0) ? (eligibleEntries - disqualifiedEntries) : 0;

    if (nPayeeCount > 0 && nBlockPayees > 0) {
        int64_t nPayoutDelay = Params().GetConsensus().nRewardsPayoutStartDelay;

        nRewardBlocks = nPayeeCount / nBlockPayees;
        if (nPayeeCount % nBlockPayees)
            nRewardBlocks += 1;

        nLastRoundBlock = endBlockHeight + nPayoutDelay + ((nRewardBlocks - 1) * nBlockInterval);
    }
}

string CSmartRewardRound::ToString() const
{
    std::stringstream s;
    s << strprintf("CSmartRewardRound(number=%d, start(block)=%d, start(time)=%d, end(block)=%d, end(time)=%d\n"
                   "  Eligible addresses=%d\n  Eligible SMART=%d\n Percent=%f)\n",
        number,
        startBlockHeight,
        startBlockTime,
        endBlockHeight,
        endBlockTime,
        eligibleEntries,
        eligibleSmart,
        percent);
    return s.str();
}

string CSmartRewardResultEntry::GetAddress() const
{
    return entry.id.ToString();
}

string CSmartRewardResultEntry::ToString() const
{
    std::stringstream s;
    s << strprintf("CSmartRewardResultEntry(id=%d, balance=%d, reward=%d\n",
        GetAddress(),
        entry.balance,
        reward);
    return s.str();
}

arith_uint256 CSmartRewardResultEntry::CalculateScore(const uint256& blockHash) const
{
    // Deterministically calculate a "score" for a CSmartRewardResultEntry based on any given (block)hash
    // Used to sort the payout list for 1.3 smartreward payouts
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << reward << entry.id << blockHash;
    return UintToArith256(ss.GetHash());
}

string CSmartRewardTransaction::ToString() const
{
    std::stringstream s;
    s << strprintf("CSmartRewardTransaction(hash=%s, blockHeight=%d\n",
        hash.ToString(),
        blockHeight);
    return s.str();
}

string CTermRewardEntry::GetAddress() const
{
    return address.ToString();
}

string CTermRewardEntry::GetLevel() const
{
    switch (level) {
      case OneYear:
        return "1 Year";
      case TwoYears:
        return "2 Years";
      case ThreeYears:
        return "3 Years";
      default:
        return "Unknown";
    }
}

string CTermRewardEntry::ToString() const
{
    std::stringstream s;
    s << strprintf("CTermRewardEntry(address=%s, tx_hash=%s, balance=%d, level=%s, percent=%d, expires=%d)\n",
        GetAddress(),
        txHash.GetHex(),
        balance,
        level,
        percent,
        expires);
    return s.str();
}

/*
arith_uint256 CTermRewardResultEntry::CalculateScore(const uint256& blockHash)
{
    // Deterministically calculate a "score" for a CSmartRewardResultEntry based on any given (block)hash
    // Used to sort the payout list for 1.3 smartreward payouts
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << reward << entry.id << blockHash;
    return UintToArith256(ss.GetHash());
}
*/
