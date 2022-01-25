#include "clusterreplthread.h"
#include "client/snapshot.h"
#include "core/namespace/namespacestat.h"
#include "core/reindexerimpl.h"
#include "net/cproto/cproto.h"

namespace reindexer {
namespace cluster {

ClusterReplThread::ClusterReplThread(int serverId, ReindexerImpl& thisNode, const NsNamesHashSetT* namespaces,
									 std::shared_ptr<UpdatesQueue<UpdateRecord>> q, SharedSyncState<>& syncState,
									 SynchronizationList& syncList, std::function<void()> requestElectionsRestartCb,
									 ReplicationStatsCollector statsCollector)
	: base_(serverId, thisNode, std::move(q),
			ClusterThreadParam(namespaces, leadershipAwaitCh, syncState, syncList, std::move(requestElectionsRestartCb)), statsCollector),
	  sharedSyncState_(syncState) {
	roleSwitchAsync_.set(base_.loop);
	roleSwitchAsync_.set([this](net::ev::async& watcher) {
		watcher.loop.spawn([this]() noexcept {
			if (base_.Terminated()) {
				leadershipAwaitCh.close();
				return;
			}
			auto newState = sharedSyncState_.CurrentRole();
			base_.SetNodesRequireResync();
			if (newState.role == RaftInfo::Role::Leader) {
				if (leadershipAwaitCh.opened() && sharedSyncState_.IsInitialSyncDone()) {
					leadershipAwaitCh.close();
				}
			} else {
				if (!leadershipAwaitCh.opened()) {
					leadershipAwaitCh.reopen();
				}
				base_.DisconnectNodes();
			}
		});
	});
}

ClusterReplThread::~ClusterReplThread() {
	if (th.joinable()) {
		SendTerminate();
		th.join();
	}
}

void ClusterReplThread::Run(ReplThreadConfig config, std::vector<std::pair<uint32_t, ClusterNodeConfig>>&& nodesList,
							size_t totalNodesCount) {
	assert(!th.joinable());
	roleSwitchAsync_.start();
	th = std::thread([this, config = std::move(config), nodesList = std::move(nodesList), totalNodesCount] {
		base_.Run(std::move(config), nodesList, GetConsensusForN(totalNodesCount), totalNodesCount - 1);

		roleSwitchAsync_.stop();
	});
}

void ClusterReplThread::SendTerminate() noexcept {
	base_.SetTerminate(true);
	// This will close channels
	roleSwitchAsync_.send();
}

void ClusterReplThread::AwaitTermination() {
	assert(base_.Terminated() == true);
	th.join();
	base_.SetTerminate(false);
}

void ClusterReplThread::OnRoleSwitch() { roleSwitchAsync_.send(); }

}  // namespace cluster
}  // namespace reindexer