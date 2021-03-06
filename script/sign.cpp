// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "script/sign.h"

#include "key.h"
#include "keystore.h"
#include "transaction.h"
#include "script/standard.h"
#include "uint256.h"

#include <boost/foreach.hpp>
#include "base58.h"
#include "utilstrencodings.h"
#include "ca/ca.h"
#include "ca/camempool.h"

using namespace std;

typedef std::vector<unsigned char> valtype;

TransactionSignatureCreator::TransactionSignatureCreator(const CKeyStore* keystoreIn, const CTransaction* txToIn, unsigned int nInIn, EnumTx nInTypeIn, const CAmount& amountIn, int nHashTypeIn, const std::string& pwd) : BaseSignatureCreator(keystoreIn), txTo(txToIn), nIn(nInIn), nHashType(nHashTypeIn), amount(amountIn), checker(txTo, nIn, nInTypeIn, amountIn), strPwd(pwd) {}

bool TransactionSignatureCreator::CreateSig(std::vector<unsigned char>& vchSig, const CKeyID& address, const CScript& scriptCode, SigVersion sigversion) const
{
    if (scriptCode.IsPayToRealNamePubkeyHash() || scriptCode.IsRealNameAppendHash() || scriptCode.IsRealNameContract())
    {
        uint256 sighash = SignatureHash(scriptCode, *txTo, nIn, nHashType, amount, sigversion, checker.GetInType());
        LogPrintf("create signature hash: %s\n", sighash.ToString().c_str());
        if (scriptCode.IsContractOutput())
        {
            std::string addr = scriptCode.GetContractAddress();
            CContractAddress oldAddr(addr);
            if ((CChainParams::Base58Type)oldAddr.GetBase58prefix() == CChainParams::Base58Type::WNS_CONTRACT_ADDRESS ||
                (CChainParams::Base58Type)oldAddr.GetBase58prefix() == CChainParams::Base58Type::REALNAME_WNS_CONTRACT_ADDRESS)
            {
                std::vector<unsigned char> vecSignAfter(sighash.begin(), sighash.end());
                vchSig = vecSignAfter;
                return true;
            }
        }
        stCertFile certFile = GetLocalCaManager()->GetCa(address);
        if (certFile.Empty())
        {
            LogPrintf("can not find private key for realname utxo: %s\n", txTo->vin[nIn].prevout.hash.ToString().c_str());
            return false;
        }
        std::string tmpPwd;
        if (scriptCode.IsContractOutput())
            tmpPwd = certFile.strPwd;
        else
            tmpPwd = strPwd;

        CPrivateKey key(certFile.strKey.c_str(), tmpPwd.c_str(), FORMAT_PEM);
        if (!key.IsValid())
        {
            LogPrintf("Real name certificate key %s invalid\n", certFile.strKey.c_str());
            return false;
        }
        unsigned char CAsignafter[1024] = {0};
        unsigned int signlen = 0;
        if (!CASign((char*)key.GetKeyContent().data(), key.GetKeyContent().size(), FORMAT_PEM, tmpPwd.empty() ? "" : tmpPwd.c_str(),
            sighash.begin(),  sighash.size(), CAsignafter, &signlen))
        {
            LogPrintf("sign real name error\n");
            return false;
        }
        std::vector<unsigned char> vecSignAfter(CAsignafter, CAsignafter + signlen);
        vchSig = vecSignAfter;

        return true;

    }

    CKey key;
    if (!keystore->GetKey(address, key))
        return false;

    // Signing with uncompressed keys is disabled in witness scripts
    if (sigversion == SIGVERSION_WITNESS_V0 && !key.IsCompressed())
        return false;

    uint256 hash = SignatureHash(scriptCode, *txTo, nIn, nHashType, amount, sigversion, checker.GetInType());
    if (!key.Sign(hash, vchSig))
        return false;
    vchSig.push_back((unsigned char)nHashType);
    return true;
}

static bool Sign1(const CKeyID& address, const BaseSignatureCreator& creator, const CScript& scriptCode, std::vector<valtype>& ret, SigVersion sigversion)
{
    vector<unsigned char> vchSig;
    if (!creator.CreateSig(vchSig, address, scriptCode, sigversion))
        return false;
    ret.push_back(vchSig);
    return true;
}

static bool SignN(const vector<valtype>& multisigdata, const BaseSignatureCreator& creator, const CScript& scriptCode, std::vector<valtype>& ret, SigVersion sigversion)
{
    int nSigned = 0;
    int nRequired = multisigdata.front()[0];
    for (unsigned int i = 1; i < multisigdata.size()-1 && nSigned < nRequired; i++)
    {
        const valtype& pubkey = multisigdata[i];
        CKeyID keyID = CPubKey(pubkey).GetID();
        if (Sign1(keyID, creator, scriptCode, ret, sigversion))
            ++nSigned;
    }
    return nSigned==nRequired;
}

/**
 * Sign scriptPubKey using signature made with creator.
 * Signatures are returned in scriptSigRet (or returns false if scriptPubKey can't be signed),
 * unless whichTypeRet is TX_SCRIPTHASH, in which case scriptSigRet is the redemption script.
 * Returns false if scriptPubKey could not be completely satisfied.
 */
static bool SignStep(const BaseSignatureCreator& creator, const CScript& scriptPubKey,
                     std::vector<valtype>& ret, txnouttype& whichTypeRet, SigVersion sigversion)
{
    CScript scriptRet;
    uint160 h160;
    ret.clear();

    vector<valtype> vSolutions;
    if (!Solver(scriptPubKey, whichTypeRet, vSolutions))
        return false;

    CKeyID keyID;
    switch (whichTypeRet)
    {
    case TX_NONSTANDARD:
    case TX_NULL_DATA:
        return false;
    case TX_PUBKEY:
        keyID = CPubKey(vSolutions[0]).GetID();
        return Sign1(keyID, creator, scriptPubKey, ret, sigversion);
    case TX_PUBKEYHASH:
        keyID = CKeyID(uint160(vSolutions[0]));
        if (!Sign1(keyID, creator, scriptPubKey, ret, sigversion))
            return false;
        else
        {
            CPubKey vch;
            creator.KeyStore().GetPubKey(keyID, vch);
            ret.push_back(ToByteVector(vch));
        }
        return true;
    case TX_SCRIPTHASH:
        if (creator.KeyStore().GetCScript(uint160(vSolutions[0]), scriptRet)) {
            ret.push_back(std::vector<unsigned char>(scriptRet.begin(), scriptRet.end()));
            return true;
        }
        return false;

    case TX_MULTISIG:
        {
        ret.push_back(valtype()); // workaround CHECKMULTISIG bug
        bool fRet = SignN(vSolutions, creator, scriptPubKey, ret, sigversion);
        // delete reduplicate signdata, because of we change GetKey api
        if(fRet)
        {
            sort(ret.begin(), ret.end());
            ret.erase(unique(ret.begin(), ret.end()), ret.end());
        }
        return fRet;
        }

    case TX_WITNESS_V0_KEYHASH:
        ret.push_back(vSolutions[0]);
        return true;

    case TX_WITNESS_V0_SCRIPTHASH:
        CRIPEMD160().Write(&vSolutions[0][0], vSolutions[0].size()).Finalize(h160.begin());
        if (creator.KeyStore().GetCScript(h160, scriptRet)) {
            ret.push_back(std::vector<unsigned char>(scriptRet.begin(), scriptRet.end()));
            return true;
        }
        return false;

    case TX_CONTRACT_ADDRESS:
    {
        CContractAddress address(vSolutions[0]);
        if (!address.IsValid()) {
            return false;
        }
        address.GetKeyID(keyID);
        // std::cout << "couttest TX_CONTRACT_ADDRESS Sign1 keyID:" << HexStr(keyID.begin(), keyID.end()) << std::endl;
        if (!Sign1(keyID, creator, scriptPubKey, ret, sigversion))
            return false;
        else
        {
            if (!address.IsRealNameContract())
            {
                CPubKey vch;
                creator.KeyStore().GetPubKey(keyID, vch);
                ret.push_back(ToByteVector(vch));
            }
            else
                ret.push_back(ToByteVector(keyID));
            // std::cout << "couttest TX_CONTRACT_ADDRESS CPubKey:" << HexStr(ToByteVector(vch)) << std::endl;
        }
        return true;
    }
    break;

    case TX_CONTRACT_OUTPUT:
    {
        if (vSolutions.size() == 2) {
            CContractAddress address = CContractAddress(vSolutions[1]);
            if (!address.IsValid())
                return false;
            address.GetKeyID(keyID);
            // std::cout << "couttest TX_CONTRACT_OUTPUT Sign1 keyID:" << HexStr(keyID.begin(), keyID.end()) << std::endl;
            if (!Sign1(keyID, creator, scriptPubKey, ret, sigversion))
                return false;
            else
            {
                if (!address.IsRealNameContract())
                {
                    CPubKey vch;
                    creator.KeyStore().GetPubKey(keyID, vch);
                    ret.push_back(ToByteVector(vch));
                }
                else
                    ret.push_back(ToByteVector(keyID));
                // std::cout << "couttest TX_CONTRACT_OUTPUT CPubKey:" << HexStr(ToByteVector(vch)) << std::endl;
            }
        }
        return true;
    }
    break;

    case TX_REALNAME:
    {
        if (!vSolutions.size())
        {
            return false;
        }
        keyID = CKeyID(uint160(vSolutions[0]));
        return Sign1(keyID, creator, scriptPubKey, ret, sigversion);
    }
    break;

    default:
        return false;
    }
}

static CScript PushAll(const vector<valtype>& values)
{
    CScript result;
    BOOST_FOREACH(const valtype& v, values) {
        if (v.size() == 0) {
            result << OP_0;
        } else if (v.size() == 1 && v[0] >= 1 && v[0] <= 16) {
            result << CScript::EncodeOP_N(v[0]);
        } else {
            result << v;
        }
    }
    return result;
}

bool ProduceSignature(const BaseSignatureCreator& creator, const CScript& fromPubKey, SignatureData& sigdata, bool isDummy)
{
    CScript script = fromPubKey;
    bool solved = true;
    std::vector<valtype> result;
    txnouttype whichType;
    solved = SignStep(creator, script, result, whichType, SIGVERSION_BASE);
    bool P2SH = false;
    CScript subscript;
    sigdata.scriptWitness.stack.clear();

    if (solved && whichType == TX_SCRIPTHASH)
    {
        // Solver returns the subscript that needs to be evaluated;
        // the final scriptSig is the signatures from that
        // and then the serialized subscript:
        script = subscript = CScript(result[0].begin(), result[0].end());
        solved = solved && SignStep(creator, script, result, whichType, SIGVERSION_BASE) && whichType != TX_SCRIPTHASH;
        P2SH = true;
    }

    if (solved && whichType == TX_WITNESS_V0_KEYHASH)
    {
        CScript witnessscript;
        witnessscript << OP_DUP << OP_HASH160 << ToByteVector(result[0]) << OP_EQUALVERIFY << OP_CHECKSIG;
        txnouttype subType;
        solved = solved && SignStep(creator, witnessscript, result, subType, SIGVERSION_WITNESS_V0);
        sigdata.scriptWitness.stack = result;
        result.clear();
    }
    else if (solved && whichType == TX_WITNESS_V0_SCRIPTHASH)
    {
        CScript witnessscript(result[0].begin(), result[0].end());
        txnouttype subType;
        solved = solved && SignStep(creator, witnessscript, result, subType, SIGVERSION_WITNESS_V0) && subType != TX_SCRIPTHASH && subType != TX_WITNESS_V0_SCRIPTHASH && subType != TX_WITNESS_V0_KEYHASH;
        result.push_back(std::vector<unsigned char>(witnessscript.begin(), witnessscript.end()));
        sigdata.scriptWitness.stack = result;
        result.clear();
    }

    if (P2SH) {
        result.push_back(std::vector<unsigned char>(subscript.begin(), subscript.end()));
    }
    sigdata.scriptSig = PushAll(result);

    return solved;
}

SignatureData DataFromTransaction(const CMutableTransaction& tx, unsigned int nIn)
{
    SignatureData data;
    assert(tx.vin.size() > nIn);
    data.scriptSig = tx.vin[nIn].scriptSig;
    data.scriptWitness = tx.vin[nIn].scriptWitness;

    return data;
}

SignatureData GasDataFromTransaction(const CMutableTransaction& tx, unsigned int nIn)
{
    SignatureData data;
    assert(tx.gasToken.vin.size() > nIn);
    data.scriptSig = tx.gasToken.vin[nIn].scriptSig;
    data.scriptWitness = tx.gasToken.vin[nIn].scriptWitness;
    return data;
}

void UpdateTransaction(CMutableTransaction& tx, unsigned int nIn, const SignatureData& data)
{
    assert(tx.vin.size() > nIn);
    tx.vin[nIn].scriptSig = data.scriptSig;
    tx.vin[nIn].scriptWitness = data.scriptWitness;
}

void UpdateGasTransaction(CMutableTransaction & tx, unsigned int nIn, const SignatureData& data)
{
    if (tx.GetBusinessType() == BUSINESSTYPE_TOKEN) {
        assert(tx.gasToken.vin.size() > nIn);
        tx.gasToken.vin[nIn].scriptSig = data.scriptSig;
        tx.gasToken.vin[nIn].scriptWitness = data.scriptWitness;
    }
}

bool SignSignature(const CKeyStore &keystore, const CScript& fromPubKey, CMutableTransaction& txTo, unsigned int nIn, const CAmount& amount, int nHashType)
{
    assert(nIn < txTo.vin.size());

    CTransaction txToConst(txTo);
    TransactionSignatureCreator creator(&keystore, &txToConst, nIn, EnumTx::TX_GAS, amount, nHashType);

    SignatureData sigdata;
    bool ret = ProduceSignature(creator, fromPubKey, sigdata);
    UpdateTransaction(txTo, nIn, sigdata);
    return ret;
}

bool SignSignature(const CKeyStore &keystore, const CTransaction& txFrom, CMutableTransaction& txTo, unsigned int nIn, int nHashType)
{
    assert(nIn < txTo.vin.size());
    CTxIn& txin = txTo.vin[nIn];
    assert(txin.prevout.n < txFrom.vout.size());
    const CTxOut& txout = txFrom.vout[txin.prevout.n];

    return SignSignature(keystore, txout.scriptPubKey, txTo, nIn, txout.nValue, nHashType);
}

static vector<valtype> CombineMultisig(const CScript& scriptPubKey, const BaseSignatureChecker& checker,
                               const vector<valtype>& vSolutions,
                               const vector<valtype>& sigs1, const vector<valtype>& sigs2, SigVersion sigversion)
{
    // Combine all the signatures we've got:
    set<valtype> allsigs;
    BOOST_FOREACH(const valtype& v, sigs1)
    {
        if (!v.empty())
            allsigs.insert(v);
    }
    BOOST_FOREACH(const valtype& v, sigs2)
    {
        if (!v.empty())
            allsigs.insert(v);
    }

    // Build a map of pubkey -> signature by matching sigs to pubkeys:
    assert(vSolutions.size() > 1);
    unsigned int nSigsRequired = vSolutions.front()[0];
    unsigned int nPubKeys = vSolutions.size()-2;

    map<valtype, valtype> sigs;
    BOOST_FOREACH(const valtype& sig, allsigs)
    {
        for (unsigned int i = 0; i < nPubKeys; i++)
        {
            const valtype& pubkey = vSolutions[i+1];
            if (sigs.count(pubkey))
                continue; // Already got a sig for this pubkey
            if (checker.CheckSig(sig, pubkey, scriptPubKey, sigversion))
            {
                sigs[pubkey] = sig;
                break;
            }
        }
    }
    // Now build a merged CScript:
    unsigned int nSigsHave = 0;
    std::vector<valtype> result; result.push_back(valtype()); // pop-one-too-many workaround
    for (unsigned int i = 0; i < nPubKeys && nSigsHave < nSigsRequired; i++)
    {
        if (sigs.count(vSolutions[i+1]))
        {
            result.push_back(sigs[vSolutions[i+1]]);
            ++nSigsHave;
        }
    }
    // Fill any missing with OP_0:
    for (unsigned int i = nSigsHave; i < nSigsRequired; i++)
        result.push_back(valtype());

    return result;
}

namespace
{
struct Stacks
{
    std::vector<valtype> script;
    std::vector<valtype> witness;

    Stacks() {}
    explicit Stacks(const std::vector<valtype>& scriptSigStack_) : script(scriptSigStack_), witness() {}
    explicit Stacks(const SignatureData& data) : witness(data.scriptWitness.stack) {
        EvalScript(script, data.scriptSig, SCRIPT_VERIFY_STRICTENC, BaseSignatureChecker(), SIGVERSION_BASE);
    }

    SignatureData Output() const {
        SignatureData result;
        result.scriptSig = PushAll(script);
        result.scriptWitness.stack = witness;
        return result;
    }
};
}

static Stacks CombineSignatures(const CScript& scriptPubKey, const BaseSignatureChecker& checker,
                                 const txnouttype txType, const vector<valtype>& vSolutions,
                                 Stacks sigs1, Stacks sigs2, SigVersion sigversion)
{
    switch (txType)
    {
    case TX_NONSTANDARD:
    case TX_NULL_DATA:
        // Don't know anything about this, assume bigger one is correct:
        if (sigs1.script.size() >= sigs2.script.size())
            return sigs1;
        return sigs2;
    case TX_PUBKEY:
    case TX_PUBKEYHASH:
    case TX_CONTRACT_ADDRESS:
    case TX_CONTRACT_OUTPUT:
    case TX_REALNAME:
        // Signatures are bigger than placeholders or empty scripts:
        if (sigs1.script.empty() || sigs1.script[0].empty())
            return sigs2;
        return sigs1;
    case TX_WITNESS_V0_KEYHASH:
        // Signatures are bigger than placeholders or empty scripts:
        if (sigs1.witness.empty() || sigs1.witness[0].empty())
            return sigs2;
        return sigs1;
    case TX_SCRIPTHASH:
        if (sigs1.script.empty() || sigs1.script.back().empty())
            return sigs2;
        else if (sigs2.script.empty() || sigs2.script.back().empty())
            return sigs1;
        else
        {
            // Recur to combine:
            valtype spk = sigs1.script.back();
            CScript pubKey2(spk.begin(), spk.end());

            txnouttype txType2;
            vector<vector<unsigned char> > vSolutions2;
            Solver(pubKey2, txType2, vSolutions2);
            sigs1.script.pop_back();
            sigs2.script.pop_back();
            Stacks result = CombineSignatures(pubKey2, checker, txType2, vSolutions2, sigs1, sigs2, sigversion);
            result.script.push_back(spk);
            return result;
        }
    case TX_MULTISIG:
        return Stacks(CombineMultisig(scriptPubKey, checker, vSolutions, sigs1.script, sigs2.script, sigversion));
    case TX_WITNESS_V0_SCRIPTHASH:
        if (sigs1.witness.empty() || sigs1.witness.back().empty())
            return sigs2;
        else if (sigs2.witness.empty() || sigs2.witness.back().empty())
            return sigs1;
        else
        {
            // Recur to combine:
            CScript pubKey2(sigs1.witness.back().begin(), sigs1.witness.back().end());
            txnouttype txType2;
            vector<valtype> vSolutions2;
            Solver(pubKey2, txType2, vSolutions2);
            sigs1.witness.pop_back();
            sigs1.script = sigs1.witness;
            sigs1.witness.clear();
            sigs2.witness.pop_back();
            sigs2.script = sigs2.witness;
            sigs2.witness.clear();
            Stacks result = CombineSignatures(pubKey2, checker, txType2, vSolutions2, sigs1, sigs2, SIGVERSION_WITNESS_V0);
            result.witness = result.script;
            result.script.clear();
            result.witness.push_back(valtype(pubKey2.begin(), pubKey2.end()));
            return result;
        }
    default:
        return Stacks();
    }
}

SignatureData CombineSignatures(const CScript& scriptPubKey, const BaseSignatureChecker& checker,
                          const SignatureData& scriptSig1, const SignatureData& scriptSig2)
{
    txnouttype txType;
    vector<vector<unsigned char> > vSolutions;
    Solver(scriptPubKey, txType, vSolutions);

    return CombineSignatures(scriptPubKey, checker, txType, vSolutions, Stacks(scriptSig1), Stacks(scriptSig2), SIGVERSION_BASE)
            .Output();
}

bool DummySignatureCreator::CreateSig(std::vector<unsigned char>& vchSig, const CKeyID& keyid, const CScript& scriptCode, SigVersion sigversion) const
{
    if (scriptCode.IsPayToRealNamePubkeyHash())
    {
        LogPrintf("dummy real name sig\n");
        vchSig.assign(512, '\000');
        for (int i = 0; i < 6; i++)
        {
            vchSig[0 + i*72] = 0x30;
            vchSig[1 + i*72] = 69;
            vchSig[2 + i*72] = 0x02;
            vchSig[3 + i*72] = 33;
            vchSig[4 + i*72] = 0x01;
            vchSig[48 + i*72] = 0x02;
            vchSig[49 + i*72] = 32;
            vchSig[50 + i*72] = 0x01;
        }
        vchSig[504] = 7;
        vchSig[504] = 0x01;
        vchSig[511] = SIGHASH_ALL;
        return true;
    }
    // Create a dummy signature that is a valid DER-encoding
    vchSig.assign(72, '\000');
    vchSig[0] = 0x30;
    vchSig[1] = 69;
    vchSig[2] = 0x02;
    vchSig[3] = 33;
    vchSig[4] = 0x01;
    vchSig[4 + 33] = 0x02;
    vchSig[5 + 33] = 32;
    vchSig[6 + 33] = 0x01;
    vchSig[6 + 33 + 32] = SIGHASH_ALL;
    return true;
}
