#include "client/coroqueryresults.h"
#include "client/cororpcclient.h"
#include "core/cjson/baseencoder.h"
#include "core/cjson/cjsondecoder.h"
#include "core/cjson/jsondecoder.h"
#include "core/cjson/msgpackbuilder.h"
#include "core/cjson/msgpackdecoder.h"
#include "itemimpl.h"

using std::move;

namespace reindexer {
namespace client {

// Construct item from compressed json
Error ItemImplBase::FromCJSON(std::string_view slice) {
	GetPayload().Reset();
	std::string_view data = slice;
	if (!unsafe_) {
		holder_.reset(new char[slice.size()]);
		std::copy(slice.begin(), slice.end(), holder_.get());
		data = std::string_view(holder_.get(), slice.size());
	}

	Serializer rdser(data);
	// check tags matcher update
	int tag = rdser.GetVarUint();
	uint32_t tmOffset = 0;
	const bool hasBundledTm = (tag == TAG_END);
	if (hasBundledTm) {
		tmOffset = rdser.GetUInt32();
		// read tags matcher update
		Serializer tser(slice.substr(tmOffset));
		tagsMatcher_.deserialize(tser);
		tagsMatcher_.setUpdated();
	} else
		rdser.SetPos(0);

	Payload pl = GetPayload();
	CJsonDecoder decoder(tagsMatcher_);
	ser_.Reset();
	auto err = decoder.Decode(&pl, rdser, ser_);

	if (!hasBundledTm && !err.ok()) {
		err = tryToUpdateTagsMatcher();
		if (!err.ok()) {
			return Error(errParseJson, "Error parsing CJSON: %s", err.what());
		}
		ser_.Reset();
		rdser.SetPos(0);
		CJsonDecoder decoder(tagsMatcher_);
		err = decoder.Decode(&pl, rdser, ser_);
	}

	if (err.ok() && !rdser.Eof() && rdser.Pos() != tmOffset) {
		return Error(errParseJson, "Internal error - left unparsed data %d", rdser.Pos());
	}

	if (err.ok()) {
		const auto tupleSize = ser_.Len();
		tupleHolder_ = ser_.DetachBuf();
		tupleData_ = std::string_view(reinterpret_cast<char *>(tupleHolder_.get()), tupleSize);
		pl.Set(0, {Variant(p_string(&tupleData_))});
	}

	return err;
}

Error ItemImplBase::FromJSON(std::string_view slice, char **endp, bool /*pkOnly*/) {
	std::string_view data = slice;
	if (!unsafe_ && endp == nullptr) {
		holder_.reset(new char[slice.size()]);
		std::copy(slice.begin(), slice.end(), holder_.get());
		data = std::string_view(holder_.get(), slice.size());
	}

	payloadValue_.Clone();
	char *endptr = nullptr;
	gason::JsonValue value;
	gason::JsonAllocator alloc;
	int status = jsonParse(giftStr(data), &endptr, &value, alloc);
	if (status != gason::JSON_OK) {
		return Error(errLogic, "Error parsing json: %s, pos %d", gason::jsonStrError(status), unsigned(endptr - data.data()));
	}
	if (endp) {
		*endp = endptr;
	}

	// Split parsed json into indexes and tuple
	JsonDecoder decoder(tagsMatcher_);
	Payload pl = GetPayload();
	ser_.Reset();
	auto err = decoder.Decode(&pl, ser_, value);

	if (err.ok()) {
		// Put tuple to field[0]
		const auto tupleSize = ser_.Len();
		tupleHolder_ = ser_.DetachBuf();
		tupleData_ = std::string_view(reinterpret_cast<char *>(tupleHolder_.get()), tupleSize);
		pl.Set(0, {Variant(p_string(&tupleData_))});
	}
	return err;
}

Error ItemImplBase::FromMsgPack(std::string_view buf, size_t &offset) {
	Payload pl = GetPayload();
	MsgPackDecoder decoder(&tagsMatcher_);

	std::string_view data = buf;
	if (!unsafe_) {
		holder_.reset(new char[buf.size()]);
		std::copy(buf.begin(), buf.end(), holder_.get());
		data = std::string_view(holder_.get(), buf.size());
	}

	ser_.Reset();
	Error err = decoder.Decode(data, &pl, ser_, offset);
	if (err.ok()) {
		const auto tupleSize = ser_.Len();
		tupleHolder_ = ser_.DetachBuf();
		tupleData_ = std::string_view(reinterpret_cast<char *>(tupleHolder_.get()), tupleSize);
		pl.Set(0, {Variant(p_string(&tupleData_))});
	}
	return err;
}

Error ItemImplBase::FromCJSON(ItemImplBase *other) {
	auto cjson = other->GetCJSON();
	auto err = FromCJSON(cjson);
	assert(err.ok());
	return err;
}

std::string_view ItemImplBase::GetMsgPack() {
	int startTag = 0;
	ConstPayload pl = GetConstPayload();

	MsgPackEncoder msgpackEncoder(&tagsMatcher_);
	const TagsLengths &tagsLengths = msgpackEncoder.GetTagsMeasures(&pl);

	ser_.Reset();
	MsgPackBuilder msgpackBuilder(ser_, &tagsLengths, &startTag, ObjType::TypePlain, &tagsMatcher_);
	msgpackEncoder.Encode(&pl, msgpackBuilder);

	return ser_.Slice();
}

std::string_view ItemImplBase::GetJSON() {
	ConstPayload pl(payloadType_, payloadValue_);
	JsonBuilder builder(ser_, ObjType::TypePlain);
	JsonEncoder encoder(&tagsMatcher_);

	ser_.Reset();
	encoder.Encode(&pl, builder);

	return ser_.Slice();
}

std::string_view ItemImplBase::GetCJSON() {
	ConstPayload pl(payloadType_, payloadValue_);
	CJsonBuilder builder(ser_, ObjType::TypePlain);
	CJsonEncoder encoder(&tagsMatcher_);

	ser_.Reset();
	ser_.PutVarUint(TAG_END);
	int pos = ser_.Len();
	ser_.PutUInt32(0);
	encoder.Encode(&pl, builder);

	if (tagsMatcher_.isUpdated()) {
		uint32_t tmOffset = ser_.Len();
		memcpy(ser_.Buf() + pos, &tmOffset, sizeof(tmOffset));
		tagsMatcher_.serialize(ser_);
		return ser_.Slice();
	}

	return ser_.Slice().substr(sizeof(uint32_t) + 1);
}

void ItemImplBase::GetPrecepts(WrSerializer &ser) {
	if (precepts_.size()) {
		ser.PutVarUint(precepts_.size());
		for (auto &p : precepts_) {
			ser.PutVString(p);
		}
	}
}

}  // namespace client
}  // namespace reindexer