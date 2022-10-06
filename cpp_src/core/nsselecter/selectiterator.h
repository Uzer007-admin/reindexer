#pragma once

#include "core/selectkeyresult.h"

namespace reindexer {

/// Allows to iterate over a result of selecting
/// data for one certain key.
class SelectIterator : public SelectKeyResult {
public:
	enum {
		Forward,
		Reverse,
		SingleRange,
		SingleIdset,
		SingleIdSetWithDeferedSort,
		RevSingleRange,
		RevSingleIdset,
		RevSingleIdSetWithDeferedSort,
		OnlyComparator,
		Unsorted,
		UnbuiltSortOrdersIndex,
	};

	SelectIterator() = default;
	SelectIterator(SelectKeyResult res, bool distinct, string name, bool forcedFirst = false);

	/// Starts iteration process: prepares
	/// object for further work.
	/// @param reverse - direction of iteration.
	/// @param maxIterations - expected max iterations in select loop
	void Start(bool reverse, int maxIterations);
	/// Signalizes if iteration is over.
	/// @return true if iteration is done.
	inline bool End() const noexcept { return lastVal_ == (isReverse_ ? INT_MIN : INT_MAX) && !comparators_.size(); }
	/// Iterates to a next item of result.
	/// @param minHint - rowId value to start from.
	/// @return true if operation succeed.
	inline bool Next(IdType minHint) {
		bool res = false;
		switch (type_) {
			case Forward:
				res = nextFwd(minHint);
				break;
			case Reverse:
				res = nextRev(minHint);
				break;
			case SingleRange:
				res = nextFwdSingleRange(minHint);
				break;
			case SingleIdset:
			case SingleIdSetWithDeferedSort:
				res = nextFwdSingleIdset(minHint);
				break;
			case RevSingleRange:
				res = nextRevSingleRange(minHint);
				break;
			case RevSingleIdset:
			case RevSingleIdSetWithDeferedSort:
				res = nextRevSingleIdset(minHint);
				break;
			case OnlyComparator:
				return false;
			case Unsorted:
				res = nextUnsorted();
				break;
			case UnbuiltSortOrdersIndex:
				res = nextUnbuiltSortOrders();
				break;
		}
		if (res) ++matchedCount_;
		return res;
	}

	/// Sets Unsorted iteration mode
	inline void SetUnsorted() noexcept { isUnsorted = true; }

	/// Current rowId
	IdType Val() const noexcept;

	/// Current rowId index since the beginning
	/// of current SingleKeyValue object.
	int Pos() const noexcept {
		assertrx(!lastIt_->useBtree_ && (type_ != UnbuiltSortOrdersIndex));
		return lastIt_->it_ - lastIt_->begin_ - 1;
	}

	/// Binding to comparators
	/// @param type - PayloadType of selected ns.
	/// @param field - field index.
	void Bind(PayloadType type, int field);
	/// Uses each comparator to compare with pl.
	/// @param pl - PayloadValue to be compared.
	/// @param rowId - rowId.
	inline bool TryCompare(const PayloadValue &pl, int rowId) noexcept {
		for (auto &cmp : comparators_)
			if (cmp.Compare(pl, rowId)) {
				matchedCount_++;
				return true;
			}
		return false;
	}
	/// @return amonut of matched items
	int GetMatchedCount() const noexcept { return matchedCount_; }

	/// Excludes last set of ids from each result
	/// to remove duplicated keys
	void ExcludeLastSet(const PayloadValue &, IdType rowId, IdType properRowId);

	/// Appends result to an existing set.
	/// @param other - results to add.
	void Append(SelectKeyResult &other);
	/// Appends result to existing set performing
	/// binding to comparators at the same time.
	/// @param other - results to add.
	/// @param type - PayloadType of selected ns to bind.
	/// @param field - field idx to bind.
	void AppendAndBind(SelectKeyResult &other, PayloadType type, int field);
	/// Cost value used for sorting: object with a smaller
	/// cost goes before others.
	double Cost(int expectedIterations) const noexcept;

	/// Switches SingleSelectKeyResult to btree search
	/// mode if it's more efficient than just comparing
	/// each object in sequence.
	void SetExpectMaxIterations(int expectedIterations_) noexcept;

	int Type() const noexcept { return type_; }

	std::string_view TypeName() const noexcept;
	string Dump() const;

	bool distinct = false;
	string name;

protected:
	// Iterates to a next item of result
	// depending on iterator type starting
	// from minHint which is the least rowId.
	bool nextFwd(IdType minHint);
	bool nextRev(IdType minHint);
	bool nextFwdSingleRange(IdType minHint);
	bool nextFwdSingleIdset(IdType minHint);
	bool nextRevSingleRange(IdType minHint);
	bool nextRevSingleIdset(IdType minHint);
	bool nextUnbuiltSortOrders();
	bool nextUnsorted();

	/// Performs ID sets merge and sort in case, when this sort was defered earlier and still effective with current maxIterations value
	bool applyDeferedSort(int maxIterations) {
		if (deferedExplicitSort && maxIterations > 0 && !distinct) {
			const auto idsCount = GetMaxIterations();
			if (IsGenericSortRecommended(size(), idsCount, size_t(maxIterations))) {
				MergeIdsets(true, idsCount);
				return true;
			}
		}
		return false;
	}

	bool isUnsorted = false;
	bool isReverse_ = false;
	bool forcedFirst_ = false;
	int type_ = 0;
	IdType lastVal_ = INT_MIN;
	iterator lastIt_ = nullptr;
	IdType end_ = 0;
	int matchedCount_ = 0;
};

}  // namespace reindexer
