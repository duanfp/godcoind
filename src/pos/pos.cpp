// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The BlackCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>

#include "txdb.h"
#include "validation.h"
#include "arith_uint256.h"
#include "hash.h"
#include "timedata.h"
#include "chainparams.h"
#include "script/sign.h"
#include "consensus/consensus.h"
#include "pos/pos.h"

using namespace std;

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifier(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    //gocoin:pos nNonce
    int32_t nStakeModifier = 0;
    if(pindexPrev->nHeight > LAST_POW_BLOCK_HEIGHT){
        nStakeModifier = pindexPrev->nNonce;
    }

    if(nStakeModifier == 0 && pindexPrev->nHeight > LAST_POW_BLOCK_HEIGHT){
        LogPrintf("prev stake modifider is null, pindexPrev:height:%d, hash:%s,nStakeModifier:%d\n", 
                pindexPrev->nHeight,
                pindexPrev->GetBlockHash().GetHex(), 
                nStakeModifier);
        assert(false);
    }

    CDataStream ss(SER_GETHASH, 0);
    ss << kernel << nStakeModifier;
    return Hash(ss.begin(), ss.end());
}

// BlackCoin kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + blockFrom.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coins one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   blockFrom.nTime: slightly scrambles computation
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t blockFromTime, CAmount prevoutValue, const COutPoint& prevout, unsigned int nTimeBlock, uint256& hashProofOfStake, uint256& targetProofOfStake, bool fPrintProofOfStake)
{
    if (nTimeBlock < blockFromTime)  // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    // Base target
    arith_uint256 bnTarget;
    arith_uint256 bnTarget2;
    bnTarget2.SetCompact(nBits);
    bnTarget.SetCompact(nBits);

    // Weighted target
    int64_t nValueIn = prevoutValue;
    arith_uint256 bnWeight = arith_uint256(nValueIn);
    //bnTarget *= bnWeight;
    targetProofOfStake = ArithToUint256(bnTarget);
    //gocoin:pos nNonce
    int32_t nStakeModifier = 0;
    if(pindexPrev->nHeight > LAST_POW_BLOCK_HEIGHT){
        nStakeModifier = pindexPrev->nNonce;
    }

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    ss << nStakeModifier;
    ss << blockFromTime << prevout.hash << prevout.n << nTimeBlock;
    hashProofOfStake = Hash(ss.begin(), ss.end());

    bool checkSucceed = true;
    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) > bnTarget){
        checkSucceed = false;
    } 

    LogPrintf("%s:CheckStakeKernelHash() nBits=%d weight=%s modifier=%d nTimeBlockFrom=%u nPrevout=%s nTimeBlock=%u hashProof=%s prev=%s "
              "target:%d, weight:%d, finalTarget:%d, diff:%d，limit:%d\n",
            checkSucceed ? "succeed" : "failed",
            nBits,
            bnWeight.ToString(),
            nStakeModifier,
            blockFromTime, prevout.ToString(), nTimeBlock,
            hashProofOfStake.ToString(),
            pindexPrev->ToString(),
            bnTarget2.GetCompact(),
            bnWeight.GetCompact(),
            bnTarget.GetCompact(),
            UintToArith256(hashProofOfStake).GetCompact(),
            UintToArith256(uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff")).GetCompact());

    return checkSucceed;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, CValidationState& state, const CTransaction& tx, unsigned int nBits, uint32_t nTimeBlock, uint256& hashProofOfStake, uint256& targetProofOfStake, CCoinsViewCache& view)
{
    //
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString());

    //get prevout Tx
    // Kernel (input 0) must match the stake hash target (nBits)
    const CTxIn& txin = tx.vin[0];

    Coin coinPrev;

    //check prevout is existing
    if(!view.GetCoin(txin.prevout, coinPrev))
        return state.DoS(100, error("CheckProofOfStake() : Stake prevout does not exist %s", txin.prevout.hash.ToString()));

    if(pindexPrev->nHeight + 1 - coinPrev.nHeight < COINBASE_MATURITY)
        return state.DoS(100, error("CheckProofOfStake() : Stake prevout is not mature, expecting %i and only matured to %i", COINBASE_MATURITY, pindexPrev->nHeight + 1 - coinPrev.nHeight));
    
    //check prevout's block
    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if(!blockFrom) 
        return state.DoS(100, error("CheckProofOfStake() : Block at height %i for prevout can not be loaded", coinPrev.nHeight));

    // Verify signature
    //check prevout signature
    if (!VerifySignatureStake(coinPrev, txin.prevout.hash, tx, 0, SCRIPT_VERIFY_NONE))
        return state.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));

    //check kernel hash
    if (!CheckStakeKernelHash(pindexPrev, nBits, blockFrom->nTime, coinPrev.out.nValue, txin.prevout, nTimeBlock, hashProofOfStake, targetProofOfStake, true))
        return state.DoS(1, error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx.GetHash().ToString(), hashProofOfStake.ToString())); // may occur during initial download or if behind on block chain sync

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(uint32_t nTimeBlock)
{
    return (nTimeBlock & STAKE_TIMESTAMP_MASK) == 0;
}


bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTimeBlock, const COutPoint& prevout, CCoinsViewCache& view)
{
    std::map<COutPoint, CStakeCache> tmp;
    return CheckKernel(pindexPrev, nBits, nTimeBlock, prevout, view, tmp);
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTimeBlock, const COutPoint& prevout, CCoinsViewCache& view, const std::map<COutPoint, CStakeCache>& cache)
{
    uint256 hashProofOfStake, targetProofOfStake;
    auto it=cache.find(prevout);
    if(it == cache.end()) {
        //not found in cache (shouldn't happen during staking, only during verification which does not use cache)
        Coin coinPrev;
        if(!view.GetCoin(prevout, coinPrev))
            return false;
        
        if(pindexPrev->nHeight + 1 - coinPrev.nHeight < COINBASE_MATURITY)
            return false;
        
        CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
        if(!blockFrom) 
            return false;
        
        if(coinPrev.IsSpent())
            return false;
        
        return CheckStakeKernelHash(pindexPrev, nBits, blockFrom->nTime, coinPrev.out.nValue, prevout,
                                    nTimeBlock, hashProofOfStake, targetProofOfStake);
    } else{
        //found in cache
        const CStakeCache& stake = it->second;
        if(CheckStakeKernelHash(pindexPrev, nBits, stake.blockFromTime, stake.amount, prevout,
                                    nTimeBlock, hashProofOfStake, targetProofOfStake)){
            //Cache could potentially cause false positive stakes in the event of deep reorgs, so check without cache also
            return CheckKernel(pindexPrev, nBits, nTimeBlock, prevout, view);
        }
    }
    return false;
}

void CacheKernel(std::map<COutPoint, CStakeCache>& cache, const COutPoint& prevout, CBlockIndex* pindexPrev, CCoinsViewCache& view){
    if(cache.find(prevout) != cache.end())
        return;//already in cache
    
    Coin coinPrev;
    if(!view.GetCoin(prevout, coinPrev))
        return;
    
    if(pindexPrev->nHeight + 1 - coinPrev.nHeight < COINBASE_MATURITY)
        return;
    
    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if(!blockFrom) 
        return;
    
    CStakeCache c(blockFrom->nTime, coinPrev.out.nValue);
    cache.insert({prevout, c});
}

const CBlockIndex* GetLastBlockIndex(const CBlockIndex* pindex){
    //CBlockIndex will be updated with information about the proof type later
    while (pindex && pindex->pprev && (pindex->IsProofOfWork()))
        pindex = pindex->pprev;

    return pindex;
}

inline arith_uint256 GetLimit(const Consensus::Params& params){
    return  UintToArith256(params.posLimit);
}

unsigned int GetNextPosWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params){


    //first and second pos block use target limit
    if (pindexLast->nHeight < LAST_POW_BLOCK_HEIGHT + 2) 
        return GetLimit(params).GetCompact();

    if (params.fPoSNoRetargeting)
        return pindexLast->nBits;
    
    // Limit adjustment step
    int64_t nTargetSpacing = params.nPosTargetSpacing;
    int64_t nActualSpacing = pindexLast->GetBlockTime() - pindexLast->pprev->GetBlockTime();
    if (nActualSpacing < 0)
        nActualSpacing = nTargetSpacing;
    if (nActualSpacing > nTargetSpacing * 10)
        nActualSpacing = nTargetSpacing * 10;

    // ppcoin: target change every block
    // ppcoin: retarget with exponential moving toward target spacing
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    int64_t nInterval = params.nPosTargetTimespan / params.nPosTargetSpacing;;
    bnNew *= ((nInterval - 1) * nTargetSpacing + nActualSpacing + nActualSpacing);
    bnNew /= ((nInterval + 1) * nTargetSpacing);

    const arith_uint256 bnTargetLimit = GetLimit(params);
    if (bnNew <= 0 || bnNew > bnTargetLimit)
        bnNew = bnTargetLimit;

    return bnNew.GetCompact();
}
