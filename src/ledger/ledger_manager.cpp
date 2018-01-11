﻿/*
	bumo is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	bumo is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with bumo.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <overlay/peer_manager.h>
#include <glue/glue_manager.h>
#include <api/websocket_server.h>
#include <monitor/monitor_manager.h>
#include "ledger_manager.h"
#include "contract_manager.h"

namespace bumo {
	LedgerManager::LedgerManager() : tree_(NULL) {
		check_interval_ = 500 * utils::MICRO_UNITS_PER_MILLI;
		timer_name_ = "Ledger Mananger";
	}

	LedgerManager::~LedgerManager() {
		if (tree_) {
			delete tree_;
			tree_ = NULL;
		}
	}

	bool LedgerManager::GetValidators(int64_t seq, protocol::ValidatorSet& validators_set) {
		LedgerFrm frm;
		if (!frm.LoadFromDb(seq)) {
			return false;
		}

		std::string hashhex = utils::String::BinToHexString(frm.GetProtoHeader().validators_hash());
		std::string key = utils::String::Format("validators-%s", hashhex.c_str());
		auto db = Storage::Instance().account_db();
		std::string str;
		if (!db->Get(key, str)) {
			return false;
		}
		return validators_set.ParseFromString(str);
	}

	bool LedgerManager::Initialize() {
		HashWrapper::SetLedgerHashType(Configure::Instance().ledger_configure_.hash_type_);

		tree_ = new KVTrie();
		auto batch = std::make_shared<WRITE_BATCH>();
		tree_->Init(Storage::Instance().account_db(), batch, General::ACCOUNT_PREFIX, 4);

		context_manager_.Initialize();

		auto kvdb = Storage::Instance().account_db();
		std::string str_max_seq;
		int64_t seq_kvdb = 0;
		if (kvdb->Get(General::KEY_LEDGER_SEQ, str_max_seq)) {
			seq_kvdb = utils::String::Stoi64(str_max_seq);
			int64_t seq_rational = GetMaxLedger();
			if (seq_kvdb != seq_rational) {
				LOG_ERROR("fatal error:ledger_seq from kvdb(" FMT_I64 ") != ledger_seq from rational db(" FMT_I64 ")",
					seq_kvdb, seq_rational);
			}

			LOG_INFO("max closed ledger seq=" FMT_I64, seq_rational);
			last_closed_ledger_ = std::make_shared<LedgerFrm>();
			if (!last_closed_ledger_->LoadFromDb(seq_rational)) {
				return false;
			}
		}
		else {
			if (!CreateGenesisAccount()) {
				LOG_ERROR("Create genesis account failed");
				return false;
			}
			seq_kvdb = 1;
		}

		std::string str;
		if (kvdb->Get(General::STATISTICS, str)) {
			statistics_.fromString(str);
		}
		//avoid dead lock
		utils::WriteLockGuard guard(lcl_header_mutex_);
		lcl_header_ = last_closed_ledger_->GetProtoHeader();

		tree_->UpdateHash();
		const protocol::LedgerHeader& lclheader = last_closed_ledger_->GetProtoHeader();
		std::string validators_hash = lclheader.validators_hash();
		if (!ValidatorsGet(validators_hash, validators_)) {
			LOG_ERROR("Get validators failed!");
			return false;
		}

		//fee
		std::string fees_hash = lclheader.fees_hash();
		if (!FeesConfigGet(fees_hash, fees_)) {
			LOG_ERROR("Get config fee failed!");
			return false;
		}
	
		LOG_INFO("Byte fee :" FMT_I64 " Base reserver fee:" FMT_I64 " Pay fee:" FMT_I64 " .", fees_.byte_fee(), fees_.base_reserve(), fees_.pay_fee());

		//load proof
		Storage::Instance().account_db()->Get(General::LAST_PROOF, proof_);

		//update consensus configure
		Global::Instance().GetIoService().post([this]() {
			GlueManager::Instance().UpdateValidators(validators_, proof_);
		});

		std::string account_tree_hash = lclheader.account_tree_hash();
		if (account_tree_hash != tree_->GetRootHash()) {
			LOG_ERROR("ledger account_tree_hash(%s)!=account_root_hash(%s)",
				utils::String::Bin4ToHexString(account_tree_hash).c_str(),
				utils::String::Bin4ToHexString(tree_->GetRootHash()).c_str());
			return false;
		}

		if (lclheader.version() > General::LEDGER_VERSION) {
			PROCESS_EXIT("consensus ledger version:%d,software ledger version:%d", lclheader.version(), General::LEDGER_VERSION);
		}

		ExprCondition::RegisterFunctions();
		TimerNotify::RegisterModule(this);
		StatusModule::RegisterModule(this);
		return true;
	}

	bool LedgerManager::Exit() {
		LOG_INFO("Ledger manager stoping...");

		if (tree_) {
			delete tree_;
			tree_ = NULL;
		}
		LOG_INFO("Ledger manager stop [OK]");
		return true;
	}

	int LedgerManager::GetAccountNum() {
		utils::MutexGuard guard(gmutex_);
		return statistics_["account_count"].asInt();
	}

	void LedgerManager::OnTimer(int64_t current_time) {
		int64_t next_seq = 0;
		std::set<int64_t> active_peers = PeerManager::Instance().ConsensusNetwork().GetActivePeerIds();
		std::set<int64_t> enable_peers;
		protocol::GetLedgers gl;

		do {
			utils::MutexGuard guard(gmutex_);
			if (current_time - sync_.update_time_ <= 30 * 1000000) {
				return;
			}

			for (auto it = sync_.peers_.begin(); it != sync_.peers_.end();) {
				int64_t pid = it->first;
				if (active_peers.find(pid) == active_peers.end()) {
					it = sync_.peers_.erase(it);
				}
				else {
					it++;
				}
			}

			next_seq = last_closed_ledger_->GetProtoHeader().seq() + 1;

			sync_.update_time_ = current_time;
			LOG_INFO("OnTimer. request max ledger seq from neighbours. BEGIN");

			gl.set_begin(next_seq);
			gl.set_end(next_seq);
			gl.set_timestamp(current_time);

			for (std::set<int64_t>::iterator it = active_peers.begin(); it != active_peers.end(); it++) {
				int64_t pid = *it;
				auto iter = sync_.peers_.find(pid);
				if (iter != sync_.peers_.end()) {
					SyncStat& st = iter->second;
					if (current_time - st.send_time_ > 30 * utils::MICRO_UNITS_PER_SEC) {
						st.send_time_ = 0;
					}
					if (st.probation_ > current_time) {
						continue;
					}
					if (st.send_time_ != 0) {
						continue;
					}
				}
				enable_peers.insert(pid);
			}
			LOG_INFO("OnTimer. request max ledger seq from neighbours. END");
		} while (false);

		for (auto it = enable_peers.begin(); it != enable_peers.end(); it++)
			RequestConsensusValues(*it, gl, current_time);
	}

	void LedgerManager::OnSlowTimer(int64_t current_time) {

	}

	protocol::LedgerHeader LedgerManager::GetLastClosedLedger() {
		utils::ReadLockGuard guard(lcl_header_mutex_);
		return lcl_header_;
	}

	void LedgerManager::ValidatorsSet(std::shared_ptr<WRITE_BATCH> batch, const protocol::ValidatorSet& validators) {
		//should be recode ?
		std::string hash = HashWrapper::Crypto(validators.SerializeAsString());
		batch->Put(utils::String::Format("validators-%s", utils::String::BinToHexString(hash).c_str()), validators.SerializeAsString());
	}

	bool LedgerManager::ValidatorsGet(const std::string& hash, protocol::ValidatorSet& vlidators_set) {
		std::string key = utils::String::Format("validators-%s", utils::String::BinToHexString(hash).c_str());
		auto db = Storage::Instance().account_db();
		std::string str;
		if (!db->Get(key, str)) {
			return false;
		}
		return vlidators_set.ParseFromString(str);
	}
	void LedgerManager::UpdateValidatorset(const std::set<std::string>& newSet)
	{
		validators_.clear_validators();

		for (auto validator : newSet)
			validators_.add_validators(validator);
	}
	
	void LedgerManager::FeesConfigSet(std::shared_ptr<WRITE_BATCH> batch, const protocol::FeeConfig &fee) {
		std::string hash = HashWrapper::Crypto(fee.SerializeAsString());
		batch->Put(utils::String::Format("fees-%s", utils::String::BinToHexString(hash).c_str()), fee.SerializeAsString());
	}

	bool LedgerManager::FeesConfigGet(const std::string& hash, protocol::FeeConfig &fee) {
		std::string key = utils::String::Format("fees-%s", utils::String::BinToHexString(hash).c_str());
		auto db = Storage::Instance().account_db();
		std::string str;
		if (!db->Get(key, str)) {
			return false;
		}
		return fee.ParseFromString(str);
	}

	bool LedgerManager::CreateGenesisAccount() {
		LOG_INFO("There is no ledger exist,then create a init ledger");
		//set global hash caculate
		statistics_["account_count"] = 1;
		protocol::Account acc;
		acc.set_address(Configure::Instance().ledger_configure_.genesis_account_);
		acc.set_nonce(0);
		acc.set_balance(100000000000000000);

		AccountFrm::pointer acc_frm = std::make_shared<AccountFrm>(acc);
		acc_frm->SetProtoMasterWeight(1);
		acc_frm->SetProtoTxThreshold(1);

        tree_->Set(DecodeAddress(acc.address()), acc_frm->Serializer());
		tree_->UpdateHash();
		protocol::Ledger ledger;
		protocol::LedgerHeader *header = ledger.mutable_header();
		protocol::ConsensusValue request;
		request.mutable_ledger_upgrade()->set_new_ledger_version(1000);
		request.set_close_time(0);

		header->set_previous_hash("");
		header->set_seq(1);
		header->set_close_time(0);
		header->set_consensus_value_hash(HashWrapper::Crypto(request.SerializeAsString()));

		header->set_version(1000);
		header->set_tx_count(0);
		header->set_account_tree_hash(tree_->GetRootHash());

		//for validators
		const utils::StringList &list = Configure::Instance().validation_configure_.validators_;
		for (utils::StringList::const_iterator iter = list.begin(); iter != list.end(); iter++) {
			validators_.add_validators(*iter);
		}
		std::string validators_hash = HashWrapper::Crypto(validators_.SerializeAsString());
		header->set_validators_hash(validators_hash);

		//for fee
		fees_.set_byte_fee(Configure::Instance().ledger_configure_.fees_.byte_fee_);
		fees_.set_base_reserve(Configure::Instance().ledger_configure_.fees_.base_reserve_);
		fees_.set_create_account_fee(Configure::Instance().ledger_configure_.fees_.create_account_fee_);
		fees_.set_pay_fee(Configure::Instance().ledger_configure_.fees_.pay_fee_);
		fees_.set_issue_asset_fee(Configure::Instance().ledger_configure_.fees_.issue_asset_fee_);
		fees_.set_set_metadata_fee(Configure::Instance().ledger_configure_.fees_.set_metadata_fee_);
		fees_.set_set_sigure_weight_fee(Configure::Instance().ledger_configure_.fees_.set_sigure_weight_fee_);
		fees_.set_set_threshold_fee(Configure::Instance().ledger_configure_.fees_.set_threshold_fee_);
		fees_.set_pay_coin_fee(Configure::Instance().ledger_configure_.fees_.pay_coin_fee_);
		std::string fees_hash = HashWrapper::Crypto(fees_.SerializeAsString());
		header->set_fees_hash(HashWrapper::Crypto(fees_.SerializeAsString()));

		header->set_hash("");
		header->set_hash(HashWrapper::Crypto(ledger.SerializeAsString()));

		last_closed_ledger_ = std::make_shared<LedgerFrm>();
		last_closed_ledger_->ProtoLedger().CopyFrom(ledger);
		auto batch = tree_->batch_;
		batch->Put(bumo::General::KEY_LEDGER_SEQ, "1");
		batch->Put(bumo::General::KEY_GENE_ACCOUNT, Configure::Instance().ledger_configure_.genesis_account_);
		ValidatorsSet(batch, validators_);
		FeesConfigSet(batch, fees_);

		WRITE_BATCH batch_ledger;
		if (!last_closed_ledger_->AddToDb(batch_ledger)) {
			PROCESS_EXIT("AddToDb failed");
		}

		batch->Put(General::STATISTICS, statistics_.toFastString());
		if (!Storage::Instance().account_db()->WriteBatch(*batch)) {
			PROCESS_EXIT("Write account batch failed, %s", Storage::Instance().account_db()->error_desc().c_str());
		}

		return true;
	}

	//warn
	void LedgerManager::CreateHardforkLedger() {
		LOG_INFO("Are you sure to create hardfork ledger? Press y to continue.");
		char ch;
		std::cin >> ch;
		if (ch != 'y') {
			LOG_INFO("Do nothing.");
			return;
		}

		//get last ledger
		do {
			HashWrapper::SetLedgerHashType(Configure::Instance().ledger_configure_.hash_type_);
			KeyValueDb *account_db = Storage::Instance().account_db();

			//load max seq
			std::string str_max_seq;
			account_db->Get(General::KEY_LEDGER_SEQ, str_max_seq);
			int64_t seq_kvdb = utils::String::Stoi64(str_max_seq);
			LOG_INFO("Max closed ledger seq=" FMT_I64, seq_kvdb);

			//load ledger from db
			protocol::LedgerHeader last_closed_ledger_hdr;
			bumo::KeyValueDb *ledger_db = bumo::Storage::Instance().ledger_db();
			std::string ledger_header;
			if (ledger_db->Get(ComposePrefix(General::LEDGER_PREFIX, seq_kvdb), ledger_header) <= 0) {
				LOG_ERROR("Load ledger from db failed, error(%s)", ledger_db->error_desc().c_str());
				break;
			}
			if (!last_closed_ledger_hdr.ParseFromString(ledger_header)) {
				LOG_ERROR("Parse last closed ledger failed");
				break;
			}

			//load validators
			protocol::ValidatorSet validator_set;
			ValidatorsGet(last_closed_ledger_hdr.validators_hash(), validator_set);

			//load proof
			std::string str_proof;
			account_db->Get(General::LAST_PROOF, str_proof);

			//this validator 
			PrivateKey private_key(Configure::Instance().validation_configure_.node_privatekey_);
            std::string this_node_address = private_key.GetEncAddress();

			//compose the new ledger
			protocol::Ledger ledger;
			protocol::LedgerHeader *header = ledger.mutable_header();
			protocol::ConsensusValue request;
			protocol::LedgerUpgrade *ledger_upgrade = request.mutable_ledger_upgrade();
			ledger_upgrade->set_new_validator(this_node_address);
			//for validators
			protocol::ValidatorSet new_validator_set;
			new_validator_set.add_validators(this_node_address);

			request.set_previous_ledger_hash(last_closed_ledger_hdr.hash());
			request.set_close_time(last_closed_ledger_hdr.close_time() + Configure::Instance().validation_configure_.close_interval_);
			request.set_ledger_seq(last_closed_ledger_hdr.seq() + 1);
			request.set_previous_proof(str_proof);
			std::string consensus_value_hash = HashWrapper::Crypto(request.SerializeAsString());

			header->set_previous_hash(last_closed_ledger_hdr.hash());
			header->set_seq(request.ledger_seq());
			header->set_close_time(request.close_time());
			header->set_consensus_value_hash(consensus_value_hash);
			header->set_version(last_closed_ledger_hdr.version());
			header->set_tx_count(last_closed_ledger_hdr.tx_count());
			header->set_account_tree_hash(last_closed_ledger_hdr.account_tree_hash());

			std::string validators_hash = HashWrapper::Crypto(new_validator_set.SerializeAsString());
			header->set_validators_hash(validators_hash);

			header->set_hash("");
			header->set_hash(HashWrapper::Crypto(ledger.SerializeAsString()));

			std::shared_ptr<WRITE_BATCH> batch = std::make_shared<WRITE_BATCH>();
			batch->Put(bumo::General::KEY_LEDGER_SEQ, utils::String::ToString(header->seq()));
			batch->Put(bumo::General::LAST_PROOF, "");

			ValidatorsSet(batch, new_validator_set);

			//write ledger db
			WRITE_BATCH batch_ledger;
			batch_ledger.Put(bumo::General::KEY_LEDGER_SEQ, utils::String::ToString(header->seq()));
			batch_ledger.Put(ComposePrefix(General::LEDGER_PREFIX, header->seq()), header->SerializeAsString());
			batch_ledger.Put(ComposePrefix(General::CONSENSUS_VALUE_PREFIX, header->seq()), request.SerializeAsString());
			if (!ledger_db->WriteBatch(batch_ledger)) {
				PROCESS_EXIT("Write ledger and transaction failed(%s)", ledger_db->error_desc().c_str());
			}

			//write acount db
			if (!Storage::Instance().account_db()->WriteBatch(*batch)) {
				PROCESS_EXIT("Write account batch failed, %s", Storage::Instance().account_db()->error_desc().c_str());
			}

			LOG_INFO("Create hard fork ledger successful, seq(" FMT_I64 "), consensus value hash(%s)",
				header->seq(),
				utils::String::BinToHexString(header->consensus_value_hash()).c_str());

		} while (false);
	}


	bool LedgerManager::ConsensusValueFromDB(int64_t seq, protocol::ConsensusValue& consensus_value) {
		KeyValueDb *ledger_db = Storage::Instance().ledger_db();
		std::string str_cons;
		if (ledger_db->Get(ComposePrefix(General::CONSENSUS_VALUE_PREFIX, seq), str_cons) <= 0) {
			return false;
		}

		return consensus_value.ParseFromString(str_cons);
	}

	int LedgerManager::OnConsent(const protocol::ConsensusValue &consensus_value, const std::string& proof) {
		LOG_INFO("OnConsent Ledger consensus_value seq(" FMT_I64 ")", consensus_value.ledger_seq());

		utils::MutexGuard guard(gmutex_);
		if (last_closed_ledger_->GetProtoHeader().seq() >= consensus_value.ledger_seq()) {
			LOG_ERROR("received duplicated consensus, max closed ledger seq(" FMT_I64 ")>= received request(" FMT_I64 ")",
				last_closed_ledger_->GetProtoHeader().seq(),
				consensus_value.ledger_seq());
			return 1;
		}

		if (last_closed_ledger_->GetProtoHeader().seq() + 1 == consensus_value.ledger_seq()) {
			sync_.update_time_ = utils::Timestamp::HighResolution();
			CloseLedger(consensus_value, proof);
		}
		return 0;
	}

	int64_t LedgerManager::GetMaxLedger() {
		KeyValueDb *ledger_db = Storage::Instance().ledger_db();
		std::string str_value;
		int32_t ret = ledger_db->Get(General::KEY_LEDGER_SEQ, str_value);
		if (ret < 0) {
			PROCESS_EXIT_ERRNO("Get max ledger seq failed, error desc(%s)", ledger_db->error_desc().c_str());
		}
		else if (ret == 0) {
			return 0;
		}

		return utils::String::Stoi64(str_value);
	}

	void LedgerManager::GetModuleStatus(Json::Value &data) {
		utils::MutexGuard guard(gmutex_);
		int64_t begin_time = utils::Timestamp::HighResolution();
		data["name"] = "ledger_manager";
		data["tx_count"] = GetLastClosedLedger().tx_count();
		data["account_count"] = GetAccountNum();
		data["ledger_sequence"] = GetLastClosedLedger().seq();
		data["time"] = utils::String::Format(FMT_I64 " ms",
			(utils::Timestamp::HighResolution() - begin_time) / utils::MICRO_UNITS_PER_MILLI);
		data["hash_type"] = HashWrapper::GetLedgerHashType() == HashWrapper::HASH_TYPE_SM3 ? "sm3" : "sha256";
		data["sync"] = sync_.ToJson();
		context_manager_.GetModuleStatus(data["ledger_context"]);
	}


	bool LedgerManager::CloseLedger(const protocol::ConsensusValue& consensus_value, const std::string& proof) {
	//	LOG_TRACE("Closing ledger, check value");
		if (!GlueManager::Instance().CheckValueAndProof(consensus_value.SerializeAsString(), proof)) {

			protocol::PbftProof proof_proto;
			proof_proto.ParseFromString(proof);

			LOG_ERROR("CheckValueAndProof failed:%s\nproof=%s",
				Proto2Json(consensus_value).toStyledString().c_str(),
				Proto2Json(proof_proto).toStyledString().c_str());

			return false;
		}

//		LOG_TRACE("Closing ledger, check complete");
		std::string con_str = consensus_value.SerializeAsString();
		std::string chash = HashWrapper::Crypto(con_str);
		LedgerFrm::pointer closing_ledger = context_manager_.SyncProcess(consensus_value);

		protocol::Ledger& ledger = closing_ledger->ProtoLedger();
		auto header = ledger.mutable_header();
		header->set_seq(consensus_value.ledger_seq());
		header->set_close_time(consensus_value.close_time());
		header->set_previous_hash(consensus_value.previous_ledger_hash());
		header->set_consensus_value_hash(chash);
		//LOG_INFO("set_consensus_value_hash:%s,%s", utils::String::BinToHexString(con_str).c_str(), utils::String::BinToHexString(chash).c_str());
		header->set_version(last_closed_ledger_->GetProtoHeader().version());

		int64_t time0 = utils::Timestamp().HighResolution();
		int64_t new_count = 0, change_count = 0;
		closing_ledger->Commit(tree_, new_count, change_count);
		statistics_["account_count"] = statistics_["account_count"].asInt64() + new_count;

		int64_t time1 = utils::Timestamp().HighResolution();

		tree_->UpdateHash();
		int64_t time2 = utils::Timestamp().HighResolution();

		header->set_account_tree_hash(tree_->GetRootHash());
		header->set_tx_count(last_closed_ledger_->GetProtoHeader().tx_count() + closing_ledger->ProtoLedger().transaction_envs_size());

		protocol::ValidatorSet new_set = validators_;
		bool has_upgrade = consensus_value.has_ledger_upgrade();
		if (has_upgrade) {
			const protocol::LedgerUpgrade &ledger_upgrade = consensus_value.ledger_upgrade();

			//for ledger version
			if (ledger_upgrade.new_ledger_version() > 0) {
				header->set_version(ledger_upgrade.new_ledger_version());
			}

			//for hardfork new validator
			if (ledger_upgrade.new_validator().size() > 0) {
				new_set.Clear();
				new_set.add_validators(ledger_upgrade.new_validator());
			} 
		}
		std::string validators_hash = HashWrapper::Crypto(new_set.SerializeAsString());
		header->set_validators_hash(validators_hash);//TODO
		header->set_tx_count(last_closed_ledger_->GetProtoHeader().tx_count() + closing_ledger->ProtoLedger().transaction_envs_size());
		header->set_hash("");
		header->set_hash(HashWrapper::Crypto(closing_ledger->ProtoLedger().SerializeAsString()));

		//LOG_INFO("%s", Proto2Json(context->closing_ledger_->GetProtoHeader()).toStyledString().c_str());

		///////////////////////////////////////////////////////////////////////////////////////////////

		int64_t ledger_seq = closing_ledger->GetProtoHeader().seq();
		std::shared_ptr<WRITE_BATCH> account_db_batch = tree_->batch_;
		account_db_batch->Put(bumo::General::KEY_LEDGER_SEQ, utils::String::Format(FMT_I64, ledger_seq));
		ValidatorsSet(account_db_batch, new_set);
		validators_ = new_set;

		//fee
		protocol::FeeConfig new_fees;
		if (closing_ledger->GetVotedFee(new_fees)) {
			FeesConfigSet(account_db_batch, new_fees);
			fees_ = new_fees;
		}
		header->set_fees_hash(HashWrapper::Crypto(fees_.SerializeAsString()));

		//proof
		account_db_batch->Put(bumo::General::LAST_PROOF, proof);
		account_db_batch->Put(bumo::General::STATISTICS, statistics_.toFastString());
		proof_ = proof;

		//consensus value
		WRITE_BATCH ledger_db_batch;
		ledger_db_batch.Put(ComposePrefix(General::CONSENSUS_VALUE_PREFIX, consensus_value.ledger_seq()), consensus_value.SerializeAsString());

		if (!closing_ledger->AddToDb(ledger_db_batch)) {
			PROCESS_EXIT("AddToDb failed");
		}

		if (!Storage::Instance().account_db()->WriteBatch(*account_db_batch)) {
			PROCESS_EXIT("Write batch failed: %s", Storage::Instance().account_db()->error_desc().c_str());
		}
		///////////////////////////////////////////////////////////////////////////////////////////////

		last_closed_ledger_ = closing_ledger;

		//avoid dead lock
		protocol::LedgerHeader tmp_lcl_header;
		do {
			utils::WriteLockGuard guard(lcl_header_mutex_);
			tmp_lcl_header = lcl_header_ = last_closed_ledger_->GetProtoHeader();
		} while (false);

		Global::Instance().GetIoService().post([new_set, proof, consensus_value, has_upgrade]() { //avoid deadlock
			GlueManager::Instance().UpdateValidators(new_set, proof);
			if (has_upgrade) GlueManager::Instance().LedgerHasUpgrade();
		});

		context_manager_.RemoveCompleted(tmp_lcl_header.seq());

		////////////////////////////

		int64_t time3 = utils::Timestamp().HighResolution();
		tree_->batch_ = std::make_shared<WRITE_BATCH>();
		tree_->FreeMemory(4);
		LOG_INFO("ledger(" FMT_I64 ") closed txcount(" FMT_SIZE ") hash(%s) apply="  FMT_I64_EX(-8) " calc_hash="  FMT_I64_EX(-8) " addtodb=" FMT_I64_EX(-8)
			" total=" FMT_I64_EX(-8) " LoadValue=" FMT_I64 " tsize=" FMT_SIZE,
			closing_ledger->GetProtoHeader().seq(),
			closing_ledger->GetTxOpeCount(),
			utils::String::Bin4ToHexString(closing_ledger->GetProtoHeader().hash()).c_str(),
			time1 - time0 + closing_ledger->apply_time_,
			time2 - time1,
			time3 - time2,
			time3 - time0 + closing_ledger->apply_time_,
			tree_->time_,
			closing_ledger->GetTxCount());

		//notice ledger closed
		WebSocketServer::Instance().BroadcastMsg(protocol::CHAIN_LEDGER_HEADER, tmp_lcl_header.SerializeAsString());

		// notice applied
		for (size_t i = 0; i < closing_ledger->apply_tx_frms_.size(); i++) {
			TransactionFrm::pointer tx = closing_ledger->apply_tx_frms_[i];
			WebSocketServer::Instance().BroadcastChainTxMsg(tx->GetContentHash(), tx->GetSourceAddress(),
				tx->GetResult(), tx->GetResult().code() == protocol::ERRCODE_SUCCESS ? protocol::ChainTxStatus_TxStatus_COMPLETE : protocol::ChainTxStatus_TxStatus_FAILURE);
		}
		// notice dropped
		for (size_t i = 0; i < closing_ledger->dropped_tx_frms_.size(); i++) {
			TransactionFrm::pointer tx = closing_ledger->dropped_tx_frms_[i];
			WebSocketServer::Instance().BroadcastChainTxMsg(tx->GetContentHash(), tx->GetSourceAddress(),
				tx->GetResult(), tx->GetResult().code() == protocol::ERRCODE_SUCCESS ? protocol::ChainTxStatus_TxStatus_COMPLETE : protocol::ChainTxStatus_TxStatus_FAILURE);
		}

		// monitor
		monitor::LedgerStatus ledger_status;
		ledger_status.mutable_ledger_header()->CopyFrom(tmp_lcl_header);
		ledger_status.set_transaction_size(GlueManager::Instance().GetTransactionCacheSize());
		ledger_status.set_account_count(GetAccountNum());
		ledger_status.set_timestamp(utils::Timestamp::HighResolution());
		MonitorManager::Instance().SendMonitor(monitor::MONITOR_MSGTYPE_LEDGER, ledger_status.SerializeAsString());
		return true;
	}


	void LedgerManager::OnRequestLedgers(const protocol::GetLedgers &message, int64_t peer_id) {
		bool ret = true;
		protocol::Ledgers ledgers;

		do {
			utils::MutexGuard guard(gmutex_);
			LOG_INFO("OnRequestLedgers pid(" FMT_I64 "),[" FMT_I64 ", " FMT_I64 "]", peer_id, message.begin(), message.end());
			if (message.end() - message.begin() + 1 > 5) {
				LOG_ERROR("Only 5 blocks can be requested at a time while try to (" FMT_I64 ")", message.end() - message.begin());
				return;
			}

			if (message.end() - message.begin() < 0) {
				LOG_ERROR("begin is bigger than end [" FMT_I64 "," FMT_I64 "]", message.begin(), message.end());
				return;
			}

			if (last_closed_ledger_->GetProtoHeader().seq() < message.end()) {
				LOG_INFO("peer(" FMT_I64 ") request [" FMT_I64 "," FMT_I64 "] while the max consensus_value is (" FMT_I64 ")",
					peer_id, message.begin(), message.end(), last_closed_ledger_->GetProtoHeader().seq());
				return;
			}


			ledgers.set_max_seq(last_closed_ledger_->GetProtoHeader().seq());

			for (int64_t i = message.begin(); i <= message.end(); i++) {
				protocol::ConsensusValue item;
				if (!ConsensusValueFromDB(i, item)) {
					ret = false;
					LOG_ERROR("ConsensusValueFromDB failed seq=" FMT_I64, i);
					break;
				}
				ledgers.add_values()->CopyFrom(item);
			}

			int64_t seq = message.end();
			protocol::ConsensusValue next;

			if (seq == last_closed_ledger_->GetProtoHeader().seq())
				ledgers.set_proof(proof_);
			else if (ConsensusValueFromDB(seq + 1, next))
				ledgers.set_proof(next.previous_proof());
			else {
				LOG_ERROR("");
			}
		} while (false);
		if (ret) {
			bumo::WsMessagePointer ws = std::make_shared<protocol::WsMessage>();
			ws->set_data(ledgers.SerializeAsString());
			ws->set_type(protocol::OVERLAY_MSGTYPE_LEDGERS);
			ws->set_request(false);
			LOG_INFO("Send ledgers[" FMT_I64 "," FMT_I64 "] to(" FMT_I64 ")", message.begin(), message.end(), peer_id);
			PeerManager::Instance().ConsensusNetwork().SendMsgToPeer(peer_id, ws);
		}
	}

	void LedgerManager::OnReceiveLedgers(const protocol::Ledgers &ledgers, int64_t peer_id) {

		bool valid = false;
		int64_t next = 0;

		do {
			utils::MutexGuard guard(gmutex_);
			if (ledgers.values_size() == 0) {
				LOG_ERROR("received empty Ledgers from(" FMT_I64 ")", peer_id);
				break;
			}
			int64_t begin = ledgers.values(0).ledger_seq();
			int64_t end = ledgers.values(ledgers.values_size() - 1).ledger_seq();

			LOG_INFO("OnReceiveLedgers [" FMT_I64 "," FMT_I64 "] from peer(" FMT_I64 ")", begin, end, peer_id);

			if (sync_.peers_.find(peer_id) == sync_.peers_.end()) {
				LOG_ERROR("received unexpected ledgers [" FMT_I64 "," FMT_I64 "] from (" FMT_I64 ")",
					begin, end, peer_id);
				break;
			}

			auto& itm = sync_.peers_[peer_id];
			itm.send_time_ = 0;

			if (begin != itm.gl_.begin() || end != itm.gl_.end()) {
				LOG_ERROR("received unexpected ledgers[" FMT_I64 "," FMT_I64 "] while expect[" FMT_I64 "," FMT_I64 "]",
					begin, end, itm.gl_.begin(), itm.gl_.end());
				itm.probation_ = utils::Timestamp::HighResolution() + 60 * utils::MICRO_UNITS_PER_SEC;
				itm.gl_.set_begin(0);
				itm.gl_.set_end(0);
				break;
			}

			itm.gl_.set_begin(0);
			itm.gl_.set_end(0);

			for (int i = 0; i < ledgers.values_size(); i++) {
				const protocol::ConsensusValue& consensus_value = ledgers.values(i);
				std::string proof;
				if (i < ledgers.values_size() - 1) {
					proof = ledgers.values(i + 1).previous_proof();
				}
				else {
					proof = ledgers.proof();
				}
				if (consensus_value.ledger_seq() == last_closed_ledger_->GetProtoHeader().seq() + 1) {
					if (!CloseLedger(consensus_value, proof)) {
						valid = false;
						itm.probation_ = utils::Timestamp::HighResolution() + 60 * utils::MICRO_UNITS_PER_SEC;
						break;
					}
					valid = true;
				}
			}
			next = last_closed_ledger_->GetProtoHeader().seq() + 1;
		} while (false);


		if (valid) {
			int64_t current_time = utils::Timestamp::HighResolution();
			if (next <= ledgers.max_seq()) {
				protocol::GetLedgers gl;
				gl.set_begin(next);
				gl.set_end(MIN(ledgers.max_seq(), next + 4));
				gl.set_timestamp(current_time);
				RequestConsensusValues(peer_id, gl, current_time);
			}
		}
	}

	void LedgerManager::RequestConsensusValues(int64_t pid, protocol::GetLedgers& gl, int64_t time) {
		LOG_TRACE("RequestConsensusValues from peer(" FMT_I64 "), [" FMT_I64 "," FMT_I64 "]", pid, gl.begin(), gl.end());
		do {
			utils::MutexGuard guard(gmutex_);
			auto &peer = sync_.peers_[pid];
			peer.gl_.CopyFrom(gl);
			peer.send_time_ = time;
			sync_.update_time_ = time;
		} while (false);

		PeerManager::Instance().ConsensusNetwork().SendRequest(pid, protocol::OVERLAY_MSGTYPE_LEDGERS, gl.SerializeAsString());
	}

	ExprCondition::ExprCondition(const std::string & program, std::shared_ptr<Environment> env , const protocol::ConsensusValue &cons_value) :
		utils::ExprParser(program),
		environment_(env),
		cons_value_(cons_value){}
	ExprCondition::~ExprCondition() {}

	void ExprCondition::RegisterFunctions() {
		//utils::OneCommonArgumentFunctions["ledger"] = DoLedger;
		utils::OneCommonArgumentFunctions["account"] = DoAccount;
		utils::TwoCommonArgumentFunctions["jsonpath"] = DoJsonPath;
	}

	const utils::ExprValue ExprCondition::DoAccount(const utils::ExprValue &arg, utils::ExprParser *parser) {
		if (!arg.IsString()) {
			throw std::runtime_error("account's parameter is not a string");
		}

		std::shared_ptr<Environment> env = ((ExprCondition *)parser)->GetEnviroment();
		AccountFrm::pointer acc = NULL;
		Json::Value result;
		if (env && env->GetEntry(arg.String(), acc)) {
			acc->ToJson(result);
		}
		else if (Environment::AccountFromDB(arg.String(), acc)) {
			acc->ToJson(result);
		}
		else{
			throw std::runtime_error(utils::String::Format("Account(%s) not exist", arg.String().c_str()));
		}

		return result.toFastString();
	}

	const utils::ExprValue ExprCondition::DoLedger(const utils::ExprValue &arg, utils::ExprParser *parser) {
		int64_t ledger_seq;
		if (arg.IsNumber()) {
			ledger_seq = (int64_t)arg.Number();
		}
		else if (arg.IsInteger64()) {
			ledger_seq = arg.Integer64();
		}
		else if (arg.IsString()) {
			ledger_seq = utils::String::Stoi64(arg.String());
		}
		else {
			throw std::runtime_error(utils::String::Format("Ledger's parameter type(%s) is not supported", utils::ExprValue::GetTypeDesc(arg.type_).c_str()));
		}

		LedgerFrm frm;
		Json::Value result;
		if (!frm.LoadFromDb(ledger_seq)) {
			throw std::runtime_error(utils::String::Format("Ledger(seq:" FMT_I64 ") not exist", ledger_seq));
		}
		else {
			result = frm.ToJson();
		}

		return result.toFastString();
	}

	const utils::ExprValue ExprCondition::DoJsonPath(const utils::ExprValue &arg1, const utils::ExprValue &arg2, utils::ExprParser *parser) {
		if (!arg1.IsString() || !arg2.IsString()) {
			throw std::runtime_error("Json's parameter is not a string");
		}

		Json::Value from;
		if (!from.fromString(arg1.String())) {
			throw std::runtime_error("Json's arg1 can not be parsed");
		}

		Json::Value default_value(0);
		Json::Path path(arg2.String());
		Json::Value v = path.resolve(from, default_value);
		if (v.isNumeric()) {
			if (v.isDouble()) {
				return v.asDouble();
			}
			else {
				return v.asInt64();
			}
		}
		else if (v.isString()) {
			return v.asString();
		}
		else {
			throw std::runtime_error("Json's result type is not supported");
		}
	}

	Result ExprCondition::Eval(utils::ExprValue &value) {
		Result ret;
		try {
			if (cons_value_.ledger_seq() == 0) {
				symbols_["LEDGER_SEQ"] = LedgerManager::Instance().GetLastClosedLedger().seq();
				symbols_["LEDGER_TIME"] = (int64_t)LedgerManager::Instance().GetLastClosedLedger().close_time();
			}
			else {
				symbols_["LEDGER_SEQ"] = cons_value_.ledger_seq();
				symbols_["LEDGER_TIME"] = cons_value_.close_time();
			}
			value = Evaluate();
		}
		catch (std::exception & e) {
			ret.set_code(protocol::ERRCODE_INVALID_PARAMETER);
			ret.set_desc(e.what());
		}

		return ret;
	}

	Result ExprCondition::Parse(utils::ExprValue &value) {
		Result ret;
		try {
			symbols_["LEDGER_SEQ"] = utils::ExprValue(utils::ExprValue::UNSURE);
			symbols_["LEDGER_TIME"] = utils::ExprValue(utils::ExprValue::UNSURE);
			value = utils::ExprParser::Parse();
		}
		catch (std::exception & e) {
			ret.set_code(protocol::ERRCODE_INVALID_PARAMETER);
			ret.set_desc(e.what());
		}

		return ret;
	}

	std::shared_ptr<Environment> ExprCondition::GetEnviroment() {
		return environment_;
	}

	Result LedgerManager::DoTransaction(protocol::TransactionEnv& env, LedgerContext *ledger_context) {

		Result result;
		TransactionFrm::pointer back = ledger_context->transaction_stack_.back();
		std::shared_ptr<AccountFrm> source_account;
		back->environment_->GetEntry(env.transaction().source_address(), source_account);
		env.mutable_transaction()->set_nonce(source_account->GetAccountNonce() + 1);

		//auto header = std::make_shared<protocol::LedgerHeader>(LedgerManager::Instance().closing_ledger_->GetProtoHeader());
		auto header = std::make_shared<protocol::LedgerHeader>(ledger_context->closing_ledger_->GetProtoHeader());

		TransactionFrm::pointer txfrm = std::make_shared<bumo::TransactionFrm >(env);

		do {
			if (ledger_context->transaction_stack_.size() > General::CONTRACT_MAX_RECURSIVE_DEPTH) {
				txfrm->result_.set_code(protocol::ERRCODE_CONTRACT_TOO_MANY_RECURSION);
				break;
			}

			int64_t top_contract_id = ledger_context->GetTopContractId();
			if (top_contract_id > 0 ) {
				Contract *contract = ContractManager::Instance().GetContract(top_contract_id);
				contract->IncTxDoCount();
				if (contract->GetTxDoCount() > General::CONTRACT_TRANSACTION_LIMIT) {
					//txfrm->result_.set_code(protocol::ERRCODE_CONTRACT_TOO_MANY_TRANSACTIONS);
					//break;
					result.set_code(protocol::ERRCODE_CONTRACT_TOO_MANY_TRANSACTIONS);
					result.set_desc("Too many transaction");
					LOG_ERROR("Too many transaction called by transaction(hash:%s)", contract->GetParameter().sender_.c_str());
					return result;
				}
			}

			ledger_context->transaction_stack_.push_back(txfrm);
			txfrm->SetMaxEndTime(back->GetMaxEndTime());
			txfrm->NonceIncrease(ledger_context->closing_ledger_.get(), back->environment_);
			if (txfrm->ValidForParameter()) {
				txfrm->Apply(ledger_context->closing_ledger_.get(), back->environment_, true);
			}

			TransactionFrm::pointer bottom_tx = ledger_context->GetBottomTx();
			//throw the contract
			if (txfrm->GetResult().code() == protocol::ERRCODE_FEE_NOT_ENOUGH ||
				txfrm->GetResult().code() == protocol::ERRCODE_CONTRACT_TOO_MANY_TRANSACTIONS) {
				result = txfrm->GetResult();
				LOG_ERROR("%s", txfrm->GetResult().desc().c_str());
				return result;
			}
	
			//throw the contract
			bottom_tx->AddRealFee(txfrm->GetRealFee());
			if (bottom_tx->GetRealFee() > bottom_tx->GetFee()) {
				result.set_code(protocol::ERRCODE_FEE_NOT_ENOUGH);
				result.set_desc("Fee not enough");
				LOG_ERROR_ERRNO("Transaction(%s) Fee not enough", utils::String::BinToHexString(bottom_tx->GetContentHash()).c_str());
				return result;
			}

			protocol::TransactionEnvStore tx_store;
			tx_store.mutable_transaction_env()->CopyFrom(txfrm->GetProtoTxEnv());
			auto trigger = tx_store.mutable_transaction_env()->mutable_trigger();
			trigger->mutable_transaction()->set_hash(back->GetContentHash());
			trigger->mutable_transaction()->set_index(back->processing_operation_);


			if (txfrm->GetResult().code() == protocol::ERRCODE_SUCCESS) {
				back->instructions_.insert(back->instructions_.end(), txfrm->instructions_.begin(), txfrm->instructions_.end());
				txfrm->environment_->Commit();
			}
			tx_store.set_error_code(txfrm->GetResult().code());
			tx_store.set_error_desc(txfrm->GetResult().desc());
			back->instructions_.push_back(tx_store);
			ledger_context->transaction_stack_.pop_back();

			result.set_code(-1);
			return result;
		} while (false);

		//caculate byte fee
		back->AddRealFee(txfrm->GetRealFee());
		//
		protocol::TransactionEnvStore tx_store;
		tx_store.set_error_code(txfrm->GetResult().code());
		tx_store.set_error_desc(txfrm->GetResult().desc());
		tx_store.mutable_transaction_env()->CopyFrom(txfrm->GetProtoTxEnv());
		auto trigger = tx_store.mutable_transaction_env()->mutable_trigger();
		trigger->mutable_transaction()->set_hash(back->GetContentHash());
		trigger->mutable_transaction()->set_index(back->processing_operation_);
		back->instructions_.push_back(tx_store);
		
		result.set_code(-1);
		return result;
	}
}