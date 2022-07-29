#include "synccororeindexerimpl.h"
#include "client/connectionspool.h"
#include "client/itemimpl.h"

namespace reindexer {
namespace client {

using std::chrono::milliseconds;
constexpr size_t kAsyncCoroStackSize = 32 * 1024;

SyncCoroReindexerImpl::SyncCoroReindexerImpl(const CoroReindexerConfig &conf, uint32_t connCount, uint32_t threadsCount) : conf_(conf) {
	const auto conns = connCount > 0 ? connCount : 1;
	if (threadsCount > 1) {
		const auto connsPerThread = conns / threadsCount;
		auto mod = conns % threadsCount;
		for (unsigned i = 0; i < threadsCount; ++i) {
			if (mod) {
				--mod;
				workers_.emplace_back(connsPerThread + 1);
			} else if (connsPerThread) {
				workers_.emplace_back(connsPerThread);
			}
		}
	} else {
		workers_.emplace_back(conns);
	}
	sharedNamespaces_ = (workers_.size() > 1) ? INamespaces::PtrT(new NamespacesImpl<shared_timed_mutex>())
											  : INamespaces::PtrT(new NamespacesImpl<dummy_mutex>());
	commandsQueue_.Init(workers_);
}

SyncCoroReindexerImpl::~SyncCoroReindexerImpl() { stop(); }

Error SyncCoroReindexerImpl::Connect(const string &dsn, const client::ConnectOpts &opts) {
	std::lock_guard lock(workersMtx_);
	if (workers_.size() && workers_[0].th.joinable()) {
		return Error(errLogic, "Client is already started (%s)", dsn);
	}
	lastError_.Set(Error());
	runningWorkers_ = 0;
	commandsQueue_.ClearConnectionsMapping();

	std::promise<Error> isRunningPromise;
	auto isRunningFuture = isRunningPromise.get_future();
	for (uint32_t i = 0; i < workers_.size(); ++i) {
		workers_[i].th = std::thread([this, &isRunningPromise, dsn, opts, i] { this->threadLoopFun(i, isRunningPromise, dsn, opts); });
	}
	auto ret = isRunningFuture.get();
	if (ret.ok()) {
		ret = lastError_.Get();
	}
	if (!ret.ok()) {
		stop();
	}
	commandsQueue_.SetValid();
	return ret;
}

Error SyncCoroReindexerImpl::Stop() {
	std::lock_guard lock(workersMtx_);
	stop();
	return Error();
}
Error SyncCoroReindexerImpl::OpenNamespace(std::string_view nsName, const InternalRdxContext &ctx, const StorageOpts &opts,
										   const NsReplicationOpts &replOpts) {
	return sendCommand<Error>(DbCmdOpenNamespace, ctx, std::move(nsName), opts, replOpts);
}
Error SyncCoroReindexerImpl::AddNamespace(const NamespaceDef &nsDef, const InternalRdxContext &ctx, const NsReplicationOpts &replOpts) {
	return sendCommand<Error>(DbCmdAddNamespace, ctx, nsDef, replOpts);
}
Error SyncCoroReindexerImpl::CloseNamespace(std::string_view nsName, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdCloseNamespace, ctx, std::move(nsName));
}
Error SyncCoroReindexerImpl::DropNamespace(std::string_view nsName, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdDropNamespace, ctx, std::move(nsName));
}
Error SyncCoroReindexerImpl::TruncateNamespace(std::string_view nsName, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdTruncateNamespace, ctx, std::move(nsName));
}
Error SyncCoroReindexerImpl::RenameNamespace(std::string_view srcNsName, const std::string &dstNsName, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdRenameNamespace, ctx, std::move(srcNsName), dstNsName);
}
Error SyncCoroReindexerImpl::AddIndex(std::string_view nsName, const IndexDef &index, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdAddIndex, ctx, std::move(nsName), index);
}
Error SyncCoroReindexerImpl::UpdateIndex(std::string_view nsName, const IndexDef &index, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdUpdateIndex, ctx, std::move(nsName), index);
}
Error SyncCoroReindexerImpl::DropIndex(std::string_view nsName, const IndexDef &index, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdDropIndex, ctx, std::move(nsName), index);
}
Error SyncCoroReindexerImpl::SetSchema(std::string_view nsName, std::string_view schema, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdSetSchema, ctx, std::move(nsName), std::move(schema));
}
Error SyncCoroReindexerImpl::GetSchema(std::string_view nsName, int format, std::string &schema, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdGetSchema, ctx, std::move(nsName), std::move(format), schema);
}
Error SyncCoroReindexerImpl::EnumNamespaces(vector<NamespaceDef> &defs, EnumNamespacesOpts opts, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdEnumNamespaces, ctx, defs, std::move(opts));
}
Error SyncCoroReindexerImpl::EnumDatabases(vector<string> &dbList, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdEnumDatabases, ctx, dbList);
}
Error SyncCoroReindexerImpl::Insert(std::string_view nsName, Item &item, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdInsert, ctx, std::move(nsName), item);
}
Error SyncCoroReindexerImpl::Insert(std::string_view nsName, Item &item, SyncCoroQueryResults &result, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdInsertQR, ctx, std::move(nsName), item, result.results_);
}

Error SyncCoroReindexerImpl::Update(std::string_view nsName, Item &item, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdUpdate, ctx, std::move(nsName), item);
}
Error SyncCoroReindexerImpl::Update(std::string_view nsName, Item &item, SyncCoroQueryResults &result, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdUpdateQR, ctx, std::move(nsName), item, result.results_);
}

Error SyncCoroReindexerImpl::Upsert(std::string_view nsName, Item &item, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdUpsert, ctx, std::move(nsName), item);
}
Error SyncCoroReindexerImpl::Upsert(std::string_view nsName, Item &item, SyncCoroQueryResults &result, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdUpsertQR, ctx, std::move(nsName), item, result.results_);
}

Error SyncCoroReindexerImpl::Update(const Query &query, SyncCoroQueryResults &result, const InternalRdxContext &ctx) {
	Error err = result.setClient(this);
	if (!err.ok()) return err;
	return sendCommand<Error>(DbCmdUpdateQ, ctx, query, result.results_);
}
Error SyncCoroReindexerImpl::Delete(std::string_view nsName, Item &item, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdDelete, ctx, std::move(nsName), item);
}
Error SyncCoroReindexerImpl::Delete(std::string_view nsName, Item &item, SyncCoroQueryResults &result, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdDeleteQR, ctx, std::move(nsName), item, result.results_);
}

Error SyncCoroReindexerImpl::Delete(const Query &query, SyncCoroQueryResults &result, const InternalRdxContext &ctx) {
	Error err = result.setClient(this);
	if (!err.ok()) return err;
	return sendCommand<Error>(DbCmdDeleteQ, ctx, query, result.results_);
}
Error SyncCoroReindexerImpl::Select(std::string_view query, SyncCoroQueryResults &result, const InternalRdxContext &ctx) {
	Error err = result.setClient(this);
	if (!err.ok()) return err;
	return sendCommand<Error>(DbCmdSelectS, ctx, std::move(query), result.results_);
}
Error SyncCoroReindexerImpl::Select(const Query &query, SyncCoroQueryResults &result, const InternalRdxContext &ctx) {
	Error err = result.setClient(this);
	if (!err.ok()) return err;
	return sendCommand<Error>(DbCmdSelectQ, ctx, query, result.results_);
}
Error SyncCoroReindexerImpl::Commit(std::string_view nsName, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdCommit, ctx, std::move(nsName));
}

Item SyncCoroReindexerImpl::NewItem(std::string_view nsName, const InternalRdxContext &ctx) {
	return sendCommand<Item>(DbCmdNewItem, ctx, std::move(nsName));
}

Error SyncCoroReindexerImpl::GetMeta(std::string_view nsName, const string &key, string &data, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdGetMeta, ctx, std::move(nsName), key, data);
}
Error SyncCoroReindexerImpl::GetMeta(std::string_view nsName, const std::string &key, std::vector<ShardedMeta> &data,
									 const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdGetShardedMeta, ctx, std::move(nsName), key, data);
}
Error SyncCoroReindexerImpl::PutMeta(std::string_view nsName, const string &key, std::string_view data, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdPutMeta, ctx, std::move(nsName), key, std::move(data));
}
Error SyncCoroReindexerImpl::EnumMeta(std::string_view nsName, vector<string> &keys, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdEnumMeta, ctx, std::move(nsName), keys);
}
Error SyncCoroReindexerImpl::GetSqlSuggestions(std::string_view sqlQuery, int pos, vector<string> &suggestions) {
	return sendCommand<Error>(DbCmdGetSqlSuggestions, InternalRdxContext(), std::move(sqlQuery), std::move(pos), suggestions);
}
Error SyncCoroReindexerImpl::Status(bool forceCheck, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdStatus, ctx, std::move(forceCheck));
}

CoroTransaction SyncCoroReindexerImpl::NewTransaction(std::string_view nsName, const InternalRdxContext &ctx) {
	return sendCommand<CoroTransaction>(DbCmdNewTransaction, ctx, std::move(nsName));
}

Error SyncCoroReindexerImpl::CommitTransaction(SyncCoroTransaction &tr, SyncCoroQueryResults &results, const InternalRdxContext &ctx) {
	if (tr.IsFree()) {
		return Error(errBadTransaction, "commit free transaction");
	}
	if (tr.rx_.get() != this) {
		return Error(errTxInvalidLeader, "Commit transaction to incorrect leader");
	}
	return sendCommand<true, Error, CoroTransaction &, CoroQueryResults &>(tr.coroConnection(), DbCmdCommitTransaction, ctx, tr.tr_,
																		   results.results_);
}

Error SyncCoroReindexerImpl::RollBackTransaction(SyncCoroTransaction &tr, const InternalRdxContext &ctx) {
	if (tr.IsFree()) return tr.Status();
	if (tr.rx_.get() != this) {
		return Error(errLogic, "RollBack transaction to incorrect leader");
	}
	return sendCommand<true, Error, CoroTransaction &>(tr.coroConnection(), DbCmdRollBackTransaction, ctx, tr.tr_);
}

Error SyncCoroReindexerImpl::GetReplState(std::string_view nsName, ReplicationStateV2 &state, const InternalRdxContext &ctx) {
	return sendCommand<Error>(DbCmdGetReplState, ctx, std::move(nsName), state);
}

Error SyncCoroReindexerImpl::fetchResults(int flags, SyncCoroQueryResults &result) {
	return sendCommand<true, Error>(result.coroConnection(), DbCmdFetchResults, InternalRdxContext(), std::move(flags), result.results_);
}

Error SyncCoroReindexerImpl::closeResults(SyncCoroQueryResults &result) {
	return sendCommand<true, Error>(result.coroConnection(), DbCmdCloseResults, InternalRdxContext(), result.results_);
}

Error SyncCoroReindexerImpl::addTxItem(SyncCoroTransaction &tr, Item &&item, ItemModifyMode mode, lsn_t lsn,
									   SyncCoroTransaction::Completion cmpl) {
	return sendCommand<true, Error>(tr.coroConnection(), DbCmdAddTxItem, InternalRdxContext(std::move(cmpl)), tr.tr_, std::move(item),
									std::move(mode), std::move(lsn));
}

Error SyncCoroReindexerImpl::putTxMeta(SyncCoroTransaction &tr, std::string_view key, std::string_view value, lsn_t lsn,
									   SyncCoroTransaction::Completion cmpl) {
	return sendCommand<true, Error>(tr.coroConnection(), DbCmdPutTxMeta, InternalRdxContext(std::move(cmpl)), tr.tr_, std::move(key),
									std::move(value), std::move(lsn));
}

Error SyncCoroReindexerImpl::setTxTm(SyncCoroTransaction &tr, TagsMatcher &&tm, lsn_t lsn, SyncCoroTransaction::Completion cmpl) {
	return sendCommand<true, Error>(tr.coroConnection(), DbCmdSetTxTagsMatcher, InternalRdxContext(std::move(cmpl)), tr.tr_, std::move(tm),
									std::move(lsn));
}

Error SyncCoroReindexerImpl::modifyTx(SyncCoroTransaction &tr, Query &&q, lsn_t lsn, SyncCoroTransaction::Completion cmpl) {
	return sendCommand<true, Error>(tr.coroConnection(), DbCmdModifyTx, InternalRdxContext(std::move(cmpl)), tr.tr_, std::move(q),
									std::move(lsn));
}

Item SyncCoroReindexerImpl::newItemTx(CoroTransaction &tr) {
	return sendCommand<true, Item>(tr.getConn(), DbCmdNewItemTx, InternalRdxContext(), tr);
}

void SyncCoroReindexerImpl::threadLoopFun(uint32_t tid, std::promise<Error> &isRunning, const string &dsn,
										  const client::ConnectOpts &opts) {
	auto &th = workers_[tid];
	assert(sharedNamespaces_);
	if (!th.connData) {
		th.connData = std::make_unique<ConnectionsPoolData>(th.connCount, conf_, sharedNamespaces_);
	}
	ConnectionsPool<DatabaseCommand> connPool(*th.connData);
	struct {
		uint32_t tid;
		ConnectionsPool<DatabaseCommand> &connPool;
	} thData = {tid, connPool};

	th.commandAsync.set(th.loop);
	th.commandAsync.set([this, &thData](net::ev::async &) {
		workers_[thData.tid].loop.spawn(
			[this, &thData]() {
				h_vector<DatabaseCommand, 16> q;
				for (bool readMore = true; readMore;) {
					commandsQueue_.Get(thData.tid, q);
					readMore = q.size();
					for (auto &&cmd : q) {
						auto &conn = thData.connPool.GetConn();
						assert(conn.IsChOpened());
						conn.PushCmd(std::move(cmd));
					}
					q.clear();
				}
			},
			kAsyncCoroStackSize);
	});

	uint32_t runningCount = 0;
	for (auto &conn : connPool) {
		th.loop.spawn([this, dsn, opts, &isRunning, &conn, &thData, &runningCount]() noexcept {
			auto &th = workers_[thData.tid];
			auto err = conn.rx.Connect(dsn, th.loop, opts);
			if (err.ok()) {
				commandsQueue_.RegisterConn(thData.tid, conn.rx.GetConnPtr());
			} else {
				lastError_.Set(err);
			}
			if (++runningCount == th.connCount && ++runningWorkers_ == workers_.size()) {
				isRunning.set_value(err);
			}
			coroutine::wait_group wg;
			for (unsigned n = 0; n < conf_.SyncRxCoroCount; ++n) {
				th.loop.spawn(wg, [this, &conn, &thData]() noexcept { coroInterpreter(conn, thData.connPool, thData.tid); });
			}
			wg.wait();

			th.commandAsync.stop();
			th.closeAsync.stop();
			assert(!conn.IsChOpened());
			conn.rx.Stop();
		});
	}
	th.commandAsync.start();

	th.closeAsync.set(th.loop);
	th.closeAsync.set([this, &thData](net::ev::async &) {
		workers_[thData.tid].commandAsync.stop();
		workers_[thData.tid].loop.spawn([this, &thData]() {
			coroutine::wait_group wg;
			for (auto &conn : thData.connPool) {
				if (conn.IsChOpened()) {
					workers_[thData.tid].loop.spawn(wg, [&conn] { conn.rx.Stop(); });
				}
			}
			wg.wait();
			h_vector<DatabaseCommand, 16> q;
			commandsQueue_.Invalidate(thData.tid, q);
			for (auto &conn : thData.connPool) {
				if (conn.IsChOpened()) {
					for (auto &&cmd : q) {
						conn.PushCmd(std::move(cmd));
					}
					q.clear();
					break;
				}
			}
			assert(q.empty());
			for (auto &conn : thData.connPool) {
				conn.CloseCh();
			}
		});
	});
	th.closeAsync.start();

	th.loop.run();
	th.commandAsync.stop();
}

void SyncCoroReindexerImpl::stop() {
	for (auto &w : workers_) {
		w.closeAsync.send();
	}
	for (auto &w : workers_) {
		if (w.th.joinable()) {
			w.th.join();
		}
	}
}

void SyncCoroReindexerImpl::coroInterpreter(Connection<DatabaseCommand> &conn, ConnectionsPool<DatabaseCommand> &pool,
											uint32_t tid) noexcept {
	using namespace std::placeholders;
	for (std::pair<DatabaseCommand, bool> v = conn.PopCmd(); v.second == true; v = conn.PopCmd()) {
		const auto cmd = v.first.Data();
		switch (cmd->id) {
			case DbCmdOpenNamespace: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, const StorageOpts &sopts, const NsReplicationOpts &replOpts) {
					return conn.rx.OpenNamespace(nsName, cmd->ctx, sopts, replOpts);
				});
				break;
			}
			case DbCmdAddNamespace: {
				execCommand(cmd, [&conn, &cmd](const NamespaceDef &nsDef, const NsReplicationOpts &replOpts) {
					return conn.rx.AddNamespace(nsDef, cmd->ctx, replOpts);
				});
				break;
			}
			case DbCmdCloseNamespace: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName) { return conn.rx.CloseNamespace(nsName, cmd->ctx); });
				break;
			}
			case DbCmdDropNamespace: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName) { return conn.rx.DropNamespace(nsName, cmd->ctx); });
				break;
			}
			case DbCmdTruncateNamespace: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName) { return conn.rx.TruncateNamespace(nsName, cmd->ctx); });
				break;
			}
			case DbCmdRenameNamespace: {
				execCommand(cmd, [&conn, &cmd](std::string_view srcNsName, const std::string &dstNsName) {
					return conn.rx.RenameNamespace(srcNsName, dstNsName, cmd->ctx);
				});
				break;
			}

			case DbCmdAddIndex: {
				execCommand(
					cmd, [&conn, &cmd](std::string_view nsName, const IndexDef &iDef) { return conn.rx.AddIndex(nsName, iDef, cmd->ctx); });
				break;
			}
			case DbCmdUpdateIndex: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, const IndexDef &iDef) {
					return conn.rx.UpdateIndex(nsName, iDef, cmd->ctx);
				});
				break;
			}
			case DbCmdDropIndex: {
				execCommand(
					cmd, [&conn, &cmd](std::string_view nsName, const IndexDef &idx) { return conn.rx.DropIndex(nsName, idx, cmd->ctx); });
				break;
			}
			case DbCmdSetSchema: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, std::string_view schema) {
					return conn.rx.SetSchema(nsName, schema, cmd->ctx);
				});
				break;
			}
			case DbCmdGetSchema: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, int format, std::string &schema) {
					return conn.rx.GetSchema(nsName, format, schema, cmd->ctx);
				});
				break;
			}
			case DbCmdEnumNamespaces: {
				execCommand(cmd, [&conn, &cmd](vector<NamespaceDef> &defs, EnumNamespacesOpts opts) {
					return conn.rx.EnumNamespaces(defs, opts, cmd->ctx);
				});
				break;
			}
			case DbCmdEnumDatabases: {
				execCommand(cmd, [&conn, &cmd](std::vector<std::string> &dbList) { return conn.rx.EnumDatabases(dbList, cmd->ctx); });
				break;
			}
			case DbCmdInsert: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, Item &item) { return conn.rx.Insert(nsName, item, cmd->ctx); });
				break;
			}
			case DbCmdInsertQR: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, client::Item &item, CoroQueryResults &result) {
					return conn.rx.Insert(nsName, item, result, cmd->ctx);
				});
				break;
			}

			case DbCmdUpdate: {
				execCommand(cmd,
							[&conn, &cmd](std::string_view nsName, client::Item &item) { return conn.rx.Update(nsName, item, cmd->ctx); });
				break;
			}
			case DbCmdUpdateQR: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, client::Item &item, CoroQueryResults &result) {
					return conn.rx.Update(nsName, item, result, cmd->ctx);
				});
				break;
			}

			case DbCmdUpsert: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, Item &item) { return conn.rx.Upsert(nsName, item, cmd->ctx); });
				break;
			}
			case DbCmdUpsertQR: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, client::Item &item, CoroQueryResults &result) {
					return conn.rx.Upsert(nsName, item, result, cmd->ctx);
				});
				break;
			}

			case DbCmdUpdateQ: {
				execCommand(
					cmd, [&conn, &cmd](const Query &query, CoroQueryResults &result) { return conn.rx.Update(query, result, cmd->ctx); });
				break;
			}
			case DbCmdDelete: {
				execCommand(cmd,
							[&conn, &cmd](std::string_view nsName, client::Item &item) { return conn.rx.Delete(nsName, item, cmd->ctx); });
				break;
			}
			case DbCmdDeleteQR: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, client::Item &item, CoroQueryResults &result) {
					return conn.rx.Delete(nsName, item, result, cmd->ctx);
				});
				break;
			}
			case DbCmdDeleteQ: {
				execCommand(
					cmd, [&conn, &cmd](const Query &query, CoroQueryResults &result) { return conn.rx.Delete(query, result, cmd->ctx); });
				break;
			}

			case DbCmdNewItem: {
				auto cd = dynamic_cast<DatabaseCommandData<Item, std::string_view> *>(cmd);
				assert(cd);
				Item item = conn.rx.NewItem(std::get<0>(cd->arguments), *this, cd->ctx.execTimeout());
				cd->ret.set_value(std::move(item));
				break;
			}
			case DbCmdSelectS: {
				execCommand(cmd, [&conn, &cmd](std::string_view ns, CoroQueryResults &qr) { return conn.rx.Select(ns, qr, cmd->ctx); });
				break;
			}
			case DbCmdSelectQ: {
				execCommand(
					cmd, [&conn, &cmd](const Query &query, CoroQueryResults &result) { return conn.rx.Select(query, result, cmd->ctx); });
				break;
			}
			case DbCmdCommit: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName) { return conn.rx.Commit(nsName, cmd->ctx); });
				break;
			}
			case DbCmdGetMeta: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, const string &key, string &data) {
					return conn.rx.GetMeta(nsName, key, data, cmd->ctx);
				});
				break;
			}
			case DbCmdGetShardedMeta: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, const std::string &key, std::vector<ShardedMeta> &data) {
					return conn.rx.GetMeta(nsName, key, data, cmd->ctx);
				});
				break;
			}
			case DbCmdPutMeta: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, const string &key, std::string_view data) {
					return conn.rx.PutMeta(nsName, key, data, cmd->ctx);
				});
				break;
			}
			case DbCmdEnumMeta: {
				execCommand(
					cmd, [&conn, &cmd](std::string_view nsName, vector<string> &keys) { return conn.rx.EnumMeta(nsName, keys, cmd->ctx); });
				break;
			}
			case DbCmdGetSqlSuggestions: {
				execCommand(cmd, [&conn](std::string_view query, int pos, std::vector<std::string> &suggests) {
					return conn.rx.GetSqlSuggestions(query, pos, suggests);
				});
				break;
			}
			case DbCmdStatus: {
				auto *cd = dynamic_cast<DatabaseCommandData<Error, bool> *>(cmd);
				assert(cd);
				bool force = std::get<0>(cd->arguments);
				for (auto &c : pool) {
					if (force || c.rx.RequiresStatusCheck()) {
						force = true;
						break;
					}
				}
				execCommand(cmd, [&conn, &cmd](bool forceCheck) { return conn.rx.Status(forceCheck, cmd->ctx); });
				break;
			}
			case DbCmdNewTransaction: {
				auto *cd = dynamic_cast<DatabaseCommandData<CoroTransaction, std::string_view> *>(cmd);
				assertrx(cd);
				CoroTransaction coroTrans = conn.rx.NewTransaction(std::get<0>(cd->arguments), cd->ctx);
				cd->ret.set_value(std::move(coroTrans));
				break;
			}
			case DbCmdCommitTransaction: {
				execCommand(cmd, [&conn, &cmd](CoroTransaction &tr, CoroQueryResults &result) {
					return conn.rx.CommitTransaction(tr, result, cmd->ctx);
				});
				break;
			}
			case DbCmdRollBackTransaction: {
				execCommand(cmd, [&conn, &cmd](CoroTransaction &tr) { return conn.rx.RollBackTransaction(tr, cmd->ctx); });
				break;
			}
			case DbCmdFetchResults: {
				auto cd = dynamic_cast<DatabaseCommandData<Error, int, CoroQueryResults &> *>(cmd);
				assertrx(cd);
				CoroQueryResults &coroResults = std::get<1>(cd->arguments);
				if (!coroResults.holdsRemoteData()) {
					cd->ret.set_value(Error(errLogic, "Client query results does not hold any remote data"));
					break;
				}
				auto ret =
					coroResults.i_.conn_->Call({reindexer::net::cproto::kCmdFetchResults, coroResults.i_.requestTimeout_, milliseconds(0),
												lsn_t(), -1, ShardingKeyType::NotSetShard, nullptr, false, coroResults.i_.sessionTs_},
											   coroResults.i_.queryID_.main, std::get<0>(cd->arguments),
											   coroResults.i_.queryParams_.count + coroResults.i_.fetchOffset_, coroResults.i_.fetchAmount_,
											   coroResults.i_.queryID_.uid);
				if (!ret.Status().ok()) {
					cd->ret.set_value(ret.Status());
					break;
				}
				Error err;
				try {
					auto args = ret.GetArgs(2);

					coroResults.i_.fetchOffset_ += coroResults.i_.queryParams_.count;

					std::string_view rawResult = p_string(args[0]);
					ResultSerializer ser(rawResult);

					ser.GetRawQueryParams(coroResults.i_.queryParams_, nullptr);

					coroResults.i_.rawResult_.assign(rawResult.begin() + ser.Pos(), rawResult.end());
				} catch (Error &e) {
					err = e;
				}

				cd->ret.set_value(std::move(err));

				break;
			}
			case DbCmdCloseResults: {
				auto cd = dynamic_cast<DatabaseCommandData<Error, CoroQueryResults &> *>(cmd);
				assert(cd);
				CoroQueryResults &coroResults = std::get<0>(cd->arguments);
				Error err;
				if (coroResults.holdsRemoteData()) {
					err = coroResults.i_.conn_
							  ->Call({reindexer::net::cproto::kCmdCloseResults, coroResults.i_.requestTimeout_, milliseconds(0), lsn_t(),
									  -1, ShardingKeyType::NotSetShard, nullptr, false, coroResults.i_.sessionTs_},
									 coroResults.i_.queryID_.main, coroResults.i_.queryID_.uid)
							  .Status();
					coroResults.setClosed();
				} else {
					err = Error(errLogic, "Client query results does not hold remote data");
				}
				cd->ret.set_value(std::move(err));
				break;
			}
			case DbCmdNewItemTx: {
				auto cd = dynamic_cast<DatabaseCommandData<Item, CoroTransaction &> *>(cmd);
				assertrx(cd);
				Item item = std::get<0>(cd->arguments).NewItem(this);
				cd->ret.set_value(std::move(item));
				break;
			}
			case DbCmdAddTxItem: {
				auto cd = dynamic_cast<DatabaseCommandData<Error, CoroTransaction &, Item, ItemModifyMode, lsn_t> *>(cmd);
				assertrx(cd);
				Error err = std::get<0>(cd->arguments)
								.Modify(std::move(std::get<1>(cd->arguments)), std::get<2>(cd->arguments), std::get<3>(cd->arguments));
				if (cd->ctx.cmpl()) {
					cd->ctx.cmpl()(err);
				} else {
					cd->ret.set_value(std::move(err));
				}
				break;
			}
			case DbCmdPutTxMeta: {
				auto cd = dynamic_cast<DatabaseCommandData<Error, CoroTransaction &, std::string_view, std::string_view, lsn_t> *>(cmd);
				assertrx(cd);
				Error err = std::get<0>(cd->arguments)
								.PutMeta(std::move(std::get<1>(cd->arguments)), std::get<2>(cd->arguments), std::get<3>(cd->arguments));
				if (cd->ctx.cmpl()) {
					cd->ctx.cmpl()(err);
				} else {
					cd->ret.set_value(std::move(err));
				}
				break;
			}
			case DbCmdSetTxTagsMatcher: {
				auto cd = dynamic_cast<DatabaseCommandData<Error, CoroTransaction &, TagsMatcher, lsn_t> *>(cmd);
				assertrx(cd);
				Error err = std::get<0>(cd->arguments).SetTagsMatcher(std::move(std::get<1>(cd->arguments)), std::get<2>(cd->arguments));
				if (cd->ctx.cmpl()) {
					cd->ctx.cmpl()(err);
				} else {
					cd->ret.set_value(std::move(err));
				}
				break;
			}
			case DbCmdModifyTx: {
				auto cd = dynamic_cast<DatabaseCommandData<Error, CoroTransaction &, Query, lsn_t> *>(cmd);
				assertrx(cd);
				CoroTransaction &tr = std::get<0>(cd->arguments);
				Error err(errLogic, "Connection pointer in transaction is nullptr.");
				auto txConn = tr.getConn();
				if (txConn) {
					WrSerializer ser;
					std::get<1>(cd->arguments).Serialize(ser);
					switch (std::get<1>(cd->arguments).type_) {
						case QueryUpdate: {
							err =
								txConn
									->Call({cproto::kCmdUpdateQueryTx, tr.i_.requestTimeout_, tr.i_.execTimeout_,
											std::get<2>(cd->arguments), -1, ShardingKeyType::NotSetShard, nullptr, false, tr.i_.sessionTs_},
										   ser.Slice(), tr.i_.txId_)
									.Status();
							break;
						}
						case QueryDelete: {
							err =
								txConn
									->Call({cproto::kCmdDeleteQueryTx, tr.i_.requestTimeout_, tr.i_.execTimeout_,
											std::get<2>(cd->arguments), -1, ShardingKeyType::NotSetShard, nullptr, false, tr.i_.sessionTs_},
										   ser.Slice(), tr.i_.txId_)
									.Status();
							break;
						}
						default:
							err = Error(errParams, "Incorrect query type in transaction modify %d", std::get<1>(cd->arguments).type_);
					}
				}
				if (cd->ctx.cmpl()) {
					cd->ctx.cmpl()(err);
				} else {
					cd->ret.set_value(std::move(err));
				}
				break;
			}
			case DbCmdGetReplState: {
				execCommand(cmd, [&conn, &cmd](std::string_view nsName, ReplicationStateV2 &state) {
					return conn.rx.GetReplState(nsName, state, cmd->ctx);
				});
				break;
			}
			default:
				assert(false);
				break;
		}
		conn.OnRequestDone();
		commandsQueue_.OnCmdDone(tid);
	}
}

SyncCoroReindexerImpl::WorkerThread::WorkerThread(uint32_t _connCount) : connCount(_connCount) {}
SyncCoroReindexerImpl::WorkerThread::~WorkerThread() = default;

void SyncCoroReindexerImpl::CommandsQueue::Get(uint32_t tid, h_vector<DatabaseCommand, 16> &cmds) {
	assertrx(tid < thData_.size());
	auto &thD = thData_[tid];
	std::lock_guard lck(mtx_);
	if (thD.personalQueue.size()) {
		std::swap(thD.personalQueue, cmds);
		thD.reqCnt.fetch_add(cmds.size(), std::memory_order_relaxed);
		return;
	}
	if (sharedQueue_.size()) {
		cmds.emplace_back(std::move(*sharedQueue_.begin()));
		sharedQueue_.pop_front();
		thD.reqCnt.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	thD.isReading = false;
}

void SyncCoroReindexerImpl::CommandsQueue::OnCmdDone(uint32_t tid) {
	assertrx(tid < thData_.size());
	auto &thD = thData_[tid];
	thD.reqCnt.fetch_sub(1, std::memory_order_release);
}

void SyncCoroReindexerImpl::CommandsQueue::Init(std::deque<WorkerThread> &threads) {
	std::lock_guard<std::mutex> lock(mtx_);
	assertrx(thData_.empty());
	for (auto &th : threads) {
		thData_.emplace_back(th.commandAsync);
	}
}

void SyncCoroReindexerImpl::CommandsQueue::Invalidate(uint32_t tid, h_vector<DatabaseCommand, 16> &cmds) {
	assertrx(tid < thData_.size());
	auto &thD = thData_[tid];
	std::lock_guard<std::mutex> lock(mtx_);
	std::swap(thD.personalQueue, cmds);
	thD.reqCnt.store(0, std::memory_order_relaxed);
	while (sharedQueue_.size()) {
		cmds.emplace_back(std::move(*sharedQueue_.begin()));
		sharedQueue_.pop_front();
	}
	thByConns_.clear();
	isValid_ = false;
}

void SyncCoroReindexerImpl::CommandsQueue::SetValid() {
	std::lock_guard<std::mutex> lock(mtx_);
	isValid_ = true;
}

void SyncCoroReindexerImpl::CommandsQueue::ClearConnectionsMapping() {
	std::lock_guard<std::mutex> lock(mtx_);
	thByConns_.clear();
}

void SyncCoroReindexerImpl::CommandsQueue::RegisterConn(uint32_t tid, const void *conn) {
	assertrx(tid < thData_.size());
	auto &thD = thData_[tid];
	std::lock_guard<std::mutex> lock(mtx_);
	const auto res = thByConns_.emplace(conn, &thD);
	assertrx(res.second);
	(void)res;
}

}  // namespace client
}  // namespace reindexer
