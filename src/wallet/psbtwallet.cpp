// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <confidential_validation.h>
#include <wallet/psbtwallet.h>

TransactionError FillPSBTInputsData(const CWallet* pwallet, PartiallySignedTransaction& psbtx, bool bip32derivs)
{
    LOCK(pwallet->cs_wallet);
    CMutableTransaction& tx = *psbtx.tx;

    // Get all of the previous transactions
    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        const CTxIn& txin = tx.vin[i];
        PSBTInput& input = psbtx.inputs.at(i);

        if (PSBTInputSigned(input)) {
            continue;
        }

        // Verify input looks sane. This will check that we have at most one uxto, witness or non-witness.
        if (!input.IsSane()) {
            return TransactionError::INVALID_PSBT;
        }

        const uint256& txhash = txin.prevout.hash;
        const auto it = pwallet->mapWallet.find(txhash);
        if (it != pwallet->mapWallet.end()) {
            const CWalletTx& wtx = it->second;
            // If we have no utxo, use the one from the wallet.
            if (!input.non_witness_utxo && input.witness_utxo.IsNull()) {
                // We only need the non_witness_utxo, which is a superset of the witness_utxo.
                //   The signing code will switch to the smaller witness_utxo if this is ok.
                input.non_witness_utxo = wtx.tx;
            }

            // Grab the CA data
            CAmount val_tmp;
            wtx.GetNonIssuanceBlindingData(txin.prevout.n, nullptr, &val_tmp, &input.value_blinding_factor, &input.asset, &input.asset_blinding_factor);
            if (val_tmp != -1) {
                input.value = val_tmp;
            }
        }

        // Get key origin info for input, if bip32derivs is true. Does not actually sign anything.
        SignPSBTInput(HidingSigningProvider(pwallet, true /* don't sign */, !bip32derivs), psbtx, i, 1 /* SIGHASH_ALL, ignored */);
    }

    return TransactionError::OK;
}

TransactionError SignPSBT(const CWallet* pwallet, PartiallySignedTransaction& psbtx, bool& complete, int sighash_type, bool sign, bool imbalance_ok)
{
    complete = false;
    // Check that the transaction is not still in need of blinding
    for (const PSBTOutput& o : psbtx.outputs) {
        if (o.blinding_pubkey.IsValid()) {
            return TransactionError::BLINDING_REQUIRED;
        }
    }

    // Save the original transaction since we need to munge it temporarily, which would violate the PSBT rules
    CTransaction oldtx = CTransaction(*psbtx.tx);

    LOCK(pwallet->cs_wallet);
    CMutableTransaction& tx = *psbtx.tx;
    tx.witness.vtxoutwit.resize(tx.vout.size());

    // Stuff in auxiliary CA blinding data, if we have it
    for (unsigned int i = 0; i < tx.vout.size(); ++i) {
        PSBTOutput& output = psbtx.outputs.at(i);
        CTxOut& out = tx.vout[i];

        if (!output.value_commitment.IsNull()) {
            out.nValue = output.value_commitment;
        }
        if (!output.asset_commitment.IsNull()) {
            out.nAsset = output.asset_commitment;
        }
        if (!output.nonce_commitment.IsNull()) {
            out.nNonce = output.nonce_commitment;
        }

        // The signature can't depend on witness contents, so these are technically not necessary to sign.
        // HOWEVER, as long as we're checking that values balance before signing, they are required.
        CTxOutWitness& outwit = tx.witness.vtxoutwit[i];
        if (!output.range_proof.empty()) {
            outwit.vchRangeproof = output.range_proof;
        }
        if (!output.surjection_proof.empty()) {
            outwit.vchSurjectionproof = output.surjection_proof;
        }
    }

    // This is a convenience/usability check -- it's not invalid to sign an unbalanced transaction, but it's easy to shoot yourself in the foot.
    if (!imbalance_ok) {
        // Get UTXOs for all inputs, to check that amounts balance before signing.
        std::vector<CTxOut> inputs_utxos;
        for (size_t i = 0; i < psbtx.inputs.size(); ++i) {
            PSBTInput& inp = psbtx.inputs[i];
            if (inp.non_witness_utxo) {
                if (inp.non_witness_utxo->GetHash() != tx.vin[i].prevout.hash) {
                    return TransactionError::INVALID_PSBT;
                }
                if (!inp.witness_utxo.IsNull() && inp.non_witness_utxo->vout[tx.vin[i].prevout.n] != inp.witness_utxo) {
                    return TransactionError::INVALID_PSBT;
                }
                inputs_utxos.push_back(inp.non_witness_utxo->vout[tx.vin[i].prevout.n]);
            } else if (!inp.witness_utxo.IsNull()) {
                inputs_utxos.push_back(inp.witness_utxo);
            } else {
                return TransactionError::UTXOS_MISSING_BALANCE_CHECK;
            }
        }

        CTransaction tx_tmp(tx);
        if (!VerifyAmounts(inputs_utxos, tx_tmp, nullptr, false)) {
            return TransactionError::VALUE_IMBALANCE;
        }
    }

    complete = true;
    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        // Get the Sighash type
        if (sign && psbtx.inputs[i].sighash_type > 0 && psbtx.inputs[i].sighash_type != sighash_type) {
            complete = false;
            return TransactionError::SIGHASH_MISMATCH;
        }

        // Here we _only_ sign, and do not e.g. fill in key origin data.
        complete &= SignPSBTInput(HidingSigningProvider(pwallet, !sign, true /*  no key origins */), psbtx, i, sighash_type);
    }

    // Restore the saved transaction, to remove our temporary munging.
    psbtx.tx = (CMutableTransaction)oldtx;
    return TransactionError::OK;
}

void FillPSBTOutputsData(const CWallet* pwallet, PartiallySignedTransaction& psbtx, bool bip32derivs) {
    LOCK(pwallet->cs_wallet);
    const CMutableTransaction& tx = *psbtx.tx;

    // Fill in the bip32 keypaths and redeemscripts for the outputs so that hardware wallets can identify change
    for (unsigned int i = 0; i < psbtx.tx->vout.size(); ++i) {
        const CTxOut& out = tx.vout.at(i);
        PSBTOutput& psbt_out = psbtx.outputs.at(i);

        // Fill a SignatureData with output info
        SignatureData sigdata;
        psbt_out.FillSignatureData(sigdata);

        MutableTransactionSignatureCreator creator(&tx, 0 /* nIn, ignored */, out.nValue, 1 /* sighashtype, ignored */);
        ProduceSignature(HidingSigningProvider(pwallet, true /* don't sign */, !bip32derivs), creator, out.scriptPubKey, sigdata);
        psbt_out.FromSignatureData(sigdata);
    }
}

TransactionError FillPSBTData(const CWallet* pwallet, PartiallySignedTransaction& psbtx, bool bip32derivs) {
    LOCK(pwallet->cs_wallet);
    TransactionError te;
    te = FillPSBTInputsData(pwallet, psbtx, bip32derivs);
    if (te != TransactionError::OK) {
        return te;
    }
    FillPSBTOutputsData(pwallet, psbtx, bip32derivs);
    return TransactionError::OK;
}

// This function remains for backwards compatibility. It will not succeed in Elements unless everything involved is non-blinded.
TransactionError FillPSBT(const CWallet* pwallet, PartiallySignedTransaction& psbtx, bool& complete, int sighash_type, bool sign, bool bip32derivs)
{
    complete = false;
    TransactionError te;
    te = FillPSBTInputsData(pwallet, psbtx, bip32derivs);
    if (te != TransactionError::OK) {
        return te;
    }
    // For backwards compatibility, do not check if amounts balance before signing in this case.
    te = SignPSBT(pwallet, psbtx, complete, sighash_type, sign, true);
    if (te != TransactionError::OK) {
        return te;
    }
    FillPSBTOutputsData(pwallet, psbtx, bip32derivs);
    return TransactionError::OK;
}
