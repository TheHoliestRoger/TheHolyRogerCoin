// Copyright (c) 2018-2019 The UNIGRID organization
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <supplycache.h>
#include <primitives/block.h>
#include <chainparams.h>
#include <validation.h>
#include <clientversion.h>
#include <hash.h>
#include <spork.h>
#include <sporknames.h>
#include <streams.h>
#include <util.h>
#include <uint256.h>

CCriticalSection cs_supplycache;

void SupplyCache::Initialize()
{
	LOCK(cs_supplycache);
    pathDB = GetDataDir() / "supplycache.dat";
    strMagicMessage = "SupplyCache";
    RefreshReferenceBlackList();
}

bool SupplyCache::IsInitialized()
{
    return !pathDB.empty();
}

bool SupplyCache::HasReferenceList()
{
    return !blacklistedAddresses.empty();
}

void SupplyCache::RefreshReferenceBlackList()
{
    blacklistedAddresses.clear();
    sporkValue = GetSporkValue(SPORK_1_BLACKLIST_BLOCK_REFERENCE);
    GetBannedPubkeys(blacklistedAddresses, Params().GetConsensus());
}

void SupplyCache::AddBlackListed(CAmount amount)
{
	LOCK(cs_supplycache);
	blackListedSum += amount;
}

CAmount SupplyCache::GetBlackListedSum()
{
    return blackListedSum;
}

void SupplyCache::SumNonCirculatingAmounts(const CBlock& block, CAmount& blackListedAmount)
{
    if (IsSporkActive(SPORK_1_BLACKLIST_BLOCK_REFERENCE))
    {
        for (unsigned int i = 0; i < block.vtx.size(); i++) {
            /* receving addresses */
            for (unsigned int j = 0; j < block.vtx[i]->vout.size(); j++) {
                for (auto it = blacklistedAddresses.begin(); it != blacklistedAddresses.end(); ++it) {
                    if (block.vtx[i]->vout[j].scriptPubKey == *it) {
                        blackListedAmount += block.vtx[i]->vout[j].nValue;
                    }
                }
            }
    
            /* spending addresses */
            for (unsigned int j = 0; j < block.vtx[i]->vin.size(); j++) {
                CTransactionRef prevoutTx;
                uint256 prevoutHashBlock;
    
                if (GetTransaction(block.vtx[i]->vin[j].prevout.hash, prevoutTx, Params().GetConsensus(), prevoutHashBlock)) {
                    for (auto it = blacklistedAddresses.begin(); it != blacklistedAddresses.end(); ++it) {
                        if (prevoutTx->vout[block.vtx[i]->vin[j].prevout.n].scriptPubKey == *it) {
                            blackListedAmount -= prevoutTx->vout[block.vtx[i]->vin[j].prevout.n].nValue;
                        }
                    }
                }
            }
        }
    }
}

void SupplyCache::SumNonCirculatingAmounts()
{
    {
        LOCK(cs_supplycache);
        blackListedSum = 0;
    }

    RefreshReferenceBlackList();
    CBlock block;

    for (int i = 1; i < chainActive.Height(); i++) {
        ReadBlockFromDisk(block, chainActive[i], Params().GetConsensus());
        {
            LOCK(cs_supplycache);
            SumNonCirculatingAmounts(block, blackListedSum);
        }
    }
}

bool SupplyCache::IsDirty()
{
	uint64_t v = 0;

    if (IsSporkActive(SPORK_1_BLACKLIST_BLOCK_REFERENCE)) {
        v = GetSporkValue(SPORK_1_BLACKLIST_BLOCK_REFERENCE);
    }

	return v != sporkValue;
}

bool SupplyCache::Write()
{
	LOCK(cs_supplycache);
    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssObj(SER_DISK, CLIENT_VERSION);
    ssObj << strMagicMessage;
    ssObj << blackListedSum;
    ssObj << sporkValue;

    uint256 hash = Hash(ssObj.begin(), ssObj.end());
    ssObj << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fopen(pathDB.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);

    if (fileout.IsNull()) {
        return error("%s : failed to open file %s", __func__, pathDB.string());
    }

    try {
        fileout << ssObj;
    } catch (std::exception& e) {
        return error("%s : serialize or I/O error - %s", __func__, e.what());
    }

    fileout.fclose();
    // LogPrint("supplycache", "written info to supplycache.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrintf("written info to supplycache.dat  %dms\n", GetTimeMillis() - nStart);
    return true;
}

SupplyCache::ReadResult SupplyCache::Read(CAmount& blackListedSum, uint64_t& sporkValue)
{
    int64_t nStart = GetTimeMillis();

    // open input file, and associate with CAutoFile
    FILE* file = fopen(pathDB.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);

    if (filein.IsNull()) {
        error("%s : failed to open file %s", __func__, pathDB.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = boost::filesystem::file_size(pathDB);
    int dataSize = fileSize - sizeof(uint256);

    // Don't try to resize to a negative number if file is small
    if (dataSize < 0) {
        dataSize = 0;
    }

    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    try {
        filein.read((char*)&vchData[0], dataSize);
        filein >> hashIn;
    } catch (std::exception& e) {
        error("%s : deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }

    filein.fclose();
    CDataStream ssObj(vchData, SER_DISK, CLIENT_VERSION);

    uint256 hashTmp = Hash(ssObj.begin(), ssObj.end());
    std::string strMagicMessageTmp;

    // verify stored checksum matches input data
    if (hashIn != hashTmp) {
        error("%s : checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }

    try {
        ssObj >> strMagicMessageTmp;

        // verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp) {
            error("%s : invalid supply cache magic message", __func__);
            return IncorrectMagicMessage;
        }

        ssObj >> blackListedSum;
        ssObj >> sporkValue;
    } catch (std::exception& e) {
        error("%s : deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    // LogPrint("supplycache","Loaded info from supplycache.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrintf("Loaded info from supplycache.dat  %dms\n", GetTimeMillis() - nStart);
    // LogPrint("supplycache (blacklisted): ","  %ld\n", blackListedSum);
    LogPrintf("supplycache (blacklisted): %ld\n", blackListedSum);

    return Ok;
}

SupplyCache::ReadResult SupplyCache::Read()
{
	LOCK(cs_supplycache);
	return Read(blackListedSum, sporkValue);
}

