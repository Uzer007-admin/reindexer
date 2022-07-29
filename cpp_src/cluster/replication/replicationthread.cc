#include "asyncreplthread.h"
#include "client/snapshot.h"
#include "clusterreplthread.h"
#include "core/defnsconfigs.h"
#include "core/namespace/snapshot/snapshot.h"
#include "core/reindexerimpl.h"
#include "tools/flagguard.h"
#include "updatesbatcher.h"
#include "updatesqueue.h"
#include "vendor/spdlog/common.h"

namespace reindexer {
namespace cluster {

constexpr size_t kMaxRetriesOnRoleSwitchAwait = 50;
constexpr auto kRoleSwitchStepTime = std::chrono::milliseconds(150);
constexpr auto kAwaitNsCopyInterval = std::chrono::milliseconds(2000);
constexpr auto kCoro32KStackSize = 32 * 1024;

template <typename BehaviourParamT>
bool UpdateApplyStatus::IsHaveToResync() const noexcept {
	static_assert(std::is_same_v<BehaviourParamT, AsyncThreadParam> || std::is_same_v<BehaviourParamT, ClusterThreadParam>,
				  "Unexpected param type");
	if constexpr (std::is_same_v<BehaviourParamT, ClusterThreadParam>) {
		return type == UpdateRecord::Type::ResyncNamespaceGeneric || type == UpdateRecord::Type::ResyncOnUpdatesDrop;
	} else {
		return type == UpdateRecord::Type::ResyncNamespaceGeneric || type == UpdateRecord::Type::ResyncNamespaceLeaderInit ||
			   type == UpdateRecord::Type::ResyncOnUpdatesDrop;
	}
}

template <typename BehaviourParamT>
ReplThread<BehaviourParamT>::ReplThread(int serverId, ReindexerImpl& _thisNode, std::shared_ptr<UpdatesQueueT> shard,
										BehaviourParamT&& bhvParam, ReplicationStatsCollector statsCollector)
	: thisNode(_thisNode),
	  serverId_(serverId),
	  bhvParam_(std::move(bhvParam)),
	  updates_(std::move(shard)),
	  statsCollector_(statsCollector) {
	assert(updates_);
	updatesAsync_.set(loop);
	updatesAsync_.set([this](net::ev::async& watcher) {
		hasPendingNotificaions_ = true;
		if (!notificationInProgress_) {
			logPrintf(LogTrace, "[cluster:replicator]%s %d: new updates notification", typeString(), serverId_);
			notificationInProgress_ = true;
			watcher.loop.spawn(
				wg,
				[this]() noexcept {
					while (hasPendingNotificaions_) {
						hasPendingNotificaions_ = false;
						updatesNotifier();
					}
					notificationInProgress_ = false;
				},
				kCoro32KStackSize);
		}
	});
}

template <typename BehaviourParamT>
template <typename NodeConfigT>
void ReplThread<BehaviourParamT>::Run(ReplThreadConfig config, const std::vector<std::pair<uint32_t, NodeConfigT>>& nodesList,
									  size_t consensusCnt, size_t requiredReplicas) {
	config_ = std::move(config);
	consensusCnt_ = consensusCnt;
	requiredReplicas_ = requiredReplicas;

	loop.spawn([this, &nodesList]() noexcept {
		nodes.clear();
		if (config_.ParallelSyncsPerThreadCount > 0) {
			nsSyncTokens_ = std::make_unique<coroutine::tokens_pool<bool>>(config_.ParallelSyncsPerThreadCount);
		} else {
			nsSyncTokens_.reset();
		}
		client::CoroReindexerConfig rpcCfg;
		rpcCfg.AppName = config_.AppName;
		rpcCfg.NetTimeout = std::chrono::seconds(config_.UpdatesTimeoutSec);
		rpcCfg.EnableCompression = config_.EnableCompression;
		for (const auto& nodeP : nodesList) {
			nodes.emplace_back(nodeP.second.GetServerID(), nodeP.first, rpcCfg);
			nodes.back().dsn = nodeP.second.GetRPCDsn();
		}

		bhvParam_.AwaitReplPermission();
		if (!terminate_) {
			{
				std::string nodesString;
				for (size_t i = 0; i < nodes.size(); ++i) {
					if (i > 0) {
						nodesString.append(", ");
					}
					nodesString.append(fmt::sprintf("Node %d - server ID %d", nodes[i].uid, nodes[i].serverId));
				}
				logPrintf(LogInfo, "[cluster:replicator]%s %d: starting dataReplicationThread. Nodes:'%s'", typeString(), serverId_,
						  nodesString);
			}
			updates_->AddDataNotifier(std::this_thread::get_id(), [this] { updatesAsync_.send(); });

			for (size_t i = 0; i < nodes.size(); ++i) {
				loop.spawn(wg, [this, i]() noexcept {
					// 3) Perform wal-sync/force-sync for each follower
					nodeReplicationRoutine(nodes[i]);
				});
			}
			// Await termination
			if (!terminateCh_.opened()) {
				terminateCh_.reopen();
			}
			terminateCh_.pop();
			wg.wait();
		}
	});

	updatesAsync_.start();

	loop.run();

	updates_->RemoveDataNotifier(std::this_thread::get_id());
	updatesAsync_.stop();

	logPrintf(LogInfo, "[cluster:replicator]%s %d: Replication thread was terminated. TID: %d", typeString(), serverId_,
			  std::this_thread::get_id());
}

template <typename BehaviourParamT>
void ReplThread<BehaviourParamT>::SetTerminate(bool val) noexcept {
	terminate_ = val;
	if (val) {
		updatesAsync_.send();
	}
}

template <typename BehaviourParamT>
constexpr bool ReplThread<BehaviourParamT>::isClusterReplThread() noexcept {
	static_assert(std::is_same_v<BehaviourParamT, AsyncThreadParam> || std::is_same_v<BehaviourParamT, ClusterThreadParam>,
				  "Unexpected param type");
	if constexpr (std::is_same_v<BehaviourParamT, ClusterThreadParam>) {
		return true;
	} else {
		return false;
	}
}

template <>
void ReplThread<AsyncThreadParam>::updateNodeStatus(size_t uid, NodeStats::Status st) {
	statsCollector_.OnStatusChanged(uid, st);
}
template <>
void ReplThread<ClusterThreadParam>::updateNodeStatus(size_t, NodeStats::Status) {}

template <typename BehaviourParamT>
void ReplThread<BehaviourParamT>::nodeReplicationRoutine(Node& node) {
	Error err;
	bool expectingReconnect = true;
	while (!terminate_) {
		statsCollector_.OnSyncStateChanged(node.uid, NodeStats::SyncState::AwaitingResync);
		bhvParam_.AwaitReplPermission();
		if (terminate_) {
			break;
		}
		if (expectingReconnect && (!err.ok() || !node.client.WithTimeout(kStatusCmdTimeout).Status(true).ok())) {
			logPrintf(LogInfo, "[cluster:replicator]%s %d:%d Reconnecting... Reason: %s", typeString(), serverId_, node.uid,
					  err.ok() ? "Not connected yet" : ("Error: " + err.what()));
			node.Reconnect(loop, config_);
		}
		LogLevel logLevel = LogTrace;
		err = checkIfReplicationAllowed(node, logLevel);
		statsCollector_.SaveNodeError(node.uid, err);  // Reset last node error after checking node replication allowance
		if (err.ok()) {
			expectingReconnect = true;
			if (!node.connObserverId.has_value()) {
				node.connObserverId = node.client.AddConnectionStateObserver([this, &node](const Error& err) noexcept {
					if (!err.ok() && updates_ && !terminate_) {
						logPrintf(LogInfo, "[cluster:replicator]%s %d:%d Connection error: %s", typeString(), serverId_, node.uid,
								  err.what());
						UpdatesContainer recs(1);
						recs[0] = UpdateRecord{UpdateRecord::Type::NodeNetworkCheck, node.uid, false};
						node.requireResync = true;
						updates_->template PushAsync<true>(std::move(recs));
					}
				});
			}
			err = nodeReplicationImpl(node);
			statsCollector_.SaveNodeError(node.uid, err);
		} else {
			expectingReconnect = false;
			logPrintf(logLevel, "[cluster:replicator]%s %d:%d Replication is not allowed: %s", typeString(), serverId_, node.uid,
					  err.what());
		}
		// Wait before next sync retry
		constexpr auto kGranularSleepInterval = std::chrono::milliseconds(150);
		auto awaitTime = isTxCopyError(err) ? kAwaitNsCopyInterval : std::chrono::milliseconds(config_.RetrySyncIntervalMSec);
		if (!terminate_) {
			if (err.ok()) {
				logPrintf(LogTrace, "[cluster:replicator]%s %d:%d Doing resync...", typeString(), serverId_, node.uid);
				continue;
			}
			bhvParam_.OnNodeBecameUnsynchonized(node.uid);
			updateNodeStatus(node.uid, NodeStats::Status::Offline);
			statsCollector_.OnSyncStateChanged(node.uid, NodeStats::SyncState::AwaitingResync);
		}
		while (!terminate_ && awaitTime.count() > 0) {
			const auto diff = std::min(awaitTime, kGranularSleepInterval);
			awaitTime -= diff;
			loop.sleep(diff);

			if (isTimeoutError(err)) {
				break;
			}
			if (isNetworkError(err) || isLeaderChangedError(err)) {
				const bool retrySync = handleUpdatesWithError(node, err);
				if (retrySync) break;
			}
		}
	}
	if (terminate_) {
		logPrintf(LogTrace, "[cluster:replicator]%s %d:%d Node replication routine was terminated", typeString(), serverId_, node.uid);
	}
	if (node.connObserverId.has_value()) {
		node.client.RemoveConnectionStateObserver(*node.connObserverId);
		node.connObserverId.reset();
	}
	node.client.Stop();
}

template <typename BehaviourParamT>
Error ReplThread<BehaviourParamT>::nodeReplicationImpl(Node& node) {
	std::vector<NamespaceDef> nsList;
	node.requireResync = false;
	logPrintf(LogTrace, "[cluster:replicator]%s %d:%d Trying to collect local namespaces...", typeString(), serverId_, node.uid);
	auto integralError = thisNode.EnumNamespaces(nsList, EnumNamespacesOpts().OnlyNames().HideSystem().HideTemporary(), RdxContext());
	if (!integralError.ok()) {
		logPrintf(LogWarning, "[cluster:replicator]%s %d:%d Unable to enum local namespaces in node replication routine: %s", typeString(),
				  serverId_, node.uid, integralError.what());
		return integralError;
	}

	logPrintf(LogTrace, "[cluster:replicator]%s %d:%d Performing ns data cleanup...", typeString(), serverId_, node.uid);
	for (auto nsDataIt = node.namespaceData.begin(); nsDataIt != node.namespaceData.end();) {
		if (!nsDataIt->second.tx.IsFree()) {
			auto err = node.client.WithLSN(lsn_t(0, serverId_)).RollBackTransaction(nsDataIt->second.tx);
			logPrintf(LogInfo, "[cluster:replicator]%s %d:%d Rollback transaction result: %s", typeString(), serverId_, node.uid,
					  err.ok() ? "OK" : ("Error:" + std::to_string(err.code()) + ". " + err.what()));
			nsDataIt->second.tx = client::CoroTransaction();
		}
		if (nsDataIt->second.isClosed) {
			nsDataIt->second.requiresTmUpdate = true;
			++nsDataIt;
		} else {
			nsDataIt = node.namespaceData.erase(nsDataIt);
		}
	}

	logPrintf(LogInfo, "[cluster:replicator]%s %d:%d Creating %d sync routines", typeString(), serverId_, node.uid, nsList.size());
	coroutine::wait_group localWg;
	for (const auto& ns : nsList) {
		if (!bhvParam_.IsNamespaceInConfig(node.uid, ns.name)) {
			continue;
		}
		logPrintf(LogTrace, "[cluster:replicator]%s %d:%d Creating sync routine for %s", typeString(), serverId_, node.uid, ns.name);
		loop.spawn(localWg, [this, &integralError, &node, &ns]() mutable noexcept {
			// 3.1) Perform wal-sync/force-sync for specified namespace in separated routine
			ReplicationStateV2 replState;
			Error err;
			size_t i = 0;
			for (i = 0; i < kMaxRetriesOnRoleSwitchAwait; ++i) {
				err = node.client.GetReplState(ns.name, replState);
				bool nsExists = true;
				if (err.code() == errNotFound) {
					nsExists = false;
					logPrintf(LogInfo,
							  "[cluster:replicator]%s %d:%d Namespace does not exist on remote node. Trying to get repl state for whole DB",
							  typeString(), serverId_, node.uid);
					err = node.client.GetReplState(std::string_view(), replState);
				}
				if (!bhvParam_.IsLeader() && integralError.ok()) {
					integralError = Error(errParams, "Leader was switched");
					return;
				} else if (!integralError.ok()) {
					return;
				}
				if (err.ok()) {
					statsCollector_.OnSyncStateChanged(node.uid, NodeStats::SyncState::Syncing);
					updateNodeStatus(node.uid, NodeStats::Status::Online);
					if constexpr (isClusterReplThread()) {
						if (replState.clusterStatus.role != ClusterizationStatus::Role::ClusterReplica ||
							replState.clusterStatus.leaderId != serverId_) {
							// Await transition
							logPrintf(LogTrace, "[cluster:replicator]%s %d:%d Awaiting NS role switch on remote node", typeString(),
									  serverId_, node.uid);
							loop.sleep(kRoleSwitchStepTime);
							// TODO: Check if cluster is configured on remote node
							continue;
						}
					} else {
						if (nsExists && (replState.clusterStatus.role != ClusterizationStatus::Role::SimpleReplica ||
										 replState.clusterStatus.leaderId != serverId_)) {
							logPrintf(LogTrace, "[cluster:replicator]%s %d:%d Switching role for '%s' on remote node", typeString(),
									  serverId_, node.uid, ns.name);
							err = node.client.SetClusterizationStatus(
								ns.name, ClusterizationStatus{serverId_, ClusterizationStatus::Role::SimpleReplica});
						}
					}
				} else {
					logPrintf(LogWarning, "[cluster:replicator]%s %d:%d Unable to get repl state: %s", typeString(), serverId_, node.uid,
							  err.what());
				}

				if (err.ok()) {
					if (!nsExists) {
						replState = ReplicationStateV2();
					}
					err = syncNamespace(node, ns.name, replState);
					if (!err.ok()) {
						logPrintf(LogWarning, "[cluster:replicator]%s %d:%d Namespace sync error: %s", typeString(), serverId_, node.uid,
								  err.what());
						if (err.code() == errNotFound) {
							err = Error();
							logPrintf(LogWarning, "[cluster:replicator]%s %d:%d Expecting drop namespace record for '%s'", typeString(),
									  serverId_, node.uid, ns.name);
						} else if (err.code() == errDataHashMismatch) {
							replState = ReplicationStateV2();
							err = syncNamespace(node, ns.name, replState);
							if (!err.ok()) {
								logPrintf(LogWarning,
										  "[cluster:replicator]%s %d:%d Namespace sync error (resync due to datahash missmatch): %s",
										  typeString(), serverId_, node.uid, err.what());
							}
						}
					}
				}
				if (err.ok()) {
					return;
				} else if (integralError.ok()) {
					integralError = std::move(err);
				}
				return;
			}

			if (integralError.ok()) {
				integralError = Error(errTimeout, "%d:%d Unable to sync namespace", serverId_, node.uid);
				return;
			}
		});
	}
	if constexpr (isClusterReplThread()) {
		if (!localWg.wait_count()) {
			size_t i;
			logPrintf(LogInfo, "[cluster:replicator]%s %d:%d No sync coroutines were created. Just awating DB role switch...", typeString(),
					  serverId_, node.uid);
			for (i = 0; i < kMaxRetriesOnRoleSwitchAwait; ++i) {
				ReplicationStateV2 replState;
				auto err = node.client.GetReplState(std::string_view(), replState);
				if (!err.ok()) {
					return err;
				}
				if (!bhvParam_.IsLeader()) {
					return Error(errParams, "Leader was switched");
				}
				if (replState.clusterStatus.role != ClusterizationStatus::Role::ClusterReplica ||
					replState.clusterStatus.leaderId != serverId_) {
					// Await transition
					logPrintf(LogTrace, "[cluster:replicator]%s %d:%d Awaiting DB role switch on remote node", typeString(), serverId_,
							  node.uid);
					loop.sleep(kRoleSwitchStepTime);
					// TODO: Check if cluster is configured on remote node
					continue;
				}
				break;
			}
			if (i == kMaxRetriesOnRoleSwitchAwait) {
				return Error(errTimeout, "%d:%d DB role switch waiting timeout", serverId_, node.uid);
			}
		}
	}
	localWg.wait();
	if (!integralError.ok()) {
		logPrintf(LogWarning, "[cluster:replicator]%s %d:%d Unable to sync remote namespaces: %s", typeString(), serverId_, node.uid,
				  integralError.what());
		return integralError;
	}
	updateNodeStatus(node.uid, NodeStats::Status::Online);
	statsCollector_.OnSyncStateChanged(node.uid, NodeStats::SyncState::OnlineReplication);

	// 4) Sending updates for this namespace
	const UpdateApplyStatus res = nodeUpdatesHandlingLoop(node);
	logPrintf(LogInfo, "[cluster:replicator]%s %d:%d Updates handling loop was terminated", typeString(), serverId_, node.uid);
	return res.err;
}

template <typename BehaviourParamT>
void ReplThread<BehaviourParamT>::updatesNotifier() noexcept {
	if (!terminate_) {
		for (auto& node : nodes) {
			if (node.updateNotifier->opened() && !node.updateNotifier->full()) {
				node.updateNotifier->push(true);
			}
		}
	} else {
		logPrintf(LogTrace, "[cluster:replicator]%s %d: got termination signal", typeString(), serverId_);
		DisconnectNodes();
		for (auto& node : nodes) {
			node.updateNotifier->close();
		}
		terminateCh_.close();
	}
}

template <typename BehaviourParamT>
std::tuple<bool, UpdateApplyStatus> ReplThread<BehaviourParamT>::handleNetworkCheckRecord(Node& node, UpdatesQueueT::UpdatePtr& updPtr,
																						  uint16_t offset, bool currentlyOnline,
																						  const UpdateRecord& rec) noexcept {
	bool hadActualNetworkCheck = false;
	auto& data = std::get<std::unique_ptr<NodeNetworkCheckRecord>>(rec.data);
	if (node.uid == data->nodeUid) {
		Error err;
		if (data->online != currentlyOnline) {
			logPrintf(LogTrace, "[cluster:replicator]%s %d:%d: Checking network...", typeString(), serverId_, node.uid);
			err = node.client.WithTimeout(kStatusCmdTimeout).Status(true);
			hadActualNetworkCheck = true;
		}
		updPtr->OnUpdateReplicated(node.uid, consensusCnt_, requiredReplicas_, offset, false, Error());
		return std::make_tuple(hadActualNetworkCheck, UpdateApplyStatus(std::move(err), UpdateRecord::Type::NodeNetworkCheck));
	}
	updPtr->OnUpdateReplicated(node.uid, consensusCnt_, requiredReplicas_, offset, false, Error());
	return std::make_tuple(hadActualNetworkCheck, UpdateApplyStatus(Error(), UpdateRecord::Type::NodeNetworkCheck));
}

template <typename BehaviourParamT>
Error ReplThread<BehaviourParamT>::syncNamespace(Node& node, const std::string& nsName, const ReplicationStateV2& followerState) {
	try {
		struct TmpNsGuard {
			std::string tmpNsName;
			client::CoroReindexer& client;
			int serverId;

			~TmpNsGuard() {
				if (tmpNsName.size()) {
					logPrintf(LogWarning, "[cluster:replicator]%s %d: Removing tmp ns on error: %s", typeString(), serverId, tmpNsName);
					client.WithLSN(lsn_t(0, serverId)).DropNamespace(tmpNsName);
				}
			}
		};

		coroutine::tokens_pool<bool>::token syncToken;
		if (nsSyncTokens_) {
			logPrintf(LogTrace, "[cluster:replicator]%s %d:%d:%s Awaiting sync token", typeString(), serverId_, node.uid, nsName);
			syncToken = nsSyncTokens_->await_token();
			logPrintf(LogTrace, "[cluster:replicator]%s %d:%d:%s Got sync token", typeString(), serverId_, node.uid, nsName);
		}
		if (!bhvParam_.IsLeader()) {
			return Error(errParams, "Leader was switched");
		}
		SyncTimeCounter timeCounter(SyncTimeCounter::Type::WalSync, statsCollector_);

		ReplicationStateV2 localState;
		Snapshot snapshot;
		ExtendedLsn requiredLsn(followerState.nsVersion, followerState.lastLsn);
		bool createTmpNamespace = false;
		auto client = node.client.WithTimeout(std::chrono::seconds(config_.syncTimeoutSec));
		TmpNsGuard tmpNsGuard = {std::string(), client, serverId_};

		auto err = thisNode.GetReplState(nsName, localState, RdxContext());
		if (!err.ok()) {
			if (err.code() == errNotFound) {
				if (requiredLsn.IsEmpty()) {
					logPrintf(LogInfo, "[cluster:replicator]%s %d:%d: Namespace '%s' does not exist on both follower and leader",
							  typeString(), serverId_, node.uid, nsName);
					return Error();
				}
				if (node.namespaceData[nsName].isClosed) {
					logPrintf(LogInfo, "[cluster:replicator]%s %d:%d Namespace '%s' is closed on leader. Skipping it", typeString(),
							  serverId_, node.uid, nsName);
					return Error();
				} else {
					logPrintf(LogInfo,
							  "[cluster:replicator]%s %d:%d Namespace '%s' does not exist on leader, but exist on follower. Trying to "
							  "remove it...",
							  typeString(), serverId_, node.uid, nsName);
					auto dropRes = client.WithLSN(lsn_t(0, serverId_)).DropNamespace(nsName);
					if (dropRes.ok()) {
						logPrintf(LogInfo, "[cluster:replicator]%s %d:%d Namespace '%s' was removed", typeString(), serverId_, node.uid,
								  nsName);
					} else {
						logPrintf(LogInfo, "[cluster:replicator]%s %d:%d Unable to remove namespace '%s': %s", typeString(), serverId_,
								  node.uid, nsName, dropRes.what());
						return dropRes;
					}
				}
			}
			return err;
		}
		const ExtendedLsn localLsn(localState.nsVersion, localState.lastLsn);

		logPrintf(LogInfo,
				  "[cluster:replicator]%s %d:%d ReplState for '%s': { local: { ns_version: %d, lsn: %d, data_hash: %d }, remote: { "
				  "ns_version: %d, lsn: %d, data_hash: %d } }",
				  typeString(), serverId_, node.uid, nsName, localState.nsVersion, localState.lastLsn, localState.dataHash,
				  followerState.nsVersion, followerState.lastLsn, followerState.dataHash);

		if (!requiredLsn.IsEmpty() && localLsn.IsCompatibleByNsVersion(requiredLsn)) {
			if (requiredLsn.LSN().Counter() > localLsn.LSN().Counter()) {
				logPrintf(LogWarning, "[cluster:replicator]%s %d:%d:%s unexpected follower's lsn: %d. Local lsn: %d", typeString(),
						  serverId_, node.uid, nsName, requiredLsn.LSN(), localLsn.LSN());
				requiredLsn = ExtendedLsn();
			} else if (requiredLsn.LSN().Counter() == localState.lastLsn.Counter() &&
					   requiredLsn.LSN().Server() != localState.lastLsn.Server()) {
				logPrintf(LogWarning,
						  "[cluster:replicator]%s %d:%d:%s unexpected follower's lsn: %d. Local lsn: %d. LSNs have different server ids",
						  typeString(), serverId_, node.uid, nsName, requiredLsn.LSN(), localLsn.LSN());
				requiredLsn = ExtendedLsn();
			} else if (requiredLsn.LSN() == localLsn.LSN() && followerState.dataHash != localState.dataHash) {
				logPrintf(LogWarning, "[cluster:replicator]%s %d:%d:%s Datahash missmatch. Expected: %d, actual: %d", typeString(),
						  serverId_, node.uid, nsName, localState.dataHash, followerState.dataHash);
				requiredLsn = ExtendedLsn();
			}
		}

		err = thisNode.GetSnapshot(nsName, SnapshotOpts(requiredLsn, config_.MaxWALDepthOnForceSync), snapshot, RdxContext());
		if (!err.ok()) return err;
		if (snapshot.HasRawData()) {
			logPrintf(LogInfo, "[cluster:replicator]%s %d:%d:%s Snapshot has raw data, creating tmp namespace", typeString(), serverId_,
					  node.uid, nsName);
			createTmpNamespace = true;
		} else if (snapshot.NsVersion().Server() != requiredLsn.NsVersion().Server() ||
				   snapshot.NsVersion().Counter() != requiredLsn.NsVersion().Counter()) {
			logPrintf(LogInfo, "[cluster:replicator]%s %d:%d:%s Snapshot has different ns version (%d vs %d), creating tmp namespace",
					  typeString(), serverId_, node.uid, nsName, snapshot.NsVersion(), requiredLsn.NsVersion());
			createTmpNamespace = true;
		}

		std::string_view replNsName;
		if (createTmpNamespace) {
			timeCounter.SetType(SyncTimeCounter::Type::ForceSync);
			// TODO: Allow tmp ns without storage via config
			err = client.WithLSN(lsn_t(0, serverId_))
					  .CreateTemporaryNamespace(nsName, tmpNsGuard.tmpNsName, StorageOpts().Enabled(), snapshot.NsVersion());
			if (!err.ok()) return err;
			if constexpr (std::is_same_v<BehaviourParamT, AsyncThreadParam>) {
				err = client.SetClusterizationStatus(tmpNsGuard.tmpNsName,
													 ClusterizationStatus{serverId_, ClusterizationStatus::Role::SimpleReplica});
				if (!err.ok()) return err;
			}
			replNsName = tmpNsGuard.tmpNsName;
		} else {
			replNsName = nsName;
		}
		logPrintf(LogInfo, "[cluster:replicator]%s %d:%d:%s Target ns name: %s", typeString(), serverId_, node.uid, nsName, replNsName);
		for (auto& it : snapshot) {
			if (terminate_) {
				logPrintf(LogInfo, "[cluster:replicator]%s %d:%d:%s Terminated, while syncing namespace", typeString(), serverId_, node.uid,
						  nsName);
				return Error();
			}
			if (!bhvParam_.IsLeader()) {
				return Error(errParams, "Leader was switched");
			}
			err = client.WithLSN(lsn_t(0, serverId_)).ApplySnapshotChunk(replNsName, it.Chunk());
			if (!err.ok()) {
				return err;
			}
		}
		if (createTmpNamespace) {
			logPrintf(LogTrace, "[cluster:replicator]%s %d:%d:%s Renaming: %s -> %s", typeString(), serverId_, node.uid, nsName, replNsName,
					  nsName);
			err = client.WithLSN(lsn_t(0, serverId_)).RenameNamespace(replNsName, nsName);
			if (!err.ok()) return err;
			tmpNsGuard.tmpNsName.clear();
		}

		{
			ReplicationStateV2 replState;
			err = client.GetReplState(nsName, replState);
			if (!err.ok() && err.code() != errNotFound) return err;
			logPrintf(LogInfo,
					  "[cluster:replicator]%s %d:%d:%s Sync done. { snapshot: { ns_version: %d, lsn: %d, data_hash: %d }, remote: { "
					  "ns_version: %d, lsn: %d, data_hash: %d } }",
					  typeString(), serverId_, node.uid, nsName, snapshot.NsVersion(), snapshot.LastLSN(), snapshot.ExpectedDatahash(),
					  replState.nsVersion, replState.lastLsn, replState.dataHash);

			node.namespaceData[nsName].latestLsn = ExtendedLsn(replState.nsVersion, replState.lastLsn);

			const bool dataMissmatch = (!snapshot.LastLSN().isEmpty() && snapshot.LastLSN() != replState.lastLsn) ||
									   (!snapshot.NsVersion().isEmpty() && snapshot.NsVersion() != replState.nsVersion);
			if (dataMissmatch || snapshot.ExpectedDatahash() != replState.dataHash) {
				logPrintf(LogInfo, "[cluster:replicator]%s %d:%d:%s Snapshot dump on data missmatch: %s", typeString(), serverId_, node.uid,
						  nsName, snapshot.Dump());
				return Error(errDataHashMismatch,
							 "%d:%d:%s: Datahash missmatcher after sync. Actual: { data_hash: %d, ns_version: %d, lsn: %d }; expected: { "
							 "data_hash: %d, ns_version: %d, lsn: %d }",
							 serverId_, node.uid, nsName, replState.dataHash, replState.nsVersion, replState.lastLsn,
							 snapshot.ExpectedDatahash(), snapshot.NsVersion(), snapshot.LastLSN());
			}
		}
	} catch (Error& err) {
		return err;
	}
	return Error();
}

template <typename BehaviourParamT>
UpdateApplyStatus ReplThread<BehaviourParamT>::nodeUpdatesHandlingLoop(Node& node) noexcept {
	logPrintf(LogInfo, "[cluster:replicator]%s %d:%d Start updates handling loop", typeString(), serverId_, node.uid);

	struct Context {
		UpdatesQueueT::UpdatePtr updPtr;
		NamespaceData* nsData;
		uint16_t offset;
	};
	UpdatesChT& updatesNotifier = *node.updateNotifier;
	bool requireReelections = false;

	UpdatesBatcher<UpdatesQueueT::UpdateT::Value, Context> batcher(
		loop, config_.BatchingRoutinesCount,
		[this, &node](const UpdatesQueueT::UpdateT::Value& upd, Context& ctx) {
			auto& it = upd.Data();
			auto& nsName = it.GetNsName();
			auto& nsData = *ctx.nsData;
			logPrintf(
				LogTrace,
				"[cluster:replicator]%s %d:%d:%s Applying update with type %d (batched), id: %d, ns version: %d, lsn: %d, last synced ns "
				"version: %d, last synced lsn: %d",
				typeString(), serverId_, node.uid, nsName, int(it.type), ctx.updPtr->ID() + ctx.offset, it.extLsn.NsVersion(),
				it.extLsn.LSN(), nsData.latestLsn.NsVersion(), nsData.latestLsn.LSN());
			return applyUpdate(it, node, *ctx.nsData);
		},
		[this, &node, &requireReelections](const UpdatesQueueT::UpdateT::Value& upd, const UpdateApplyStatus& res, Context&& ctx) {
			auto& it = upd.Data();
			ctx.nsData->UpdateLsnOnRecord(it);
			auto counters = upd.GetCounters();
			logPrintf(LogTrace, "[cluster:replicator]%s %d:%d:%s Apply update (lsn: %d, id: %d) result: %s. Replicas: %d", typeString(),
					  serverId_, node.uid, it.GetNsName(), it.extLsn.LSN(), ctx.updPtr->ID() + ctx.offset,
					  (res.err.ok() ? "OK" : "ERROR:" + res.err.what()), counters.replicas + 1);
			const auto replRes = ctx.updPtr->OnUpdateReplicated(node.uid, consensusCnt_, requiredReplicas_, ctx.offset,
																it.emmiterServerId == node.serverId, res.err);
			if (res.err.ok()) {
				bhvParam_.OnUpdateSucceed(node.uid, ctx.updPtr->ID() + ctx.offset);
			}
			requireReelections = requireReelections || (replRes == ReplicationResult::Error);
		},
		[](Error&& err, const UpdatesQueueT::UpdateT::Value& upd) { return UpdateApplyStatus(std::move(err), upd.Data().type); });

	while (!terminate_) {
		UpdateApplyStatus res;
		UpdatesQueueT::UpdatePtr updatePtr;
		do {
			if (node.requireResync) {
				logPrintf(LogTrace, "[cluster:replicator]%s %d:%d Node is requiring resync", typeString(), serverId_, node.uid);
				return UpdateApplyStatus();
			}
			if (!bhvParam_.IsLeader()) {
				logPrintf(LogTrace, "[cluster:replicator]%s %d:%d: Is not leader anymore", typeString(), serverId_, node.uid);
				return UpdateApplyStatus();
			}
			updatePtr = updates_->Read(node.nextUpdateId, std::this_thread::get_id());
			if (!updatePtr) {
				break;
			}
			if (updatePtr->IsUpdatesDropBlock) {
				const auto nextUpdateID = updatePtr->ID() + 1;
				logPrintf(LogInfo, "[cluster:replicator]%s %d:%d Got updates drop block. Last replicated id: %d, Next update id: %d",
						  typeString(), serverId_, node.uid, node.nextUpdateId, nextUpdateID);
				node.nextUpdateId = nextUpdateID;
				statsCollector_.OnUpdateApplied(node.uid, updatePtr->ID());
				return UpdateApplyStatus(Error(), UpdateRecord::Type::ResyncOnUpdatesDrop);
			}
			logPrintf(LogTrace, "[cluster:replicator]%s %d:%d Got new update. Next update id: %d", typeString(), serverId_, node.uid,
					  node.nextUpdateId);
			node.nextUpdateId = updatePtr->ID() > node.nextUpdateId ? updatePtr->ID() : node.nextUpdateId;
			for (uint16_t offset = node.nextUpdateId - updatePtr->ID(); offset < updatePtr->Count(); ++offset) {
				if (updatePtr->IsInvalidated()) {
					logPrintf(LogInfo, "[cluster:replicator]%s %d:%d Current update is invalidated", typeString(), serverId_, node.uid);
					break;
				}
				++node.nextUpdateId;
				auto& upd = updatePtr->GetUpdate(offset);
				auto& it = upd.Data();
				if (it.IsNetworkCheckRecord()) {
					[[maybe_unused]] bool v;
					std::tie(v, res) = handleNetworkCheckRecord(node, updatePtr, offset, true, it);
					if (!res.err.ok()) {
						break;
					}
					continue;
				}
				const std::string& nsName = it.GetNsName();
				if constexpr (!isClusterReplThread()) {
					if (!bhvParam_.IsNamespaceInConfig(node.uid, nsName)) {
						updatePtr->OnUpdateReplicated(node.uid, consensusCnt_, requiredReplicas_, offset, false, Error());
						bhvParam_.OnUpdateSucceed(node.uid, updatePtr->ID() + offset);
						continue;
					}
				}

				if (it.type == UpdateRecord::Type::AddNamespace) {
					bhvParam_.OnNewNsAppearance(nsName);
				}

				auto& nsData = node.namespaceData[nsName];

				const bool isOutdatedRecord = !it.extLsn.HasNewerCounterThan(nsData.latestLsn) || nsData.latestLsn.IsEmpty();
				if ((!it.IsDbRecord() && isOutdatedRecord) || it.IsEmptyRecord()) {
					logPrintf(
						LogTrace,
						"[cluster:replicator]%s %d:%d:%s Skipping update with type %d, id: %d, ns version: %d, lsn: %d, last synced ns "
						"version: %d, last synced lsn: %d",
						typeString(), serverId_, node.uid, nsName, int(it.type), updatePtr->ID() + offset, it.extLsn.NsVersion(),
						it.extLsn.LSN(), nsData.latestLsn.NsVersion(), nsData.latestLsn.LSN());
					updatePtr->OnUpdateReplicated(node.uid, consensusCnt_, requiredReplicas_, offset, it.emmiterServerId == node.serverId,
												  Error());
					continue;
				}
				if (nsData.tx.IsFree() && it.IsRequiringTx()) {
					res = UpdateApplyStatus(Error(errTxDoesNotExist, "Update requires tx. ID: %d, lsn: %d, type: %d",
												  updatePtr->ID() + offset, it.extLsn.LSN(), int(it.type)));
					--node.nextUpdateId;  // Have to read this update again
					break;
				}
				if (nsData.requiresTmUpdate && (it.IsBatchingAllowed() || it.IsTxBeginning())) {
					nsData.requiresTmUpdate = false;
					// Explicitly update tm for this namespace
					// TODO: Find better solution?
					logPrintf(LogTrace, "[cluster:replicator]%s %d:%d:%s Executing select to update tm...", typeString(), serverId_,
							  node.uid, nsName);
					client::CoroQueryResults qr;
					res = node.client.WithShardId(ShardingKeyType::ProxyOff, false).Select(Query(nsName).Limit(0), qr);
					if (!res.err.ok()) {
						--node.nextUpdateId;  // Have to read this update again
						break;
					}
				}
				if (it.IsBatchingAllowed()) {
					res = batcher.Batch(upd, Context{updatePtr, &nsData, offset});
					if (!res.err.ok()) {
						--node.nextUpdateId;  // Have to read this update again
						break;
					}
					continue;
				} else {
					res = batcher.AwaitBatchedUpdates();
					if (!res.err.ok()) {
						--node.nextUpdateId;  // Have to read this update again
						break;
					}

					logPrintf(
						LogTrace,
						"[cluster:replicator]%s %d:%d:%s Applying update with type %d (no batching), id: %d, ns version: %d, lsn: %d, "
						"last synced ns "
						"version: %d, last synced lsn: %d",
						typeString(), serverId_, node.uid, nsName, int(it.type), updatePtr->ID() + offset, it.extLsn.NsVersion(),
						it.extLsn.LSN(), nsData.latestLsn.NsVersion(), nsData.latestLsn.LSN());
					res = applyUpdate(it, node, nsData);
					logPrintf(LogTrace,
							  "[cluster:replicator]%s %d:%d:%s Apply update result (id: %d, ns version: %d, lsn: %d): %s. Replicas: %d",
							  typeString(), serverId_, node.uid, nsName, updatePtr->ID() + offset, it.extLsn.NsVersion(), it.extLsn.LSN(),
							  (res.err.ok() ? "OK" : "ERROR:" + res.err.what()), upd.GetCounters().replicas + 1);

					const auto replRes = updatePtr->OnUpdateReplicated(node.uid, consensusCnt_, requiredReplicas_, offset,
																	   it.emmiterServerId == node.serverId, res.err);
					if (res.err.ok()) {
						nsData.UpdateLsnOnRecord(it);
						bhvParam_.OnUpdateSucceed(node.uid, updatePtr->ID() + offset);
						nsData.requiresTmUpdate = it.IsRequiringTmUpdate();
					} else {
						requireReelections = requireReelections || (replRes == ReplicationResult::Error);
					}
				}

				if (!res.err.ok()) {
					break;
				} else if (res.IsHaveToResync<BehaviourParamT>()) {
					logPrintf(LogInfo, "[cluster:replicator]%s %d:%d Resync was requested", typeString(), serverId_, node.uid);
					break;
				}
			}

			if (batcher.BatchedUpdatesCount()) {
				assert(!res.IsHaveToResync<BehaviourParamT>());	 // In this cases batchedUpdatesCount has to be 0
				auto batchedRes = batcher.AwaitBatchedUpdates();
				if (res.err.ok()) {
					res = std::move(batchedRes);
				}
			}

			if (requireReelections) {
				logPrintf(LogWarning, "[cluster:replicator]%s %d:%d Requesting leader reelection on error: %s", typeString(), serverId_,
						  node.uid, res.err.what());
				requireReelections = false;
				bhvParam_.OnUpdateReplicationFailure();
				return res;
			}
			if (!res.err.ok() || res.IsHaveToResync<BehaviourParamT>()) {
				return res;
			}
		} while (!terminate_);

		if (updatesNotifier.empty()) {
			bhvParam_.OnAllUpdatesReplicated(node.uid, int64_t(node.nextUpdateId) - 1);
			logPrintf(LogTrace, "[cluster:replicator]%s %d:%d Awaiting updates...", typeString(), serverId_, node.uid);
		}
		updatesNotifier.pop();
	}
	if (terminate_) {
		logPrintf(LogTrace, "[cluster:replicator]%s %d: updates handling loop was terminated", typeString(), serverId_);
	}
	return Error();
}

template <typename BehaviourParamT>
bool ReplThread<BehaviourParamT>::handleUpdatesWithError(Node& node, const Error& err) {
	UpdatesChT& updatesNotifier = *node.updateNotifier;
	UpdatesQueueT::UpdatePtr updatePtr;
	bool hadErrorOnLastUpdate = false;

	if (!updatesNotifier.empty()) updatesNotifier.pop();
	do {
		updatePtr = updates_->Read(node.nextUpdateId, std::this_thread::get_id());
		if (!updatePtr) {
			break;
		}
		if (updatePtr->IsUpdatesDropBlock) {
			node.nextUpdateId = updatePtr->ID() + 1;
			continue;
		}
		node.nextUpdateId = updatePtr->ID() > node.nextUpdateId ? updatePtr->ID() : node.nextUpdateId;
		for (uint16_t offset = node.nextUpdateId - updatePtr->ID(); offset < updatePtr->Count(); ++offset) {
			++node.nextUpdateId;

			auto& upd = updatePtr->GetUpdate(offset);
			auto& it = upd.Data();
			if (it.IsNetworkCheckRecord()) {
				const auto [hadActualNetworkCheck, res] = handleNetworkCheckRecord(node, updatePtr, offset, false, it);
				if (hadActualNetworkCheck && res.err.ok()) {
					return true;  // Retry sync after succeed network check
				}
				continue;
			}
			const std::string& nsName = it.GetNsName();
			if (!bhvParam_.IsNamespaceInConfig(node.uid, nsName)) continue;

			if (it.type == UpdateRecord::Type::AddNamespace || it.type == UpdateRecord::Type::DropNamespace) {
				node.namespaceData[nsName].isClosed = false;
				bhvParam_.OnNewNsAppearance(nsName);
			} else if (it.type == UpdateRecord::Type::CloseNamespace) {
				node.namespaceData[nsName].isClosed = true;
			}

			if (updatePtr->IsInvalidated()) {
				logPrintf(LogTrace, "[cluster:replicator]%s %d:%d:%s Update %d was invalidated", typeString(), serverId_, node.uid, nsName,
						  updatePtr->ID());
				break;
			}

			assert(it.emmiterServerId != serverId_);
			const bool isEmmiter = it.emmiterServerId == node.serverId;
			if (isEmmiter) {
				--node.nextUpdateId;
				return true;  // Retry sync after receiving update from offline node
			}
			const auto replRes = updatePtr->OnUpdateReplicated(
				node.uid, consensusCnt_, requiredReplicas_, offset, isEmmiter,
				Error(errUpdateReplication, "Unable to send update to enough amount of replicas. Last error: %s", err.what()));

			if (replRes == ReplicationResult::Error && !hadErrorOnLastUpdate) {
				hadErrorOnLastUpdate = true;
				logPrintf(LogWarning, "[cluster:replicator]%s %d:%d Requesting leader reelection on error: %s", typeString(), serverId_,
						  node.uid, err.what());
				bhvParam_.OnUpdateReplicationFailure();
			}

			auto counters = upd.GetCounters();
			logPrintf(LogTrace,
					  "[cluster:replicator]%s %d:%d:%s Dropping update with error: %s. Type %d, ns version: %d, lsn: %d, emmiter: %d. "
					  "Required: "
					  "%d, succeed: "
					  "%d, failed: %d, replicas: %d",
					  typeString(), serverId_, node.uid, nsName, err.what(), int(it.type), it.extLsn.NsVersion(), it.extLsn.LSN(),
					  (isEmmiter ? node.serverId : it.emmiterServerId), consensusCnt_, counters.approves, counters.errors,
					  counters.replicas + 1);
		}
	} while (!terminate_);
	return false;
}

template <typename BehaviourParamT>
Error ReplThread<BehaviourParamT>::checkIfReplicationAllowed(Node& node, LogLevel& logLevel) {
	if constexpr (!isClusterReplThread()) {
		auto err = bhvParam_.CheckReplicationMode(node.uid);
		if (!err.ok()) {
			logLevel = LogTrace;
			return err;
		}
		logLevel = LogError;
		logPrintf(LogWarning, "[cluster:replicator]%s %d:%d Checking if replication is allowed for this node", typeString(), serverId_,
				  node.uid);
		const Query q = Query(std::string(kReplicationStatsNamespace)).Where("type", CondEq, Variant(cluster::kClusterReplStatsType));
		client::CoroQueryResults qr;
		err = node.client.Select(q, qr);
		if (!err.ok()) return err;

		if (qr.Count() == 1) {
			WrSerializer wser;
			err = qr.begin().GetJSON(wser, false);
			if (!err.ok()) return err;

			ReplicationStats stats;
			err = stats.FromJSON(wser.Slice());
			if (!err.ok()) return err;

			if (stats.nodeStats.size()) {
				if (stats.nodeStats[0].namespaces.size()) {
					for (const auto& ns : stats.nodeStats[0].namespaces) {
						if (bhvParam_.IsNamespaceInConfig(node.uid, ns)) {
							return Error(
								errParams,
								"Replication namespace '%s' is present on target node in sync cluster config. Target namespace can "
								"not be a part of sync cluster",
								ns);
						}
					}
				} else {
					return Error(errParams,
								 "Target node has sync cluster config over all the namespaces. Target namespace can "
								 "not be a part of sync cluster");
				}
			}
		}
	} else {
		(void)node;
		(void)logLevel;
	}
	return Error();
}

template <typename BehaviourParamT>
UpdateApplyStatus ReplThread<BehaviourParamT>::applyUpdate(const UpdateRecord& rec, ReplThread::Node& node,
														   ReplThread::NamespaceData& nsData) noexcept {
	auto lsn = rec.extLsn.LSN();
	std::string_view nsName = rec.GetNsName();
	auto& client = node.client;
	try {
		switch (rec.type) {
			case UpdateRecord::Type::ItemUpdate:
			case UpdateRecord::Type::ItemUpsert:
			case UpdateRecord::Type::ItemDelete:
			case UpdateRecord::Type::ItemInsert: {
				auto& data = std::get<std::unique_ptr<ItemReplicationRecord>>(rec.data);
				client::Item item = client.NewItem(nsName);
				auto err = item.Unsafe().FromCJSON(data->cjson.Slice());
				assert(!item.IsTagsUpdated());
				if (err.ok()) {
					switch (rec.type) {
						case UpdateRecord::Type::ItemUpdate:
							return UpdateApplyStatus(client.WithLSN(lsn).Update(nsName, item), rec.type);
						case UpdateRecord::Type::ItemUpsert:
							return UpdateApplyStatus(client.WithLSN(lsn).Upsert(nsName, item), rec.type);
						case UpdateRecord::Type::ItemDelete:
							return UpdateApplyStatus(client.WithLSN(lsn).Delete(nsName, item), rec.type);
						case UpdateRecord::Type::ItemInsert:
							return UpdateApplyStatus(client.WithLSN(lsn).Insert(nsName, item), rec.type);
						default:
							std::abort();
					}
				}
				return UpdateApplyStatus(std::move(err), rec.type);
			}
			case UpdateRecord::Type::IndexAdd: {
				auto& data = std::get<std::unique_ptr<IndexReplicationRecord>>(rec.data);
				return UpdateApplyStatus(client.WithLSN(lsn).AddIndex(nsName, data->idef), rec.type);
			}
			case UpdateRecord::Type::IndexDrop: {
				auto& data = std::get<std::unique_ptr<IndexReplicationRecord>>(rec.data);
				return UpdateApplyStatus(client.WithLSN(lsn).DropIndex(nsName, data->idef), rec.type);
			}
			case UpdateRecord::Type::IndexUpdate: {
				auto& data = std::get<std::unique_ptr<IndexReplicationRecord>>(rec.data);
				return UpdateApplyStatus(client.WithLSN(lsn).UpdateIndex(nsName, data->idef), rec.type);
			}
			case UpdateRecord::Type::PutMeta: {
				auto& data = std::get<std::unique_ptr<MetaReplicationRecord>>(rec.data);
				return UpdateApplyStatus(client.WithLSN(lsn).PutMeta(nsName, data->key, data->value), rec.type);
			}
			case UpdateRecord::Type::PutMetaTx: {
				if (nsData.tx.IsFree()) {
					return UpdateApplyStatus(Error(errLogic, "Tx is empty"), rec.type);
				}
				auto& data = std::get<std::unique_ptr<MetaReplicationRecord>>(rec.data);
				return UpdateApplyStatus(nsData.tx.PutMeta(data->key, data->value, lsn), rec.type);
			}
			case UpdateRecord::Type::UpdateQuery: {
				auto& data = std::get<std::unique_ptr<QueryReplicationRecord>>(rec.data);
				client::CoroQueryResults qr;
				Query q;
				q.FromSQL(data->sql);
				return UpdateApplyStatus(client.WithLSN(lsn).Update(q, qr), rec.type);
			}
			case UpdateRecord::Type::DeleteQuery: {
				auto& data = std::get<std::unique_ptr<QueryReplicationRecord>>(rec.data);
				client::CoroQueryResults qr;
				Query q;
				q.FromSQL(data->sql);
				return UpdateApplyStatus(client.WithLSN(lsn).Delete(q, qr), rec.type);
			}
			case UpdateRecord::Type::SetSchema: {
				auto& data = std::get<std::unique_ptr<SchemaReplicationRecord>>(rec.data);
				return UpdateApplyStatus(client.WithLSN(lsn).SetSchema(nsName, data->schema), rec.type);
			}
			case UpdateRecord::Type::Truncate: {
				return UpdateApplyStatus(client.WithLSN(lsn).TruncateNamespace(nsName), rec.type);
			}
			case UpdateRecord::Type::BeginTx: {
				if (!nsData.tx.IsFree()) {
					return UpdateApplyStatus(Error(errLogic, "Tx is not empty"), rec.type);
				}
				nsData.tx = node.client.WithLSN(lsn).NewTransaction(nsName);
				return UpdateApplyStatus(Error(nsData.tx.Status()), rec.type);
			}
			case UpdateRecord::Type::CommitTx: {
				if (nsData.tx.IsFree()) {
					return UpdateApplyStatus(Error(errLogic, "Tx is empty"), rec.type);
				}
				client::CoroQueryResults qr;
				return UpdateApplyStatus(node.client.WithLSN(lsn).CommitTransaction(nsData.tx, qr), rec.type);
			}
			case UpdateRecord::Type::ItemUpdateTx:
			case UpdateRecord::Type::ItemUpsertTx:
			case UpdateRecord::Type::ItemDeleteTx:
			case UpdateRecord::Type::ItemInsertTx: {
				if (nsData.tx.IsFree()) {
					return UpdateApplyStatus(Error(errLogic, "Tx is empty"), rec.type);
				}
				auto& data = std::get<std::unique_ptr<ItemReplicationRecord>>(rec.data);
				client::Item item = nsData.tx.NewItem();
				auto err = item.Unsafe().FromCJSON(data->cjson.Slice());
				assert(!item.IsTagsUpdated());
				if (err.ok()) {
					switch (rec.type) {
						case UpdateRecord::Type::ItemUpdateTx:
							return UpdateApplyStatus(nsData.tx.Update(std::move(item), lsn), rec.type);
						case UpdateRecord::Type::ItemUpsertTx:
							return UpdateApplyStatus(nsData.tx.Upsert(std::move(item), lsn), rec.type);
						case UpdateRecord::Type::ItemDeleteTx:
							return UpdateApplyStatus(nsData.tx.Delete(std::move(item), lsn), rec.type);
						case UpdateRecord::Type::ItemInsertTx:
							return UpdateApplyStatus(nsData.tx.Insert(std::move(item), lsn), rec.type);
						default:
							std::abort();
					}
				}
				return UpdateApplyStatus(std::move(err), rec.type);
			}
			case UpdateRecord::Type::UpdateQueryTx:
			case UpdateRecord::Type::DeleteQueryTx: {
				if (nsData.tx.IsFree()) {
					return UpdateApplyStatus(Error(errLogic, "Tx is empty"), rec.type);
				}
				auto& data = std::get<std::unique_ptr<QueryReplicationRecord>>(rec.data);
				Query q;
				q.FromSQL(data->sql);
				return UpdateApplyStatus(nsData.tx.Modify(std::move(q), lsn), rec.type);
			}
			case UpdateRecord::Type::SetTagsMatcherTx: {
				if (nsData.tx.IsFree()) {
					return UpdateApplyStatus(Error(errLogic, "Tx is empty"), rec.type);
				}
				auto& data = std::get<std::unique_ptr<TagsMatcherReplicationRecord>>(rec.data);
				TagsMatcher tm = data->tm;
				return UpdateApplyStatus(nsData.tx.SetTagsMatcher(std::move(tm), lsn), rec.type);
			}
			case UpdateRecord::Type::AddNamespace: {
				auto& data = std::get<std::unique_ptr<AddNamespaceReplicationRecord>>(rec.data);
				const auto sid = rec.extLsn.NsVersion().Server();
				auto err =
					client.WithLSN(lsn_t(0, sid)).AddNamespace(data->def, NsReplicationOpts{{data->stateToken}, rec.extLsn.NsVersion()});
				if (err.ok() && nsData.isClosed) {
					nsData.isClosed = false;
					logPrintf(LogTrace, "[cluster:replicator]%s %d:%d:%s Namespace is closed on leader. Scheduling resync for followers",
							  typeString(), serverId_, node.uid, nsName);
					return UpdateApplyStatus(Error(), UpdateRecord::Type::ResyncNamespaceGeneric);	// Perform resync on ns reopen
				}
				nsData.isClosed = false;
				if constexpr (!isClusterReplThread()) {
					if (err.ok()) {
						err = client.SetClusterizationStatus(nsName,
															 ClusterizationStatus{serverId_, ClusterizationStatus::Role::SimpleReplica});
					}
				}
				return UpdateApplyStatus(std::move(err), rec.type);
			}
			case UpdateRecord::Type::DropNamespace: {
				lsn.SetServer(serverId_);
				auto err = client.WithLSN(lsn).DropNamespace(nsName);
				nsData.isClosed = false;
				if (!err.ok() && err.code() == errNotFound) {
					return UpdateApplyStatus(Error(), rec.type);
				}
				return UpdateApplyStatus(std::move(err), rec.type);
			}
			case UpdateRecord::Type::CloseNamespace: {
				nsData.isClosed = true;
				logPrintf(LogTrace, "[cluster:replicator]%s %d:%d:%s Namespace was closed on leader", typeString(), serverId_, node.uid,
						  nsName);
				return UpdateApplyStatus(Error(), rec.type);
			}
			case UpdateRecord::Type::RenameNamespace: {
				assert(false);	// TODO: Rename is not supported yet
				//				auto& data = std::get<std::unique_ptr<RenameNamespaceReplicationRecord>>(rec.data);
				//				lsn.SetServer(serverId);
				//				return client.WithLSN(lsn).RenameNamespace(nsName, data->dstNsName);
				return UpdateApplyStatus(Error(), rec.type);
			}
			case UpdateRecord::Type::ResyncNamespaceGeneric:
			case UpdateRecord::Type::ResyncNamespaceLeaderInit:
				return UpdateApplyStatus(Error(), rec.type);
			case UpdateRecord::Type::SetTagsMatcher: {
				auto& data = std::get<std::unique_ptr<TagsMatcherReplicationRecord>>(rec.data);
				TagsMatcher tm = data->tm;
				return UpdateApplyStatus(client.WithLSN(lsn).SetTagsMatcher(nsName, std::move(tm)), rec.type);
			}
			default:
				std::abort();
		}
	} catch (std::bad_variant_access&) {
		assert(false);
	} catch (const fmt::internal::RuntimeError& err) {
		return Error(errLogic, err.what());
	} catch (const spdlog::spdlog_ex& err) {
		return Error(errLogic, err.what());
	} catch (Error err) {
		return UpdateApplyStatus(std::move(err));
	} catch (const std::exception& err) {
		return Error(errLogic, err.what());
	} catch (...) {
		assert(false);
	}
	return Error();
}

template <typename BehaviourParamT>
constexpr std::string_view ReplThread<BehaviourParamT>::typeString() noexcept {
	using namespace std::string_view_literals;
	if constexpr (isClusterReplThread()) {
		return "[sync]"sv;
	} else {
		return "[async]"sv;
	}
}

template class ReplThread<ClusterThreadParam>;
template void ReplThread<ClusterThreadParam>::Run<ClusterNodeConfig>(ReplThreadConfig,
																	 const std::vector<std::pair<uint32_t, ClusterNodeConfig>>&, size_t,
																	 size_t);
template class ReplThread<AsyncThreadParam>;
template void ReplThread<AsyncThreadParam>::Run<AsyncReplNodeConfig>(ReplThreadConfig,
																	 const std::vector<std::pair<uint32_t, AsyncReplNodeConfig>>&, size_t,
																	 size_t);

}  // namespace cluster
}  // namespace reindexer
