/** FioFee implementation file
 *  Description: FioFee is the smart contract that manages fees.
 *  @author Adam Androulidakis, Casey Gardiner, Ed Rotthoff
 *  @modifedby
 *  @file fio.fee.cpp
 *  @license FIO Foundation ( https://github.com/fioprotocol/fio/blob/master/LICENSE ) Dapix
 */

#include "fio.fee.hpp"
#include "../fio.address/fio.address.hpp"
#include <fio.common/fio.common.hpp>
#include <fio.common/fioerror.hpp>
#include <eosio/native/intrinsics.hpp>
#include <string>
#include <map>

using std::string;

namespace fioio {

    /***
     * This contract maintains fee related actions and data. It provides voting actions which allow the
     * setting of fee amounts for mandatory fees, and the setting of bundle counts for bundled fees
     * by the active block producers. It provides datga structures representing the fee construct within
     * the FIO protocol. It provides the computation of fees from the voting
     * results of block producers.
     */
    class [[eosio::contract("FioFee")]]  FioFee : public eosio::contract {

    private:
        const int MIN_FEE_VOTERS_FOR_MEDIAN = 15;
        fiofee_table fiofees;
        feevoters_table feevoters;
        bundlevoters_table bundlevoters;
        feevotes_table feevotes;
        eosiosystem::top_producers_table topprods;
        eosiosystem::producers_table prods;



        vector<name> getTopProds(){
            int NUMBER_TO_SELECT = 42;
            auto idx = prods.get_index<"prototalvote"_n>();

            std::vector< name > topprods;
            topprods.reserve(NUMBER_TO_SELECT);

            for( auto it = idx.cbegin(); it != idx.cend() && topprods.size() < NUMBER_TO_SELECT && 0 < it->total_votes && it->active(); ++it ) {
                topprods.push_back(it->owner);
            }
            return topprods;
        }

        uint32_t update_fees() {
            vector<uint64_t> fee_ids; //hashes for endpoints to process.
            
            int NUMBER_FEES_TO_PROCESS = 10;

            //get the fees needing processing.
            auto fee = fiofees.begin();
            while (fee != fiofees.end()) {
                if(fee->votes_pending.value()){
                    fee_ids.push_back(fee->fee_id);
                    //only get the specified number of fees to process.
                    if (fee_ids.size() == NUMBER_FEES_TO_PROCESS){
                        break;
                    }
                }
                fee++;
            }

            //throw a 400 error if fees to process is empty.
            fio_400_assert(fee_ids.size() > 0, "compute fees", "compute fees",
                           "No Work.", ErrorNoWork);


            vector<uint64_t> votesufs;
            int processed_fees = 0;

            for(int i=0;i<fee_ids.size();i++) { //for each fee to process
                print("EDEDEDED process fee id ",fee_ids[i]);
                votesufs.clear();
                auto topprod = topprods.begin();
                while (topprod != topprods.end()) { //get the votes of the producers, compute the voted fee, and median.
                    //get the fee voters record of this BP.
                    auto voters_iter = feevoters.find(topprod->producer.value);
                    //if there is no fee voters record, then there is not a multiplier, skip this BP.
                    if (voters_iter != feevoters.end()) {
                        //get all the fee votes made by this BP.
                        auto votesbybpname = feevotes.get_index<"bybpname"_n>();
                        auto bpvote_iter = votesbybpname.find(topprod->producer.value);
                        int countem = 0;

                        if (bpvote_iter != votesbybpname.end()) {
                            //if its in the votes list, and if it has a vote, IE end_point is greater 0, then use if.
                            if ((bpvote_iter->feevotes.size() > fee_ids[i]) &&
                                (bpvote_iter->feevotes[fee_ids[i]].end_point.length() > 0)) {
                                const double dresult = voters_iter->fee_multiplier *
                                                       (double) bpvote_iter->feevotes[fee_ids[i]].value;
                                const uint64_t voted_fee = (uint64_t)(dresult);
                                votesufs.push_back(voted_fee);
                                print("EDEDEDED added a value to the list for id ",fee_ids[i]);
                            }
                        }
                    }else {
                        print("EDEDEDED  fee id ",fee_ids[i]);
                    }
                    topprod++;
                }

                //compute the median from the votesufs.
                int64_t median_fee = -1;
                //KLUDGE for testing
                if (votesufs.size() >= 2) {
                    // if (votesufs.size() >= MIN_FEE_VOTERS_FOR_MEDIAN) {
                    sort(votesufs.begin(), votesufs.end());
                    int size = votesufs.size();
                    if (votesufs.size() % 2 == 0) {
                        median_fee = (votesufs[size / 2 - 1] + votesufs[size / 2]) / 2;
                    } else {
                        median_fee = votesufs[size / 2];
                    }
                }

                //set median as the new fee amount.
                if (median_fee > 0) {
                    //update the fee.
                    auto fee_iter = fiofees.find(fee_ids[i]);
                    if (fee_iter != fiofees.end()) {
                        fiofees.modify(fee_iter, _self, [&](struct fiofee &ff) {
                            ff.suf_amount = median_fee;
                            ff.votes_pending.emplace(false);
                        });
                        processed_fees++;
                        print("EDEDEDED set final fee for id ",fee_ids[i]);
                    }
                }

            }

            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

            return processed_fees;
        }

    public:
        using contract::contract;

        FioFee(name s, name code, datastream<const char *> ds)
                : contract(s, code, ds),
                  fiofees(_self, _self.value),
                  bundlevoters(_self, _self.value),
                  feevoters(_self, _self.value),
                  feevotes(_self, _self.value),
                  topprods(SYSTEMACCOUNT, SYSTEMACCOUNT.value),
                  prods(SYSTEMACCOUNT,SYSTEMACCOUNT.value){
        }

        /*********
         * This action provides the ability to set a fee vote by a block producer.
         * the user submits a list of feevalue objects each of which contains a suf amount
         * (to which a multiplier will later be applied, and an endpoint name which must
         * match one of the endpoint names in the fees table.
         * @param fee_values this is a vector of feevalue objects each has the
         *                     endpoint name and desired fee in FIO SUF
         * @param actor this is the string rep of the fio account for the user making the call, a block producer.
         */
        // @abi action
        [[eosio::action]]
        void setfeevote(const vector <feevalue> &fee_values, const int64_t &max_fee, const string &actor) {

            name aactor = name(actor.c_str());
            require_auth(aactor);
            bool dbgout = false;


            //check that the actor is in the top42.
            vector<name> top_prods = getTopProds();
            fio_400_assert((std::find(top_prods.begin(), top_prods.end(), aactor)) !=
                top_prods.end(), "actor", actor," Not a top 42 BP",ErrorFioNameNotReg);

            fio_400_assert(max_fee >= 0, "max_fee", to_string(max_fee), "Invalid fee value",
                           ErrorMaxFeeInvalid);
            const uint32_t nowtime = now();

            //get all the votes made by this actor. go through the list
            //and find the fee vote to update.
            auto feevotesbybpname = feevotes.get_index<"bybpname"_n>();
            // auto votebyname_iter = feevotesbybpname.lower_bound(aactor.value);
            auto votebyname_iter = feevotesbybpname.find(aactor.value);

            if(votebyname_iter != feevotesbybpname.end()){
                fio_400_assert(!(votebyname_iter->lastvotetimestamp > (nowtime - TIME_BETWEEN_FEE_VOTES_SECONDS)), "", "", "Too soon since last call", ErrorTimeViolation);
            }
            vector<feevalue> feevotesv;
            bool emplacerec = true;

            //check for time violation.

            if (votebyname_iter != feevotesbybpname.end()){
                emplacerec = false;
                feevotesv = votebyname_iter->feevotes;
            }


            // go through all the fee values passed in.
            for (auto &feeval : fee_values) {
                //check the endpoint exists for this fee
                const uint128_t endPointHash = string_to_uint128_hash(feeval.end_point.c_str());

                auto feesbyendpoint = fiofees.get_index<"byendpoint"_n>();
                auto fees_iter = feesbyendpoint.find(endPointHash);

                fio_400_assert(fees_iter != feesbyendpoint.end(), "end_point", feeval.end_point,
                               "invalid end_point", ErrorEndpointNotFound);

                fio_400_assert(feeval.value >= 0, "fee_value", feeval.end_point,
                               "invalid fee value", ErrorFeeInvalid);

                uint64_t feeid = fees_iter->fee_id;

                // if the vector doesnt have an entry at this fees id index, add items out to this index.
                // items with an empty string for end_point will NOT be used in median calcs.
                if (feevotesv.size() < (feeid+1)){
                    for(int ix = feevotesv.size();ix<=(feeid+1);ix++)
                    {
                        feevalue tfv;
                        feevotesv.push_back(tfv);
                    }
                }

                uint64_t idtoremove;
                bool found = false;

                feevotesv[feeid].end_point = feeval.end_point;
                feevotesv[feeid].value = feeval.value;

                if(topprods.find(aactor.value) != topprods.end()) {
                    feesbyendpoint.modify(fees_iter, _self, [&](struct fiofee &a) {
                        a.votes_pending.emplace(true);
                    });
                }

            }

            //emplace or update.
            if (emplacerec){
                feevotes.emplace(aactor, [&](struct feevote &fv) {
                    fv.id = feevotes.available_primary_key();
                    fv.block_producer_name = aactor;
                    fv.feevotes = feevotesv;
                    fv.lastvotetimestamp = nowtime;
                });
            }else {
                feevotesbybpname.modify(votebyname_iter, aactor, [&](struct feevote &fv) {
                    fv.feevotes = feevotesv;
                    fv.lastvotetimestamp = nowtime;
                });

            }



            //begin new fees, logic for Mandatory fees.
            uint128_t endpoint_hash = string_to_uint128_hash("submit_fee_ratios");

            auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();
            auto fee_iter = fees_by_endpoint.find(endpoint_hash);
            //if the fee isnt found for the endpoint, then 400 error.
            fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", "submit_fee_ratios",
                           "FIO fee not found for endpoint", ErrorNoEndpoint);

            uint64_t reg_amount = fee_iter->suf_amount;
            uint64_t fee_type = fee_iter->type;

            //if its not a mandatory fee then this is an error.
            fio_400_assert(fee_type == 0, "fee_type", to_string(fee_type),
                           "submit_fee_ratios unexpected fee type for endpoint submit_fee_ratios, expected 0",
                           ErrorNoEndpoint);

            fio_400_assert(max_fee >= (int64_t)reg_amount, "max_fee", to_string(max_fee), "Fee exceeds supplied maximum.",
                           ErrorMaxFeeExceeded);

            fio_fees(aactor, asset(reg_amount, FIOSYMBOL));
            processrewardsnotpid(reg_amount, get_self());
            //end new fees, logic for Mandatory fees.


            const string response_string = string("{\"status\": \"OK\"") +
                                           string(",\"fee_collected\":") +
                                           to_string(reg_amount) + string("}");

            if (SETFEEVOTERAM > 0) {
                action(
                        permission_level{SYSTEMACCOUNT, "active"_n},
                        "eosio"_n,
                        "incram"_n,
                        std::make_tuple(aactor, SETFEEVOTERAM)
                ).send();
            }

            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

            send_response(response_string.c_str());
        }

        /**********
      * This method will update the fees based upon the present votes made by producers.
      */
        [[eosio::action]]
        void updatefees() {
            uint32_t numberprocessed = update_fees();
            const string response_string = string("{\"status\": \"OK\",\"fees_processed\":") +
                                           to_string(numberprocessed) + string("}");
            send_response(response_string.c_str());
        }

       /********
        * This action allows block producers to vote for the number of transactions that will be permitted
        * for free in the FIO bundled transaction model.
        * @param bundled_transactions the number of bundled transactions per FIO user.
        * @param actor the block producer actor that is presently voting, the signer of this tx.
        */
        // @abi action
        [[eosio::action]]
        void bundlevote(
                const int64_t &bundled_transactions,
                const int64_t &max_fee,
                const string &actor
        ) {
            const name aactor = name(actor.c_str());
            require_auth(aactor);

            //check that the actor is in the top42.
            vector<name> top_prods = getTopProds();
            fio_400_assert((std::find(top_prods.begin(), top_prods.end(), aactor)) !=
                           top_prods.end(), "actor", actor," Not a top 42 BP",ErrorFioNameNotReg);


            fio_400_assert(bundled_transactions > 0, "bundled_transactions", to_string(bundled_transactions),
                           " Must be positive",
                           ErrorFioNameNotReg);

            const uint32_t nowtime = now();

            auto voter_iter = bundlevoters.find(aactor.value);
            if (voter_iter != bundlevoters.end()) //update if it exists
            {
                const uint32_t lastupdate = voter_iter->lastvotetimestamp;
                if (lastupdate <= (nowtime - TIME_BETWEEN_VOTES_SECONDS)) {
                    bundlevoters.modify(voter_iter, _self, [&](struct bundlevoter &a) {
                        a.block_producer_name = aactor;
                        a.bundledbvotenumber = bundled_transactions;
                        a.lastvotetimestamp = nowtime;
                    });
                } else {
                    fio_400_assert(false, "", "", "Too soon since last call", ErrorTimeViolation);
                }
            } else {
                bundlevoters.emplace(aactor, [&](struct bundlevoter &f) {
                    f.block_producer_name = aactor;
                    f.bundledbvotenumber = bundled_transactions;
                    f.lastvotetimestamp = nowtime;
                });
            }

            //begin new fees, logic for Mandatory fees.
            uint128_t endpoint_hash = string_to_uint128_hash("submit_bundled_transaction");

            auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();
            auto fee_iter = fees_by_endpoint.find(endpoint_hash);
            //if the fee isnt found for the endpoint, then 400 error.
            fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", "submit_bundled_transaction",
                           "FIO fee not found for endpoint", ErrorNoEndpoint);

            uint64_t reg_amount = fee_iter->suf_amount;
            uint64_t fee_type = fee_iter->type;

            //if its not a mandatory fee then this is an error.
            fio_400_assert(fee_type == 0, "fee_type", to_string(fee_type),
                           "submit_bundled_transaction unexpected fee type for endpoint submit_bundled_transaction, expected 0",
                           ErrorNoEndpoint);

            fio_400_assert(max_fee >= (int64_t)reg_amount, "max_fee", to_string(max_fee), "Fee exceeds supplied maximum.",
                           ErrorMaxFeeExceeded);

            fio_fees(aactor, asset(reg_amount, FIOSYMBOL));
            processrewardsnotpid(reg_amount, get_self());
            //end new fees, logic for Mandatory fees.

            const string response_string = string("{\"status\": \"OK\"}");

            if (BUNDLEVOTERAM > 0) {
                action(
                        permission_level{SYSTEMACCOUNT, "active"_n},
                        "eosio"_n,
                        "incram"_n,
                        std::make_tuple(aactor, BUNDLEVOTERAM)
                ).send();
            }

            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

            send_response(response_string.c_str());
        }


        /**********
         * This action will create a new feevoters record if the specified block producer does not yet exist in the
         * feevoters table,
         * it will verify that the producer making the request is a present active block producer, it will update the
         * feevoters record if a pre-existing feevoters record exists.
         * @param multiplier this is the multiplier that will be applied to all fee votes for this producer before
         * computing the median fee.
         * @param actor this is the block producer voter.
         */
        // @abi action
        [[eosio::action]]
        void setfeemult(
                const double &multiplier,
                const int64_t &max_fee,
                const string &actor
        ) {

            const name aactor = name(actor.c_str());
            require_auth(aactor);

            //check that the actor is in the top42.
            vector<name> top_prods = getTopProds();
            fio_400_assert((std::find(top_prods.begin(), top_prods.end(), aactor)) !=
                           top_prods.end(), "actor", actor," Not a top 42 BP",ErrorFioNameNotReg);

            fio_400_assert(multiplier > 0, "multiplier", to_string(multiplier),
                           " Must be positive",
                           ErrorFioNameNotReg);

            fio_400_assert(max_fee >= 0, "max_fee", to_string(max_fee), "Invalid fee value",
                           ErrorMaxFeeInvalid);

            const uint32_t nowtime = now();

            auto voter_iter = feevoters.find(aactor.value);
            if (voter_iter != feevoters.end())
            {
                const uint32_t lastupdate = voter_iter->lastvotetimestamp;
                if (lastupdate <= (nowtime - 120)) {
                    feevoters.modify(voter_iter, _self, [&](struct feevoter &a) {
                        a.block_producer_name = aactor;
                        a.fee_multiplier = multiplier;
                        a.lastvotetimestamp = nowtime;
                    });
                } else {
                    fio_400_assert(false, "", "", "Too soon since last call", ErrorTimeViolation);
                }
            } else {
                feevoters.emplace(aactor, [&](struct feevoter &f) {
                    f.block_producer_name = aactor;
                    f.fee_multiplier = multiplier;
                    f.lastvotetimestamp = nowtime;
                });
            }

            //get all voted fees and set votes pending.
            auto feevotesbybpname = feevotes.get_index<"bybpname"_n>();
            auto votebyname_iter = feevotesbybpname.lower_bound(aactor.value);
            auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();

            if(topprods.find(aactor.value) != topprods.end()) {

                while (votebyname_iter != feevotesbybpname.end()) {
                    if (votebyname_iter->block_producer_name.value != aactor.value) {
                        //if the bp name changes we have exited the items of interest, so quit.
                        break;
                    } else {
                        /*  just to compile...bring it back
                        auto fee_iter = fees_by_endpoint.find(votebyname_iter->end_point_hash);
                        fio_400_assert((fee_iter != fees_by_endpoint.end()), "end point", votebyname_iter->end_point,
                                       " Fee lookup error",
                                       ErrorNoFeesFoundForEndpoint);
                        fees_by_endpoint.modify(fee_iter, _self, [&](struct fiofee &a) {
                            a.votes_pending.emplace(true);
                        });
                         */

                    }
                    votebyname_iter++;
                }
            }


            //begin new fees, logic for Mandatory fees.
            uint128_t endpoint_hash = string_to_uint128_hash("submit_fee_multiplier");

            auto fee_iter = fees_by_endpoint.find(endpoint_hash);
            //if the fee isnt found for the endpoint, then 400 error.
            fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", "submit_fee_multiplier",
                           "FIO fee not found for endpoint", ErrorNoEndpoint);

            uint64_t reg_amount = fee_iter->suf_amount;
            uint64_t fee_type = fee_iter->type;

            //if its not a mandatory fee then this is an error.
            fio_400_assert(fee_type == 0, "fee_type", to_string(fee_type),
                           "submit_fee_multiplier unexpected fee type for endpoint submit_fee_multiplier, expected 0",
                           ErrorNoEndpoint);

            fio_400_assert(max_fee >= (int64_t)reg_amount, "max_fee", to_string(max_fee), "Fee exceeds supplied maximum.",
                           ErrorMaxFeeExceeded);

            fio_fees(aactor, asset(reg_amount, FIOSYMBOL));
            processrewardsnotpid(reg_amount, get_self());
            //end new fees, logic for Mandatory fees.


            const string response_string = string("{\"status\": \"OK\"") +
                                           string(",\"fee_collected\":") +
                                           to_string(reg_amount) + string("}");

            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

            send_response(response_string.c_str());
        }


        // @abi action
        [[eosio::action]]
        void mandatoryfee(
                const string &end_point,
                const name &account,
                const int64_t &max_fee
        ) {
            require_auth(account);
            //begin new fees, logic for Mandatory fees.
            const uint128_t endpoint_hash = fioio::string_to_uint128_hash(end_point.c_str());

            auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();

            auto fee_iter = fees_by_endpoint.find(endpoint_hash);
            //if the fee isnt found for the endpoint, then 400 error.
            fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", "register_producer",
                           "FIO fee not found for endpoint", ErrorNoEndpoint);

            const uint64_t reg_amount = fee_iter->suf_amount;
            const uint64_t fee_type = fee_iter->type;

            //if its not a mandatory fee then this is an error.
            fio_400_assert(fee_type == 0, "fee_type", to_string(fee_type),
                           "register_producer unexpected fee type for endpoint register_producer, expected 0",
                           ErrorNoEndpoint);

            fio_400_assert(max_fee >= (int64_t)reg_amount, "max_fee", to_string(max_fee), "Fee exceeds supplied maximum.",
                           ErrorMaxFeeExceeded);

            fio_fees(account, asset(reg_amount, FIOSYMBOL));
            processrewardsnotpid(reg_amount, get_self());

            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

        }

        // @abi action
        [[eosio::action]]
        void bytemandfee(
                const string &end_point,
                const name &account,
                const int64_t &max_fee,
                const int64_t &bytesize
        ) {

            require_auth(account);
            //begin new fees, logic for Mandatory fees.
            const uint128_t endpoint_hash = fioio::string_to_uint128_hash(end_point.c_str());

            auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();

            auto fee_iter = fees_by_endpoint.find(endpoint_hash);
            //if the fee isnt found for the endpoint, then 400 error.
            fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", "register_producer",
                           "FIO fee not found for endpoint", ErrorNoEndpoint);

            uint64_t reg_amount = fee_iter->suf_amount;
            uint64_t remv = bytesize % 1000;
            uint64_t divv = bytesize / 1000;
            if (remv > 0 ){
                divv ++;
            }

            reg_amount = divv * reg_amount;

            const uint64_t fee_type = fee_iter->type;

            //if its not a mandatory fee then this is an error.
            fio_400_assert(fee_type == 0, "fee_type", to_string(fee_type),
                           "register_producer unexpected fee type for endpoint register_producer, expected 0",
                           ErrorNoEndpoint);

            fio_400_assert(max_fee >= (int64_t)reg_amount, "max_fee", to_string(max_fee), "Fee exceeds supplied maximum.",
                           ErrorMaxFeeExceeded);

            fio_fees(account, asset(reg_amount, FIOSYMBOL));
            processrewardsnotpid(reg_amount, get_self());
            //end new fees, logic for Mandatory fees.
        }

        /*******
         * This action will create a new fee on the FIO protocol.
         * @param end_point  this is the api endpoint name associated with the fee
         * @param type consult feetype, mandatory is 0 bundle eligible is 1
         * @param suf_amount this is the fee amount in FIO SUFs
         */
        // @abi action
        [[eosio::action]]
        void createfee(
                string end_point,
                int64_t type,
                int64_t suf_amount
        ) {
            require_auth(_self);

            fio_400_assert(suf_amount >= 0, "suf_amount", to_string(suf_amount),
                           " invalid suf amount",
                           ErrorFeeInvalid);

            const uint128_t endPointHash = string_to_uint128_hash(end_point.c_str());
            const uint64_t fee_id = fiofees.available_primary_key();

            auto feesbyendpoint = fiofees.get_index<"byendpoint"_n>();
            auto fees_iter = feesbyendpoint.find(endPointHash);

            if (fees_iter != feesbyendpoint.end())
            {
                    feesbyendpoint.modify(fees_iter, _self, [&](struct fiofee &a) {
                        a.type = type;
                        a.suf_amount = suf_amount;
                        //leave votes_pending as is, if votes are pending they need processed.
                    });
            } else {
                fiofees.emplace(get_self(), [&](struct fiofee &f) {
                    f.fee_id = fee_id;
                    f.end_point = end_point;
                    f.end_point_hash = endPointHash;
                    f.type = type;
                    f.suf_amount = suf_amount;
                    f.votes_pending.emplace(false);
                });
            }
            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

        }
    };

    EOSIO_DISPATCH(FioFee, (setfeevote)(bundlevote)(setfeemult)(updatefees)
                           (mandatoryfee)(bytemandfee)(createfee)
    )
}
