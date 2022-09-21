#include "client/coroqueryresults.h"
#include "client/itemimpl.h"
#include "client/namespace.h"
#include "core/keyvalue/p_string.h"
#include "core/queryresults/additionaldatasource.h"
#include "net/cproto/coroclientconnection.h"
#include "server/rpcqrwatcher.h"
#include "tools/logger.h"

namespace reindexer {
namespace client {

using namespace reindexer::net;

CoroQueryResults::~CoroQueryResults() {
	if (holdsRemoteData()) {
		i_.conn_->Call({cproto::kCmdCloseResults, i_.requestTimeout_, milliseconds(0), lsn_t(), -1, ShardingKeyType::NotSetShard, nullptr,
						false, i_.sessionTs_},
					   i_.queryID_.main, i_.queryID_.uid);
	}
}

const std::string &CoroQueryResults::GetExplainResults() {
	if (!i_.queryParams_.explainResults.has_value()) {
		parseExtraData();
	}
	return i_.queryParams_.explainResults.value();
}

const vector<AggregationResult> &CoroQueryResults::GetAggregationResults() {
	if (!i_.queryParams_.aggResults.has_value()) {
		parseExtraData();
	}
	return i_.queryParams_.aggResults.value();
}

CoroQueryResults::CoroQueryResults(NsArray &&nsArray, Item &item) : i_(std::move(nsArray)) {
	i_.queryParams_.totalcount = 0;
	i_.queryParams_.qcount = 1;
	i_.queryParams_.count = 1;
	i_.queryParams_.flags = kResultsCJson;
	std::string_view itemData = item.GetCJSON();
	i_.rawResult_.resize(itemData.size() + sizeof(uint32_t));
	uint32_t dataSize = itemData.size();
	memcpy(&i_.rawResult_[0], &dataSize, sizeof(uint32_t));
	memcpy(&i_.rawResult_[0] + sizeof(uint32_t), itemData.data(), itemData.size());
}

void CoroQueryResults::Bind(std::string_view rawResult, RPCQrId id, const Query *q) {
	i_.queryID_ = id;
	ResultSerializer ser(rawResult);

	if (q) {
		QueryData data;
		data.joinedSize = uint16_t(q->joinQueries_.size());
		data.mergedJoinedSizes.reserve(q->mergeQueries_.size());
		for (const auto &mq : q->mergeQueries_) {
			data.mergedJoinedSizes.emplace_back(mq.joinQueries_.size());
		}
		i_.qData_.emplace(std::move(data));
	} else {
		i_.qData_.reset();
	}

	try {
		ser.GetRawQueryParams(
			i_.queryParams_,
			[&ser, this](int nsIdx) {
				const uint32_t stateToken = ser.GetVarUint();
				const int version = ser.GetVarUint();
				TagsMatcher newTm;
				newTm.deserialize(ser, version, stateToken);
				i_.nsArray_[nsIdx]->TryReplaceTagsMatcher(std::move(newTm));
				// nsArray[nsIdx]->payloadType_.clone()->deserialize(ser);
				// nsArray[nsIdx]->tagsMatcher_.updatePayloadType(nsArray[nsIdx]->payloadType_, false);
				PayloadType("tmp").clone()->deserialize(ser);
			},
			i_.lazyMode_, i_.parsingData_);
	} catch (const Error &err) {
		i_.status_ = err;
	}

	if (i_.lazyMode_) {
		i_.rawResult_.assign(rawResult.begin(), rawResult.end());
	} else {
		i_.rawResult_.assign(rawResult.begin() + ser.Pos(), rawResult.end());
	}
}

void CoroQueryResults::fetchNextResults() {
	using std::chrono::seconds;
	const int flags = i_.fetchFlags_ ? (i_.fetchFlags_ & ~kResultsWithPayloadTypes) : kResultsCJson;
	auto ret = i_.conn_->Call({cproto::kCmdFetchResults, i_.requestTimeout_, milliseconds(0), lsn_t(), -1, ShardingKeyType::NotSetShard,
							   nullptr, false, i_.sessionTs_},
							  i_.queryID_.main, flags, i_.queryParams_.count + i_.fetchOffset_, i_.fetchAmount_, i_.queryID_.uid);
	if (!ret.Status().ok()) {
		throw ret.Status();
	}

	handleFetchedBuf(ret);
}

void CoroQueryResults::handleFetchedBuf(net::cproto::CoroRPCAnswer &ans) {
	auto args = ans.GetArgs(2);

	i_.fetchOffset_ += i_.queryParams_.count;

	std::string_view rawResult = p_string(args[0]);
	ResultSerializer ser(rawResult);

	ser.GetRawQueryParams(i_.queryParams_, nullptr, i_.lazyMode_, i_.parsingData_);

	if (i_.lazyMode_) {
		i_.rawResult_.assign(rawResult.begin(), rawResult.end());
	} else {
		i_.rawResult_.assign(rawResult.begin() + ser.Pos(), rawResult.end());
	}
	i_.status_ = Error();
}

void CoroQueryResults::parseExtraData() {
	if (i_.lazyMode_) {
		std::string_view rawResult(i_.rawResult_.data() + i_.parsingData_.extraData.begin,
								   i_.parsingData_.extraData.end - i_.parsingData_.extraData.begin);
		ResultSerializer ser(rawResult);
		i_.queryParams_.aggResults.emplace();
		i_.queryParams_.explainResults.emplace();
		ser.GetExtraParams(i_.queryParams_, false);
	} else {
		throw Error(errLogic, "Unable to parse clients explain or aggregation results (lazy parsing mode was expected)");
	}
}

h_vector<std::string_view, 1> CoroQueryResults::GetNamespaces() const {
	h_vector<std::string_view, 1> ret;
	ret.reserve(i_.nsArray_.size());
	for (auto &ns : i_.nsArray_) ret.emplace_back(ns->name);
	return ret;
}

TagsMatcher CoroQueryResults::GetTagsMatcher(int nsid) const noexcept {
	assert(nsid < int(i_.nsArray_.size()));
	return i_.nsArray_[nsid]->GetTagsMatcher();
}

TagsMatcher CoroQueryResults::GetTagsMatcher(std::string_view nsName) const noexcept {
	auto it =
		std::find_if(i_.nsArray_.begin(), i_.nsArray_.end(), [&nsName](Namespace *ns) { return (std::string_view(ns->name) == nsName); });
	if (it == i_.nsArray_.end()) {
		return TagsMatcher();
	}
	return (*it)->GetTagsMatcher();
}

PayloadType CoroQueryResults::GetPayloadType(int nsid) const noexcept {
	assert(nsid < int(i_.nsArray_.size()));
	return i_.nsArray_[nsid]->payloadType;
}

PayloadType CoroQueryResults::GetPayloadType(std::string_view nsName) const noexcept {
	auto it =
		std::find_if(i_.nsArray_.begin(), i_.nsArray_.end(), [&nsName](Namespace *ns) { return (std::string_view(ns->name) == nsName); });
	if (it == i_.nsArray_.end()) {
		return PayloadType();
	}
	return (*it)->payloadType;
}

const std::string &CoroQueryResults::GetNsName(int nsid) const noexcept {
	assert(nsid < int(i_.nsArray_.size()));
	return i_.nsArray_[nsid]->payloadType.Name();
}

class EncoderDatasourceWithJoins : public IEncoderDatasourceWithJoins {
public:
	EncoderDatasourceWithJoins(const CoroQueryResults::Iterator::JoinedData &joinedData, const CoroQueryResults &qr)
		: joinedData_(joinedData), qr_(qr), tm_(TagsMatcher::unsafe_empty_t()) {}
	~EncoderDatasourceWithJoins() override = default;

	size_t GetJoinedRowsCount() const final { return joinedData_.size(); }
	size_t GetJoinedRowItemsCount(size_t rowId) const final {
		const auto &fieldIt = joinedData_.at(rowId);
		return fieldIt.size();
	}
	ConstPayload GetJoinedItemPayload(size_t rowid, size_t plIndex) final {
		auto &fieldIt = joinedData_.at(rowid);
		auto &dataIt = fieldIt.at(plIndex);
		itemimpl_ = ItemImpl<CoroRPCClient>(qr_.GetPayloadType(getJoinedNsID(dataIt.nsid)), qr_.GetTagsMatcher(getJoinedNsID(dataIt.nsid)),
											nullptr, std::chrono::milliseconds());
		itemimpl_.Unsafe(true);
		auto err = itemimpl_.FromCJSON(dataIt.data);
		if (!err.ok()) throw err;
		return itemimpl_.GetConstPayload();
	}
	const TagsMatcher &GetJoinedItemTagsMatcher(size_t rowid) final {
		auto &fieldIt = joinedData_.at(rowid);
		if (fieldIt.empty()) {
			static const TagsMatcher kEmptyTm;
			return kEmptyTm;
		}
		tm_ = qr_.GetTagsMatcher(getJoinedNsID(fieldIt[0].nsid));
		return tm_;
	}
	virtual const FieldsSet &GetJoinedItemFieldsFilter(size_t /*rowid*/) final {
		static const FieldsSet empty;
		return empty;
	}
	const string &GetJoinedItemNamespace(size_t rowid) final {
		auto &fieldIt = joinedData_.at(rowid);
		if (fieldIt.empty()) {
			static const std::string empty;
			return empty;
		}
		return qr_.GetNsName(getJoinedNsID(rowid));
	}

private:
	uint32_t getJoinedNsID(int parentNsId) {
		if (cachedParentNsId_ == int64_t(parentNsId)) {
			return cachedJoinedNsId_;
		}
		uint32_t joinedNsId = 1;
		auto &qData = qr_.GetQueryData();
		if (qData.has_value()) {
			joinedNsId += qData->mergedJoinedSizes.size();
			int mergedNsIdx = parentNsId;
			if (mergedNsIdx > 0) {
				joinedNsId += qData->joinedSize;
				--mergedNsIdx;
			}
			for (int ns = 0; ns < mergedNsIdx; ++ns) {
				assert(size_t(ns) < qData->mergedJoinedSizes.size());
				joinedNsId += qData->mergedJoinedSizes[ns];
			}
		}
		cachedParentNsId_ = parentNsId;
		cachedJoinedNsId_ = joinedNsId;
		return joinedNsId;
	}

	const CoroQueryResults::Iterator::JoinedData &joinedData_;
	const CoroQueryResults &qr_;
	ItemImpl<CoroRPCClient> itemimpl_;
	int64_t cachedParentNsId_ = -1;
	uint32_t cachedJoinedNsId_ = 0;
	TagsMatcher tm_;
};

void CoroQueryResults::Iterator::getJSONFromCJSON(std::string_view cjson, WrSerializer &wrser, bool withHdrLen) const {
	auto tm = qr_->GetTagsMatcher(itemParams_.nsid);
	JsonEncoder enc(&tm);
	JsonBuilder builder(wrser, ObjType::TypePlain);
	h_vector<IAdditionalDatasource<JsonBuilder> *, 2> dss;
	int shardId = (const_cast<Iterator *>(this))->GetShardID();
	//	std::cout << "getJSONFromCJSON shardId=" << shardId << std::endl;
	AdditionalDatasourceShardId dsShardId(shardId);
	if (qr_->NeedOutputShardId() && shardId >= 0) {
		dss.push_back(&dsShardId);
	}
	if (qr_->HaveJoined() && joinedData_.size()) {
		EncoderDatasourceWithJoins joinsDs(joinedData_, *qr_);
		AdditionalDatasource ds = qr_->NeedOutputRank() ? AdditionalDatasource(itemParams_.proc, &joinsDs) : AdditionalDatasource(&joinsDs);
		dss.push_back(&ds);
		if (withHdrLen) {
			auto slicePosSaver = wrser.StartSlice();
		}
		enc.Encode(cjson, builder, dss);
		return;
	}

	AdditionalDatasource ds(itemParams_.proc, nullptr);
	AdditionalDatasource *dspPtr = qr_->NeedOutputRank() ? &ds : nullptr;
	if (dspPtr) {
		dss.push_back(dspPtr);
	}
	if (withHdrLen) {
		auto slicePosSaver = wrser.StartSlice();
	}
	enc.Encode(cjson, builder, dss);
}

Error CoroQueryResults::Iterator::GetMsgPack(WrSerializer &wrser, bool withHdrLen) {
	readNext();
	int type = qr_->i_.queryParams_.flags & kResultsFormatMask;
	if (type == kResultsMsgPack) {
		if (withHdrLen) {
			wrser.PutSlice(itemParams_.data);
		} else {
			wrser.Write(itemParams_.data);
		}
	} else {
		return Error(errParseBin, "Impossible to get data in MsgPack because of a different format: %d", type);
	}
	return errOK;
}

Error CoroQueryResults::Iterator::GetJSON(WrSerializer &wrser, bool withHdrLen) {
	readNext();
	try {
		switch (qr_->i_.queryParams_.flags & kResultsFormatMask) {
			case kResultsCJson: {
				getJSONFromCJSON(itemParams_.data, wrser, withHdrLen);
				break;
			}
			case kResultsJson:
				if (withHdrLen) {
					wrser.PutSlice(itemParams_.data);
				} else {
					wrser.Write(itemParams_.data);
				}
				break;
			case kResultsPure: {
				constexpr std::string_view kEmptyJSON("{}");
				if (withHdrLen) {
					wrser.PutSlice(kEmptyJSON);
				} else {
					wrser.Write(kEmptyJSON);
				}
				break;
			}
			default:
				return Error(errParseBin, "Server returned data in unknown format %d", qr_->i_.queryParams_.flags & kResultsFormatMask);
		}
	} catch (const Error &err) {
		return err;
	}
	return errOK;
}

Error CoroQueryResults::Iterator::GetCJSON(WrSerializer &wrser, bool withHdrLen) {
	readNext();
	try {
		switch (qr_->i_.queryParams_.flags & kResultsFormatMask) {
			case kResultsCJson:
				if (withHdrLen) {
					wrser.PutSlice(itemParams_.data);
				} else {
					wrser.Write(itemParams_.data);
				}
				break;
			case kResultsMsgPack:
				return Error(errParseBin, "Server returned data in msgpack format, can't process");
			case kResultsJson:
				return Error(errParseBin, "Server returned data in json format, can't process");
			default:
				return Error(errParseBin, "Server returned data in unknown format %d", qr_->i_.queryParams_.flags & kResultsFormatMask);
		}
	} catch (const Error &err) {
		return err;
	}
	return errOK;
}

Item CoroQueryResults::Iterator::GetItem() {
	readNext();
	Error err;
	try {
		Item item = qr_->i_.nsArray_[itemParams_.nsid]->NewItem();
		item.setID(itemParams_.id);
		item.setLSN(itemParams_.lsn);
		item.setShardID(itemParams_.shardId);

		switch (qr_->i_.queryParams_.flags & kResultsFormatMask) {
			case kResultsMsgPack: {
				size_t offset = 0;
				err = item.FromMsgPack(itemParams_.data, offset);
				break;
			}
			case kResultsCJson: {
				err = item.FromCJSON(itemParams_.data);
				break;
			}
			case kResultsJson: {
				char *endp = nullptr;
				err = item.FromJSON(itemParams_.data, &endp);
				break;
			}
			case kResultsPure:
				break;
			default:
				return Item();
		}
		if (err.ok()) {
			return item;
		}
	} catch (const Error &err) {
		return Item(err);
	}
	return Item(std::move(err));
}

lsn_t CoroQueryResults::Iterator::GetLSN() {
	readNext();
	return itemParams_.lsn;
}

int CoroQueryResults::Iterator::GetNSID() {
	readNext();
	return itemParams_.nsid;
}

int CoroQueryResults::Iterator::GetID() {
	readNext();
	return itemParams_.id;
}

int CoroQueryResults::Iterator::GetShardID() {
	readNext();
	if (qr_->i_.queryParams_.flags & kResultsWithShardId) {
		if (qr_->i_.queryParams_.shardId != ShardingKeyType::ProxyOff) {
			return qr_->i_.queryParams_.shardId;
		} else {
			return itemParams_.shardId;
		}
	}
	return ShardingKeyType::ProxyOff;
}

int16_t CoroQueryResults::Iterator::GetRank() {
	readNext();
	return itemParams_.proc;
}

bool CoroQueryResults::Iterator::IsRaw() {
	readNext();
	return itemParams_.raw;
}

std::string_view CoroQueryResults::Iterator::GetRaw() {
	readNext();
	assertrx(itemParams_.raw);
	return itemParams_.data;
}

const CoroQueryResults::Iterator::JoinedData &CoroQueryResults::Iterator::GetJoined() {
	readNext();
	return joinedData_;
}

void CoroQueryResults::Iterator::readNext() {
	if (nextPos_ != 0 || !Status().ok()) return;

	std::string_view rawResult;
	if (qr_->i_.lazyMode_) {
		rawResult = std::string_view(qr_->i_.rawResult_.data() + qr_->i_.parsingData_.itemsPos,
									 qr_->i_.rawResult_.size() - qr_->i_.parsingData_.itemsPos);
	} else {
		rawResult = std::string_view(qr_->i_.rawResult_.data(), qr_->i_.rawResult_.size());
	}
	ResultSerializer ser(rawResult.substr(pos_));

	try {
		itemParams_ = ser.GetItemData(qr_->i_.queryParams_.flags, qr_->i_.queryParams_.shardId);
		joinedData_.clear();
		if (qr_->i_.queryParams_.flags & kResultsWithJoined) {
			int format = qr_->i_.queryParams_.flags & kResultsFormatMask;
			(void)format;
			assert(format == kResultsCJson);
			int joinedFields = ser.GetVarUint();
			for (int i = 0; i < joinedFields; ++i) {
				int itemsCount = ser.GetVarUint();
				h_vector<ResultSerializer::ItemParams, 1> joined;
				joined.reserve(itemsCount);
				for (int j = 0; j < itemsCount; ++j) {
					// joined data shard id equals query shard id
					joined.emplace_back(ser.GetItemData(qr_->i_.queryParams_.flags, qr_->i_.queryParams_.shardId));
				}
				joinedData_.emplace_back(std::move(joined));
			}
		}
		nextPos_ = pos_ + ser.Pos();
	} catch (const Error &err) {
		const_cast<CoroQueryResults *>(qr_)->i_.status_ = err;
	}
}

CoroQueryResults::Iterator &CoroQueryResults::Iterator::operator++() {
	try {
		readNext();
		idx_++;
		pos_ = nextPos_;
		nextPos_ = 0;
		if (idx_ != qr_->i_.queryParams_.qcount && idx_ == qr_->i_.queryParams_.count + qr_->i_.fetchOffset_) {
			const_cast<CoroQueryResults *>(qr_)->fetchNextResults();
			pos_ = 0;
		}
	} catch (const Error &err) {
		const_cast<CoroQueryResults *>(qr_)->i_.status_ = err;
	}

	return *this;
}

CoroQueryResults::Impl::Impl(cproto::CoroClientConnection *conn, CoroQueryResults::NsArray &&nsArray, int fetchFlags, int fetchAmount,
							 std::chrono::milliseconds timeout, bool lazyMode)
	: conn_(conn),
	  nsArray_(std::move(nsArray)),
	  fetchFlags_(fetchFlags),
	  fetchAmount_(fetchAmount),
	  requestTimeout_(timeout),
	  lazyMode_(lazyMode) {
	assert(conn_);
	const auto sessionTs = conn_->LoginTs();
	if (sessionTs.has_value()) {
		sessionTs_ = sessionTs.value();
	}
	InitLazyData();
}

}  // namespace client
}  // namespace reindexer
