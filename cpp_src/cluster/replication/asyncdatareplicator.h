#pragma once

#include "asyncreplthread.h"
#include "core/dbconfig.h"

#include "updatesqueuepair.h"

namespace reindexer {
namespace cluster {

class AsyncDataReplicator {
public:
	using NsNamesHashSetT = fast_hash_set<string, nocase_hash_str, nocase_equal_str>;
	using UpdatesQueueT = UpdatesQueuePair<UpdateRecord>;

	AsyncDataReplicator(UpdatesQueueT &, ReindexerImpl &);

	void Configure(AsyncReplConfigData config);
	void Configure(ReplicationConfigData config);
	bool IsExpectingStartup() const noexcept;
	void Run();
	void Stop(bool resetConfig = false);
	const std::optional<AsyncReplConfigData> &Config() const noexcept { return config_; }
	ReplicationStats GetReplicationStats() const;

private:
	bool isExpectingStartup() const noexcept;
	size_t threadsCount() const noexcept {
		return config_.has_value() && config_->replThreadsCount > 0 ? config_->replThreadsCount : kDefaultReplThreadCount;
	}
	bool isRunning() const noexcept { return replThreads_.size(); }
	void stop();
	NsNamesHashSetT getLocalNamespaces();
	static NsNamesHashSetT getMergedNsConfig(const AsyncReplConfigData &config);

	ReplicationStatsCollector statsCollector_;
	mutable std::mutex mtx_;
	UpdatesQueueT &updatesQueue_;
	ReindexerImpl &thisNode_;
	std::deque<AsyncReplThread> replThreads_;
	std::optional<AsyncReplConfigData> config_;
	std::optional<ReplicationConfigData> baseConfig_;
	static constexpr int kDefaultReplThreadCount = 4;
};

}  // namespace cluster
}  // namespace reindexer