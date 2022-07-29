#include "proxiedtransaction.h"
#include "client/itemimpl.h"
#include "client/synccororeindexerimpl.h"
#include "core/item.h"
#include "core/itemimpl.h"
#include "core/queryresults/queryresults.h"
#include "tools/clusterproxyloghelper.h"
#include "transactionimpl.h"

namespace reindexer {

Error ProxiedTransaction::Modify(Item &&item, ItemModifyMode mode, lsn_t lsn) {
	client::Item clientItem;
	bool itemFromCache = false;
	try {
		{
			std::unique_lock lck(mtx_);
			if (itemCache_.isValid) {
				itemFromCache = true;
				clientItem = client::Item(new client::ItemImpl<client::SyncCoroReindexerImpl>(itemCache_.pt, itemCache_.tm, nullptr,
																							  std::chrono::milliseconds()));
			} else {
				lck.unlock();
				clientItem = tx_.NewItem();
			}
		}
		if (!clientItem.Status().ok()) {
			throw clientItem.Status();
		}
		std::string_view serverCJson = item.impl_->GetCJSON(true);
		auto err = clientItem.Unsafe().FromCJSON(serverCJson);
		if (!err.ok()) {
			throw err;
		}
	} catch (Error &e) {
		if (!itemFromCache) {
			return e;
		}
		// Update cache, if got error on item convertion
		itemFromCache = false;
		clientItem = tx_.NewItem();
		if (!clientItem.Status().ok()) {
			return clientItem.Status();
		}
		std::string_view serverCJson = item.impl_->GetCJSON(true);
		auto err = clientItem.Unsafe().FromCJSON(serverCJson);
		if (!err.ok()) {
			return err;
		}
	}

	clientItem.SetPrecepts(item.impl_->GetPrecepts());

	if (clientItem.impl_->tagsMatcher().isUpdated()) {
		// Disable async logic for tm updates - next items may depend on this
		asyncData_.AwaitAsyncRequests();

		std::unique_lock lck(mtx_);
		itemCache_.isValid = false;
		lck.unlock();

		return tx_.modify(std::move(clientItem), mode, lsn, nullptr);
	}
	if (!itemFromCache) {
		std::lock_guard lck(mtx_);
		itemCache_.pt = clientItem.impl_->Type();
		itemCache_.tm = clientItem.impl_->tagsMatcher();
		itemCache_.isValid = true;
	}

	try {
		asyncData_.AddNewAsyncRequest();
	} catch (Error &err) {
		return err;
	}

	return tx_.modify(std::move(clientItem), mode, lsn, [this](const Error &e) { asyncData_.OnAsyncRequestDone(e); });
}

Error ProxiedTransaction::Modify(Query &&query, lsn_t lsn) {
	try {
		asyncData_.AddNewAsyncRequest();
	} catch (Error &err) {
		return err;
	}
	return tx_.modify(std::move(query), lsn, [this](const Error &e) { asyncData_.OnAsyncRequestDone(e); });
}

Error ProxiedTransaction::PutMeta(std::string_view key, std::string_view value, lsn_t lsn) {
	try {
		asyncData_.AddNewAsyncRequest();
	} catch (Error &err) {
		return err;
	}
	return tx_.putMeta(key, value, lsn, [this](const Error &e) { asyncData_.OnAsyncRequestDone(e); });
}

Error ProxiedTransaction::SetTagsMatcher(TagsMatcher &&tm, lsn_t lsn) {
	try {
		asyncData_.AddNewAsyncRequest();
	} catch (Error &err) {
		return err;
	}
	{
		std::lock_guard lck(mtx_);
		itemCache_.isValid = false;
	}
	return tx_.setTagsMatcher(std::move(tm), lsn, [this](const Error &e) { asyncData_.OnAsyncRequestDone(e); });
}

void ProxiedTransaction::Rollback(int serverId, const RdxContext &ctx) {
	asyncData_.AwaitAsyncRequests();
	if (tx_.rx_) {
		client::SyncCoroQueryResults clientResults;
		const auto _ctx = client::InternalRdxContext().WithLSN(ctx.GetOriginLSN()).WithEmmiterServerId(serverId);
		tx_.rx_->RollBackTransaction(tx_, _ctx);
	}
}

Error ProxiedTransaction::Commit(int serverId, int shardId, QueryResults &result, const RdxContext &ctx) {
	auto err = asyncData_.AwaitAsyncRequests();
	if (!err.ok()) return err;

	if (!tx_.Status().ok()) return tx_.Status();

	assertrx(tx_.rx_);
	client::InternalRdxContext c;
	if (!proxiedViaSharding_) {
		c = client::InternalRdxContext{}.WithLSN(ctx.GetOriginLSN()).WithEmmiterServerId(serverId);
		clusterProxyLog(LogTrace, "[proxy] Proxying commit to leader. SID: %d", serverId);
	} else {
		c = client::InternalRdxContext{}.WithShardId(shardId, false);
		clusterProxyLog(LogTrace, "[proxy] Proxying commit to shard %d. SID: %d", shardId, serverId);
	}

	client::SyncCoroQueryResults clientResults;
	err = tx_.rx_->CommitTransaction(tx_, clientResults, c);
	if (err.ok()) {
		try {
			result.AddQr(std::move(clientResults), shardId);
		} catch (Error &e) {
			return e;
		}
	}
	return err;
}

void ProxiedTransaction::AsyncData::AddNewAsyncRequest() {
	std::lock_guard lck(mtx_);
	if (!err_.ok()) throw err_;
	++asyncRequests_;
}

void ProxiedTransaction::AsyncData::OnAsyncRequestDone(const Error &e) noexcept {
	std::lock_guard lck(mtx_);
	if (!e.ok()) err_ = e;
	assertrx(asyncRequests_ > 0);
	if (--asyncRequests_ == 0) {
		cv_.notify_all();
	}
}

Error ProxiedTransaction::AsyncData::AwaitAsyncRequests() noexcept {
	std::unique_lock lck(mtx_);
	cv_.wait(lck, [this] { return asyncRequests_ == 0; });
	return err_;
}

}  // namespace reindexer
