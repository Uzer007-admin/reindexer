#pragma once

#include <string>
#include "core/keyvalue/variant.h"
#include "estl/h_vector.h"

namespace reindexer {

enum token_type { TokenEnd, TokenName, TokenNumber, TokenString, TokenOp, TokenSymbol, TokenSign };

class token {
public:
	token(token_type t = TokenSymbol) noexcept : type(t) {}
	token(const token &) = delete;
	token &operator=(const token &) = delete;
	token(token &&other) = default;
	token &operator=(token &&other) = default;

	std::string_view text() const noexcept { return std::string_view(text_.data(), text_.size()); }
	void to_lower() noexcept {
		for (auto &c : text_) {
			c = tolower(c);
		}
	}

	token_type type;
	h_vector<char, 20> text_;
};

class tokenizer {
public:
	class flags {
	public:
		enum values : int { no_flags = 0, to_lower = 1, treat_sign_as_token = 1 << 1, in_order_by = 1 << 2 };

		flags(int f) noexcept : f_(f) {}

		RX_ALWAYS_INLINE bool has_to_lower() const noexcept { return f_ & values::to_lower; }
		RX_ALWAYS_INLINE bool has_treat_sign_as_token() const noexcept { return f_ & values::treat_sign_as_token; }
		RX_ALWAYS_INLINE bool has_in_order_by() const noexcept { return f_ & values::in_order_by; }

	private:
		int f_;
	};

	tokenizer(std::string_view query) noexcept : q_(query), cur_(query.begin()) {}
	token next_token(flags f = flags(flags::to_lower));
	token peek_token(flags f = flags(flags::to_lower)) {
		auto save_cur = cur_;
		auto save_pos = pos_;
		auto res = next_token(f);
		cur_ = save_cur;
		pos_ = save_pos;
		return res;
	}
	void skip_space() noexcept;
	bool end() const noexcept { return cur_ == q_.end(); }
	size_t getPos() const noexcept { return pos_; }
	void setPos(size_t pos) noexcept {
		int delta = pos - pos_;
		pos_ += delta;
		cur_ += delta;
	}
	std::string where() const;
	size_t length() const noexcept { return q_.length(); }
	const char *begin() const noexcept { return q_.data(); }

protected:
	std::string_view q_;
	std::string_view::const_iterator cur_;
	size_t pos_ = 0;
};

Variant token2kv(const token &currTok, tokenizer &parser, bool allowComposite);

}  // namespace reindexer
