/*
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

#include <common/storage.h>
#include "ledger_manager.h"

namespace bumo{

	//int64_t Environment::time_ = 0;

	Environment::Environment(Environment* parent){
		parent_ = parent;
		if (parent_){
			for (auto it = parent_->entries_.begin(); it != parent_->entries_.end(); it++){
				entries_[it->first] = std::make_shared<AccountFrm>(it->second);
			}
		}
	}

	bool Environment::GetEntry(const std::string &key, AccountFrm::pointer &frm){
		if (entries_.find(key) == entries_.end()){
			if (AccountFromDB(key, frm)){
				entries_[key] = frm;
				return true;
			}
			else{
				return false;
			}
		}
		else{
			frm = entries_[key];
			return true;
		}
	}

	void Environment::Commit(){
		parent_->entries_ = entries_;
	}

	bool Environment::AddEntry(const std::string& key, AccountFrm::pointer frm){
		entries_[key] = frm;
		return true;
	}

	bool Environment::AccountFromDB(const std::string &address, AccountFrm::pointer &account_ptr){

		auto db = Storage::Instance().account_db();
        std::string index = DecodeAddress(address);
		std::string buff;
		if (!LedgerManager::Instance().tree_->Get(index, buff)){
			return false;
		}

		protocol::Account account;
		if (!account.ParseFromString(buff)){
			PROCESS_EXIT("fatal error, account(%s) ParseFromString failed", address.c_str());
		}
		account_ptr = std::make_shared<AccountFrm>(account);
		return true;
	}
}