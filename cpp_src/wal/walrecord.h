#pragma once

#include <core/type_consts.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include "core/keyvalue/p_string.h"
#include "estl/chunk_buf.h"
#include "estl/h_vector.h"
#include "estl/span.h"

namespace reindexer {

enum WALRecType {
	WalEmpty = 0,
	WalReplState = 1,
	WalItemUpdate = 2,
	WalItemModify = 3,
	WalIndexAdd = 4,
	WalIndexDrop = 5,
	WalIndexUpdate = 6,
	WalPutMeta = 7,
	WalUpdateQuery = 8,
	WalNamespaceAdd = 9,
	WalNamespaceDrop = 10,
	WalNamespaceRename = 11,
	WalInitTransaction = 12,
	WalCommitTransaction = 13,
	WalForceSync = 14,
	WalSetSchema = 15,
	WalWALSync = 16,
	WalTagsMatcher = 17,
	WalResetLocalWal = 18,
	WalRawItem = 19,
	WalShallowItem = 20,
};

class WrSerializer;
class JsonBuilder;
struct WALRecord;

struct WALRecord {
	explicit WALRecord(span<uint8_t>);
	explicit WALRecord(std::string_view sv);
	explicit WALRecord(WALRecType _type = WalEmpty, IdType _id = 0, bool inTx = false) : type(_type), id(_id), inTransaction(inTx) {}
	explicit WALRecord(WALRecType _type, std::string_view _data, bool inTx = false) : type(_type), data(_data), inTransaction(inTx) {}
	explicit WALRecord(WALRecType _type, IdType _id, std::string_view _data) : type(_type), rawItem{_id, _data} {}
	explicit WALRecord(WALRecType _type, std::string_view key, std::string_view value, bool inTx)
		: type(_type), putMeta{key, value}, inTransaction(inTx) {}
	explicit WALRecord(WALRecType _type, std::string_view cjson, int tmVersion, int modifyMode, bool inTx = false)
		: type(_type), itemModify{cjson, tmVersion, modifyMode}, inTransaction(inTx) {}
	WrSerializer &Dump(WrSerializer &ser, const std::function<std::string(std::string_view)>& cjsonViewer) const;
	void GetJSON(JsonBuilder &jb, const std::function<std::string(std::string_view)>& cjsonViewer) const;
	void Pack(WrSerializer &ser) const;

	WALRecType type;
	union {
		IdType id;
		std::string_view data;
		struct {
			std::string_view itemCJson;
			int tmVersion;
			int modifyMode;
		} itemModify;
		struct {
			std::string_view key;
			std::string_view value;
		} putMeta;
		struct {
			IdType id;
			std::string_view itemCJson;
		} rawItem;
	};
	bool inTransaction = false;
};

struct PackedWALRecord : public h_vector<uint8_t, 12> {
	using h_vector<uint8_t, 12>::h_vector;
	void Pack(const WALRecord &rec);
};

#pragma pack(push, 1)
struct MarkedPackedWALRecord : public PackedWALRecord {
	MarkedPackedWALRecord() = default;
	template <typename RecordT>
	MarkedPackedWALRecord(int16_t s, RecordT &&rec) : PackedWALRecord(std::forward<RecordT>(rec)), server(s) {}

	int16_t server;
	void Pack(int16_t _serverId, const WALRecord &rec);
};
#pragma pack(pop)

}  // namespace reindexer