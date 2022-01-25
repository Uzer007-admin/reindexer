#pragma once

#include "cluster/stats/synchronizationlist.h"
#include "replicationthread.h"

namespace reindexer {
namespace cluster {

class ClusterThreadParam {
public:
	using NsNamesHashSetT = fast_hash_set<string, nocase_hash_str, nocase_equal_str>;

	ClusterThreadParam(const NsNamesHashSetT *namespaces, coroutine::channel<bool> &ch, SharedSyncState<> &st,
					   SynchronizationList &syncList, std::function<void()> cb)
		: namespaces_(namespaces),
		  leadershipAwaitCh_(ch),
		  sharedSyncState_(st),
		  requestElectionsRestartCb_(std::move(cb)),
		  syncList_(syncList) {
		assert(namespaces_);
	}

	bool IsLeader() const noexcept { return !leadershipAwaitCh_.opened(); }
	void AwaitReplPermission() { leadershipAwaitCh_.pop(); }
	void OnNewNsAppearance(const std::string &ns) { sharedSyncState_.MarkSynchronized(ns); }
	void OnUpdateReplicationFailure() {
		if (sharedSyncState_.GetRolesPair().second.role == RaftInfo::Role::Leader) {
			requestElectionsRestartCb_();
		}
	}
	bool IsNamespaceInConfig(size_t, std::string_view ns) const noexcept {
		return namespaces_->empty() || (namespaces_->find(ns) != namespaces_->end());
	}
	void OnNodeBecameUnsynchonized(uint32_t nodeId) { syncList_.MarkUnsynchonized(nodeId); }
	void OnAllUpdatesReplicated(uint32_t nodeId, int64_t lastUpdateID) { syncList_.MarkSynchronized(nodeId, lastUpdateID); }
	void OnUpdateSucceed(uint32_t nodeId, int64_t lastUpdateID) { syncList_.MarkSynchronized(nodeId, lastUpdateID); }

private:
	const NsNamesHashSetT *namespaces_;
	coroutine::channel<bool> &leadershipAwaitCh_;
	SharedSyncState<> &sharedSyncState_;
	std::function<void()> requestElectionsRestartCb_;
	SynchronizationList &syncList_;
};

class ClusterReplThread {
public:
	using NsNamesHashSetT = fast_hash_set<string, nocase_hash_str, nocase_equal_str>;

	ClusterReplThread(int serverId, ReindexerImpl &thisNode, const NsNamesHashSetT *namespaces,
					  std::shared_ptr<UpdatesQueue<UpdateRecord>> q, SharedSyncState<> &, SynchronizationList &,
					  std::function<void()> requestElectionsRestartCb, ReplicationStatsCollector);
	~ClusterReplThread();
	void Run(ReplThreadConfig config, std::vector<std::pair<uint32_t, ClusterNodeConfig>> &&nodesList, size_t totalNodesCount);
	void SendTerminate() noexcept;
	void AwaitTermination();
	void OnRoleSwitch();

private:
	std::thread th;
	coroutine::channel<bool> leadershipAwaitCh;
	coroutine::channel<RaftInfo::Role> nextRoleCh;
	net::ev::async roleSwitchAsync_;
	ReplThread<ClusterThreadParam> base_;
	SharedSyncState<> &sharedSyncState_;
	std::chrono::time_point<std::chrono::high_resolution_clock> roleSwitchTm_;
};

}  // namespace cluster
}  // namespace reindexer