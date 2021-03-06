#include <fstream>
#include "construct_tx.h"
#include "base58.h"
#include "keystore.h"
#include "script/script.h"
#include "script/standard.h"
#include "script/sign.h"
#include "utilstrencodings.h"
#include "net.h"
#include "netmessagemaker.h"
#include "core_io.h"

using namespace std;

enum TX_TYPE{COMMON_TX = 1, MULTISIG_TX, PUBLISH_TX, EXCHANGE_TX, CONTRACT_TX};

UniValue ParseJsonFile(const string& strFile)
{
    ifstream file(strFile);

    stringstream buff;
    buff << file.rdbuf();
    string strContent(buff.str());

    UniValue params;
    params.read(strContent.data());

    return params;
}

bool CreateTransaction(const string& strCommand, const string& strFile, int hSocket)
{
    UniValue params = ParseJsonFile(strFile);
    if(params.empty())
    {
        UniValue tempJson;
        if(!tempJson.read(strFile))
            return error("Invalid json file or json string");

        params = tempJson;
    }

    if(strCommand.empty())
        return error("Invalid command");

    string strRawTx = "";
    int nType = atoi(strCommand.data());
    if(nType == TX_TYPE::COMMON_TX)
        CreateCommonTx(strRawTx, params);
    else if(nType == TX_TYPE::MULTISIG_TX)
        CreateMultiSigTx(strRawTx, params);
    else if(nType == TX_TYPE::PUBLISH_TX)
        CreatePublishTx(strRawTx, params);
    else if(nType == TX_TYPE::EXCHANGE_TX)
        CreateExchangeTx(strRawTx, params);
    else if(nType == TX_TYPE::CONTRACT_TX)
        CreateContractTx(strRawTx, params);

    return !strRawTx.empty() && SendTransaction(strRawTx, hSocket, (nType == PUBLISH_TX));
}

// build transaction
bool BuildTx(CMutableTransaction& mtx, const UniValue& params)
{
    return BuildTxBasicPart(mtx, params) && BuildTxGasTokenPart(mtx, params);
}

// build basic part(vin and vout) of transaction
bool BuildTxBasicPart(CMutableTransaction& mtx, const UniValue& params)
{
    if(!params.exists("symbol"))
        return error("Missing symbol");

    if(!params.exists("vin") || !params["vin"].isArray() || params["vin"].empty())
        return error("Invalid vin, vin must be an array");

    if(!params.exists("vout") || params["vout"].empty())
        return error("Missing vout");

    const string& strSymbol = params["symbol"].get_str();
    const UniValue& vin = params["vin"].get_array();
    const UniValue& vout = params["vout"].get_obj();

    mtx.strPayCurrencySymbol = strSymbol;

    // fill vin
    for(unsigned int i = 0; i < vin.size(); i++)
    {
        const UniValue& txin = vin[i].get_obj();
        if(!txin.exists("txid"))
            return error("Missing txid in vin[%d]", i);
        if(!txin.exists("outtype"))
            return error("Missing outtype in vin[%d]", i);
        if(!txin.exists("vout"))
            return error("Missing out index in vin[%d]", i);

        uint256 txid = uint256S(txin["txid"].get_str());
        int nOuttype = txin["outtype"].get_int();
        int n = txin["vout"].get_int();
        int nSequence = std::numeric_limits<uint32_t>::max();

        mtx.vin.push_back(CTxIn(COutPoint(txid, (EnumTx)nOuttype, n), CScript(), nSequence));
    }

    // fill vout
    const vector<string>& vVoutKey = vout.getKeys();
    const vector<UniValue>& vVoutValue = vout.getValues();
    for(unsigned int i = 0; i < vVoutKey.size(); i++)
    {
        CAmount nAmount = AmountFromValue(vVoutValue[i], strSymbol);
        const string& strDestAddr = vVoutKey[i];
        if(strDestAddr == "contract" || strDestAddr == "contract_input") // txout of contract tx
        {
            mtx.vout.push_back(CTxOut(nAmount, CScript()));
        }
        else
        {
            CScript scriptPubKey = GetScriptForDestination(CBitcoinAddress(strDestAddr).Get());
            mtx.vout.push_back(CTxOut(nAmount, scriptPubKey));
        }
    }

    mtx.SetBusinessType(BUSINESSTYPE_TRANSACTION);
    return true;
}

// build gasToken part(gasvin and gasvout) of transaction
bool BuildTxGasTokenPart(CMutableTransaction& mtx, const UniValue& params)
{
    if(params.exists("gas_symbol"))
    {
        if(!params.exists("gas_vin") || !params["gas_vin"].isArray() || params["gas_vin"].empty())
            return error("Invalid gas vin, gas vin must be an array");

        if(!params.exists("gas_vout") || params["gas_vout"].empty())
            return error("Missing gas vout");

        const string& strGasSymbol = params["gas_symbol"].get_str();
        const UniValue& gasVin = params["gas_vin"].get_array();
        const UniValue& gasVout = params["gas_vout"].get_obj();

        mtx.gasToken.strPayCurrencySymbol = strGasSymbol;

        // fill gas vin
        for(unsigned int i = 0; i < gasVin.size(); i++)
        {
            const UniValue& txin = gasVin[i].get_obj();
            if(!txin.exists("txid"))
                return error("Missing txid in gas vin[%d]", i);
            if(!txin.exists("outtype"))
                return error("Missing outtype in gas vin[%d]", i);
            if(!txin.exists("vout"))
                return error("Missing out index in gas vin[%d]", i);

            uint256 txid = uint256S(txin["txid"].get_str());
            int nOuttype = txin["outtype"].get_int();
            int n = txin["vout"].get_int();
            int nSequence = std::numeric_limits<uint32_t>::max();

            mtx.gasToken.vin.push_back(CTxIn(COutPoint(txid, (EnumTx)nOuttype, n), CScript(), nSequence));
        }

        // fill gas vout
        const vector<string>& vGasVoutKey = gasVout.getKeys();
        const vector<UniValue>& vGasVoutValue = gasVout.getValues();
        for(unsigned int i = 0; i < vGasVoutKey.size(); i++)
        {
            CAmount nAmount = AmountFromValue(vGasVoutValue[i], strGasSymbol);
            const string& strDestAddr = vGasVoutKey[i];
            if(strDestAddr == "contract" || strDestAddr == "contract_input") // gas txout of contract tx
            {
                mtx.gasToken.vout.push_back(CTxOut(nAmount, CScript()));
            }
            else
            {
                CScript scriptPubKey = GetScriptForDestination(CBitcoinAddress(strDestAddr).Get());
                mtx.gasToken.vout.push_back(CTxOut(nAmount, scriptPubKey));
            }
        }

        // change business type
        mtx.SetBusinessType(BUSINESSTYPE_TOKEN);
    }

    return true;
}

bool SignTx(CMutableTransaction& mtx, const UniValue& params)
{
    return SignTxBasicPart(mtx, params) && SignTxGasTokenPart(mtx, params);
}

bool SignTxBasicPart(CMutableTransaction& mtx, const UniValue& params)
{
    // prepare for vin
    if(!params.exists("vin") || !params["vin"].isArray() || params["vin"].empty())
        return error("Invalid vin, vin must be an array");

    vector<CKey> vKey;
    vector<CScript> vScriptPubKey;
    const UniValue& vin = params["vin"].get_array();
    for(unsigned int i = 0; i < vin.size(); i++)
    {
        const UniValue& txin = vin[i].get_obj();
        if(!txin.exists("privkey"))
            return error("Missing privkey in vin[%d]", i);
        if(!txin.exists("scriptPubKey"))
            return error("Missing scriptPubKey in vin[%d]", i);

        CBitcoinSecret vchSecret;
        vchSecret.SetString(txin["privkey"].get_str());
        vKey.push_back(vchSecret.GetKey());

        vector<unsigned char> buf(ParseHex(txin["scriptPubKey"].get_str()));
        CScript scriptPubKey(buf.begin(), buf.end());
        vScriptPubKey.push_back(scriptPubKey);
    }

    // sign for vin
    const CTransaction txConst(mtx);
    for(unsigned int i = 0; i < txConst.vin.size(); i++)
    {
        SignatureData sigdata;
        CBasicKeyStore keystore;
        keystore.AddKey(vKey[i]);

        ProduceSignature(TransactionSignatureCreator(&keystore, &txConst, i, EnumTx::TX_TOKEN, 0), vScriptPubKey[i], sigdata);
        UpdateTransaction(mtx, i, sigdata);
    }

    return true;
}

bool SignTxGasTokenPart(CMutableTransaction& mtx, const UniValue& params)
{
    // prepare for gas vin
    vector<CKey> vGasKey;
    vector<CScript> vGasScriptPubKey;
    if(mtx.gasToken.vin.size() > 0)
    {
        if(!params.exists("gas_vin") || !params["gas_vin"].isArray() || params["gas_vin"].empty())
            return error("Invalid gas vin, gas vin must be an array");

        const UniValue& gasVin = params["gas_vin"].get_array();
        for(unsigned int i = 0; i < gasVin.size(); i++)
        {
            const UniValue& txin = gasVin[i].get_obj();
            if(!txin.exists("privkey"))
                return error("Missing privkey in vin[%d]", i);
            if(!txin.exists("scriptPubKey"))
                return error("Missing scriptPubKey in vin[%d]", i);

            CBitcoinSecret vchSecret;
            vchSecret.SetString(txin["privkey"].get_str());
            vGasKey.push_back(vchSecret.GetKey());

            vector<unsigned char> buf(ParseHex(txin["scriptPubKey"].get_str()));
            CScript scriptPubKey(buf.begin(), buf.end());
            vGasScriptPubKey.push_back(scriptPubKey);
        }
    }

    // sign for gas vin
    const CTransaction txConst(mtx);
    for(unsigned int i = 0; i < txConst.gasToken.vin.size(); i++)
    {
        SignatureData sigdata;
        CBasicKeyStore keystore;
        keystore.AddKey(vGasKey[i]);

        ProduceSignature(TransactionSignatureCreator(&keystore, &txConst, i, EnumTx::TX_GAS, 0), vGasScriptPubKey[i], sigdata);
        UpdateGasTransaction(mtx, i, sigdata);
    }

    return true;
}

bool SendTransaction(const string& strRawTx, int& hSocket, bool fWitness)
{
    LogPrintf("raw tx: %s\n", strRawTx);
    vector<unsigned char> txData(ParseHex(strRawTx));
    CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);

    CMutableTransaction mtx;
    try{
        ssData >> mtx;
        if(!ssData.empty())
            return false;
    }catch(const std::exception&){
        return error("SendTransaction: Tx decode failed");
    }

    CTransactionRef tx(MakeTransactionRef(std::move(mtx)));
    int nFlags = fWitness ? 0 : SERIALIZE_TRANSACTION_NO_WITNESS;
    PushMessage(hSocket, CNetMsgMaker(PROTOCOL_VERSION).Make(nFlags, "tx", *tx));
    return true;
}
