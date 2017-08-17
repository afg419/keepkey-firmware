/*
 * This file is part of the TREZOR project.
 *
 * Copyright (C) 2014 Pavol Rusnak <stick@satoshilabs.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

/* === Includes ============================================================ */

#include <msg_dispatch.h>
#include <ecdsa.h>
#include <secp256k1.h>
#include <crypto.h>
#include <layout.h>
#include <confirm_sm.h>

#include "crypto.h"
#include "signing.h"
#include "fsm.h"
#include "transaction.h"
#include "coins.h"
#include "home_sm.h"
#include "app_confirm.h"
#include "policy.h"
#include "exchange.h"

/* === Private Variables =================================================== */

static uint32_t inputs_count;
static uint32_t outputs_count;
static const CoinType *coin;
static const HDNode *root;
static HDNode node;
static bool signing = false;
static uint32_t idx1, idx2;
static TxRequest resp;
static TxInputType input;
static TxOutputBinType bin_output;
static TxStruct to, transaction_previous, ti;
static SHA256_CTX transaction_current;
static uint8_t hash[32], hash_check[32], privkey[32], pubkey[33], sig[64];
static uint64_t to_spend, spending, change_spend;
static bool multisig_fp_set, multisig_fp_mismatch;
static uint8_t hash_prevouts[32], hash_sequence[32],hash_outputs[32];
static SHA256_CTX hashers[3];
static uint8_t multisig_fp[32];

/* === Variables =========================================================== */

enum {
	STAGE_REQUEST_1_INPUT,
	STAGE_REQUEST_2_PREV_META,
	STAGE_REQUEST_2_PREV_INPUT,
	STAGE_REQUEST_2_PREV_OUTPUT,
	STAGE_REQUEST_3_OUTPUT,
	STAGE_REQUEST_4_INPUT,
	STAGE_REQUEST_4_OUTPUT,
	STAGE_REQUEST_5_OUTPUT
} signing_stage;
static uint32_t version = 1;
static uint32_t lock_time = 0;
static uint32_t sequence = 4294967294;
enum {
	NOT_PARSING,
	PARSING_VERSION,
	PARSING_INPUT_COUNT,
	PARSING_INPUTS,
	PARSING_OUTPUT_COUNT,
	PARSING_OUTPUTS_VALUE,
	PARSING_OUTPUTS,
	PARSING_LOCKTIME
} raw_tx_status;

enum {
	SIGHASH_ALL = 0x01,
	SIGHASH_FORKID = 0x40,
};

/* === Private Functions =================================================== */

/*
 * send_co_failed_message() - send transaction output error message to client
 *
 * INPUT  
 *     co_error - Transaction output compilation error id
 * OUTPUT 
 *     none
 */
void send_fsm_co_error_message(int co_error)
{
    switch(co_error)
    {
        case(TXOUT_COMPILE_ERROR):
        {
            fsm_sendFailure(FailureType_Failure_Other, "Failed to compile output");
            break;
        }
        case(TXOUT_CANCEL):
        {
            fsm_sendFailure(FailureType_Failure_ActionCancelled, "Transaction cancelled");
            break;
        }
        case (TXOUT_EXCHANGE_CONTRACT_ERROR):
        {
            switch(get_exchange_error())
            {
                case ERROR_EXCHANGE_SIGNATURE:
                {
                    fsm_sendFailure(FailureType_Failure_Other, "Exchange signature error");
                    break;
                }
                case ERROR_EXCHANGE_DEPOSIT_COINTYPE:
                {
                    fsm_sendFailure(FailureType_Failure_Other, "Exchange deposit coin type error");
                    break;
                }
                case ERROR_EXCHANGE_DEPOSIT_ADDRESS:
                {
                    fsm_sendFailure(FailureType_Failure_Other, "Exchange deposit address error");
                    break;
                }
                case ERROR_EXCHANGE_DEPOSIT_AMOUNT:
                {
                    fsm_sendFailure(FailureType_Failure_Other, "Exchange deposit amount error");
                    break;
                }
                case ERROR_EXCHANGE_WITHDRAWAL_COINTYPE:
                {
                    fsm_sendFailure(FailureType_Failure_Other, "Exchange withdrawal coin type error");
                    break;
                }
                case ERROR_EXCHANGE_WITHDRAWAL_ADDRESS:
                {
                    fsm_sendFailure(FailureType_Failure_Other, "Exchange withdrawal address error");
                    break;
                }
                case ERROR_EXCHANGE_WITHDRAWAL_AMOUNT:
                {
                    fsm_sendFailure(FailureType_Failure_Other, "Exchange withdrawal amount error");
                    break;
                }
                case ERROR_EXCHANGE_RETURN_COINTYPE:
                {
                    fsm_sendFailure(FailureType_Failure_Other, "Exchange return coin type error");
                    break;
                }
                case ERROR_EXCHANGE_RETURN_ADDRESS:
                {
                    fsm_sendFailure(FailureType_Failure_Other, "Exchange return address error");
                    break;
                }
                case ERROR_EXCHANGE_API_KEY:
                {
                    fsm_sendFailure(FailureType_Failure_Other, "Exchange api key error");
                    break;
                }
                case ERROR_EXCHANGE_CANCEL:
                {
                    fsm_sendFailure(FailureType_Failure_ActionCancelled, "Exchange transaction cancelled");
                    break;
                }
                case ERROR_EXCHANGE_RESPONSE_STRUCTURE:
                {
                    fsm_sendFailure(FailureType_Failure_Other, "Obsolete Response structure error");
                    break;
                }
                default:
                case NO_EXCHANGE_ERROR:
                {
                    break;
                }
            }
            break;
        }
        default:
        {
            fsm_sendFailure(FailureType_Failure_Other, "Unknown TxOut compilation error");
            break;
        }
    }
}

/*
 * check_valid_output_address() - Checks the sanity of an output
 *
 * INPUT
 *     tx_out - pointer to transaction output structure
 * OUTPUT
 *     true/false status
 *
 */
static bool check_valid_output_address(TxOutputType *tx_out)
{
    bool ret_val = false;

    switch(tx_out->address_type)
    {
        case OutputAddressType_SPEND:
        {
            if(tx_out->has_address)
            {
                /* valid address type */
                ret_val = true;
            }

            break;

        }

        case OutputAddressType_TRANSFER:
        case OutputAddressType_CHANGE:
        {
            if(tx_out->address_n_count > 0)
            {
                /* valid address type */
                ret_val = true;
            }

            break;
        }

        case OutputAddressType_EXCHANGE:
        {

            if(tx_out->has_exchange_type)
            {
                ret_val = true;
            }

            break;
        }
    }

    return(ret_val);
}

static void reset_parsing_buffer(uint8_t *buffer, uint8_t *buffer_index)
{
	memset(buffer, 0, VAR_INT_BUFFER);
	*buffer_index = 0;
}

/* === Functions =========================================================== */

/*
Workflow of streamed signing
The STAGE_ constants describe the signing_stage when request is sent.
I - input
O - output
Phase1 - check inputs, previous transactions, and outputs
       - ask for confirmations
       - check fee
=========================================================
foreach I (idx1):
    Request I                                                         STAGE_REQUEST_1_INPUT
    Add I to TransactionChecksum
    Calculate amount of I:
        Request prevhash I, META                                      STAGE_REQUEST_2_PREV_META
        foreach prevhash I (idx2):
            Request prevhash I                                        STAGE_REQUEST_2_PREV_INPUT
        foreach prevhash O (idx2):
            Request prevhash O                                        STAGE_REQUEST_2_PREV_OUTPUT
            Add amount of prevhash O (which is amount of I)
        Calculate hash of streamed tx, compare to prevhash I
foreach O (idx1):
    Request O                                                         STAGE_REQUEST_3_OUTPUT
    Add O to TransactionChecksum
    Display output
    Ask for confirmation
Check tx fee
Ask for confirmation
Phase2: sign inputs, check that nothing changed
===============================================
foreach I (idx1):  // input to sign
    foreach I (idx2):
        Request I                                                     STAGE_REQUEST_4_INPUT
        If idx1 == idx2
        Remember key for signing
            Fill scriptsig
        Add I to StreamTransactionSign
        Add I to TransactionChecksum
    foreach O (idx2):
        Request O                                                     STAGE_REQUEST_4_OUTPUT
        Add O to StreamTransactionSign
        Add O to TransactionChecksum
    Compare TransactionChecksum with checksum computed in Phase 1
    If different:
        Failure
    Sign StreamTransactionSign
    Return signed chunk
foreach O (idx1):
    Request O                                                         STAGE_REQUEST_5_OUTPUT
    Rewrite change address
    Return O
*/

void send_req_1_input(void)
{
	signing_stage = STAGE_REQUEST_1_INPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXINPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx1;
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_2_prev_meta(void)
{
	signing_stage = STAGE_REQUEST_2_PREV_META;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXMETA;
	resp.has_details = true;
	resp.details.has_tx_hash = true;
	resp.details.tx_hash.size = input.prev_hash.size;
	memcpy(resp.details.tx_hash.bytes, input.prev_hash.bytes, input.prev_hash.size);
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_2_prev_input(void)
{
	signing_stage = STAGE_REQUEST_2_PREV_INPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXINPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx2;
	resp.details.has_tx_hash = true;
	resp.details.tx_hash.size = input.prev_hash.size;
	memcpy(resp.details.tx_hash.bytes, input.prev_hash.bytes, resp.details.tx_hash.size);
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_2_prev_output(void)
{
	signing_stage = STAGE_REQUEST_2_PREV_OUTPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXOUTPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx2;
	resp.details.has_tx_hash = true;
	resp.details.tx_hash.size = input.prev_hash.size;
	memcpy(resp.details.tx_hash.bytes, input.prev_hash.bytes, resp.details.tx_hash.size);
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_3_output(void)
{
	signing_stage = STAGE_REQUEST_3_OUTPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXOUTPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx1;
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_4_input(void)
{
	signing_stage = STAGE_REQUEST_4_INPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXINPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx2;
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_4_output(void)
{
	signing_stage = STAGE_REQUEST_4_OUTPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXOUTPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx2;
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_5_output(void)
{
	signing_stage = STAGE_REQUEST_5_OUTPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXOUTPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx1;
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_finished(void)
{
	resp.has_request_type = true;
	resp.request_type = RequestType_TXFINISHED;
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void signing_init(uint32_t _inputs_count, uint32_t _outputs_count, const CoinType *_coin, const HDNode *_root, uint32_t _version, uint32_t _lock_time)
{
	inputs_count = _inputs_count;
	outputs_count = _outputs_count;
	coin = _coin;
	root = _root;
	version = _version;
	lock_time = _lock_time;

	idx1 = 0;
	to_spend = 0;
	spending = 0;
	change_spend = 0;
	memset(&input, 0, sizeof(TxInputType));
	memset(&resp, 0, sizeof(TxRequest));

	signing = true;

	multisig_fp_set = false;
	multisig_fp_mismatch = false;

	tx_init(&to, inputs_count, outputs_count, version, lock_time, false);
	sha256_Init(&transaction_current);
	sha256_Update(&transaction_current, (const uint8_t *)&inputs_count, sizeof(inputs_count));
	sha256_Update(&transaction_current, (const uint8_t *)&outputs_count, sizeof(outputs_count));
	sha256_Update(&transaction_current, (const uint8_t *)&version, sizeof(version));
	sha256_Update(&transaction_current, (const uint8_t *)&lock_time, sizeof(lock_time));

	sha256_Init(&hashers[0]);
	sha256_Init(&hashers[1]);
	sha256_Init(&hashers[2]);

	raw_tx_status = NOT_PARSING;

	send_req_1_input();
    set_exchange_error(NO_EXCHANGE_ERROR);
}

void parse_raw_txack(uint8_t *msg, uint32_t msg_size){
	static int32_t state_pos = 0;
	static uint8_t *ptr;
	static uint8_t var_int_buffer[VAR_INT_BUFFER];
	static uint8_t var_int_buffer_index;
	static uint32_t seen, script_len;
	static uint64_t current_output_val;

	for(uint32_t i = 0; i < msg_size; ++i) {

		state_pos--;

		switch(raw_tx_status) {
			case NOT_PARSING:
				tx_init(&transaction_previous, 0, 0, 0, 0, false);
				state_pos = sizeof(uint32_t);
				raw_tx_status = PARSING_VERSION;
				ptr = (uint8_t *)&transaction_previous.version;
			case PARSING_VERSION:
				*ptr++ = msg[i];

				if(state_pos == 1)
				{
					raw_tx_status = PARSING_INPUT_COUNT;
					reset_parsing_buffer(var_int_buffer, &var_int_buffer_index);
				}
				break;
			case PARSING_INPUT_COUNT:
				var_int_buffer[var_int_buffer_index++] = msg[i];

				if(var_int_buffer_index >= deser_length(var_int_buffer, &transaction_previous.inputs_len))
				{
					raw_tx_status = PARSING_INPUTS;
					state_pos = 36;
					seen = 0;
					reset_parsing_buffer(var_int_buffer, &var_int_buffer_index);
				}

				break;
			case PARSING_INPUTS:
				if(state_pos < 0 && seen < transaction_previous.inputs_len)
				{
					var_int_buffer[var_int_buffer_index++] = msg[i];

					if(var_int_buffer_index >= deser_length(var_int_buffer, &script_len))
					{
						seen++;

						if(seen < transaction_previous.inputs_len)
						{
							state_pos = script_len + 4 + 36;
						}
						else
						{
							state_pos = script_len + 3;	
						}

						script_len = 0;
						reset_parsing_buffer(var_int_buffer, &var_int_buffer_index);
					}
				}
				else if(state_pos < 0)
				{
					raw_tx_status = PARSING_OUTPUT_COUNT;
				}
				break;
			case PARSING_OUTPUT_COUNT:
				var_int_buffer[var_int_buffer_index++] = msg[i];

				if(var_int_buffer_index >= deser_length(var_int_buffer, &transaction_previous.outputs_len))
				{
					raw_tx_status = PARSING_OUTPUTS_VALUE;
					state_pos = 8;
					seen = 0;
					current_output_val = 0;
					ptr = (uint8_t *)&current_output_val;
					reset_parsing_buffer(var_int_buffer, &var_int_buffer_index);
				}
				break;
			case PARSING_OUTPUTS_VALUE:
				if(state_pos < 8)
				{
					*ptr++ = msg[i];

					if(state_pos < 1)
					{
						if (seen == input.prev_index) {
							to_spend += current_output_val;
						}

						raw_tx_status = PARSING_OUTPUTS;
						script_len = 0;
						reset_parsing_buffer(var_int_buffer, &var_int_buffer_index);
					}
				}
				break;
			case PARSING_OUTPUTS:
				if(state_pos < 0 && seen < transaction_previous.outputs_len)
				{
					var_int_buffer[var_int_buffer_index++] = msg[i];

					if(var_int_buffer_index >= deser_length(var_int_buffer, &script_len))
					{
						seen++;

						if(seen < transaction_previous.outputs_len)
						{
							current_output_val = 0;
							ptr = (uint8_t *)&current_output_val;
							raw_tx_status = PARSING_OUTPUTS_VALUE;
							state_pos = script_len + 8;
						}
						else
						{
							state_pos = script_len - 1;
						}
					}
				}
				else if(state_pos < 0)
				{
					raw_tx_status = PARSING_LOCKTIME;
					state_pos = 4;
					ptr = (uint8_t *)&transaction_previous.lock_time;
					reset_parsing_buffer(var_int_buffer, &var_int_buffer_index);
				}
				break;
			case PARSING_LOCKTIME:
            	if(state_pos >= 0)
				{
					*ptr++ = msg[i];
				}
				if(state_pos < 1)
				{
					raw_tx_status = NOT_PARSING;
					memset(&resp, 0, sizeof(TxRequest));

					sha256_Update(&(transaction_previous.ctx), (const uint8_t*)msg+i, 1);
					tx_hash_final(&transaction_previous, hash, true);
					if (memcmp(hash, input.prev_hash.bytes, 32) != 0) {
						fsm_sendFailure(FailureType_Failure_Other, "Encountered invalid prevhash");
						signing_abort();
						return;
					}
            		
            		if (idx1 < inputs_count - 1) {
						idx1++;
						send_req_1_input();
					} else {
						idx1 = 0;
						send_req_3_output();
					}

					return;
				}
				break;
		}

		sha256_Update(&(transaction_previous.ctx), (const uint8_t*)msg+i, 1);
	}
}

//Method called repeatedly by client code in transaction formation.
void signing_txack(TransactionType *tx)
{
	int co;

	if (!signing) {
		fsm_sendFailure(FailureType_Failure_UnexpectedMessage, "Not in Signing mode");
		go_home();
		return;
	}

	memset(&resp, 0, sizeof(TxRequest));

	switch (signing_stage) {

        /* We receive an input to the current transaction without sigScript. Stored in tx->inputs[0]*/
        case STAGE_REQUEST_1_INPUT:
            /*tx->inputs[0] (input of current transaction) contains:
             * address_n (pointer to address in keepkey wallet),
             * prev_hash (of previous transaction),
             * prev_index,
             * script_type,
             * and possibly multisig */

            /* compute multisig fingerprint */
            /* (if all input share the same fingerprint, outputs having the same fingerprint will be considered as change outputs) */
            if (tx->inputs[0].script_type == InputScriptType_SPENDMULTISIG) {
				if (tx->inputs[0].has_multisig && !multisig_fp_mismatch) {
					if (multisig_fp_set) {
						uint8_t h[32];
						if (cryptoMultisigFingerprint(&(tx->inputs[0].multisig), h) == 0) {
							fsm_sendFailure(FailureType_Failure_Other, "Error computing multisig fingeprint");
							signing_abort();
							return;
						}
						if (memcmp(multisig_fp, h, 32) != 0) {
							multisig_fp_mismatch = true;
						}
					} else {
						if (cryptoMultisigFingerprint(&(tx->inputs[0].multisig), multisig_fp) == 0) {
							fsm_sendFailure(FailureType_Failure_Other, "Error computing multisig fingeprint");
							signing_abort();
							return;
						}
						multisig_fp_set = true;
					}
				}
			} else { // InputScriptType_SPENDADDRESS
				multisig_fp_mismatch = true;
			}
			sha256_Update(&transaction_current, (const uint8_t *)tx->inputs, sizeof(TxInputType));
			memcpy(&input, tx->inputs, sizeof(TxInputType));

			TxInputType *txinput = &tx->inputs[0];

            //TODO: Is this necessary?
//            if(!txinput->has_sequence){
//                txinput->sequence = sequence;
//            }

            tx_prevout_hash(&hashers[0], txinput);
			tx_sequence_hash(&hashers[1], txinput);
			// hash prevout and script type to check it later (relevant for fee computation)
            tx_prevout_hash(&hashers[2], txinput);
			sha256_Update(&hashers[2], &txinput->script_type, sizeof(&txinput->script_type));

            /*For the above input, we now query for transaction referenced in prev_hash*/
			send_req_2_prev_meta();
			return;

        /* We receive metadata for a transaction containing an output refrenced by an input of current transaction
         * This metadata is just enough to tell us how many inputs and outputs we need in the following two stages */
		case STAGE_REQUEST_2_PREV_META:
			tx_init(&transaction_previous, tx->inputs_cnt, tx->outputs_cnt, tx->version, tx->lock_time, false);
			idx2 = 0; //For the time being, this will index inputs of transaction_previous
			send_req_2_prev_input();
			return;

        /* We receive a tx input for a previous transaction. $tx->inputs[0] has all input data
         * We store the data on the growing transaction_previous.ctx hash context */
		case STAGE_REQUEST_2_PREV_INPUT:
			if (!tx_serialize_input_hash(&transaction_previous, tx->inputs)) {
				fsm_sendFailure(FailureType_Failure_Other, "Failed to serialize input");
				signing_abort();
				return;
			}
			if (idx2 < transaction_previous.inputs_len - 1) {
				idx2++;
                //If more inputs for the previous transaction, run this method again asking for the next input.
				send_req_2_prev_input();
			} else {
				idx2 = 0;
                //If no more inputs for the previous transaction, get the first output.
				send_req_2_prev_output();
			}
			return;

        /* We receive a tx output for a previous transaction. $tx->bin_outputs[0] has all output data including
         * amount and script_pubkey. We store the data on the growing transaction_previous.ctx hash context */
        case STAGE_REQUEST_2_PREV_OUTPUT:
			if (!tx_serialize_output_hash(&transaction_previous, tx->bin_outputs)) {
				fsm_sendFailure(FailureType_Failure_Other, "Failed to serialize output");
				signing_abort();
				return;
			}
            //If this specific output is the one referenced by the current input, contribute its amount to spend.
			if (idx2 == input.prev_index) {
				to_spend += tx->bin_outputs[0].amount;
			}

            //If more outputs for this previous transaction, rerun this step for the next output.
			if (idx2 < transaction_previous.outputs_len - 1) {
				idx2++;
				send_req_2_prev_output();
			} else {
				/* Check next output */
				tx_hash_final(&transaction_previous, hash, true);
				if (memcmp(hash, input.prev_hash.bytes, 32) != 0) {
					fsm_sendFailure(FailureType_Failure_Other, "Encountered invalid prevhash");
					signing_abort();
					return;
				}

                /* If there are more inputs, re-run stages STAGE_REQUEST_1_INPUT --> STAGE_REQUEST_2_PREV_OUTPUT
                 * for new input */
				if (idx1 < inputs_count - 1) {
					idx1++;
					send_req_1_input();
				} else {
                    /* We have all the input codepoints + sequences for the current transaction.
                     * Finalize them for use in bip143 signing.*/
					sha256_Final(&hashers[0], hash_prevouts);
					sha256_Raw(hash_prevouts, 32, hash_prevouts);

					sha256_Final(&hashers[1], hash_sequence);
					sha256_Raw(hash_sequence, 32, hash_sequence);

                    //Use hash_check for validation later. This is hash_prevouts along with
					sha256_Final(&hashers[2], hash_check);

                    // initialize hashOutputs
					sha256_Init(&hashers[0]);

					idx1 = 0;
                    idx2 = 0;
					send_req_3_output();
				}
			}
			return;

        /* We receive a tx output for the current transaction. $tx->outputs[0] has all output data including
         * amount, address, script_type... The main thrust of this portion is creating the pubKeyScript/redeemScript
         * for the received output */
		case STAGE_REQUEST_3_OUTPUT:
		{
			/* Downloaded output idx1 the first time.
			 *  Add it to transaction check
			 *  Ask for permission.
			 */
			bool is_change = false;

			if (tx->outputs[0].script_type == OutputScriptType_PAYTOMULTISIG &&
			    tx->outputs[0].has_multisig &&
			    multisig_fp_set && !multisig_fp_mismatch) {
				uint8_t h[32];
				if (cryptoMultisigFingerprint(&(tx->outputs[0].multisig), h) == 0) {
					fsm_sendFailure(FailureType_Failure_Other, "Error computing multisig fingeprint");
					signing_abort();
					return;
				}
				if (memcmp(multisig_fp, h, 32) == 0) {
					is_change = true;
				}
            } else {
                if(tx->outputs[0].has_address_type) {
                    if(check_valid_output_address(tx->outputs) == false) {
                        fsm_sendFailure(FailureType_Failure_Other, "Invalid output address type");
                        signing_abort();
                        return;
                    }

                    if(tx->outputs[0].script_type == OutputScriptType_PAYTOADDRESS &&
                            tx->outputs[0].address_n_count > 0 &&
                            tx->outputs[0].address_type == OutputAddressType_CHANGE) {
                        is_change = true;
                    }
                }
                else if(tx->outputs[0].script_type == OutputScriptType_PAYTOADDRESS &&
                        tx->outputs[0].address_n_count > 0) {
                    is_change = true;
                }
            }

			if (is_change) {
				if (change_spend == 0) { // not set
					change_spend = tx->outputs[0].amount;
				} else {
					fsm_sendFailure(FailureType_Failure_Other, "Only one change output allowed");
					signing_abort();
					return;
			    }
			}

            //We generate script for this output and put it on bin_output
			co = run_policy_compile_output(coin, root, (void *)tx->outputs, (void *)&bin_output, !is_change);
			if (co <= TXOUT_COMPILE_ERROR) {
			    send_fsm_co_error_message(co);
			    signing_abort();
			    return;
			}

			spending += tx->outputs[0].amount;

			sha256_Update(&transaction_current, (const uint8_t *)&bin_output, sizeof(TxOutputBinType));

			tx_output_hash(&hashers[0], &bin_output);

            //Repeat this process if there are more outputs on current transaction
			if (idx1 < outputs_count - 1) {
				idx1++;
				send_req_3_output();
			} else {
                sha256_Final(&transaction_current, hash_check);

                // check fees
                if (spending > to_spend) {
                    fsm_sendFailure(FailureType_Failure_NotEnoughFunds, "Not enough funds");
                    signing_abort();
                    return;
                }

                uint64_t fee = to_spend - spending;
                uint32_t tx_est_size = transactionEstimateSizeKb(inputs_count, outputs_count);
                char total_amount_str[32];
                char fee_str[32];

                coin_amnt_to_str(coin, fee, fee_str, sizeof(fee_str));

                if (fee > (uint64_t)tx_est_size * coin->maxfee_kb) {
                    if (!confirm(ButtonRequestType_ButtonRequest_FeeOverThreshold, "Confirm Fee", "%s", fee_str)) {
                        fsm_sendFailure(FailureType_Failure_ActionCancelled, "Fee over threshold. Signing cancelled.");
                        signing_abort();
                        return;
                    }

                }

                // last confirmation
                coin_amnt_to_str(coin, to_spend - change_spend, total_amount_str, sizeof(total_amount_str));

                if(!confirm_transaction(total_amount_str, fee_str)) {
                    fsm_sendFailure(FailureType_Failure_ActionCancelled, "Signing cancelled by user");
                    signing_abort();
                    return;
                }

                // Everything was checked, now phase 2 begins and the transaction is signed.
                layout_simple_message("Signing Transaction...");

			    idx1 = 0;
			    idx2 = 0;

                //At this point hash_prevouts, hash_seqence, and hash_outputs are complete.
				sha256_Final(&hashers[0], hash_outputs);
				sha256_Raw(hash_outputs, 32, hash_outputs);

			    send_req_4_input();
			}
			return;
		}

        /* We receive a tx input for the current transaction. $tx->inputs[0] has all input data including
         * address_n, prev_hash, prev_index... The main thrust of this portion adding an output script */
		case STAGE_REQUEST_4_INPUT:
			if (idx2 == 0) {
				tx_init(&ti, inputs_count, outputs_count, version, lock_time, true);
				sha256_Init(&transaction_current);
				sha256_Update(&transaction_current, (const uint8_t *)&inputs_count, sizeof(inputs_count));
				sha256_Update(&transaction_current, (const uint8_t *)&outputs_count, sizeof(outputs_count));
				sha256_Update(&transaction_current, (const uint8_t *)&version, sizeof(version));
				sha256_Update(&transaction_current, (const uint8_t *)&lock_time, sizeof(lock_time));
				memset(privkey, 0, 32);
				memset(pubkey, 0, 33);
			}
			sha256_Update(&transaction_current, (const uint8_t *)tx->inputs, sizeof(TxInputType));
			if (idx2 == idx1) {
				memcpy(&input, tx->inputs, sizeof(TxInputType));

                /* Puts the redeemScript/pubKeyScript corresponding to the output from a previous transaction into
                 * the sigScript for this input. Though this seems backwards and insane, we do this because this is
                 * part of the digest algorithm for vanilla BTC: we sign this digest and OP_CHECKSIG will evaluate to T */
				compile_input_script_sig(&tx->inputs[0]);
				if (tx->inputs[0].script_sig.size == 0) {
					fsm_sendFailure(FailureType_Failure_Other, "Failed to compile input");
					signing_abort();
					return;
				}
				memcpy(privkey, node.private_key, 32);
				memcpy(pubkey, node.public_key, 33);
			} else {
				tx->inputs[0].script_sig.size = 0;
			}
			if (!tx_serialize_input_hash(&ti, tx->inputs)) {
				fsm_sendFailure(FailureType_Failure_Other, "Failed to serialize input");
				signing_abort();
				return;
			}
			if (idx2 < inputs_count - 1) {
				idx2++;
				send_req_4_input();
			} else {
				idx2 = 0;
				send_req_4_output();
			}
			return;


		case STAGE_REQUEST_4_OUTPUT:
			co = run_policy_compile_output(coin, root, (void *)tx->outputs, (void *)&bin_output, false);
			if (co <= TXOUT_COMPILE_ERROR) {
			    send_fsm_co_error_message(co);
			    signing_abort();
			    return;
			}
			sha256_Update(&transaction_current, (const uint8_t *)&bin_output, sizeof(TxOutputBinType));
			if (!tx_serialize_output_hash(&ti, &bin_output)) {
				fsm_sendFailure(FailureType_Failure_Other, "Failed to serialize output");
				signing_abort();
				return;
			}
			if (idx2 < outputs_count - 1) {
				idx2++;
				send_req_4_output();
			} else {
				uint8_t sighash;
				if(coin->has_forkid){
					if (!compile_input_script_sig(&input)) {
						fsm_sendFailure(FailureType_Failure_Other, ("Processor Error: Failed to compile input"));
						signing_abort();
						return;
					}
					if (!input.has_amount) {
						fsm_sendFailure(FailureType_Failure_Other, ("Data Error: SIGHASH_FORKID input without amount"));
						signing_abort();
						return;
					}
					if (input.amount > to_spend) {
						fsm_sendFailure(FailureType_Failure_Other, ("Data Error: Transaction has changed during signing"));
						signing_abort();
						return;
					}
					to_spend -= input.amount;

					sighash = SIGHASH_ALL | SIGHASH_FORKID;
					signing_hash_bip143(&input, sighash, coin->forkid, hash);
				} else {
                    sha256_Final(&transaction_current, hash);
                    sighash = SIGHASH_ALL;
				}

				if (memcmp(hash, hash_check, 32) != 0) {
					fsm_sendFailure(FailureType_Failure_Other, "Transaction has changed during signing");
					signing_abort();
					return;
				}
				tx_hash_final(&ti, hash, false);
				resp.has_serialized = true;
				resp.serialized.has_signature_index = true;
				resp.serialized.signature_index = idx1;
				resp.serialized.has_signature = true;
				resp.serialized.has_serialized_tx = true;
				ecdsa_sign_digest(&secp256k1, privkey, hash, sig, 0);
				resp.serialized.signature.size = ecdsa_sig_to_der(sig, resp.serialized.signature.bytes);

				if (input.script_type == InputScriptType_SPENDMULTISIG) {
					if (!input.has_multisig) {
						fsm_sendFailure(FailureType_Failure_Other, "Multisig info not provided");
						signing_abort();
						return;
					}
					// fill in the signature
					int pubkey_idx = cryptoMultisigPubkeyIndex(&(input.multisig), pubkey);
					if (pubkey_idx < 0) {
						fsm_sendFailure(FailureType_Failure_Other, "Pubkey not found in multisig script");
						signing_abort();
						return;
					}
					memcpy(input.multisig.signatures[pubkey_idx].bytes, resp.serialized.signature.bytes, resp.serialized.signature.size);
					input.multisig.signatures[pubkey_idx].size = resp.serialized.signature.size;
					input.script_sig.size = serialize_script_multisig(&(input.multisig), input.script_sig.bytes);
					if (input.script_sig.size == 0) {
						fsm_sendFailure(FailureType_Failure_Other, "Failed to serialize multisig script");
						signing_abort();
						return;
					}
				} else { // SPENDADDRESS
					input.script_sig.size = serialize_script_sig(resp.serialized.signature.bytes,
																 resp.serialized.signature.size, pubkey, 33, sighash,
																 input.script_sig.bytes);
				}
				resp.serialized.serialized_tx.size = tx_serialize_input(&to, &input, resp.serialized.serialized_tx.bytes);

				if (idx1 < inputs_count - 1) {
					idx1++;
					idx2 = 0;
					send_req_4_input();
				} else {
					idx1 = 0;
					send_req_5_output();
				}
			}
			return;
		case STAGE_REQUEST_5_OUTPUT:
			co = run_policy_compile_output(coin, root, (void *)tx->outputs, (void *)&bin_output, false);
			if (co <= TXOUT_COMPILE_ERROR) {
			    send_fsm_co_error_message(co);
			    signing_abort();
			    return;
			}
			resp.has_serialized = true;
			resp.serialized.has_serialized_tx = true;
			resp.serialized.serialized_tx.size = tx_serialize_output(&to, &bin_output, resp.serialized.serialized_tx.bytes);
			if (idx1 < outputs_count - 1) {
				idx1++;
				send_req_5_output();
			} else {
				send_req_finished();
				signing_abort();
			}
			return;
	}

	fsm_sendFailure(FailureType_Failure_Other, "Signing error");
	signing_abort();
}

void signing_abort(void)
{
	if (signing) {
		go_home();
		signing = false;
	}
}

/* This function is actually used to reproduce the redeemScript/scriptPubKey of the corresponding output.
 * The intention here is to use this in the signature of the script_sig. */
bool compile_input_script_sig(TxInputType *tinput)
{
	if (!multisig_fp_mismatch) {
		// check that this is still multisig
		uint8_t h[32];
		if (tinput->script_type != InputScriptType_SPENDMULTISIG
			|| cryptoMultisigFingerprint(&(tinput->multisig), h) == 0
			|| memcmp(multisig_fp, h, 32) != 0) {
			// Transaction has changed during signing
			return false;
		}
	}
	memcpy(&node, root, sizeof(HDNode));
	if (hdnode_private_ckd_cached(&node, tinput->address_n, tinput->address_n_count) == 0) {
		// Failed to derive private key
		return false;
	}
	hdnode_fill_public_key(&node);
	if (tinput->has_multisig) {
		tinput->script_sig.size = compile_script_multisig(&(tinput->multisig), tinput->script_sig.bytes);
	} else { // SPENDADDRESS
		uint8_t xhash[20];
		ecdsa_get_pubkeyhash(node.public_key, xhash);
		tinput->script_sig.size = compile_script_sig(coin->address_type, xhash, tinput->script_sig.bytes);
	}
	return tinput->script_sig.size > 0;
}

void signing_hash_bip143(const TxInputType *txinput, uint8_t sighash, uint32_t forkid, uint8_t *xhash) {
	uint32_t hash_type = (forkid << 8) | sighash;
	SHA256_CTX sigContainer;
	sha256_Init(&sigContainer);
	sha256_Update(&sigContainer, (const uint8_t *)&version, 4);
	sha256_Update(&sigContainer, hash_prevouts, 32); //hash_prevouts is serialization of all input outpoints (32byte txid followed by 4byte output index)
	sha256_Update(&sigContainer, hash_sequence, 32); //is the double SHA256 of the serialization of nSequence of all inputs;
    tx_prevout_hash(&sigContainer, txinput); //codepoint
	tx_script_hash(&sigContainer, txinput->script_sig.size, txinput->script_sig.bytes); //script
	sha256_Update(&sigContainer, (const uint8_t*) &txinput->amount, 8); //value
	tx_sequence_hash(&sigContainer, txinput); //nSequence
	sha256_Update(&sigContainer, hash_outputs, 32); //the double SHA256 of the serialization of all output amounts (8-byte little endian) paired up with their scriptPubKey
	sha256_Update(&sigContainer, (const uint8_t*) &lock_time, 4); //locktime
	sha256_Update(&sigContainer, (const uint8_t*) &hash_type, 4); //sighash type
	sha256_Final(&sigContainer, xhash);
	sha256_Raw(xhash, 32, xhash);
}

