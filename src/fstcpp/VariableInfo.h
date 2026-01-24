// SPDX-FileCopyrightText: 2025-2026 Yu-Sheng Lin <johnjohnlys@gmail.com>
// SPDX-FileCopyrightText: 2025-2026 Yoda Lee <lc85301@gmail.com>
// SPDX-License-Identifier: MIT
#pragma once
// direct include
#include "fstcpp/fst.h"
// C system headers
// C++ standard library headers
#include <algorithm>
#include <cstdint>
// #include <cstdlib>
// #include <string>
#include <vector>
// Other libraries' .h files.
// Your project's .h files.
#include "fstcpp/assertion.h"
#include "fstcpp/StreamWriteHelper.h"

namespace fst {

struct VariableInfo final {
	// begin of data members
	// 24 bytes
	std::vector<uint8_t> data;
	// 8 bytes {
	uint32_t last_written_bytes;
	const uint16_t bitwidth;
	const bool is_real;
    uint64_t prev_time;
	// } 8 bytes

	// end of data members

	template<typename Callable, typename... Args>
	auto DispatchHelper(Callable&& callable, Args&&... args) const;

	VariableInfo(uint32_t bitwidth_, bool is_real_ = false);
	~VariableInfo() = default;
	VariableInfo(VariableInfo&&) = default;

	uint32_t EmitValueChange(uint64_t current_time_index, const uint64_t val);
	uint32_t EmitValueChange(uint64_t current_time_index, const uint32_t *val, EncodingType encoding);
	uint32_t EmitValueChange(uint64_t current_time_index, const uint64_t *val, EncodingType encoding);

	void KeepOnlyTheLatestValue() {
		std::copy_n(
			data.end() - last_written_bytes,
			last_written_bytes,
			data.begin()
		);
		data.resize(last_written_bytes);
	}
	void DumpInitialBits(std::vector<uint8_t> &buf) const;
	void DumpValueChanges(std::vector<uint8_t> &buf) const;

	// We only need to make this class compatible with vector
	// delete copy constructor and assignment operator
	VariableInfo(const VariableInfo&) = delete;
	VariableInfo& operator=(const VariableInfo&) = delete;
	VariableInfo& operator=(VariableInfo&&) = delete;
};
static_assert(sizeof(VariableInfo) <= 40, "VariableInfoBase should be small");

namespace detail {

class VariableInfoDouble {
	VariableInfo& info;

public:
	VariableInfoDouble(VariableInfo& info_) : info(info_) {}

private:
	inline void EmitValueChangeCommonPart(uint64_t current_time_index, EncodingType encoding) {
		if (current_time_index+1 == 0) {
			info.data.resize(0);
		}
		StreamVectorWriteHelper(info.data)
		.Write(current_time_index) // time index
		.Write(encoding); // encoding
	}

public:
	void Construct() {
		StreamVectorWriteHelper(info.data)
		.Write(uint64_t(0)) // initial time index
		.Write(EncodingType::eBinary) // initial encoding
		.Write(double(0.0)); // initial value
	}

	uint64_t EmitValueChange(uint64_t current_time_index, const uint64_t val) {
		EmitValueChangeCommonPart(current_time_index, EncodingType::eBinary);
		// val comes in as uint64_t bits representing the double. Write it directly.
		StreamVectorWriteHelper(info.data).Write(val);
		return sizeof(uint64_t) + sizeof(EncodingType) + sizeof(double);
	}

	// Double variables should not use these array-based EmitValueChange overloads.
	// We implement them to satisfy the VairableInfo::DispatchHelper template instantiation.
	uint64_t EmitValueChange(uint64_t, const uint32_t*, EncodingType) {
		throw std::runtime_error("EmitValueChange(uint32_t*) not supported for Double");
	}
	uint64_t EmitValueChange(uint64_t, const uint64_t*, EncodingType) {
		throw std::runtime_error("EmitValueChange(uint64_t*) not supported for Double");
	}

	void DumpInitialBits(std::vector<uint8_t> &buf) const {
		DCHECK_GT(info.data.size(), sizeof(uint64_t) + sizeof(EncodingType));
		StreamVectorReaderHelper rh(info.data.data());
		StreamVectorWriteHelper wh(buf);
		rh.Skip(sizeof(uint64_t) + sizeof(EncodingType));
		auto v = rh.Read<double>();
		wh.Write<double>(v);
	}

	void DumpValueChanges(std::vector<uint8_t> &buf) const {
		StreamVectorWriteHelper wh(buf);
		StreamVectorReaderHelper rh(info.data.data());
		const uint8_t* tail = info.data.data() + info.data.size();

		bool first = true;
		uint64_t prev_time_index = 0;

		while (true) {
			if (rh.ptr == tail) break;
			CHECK_GT(tail, rh.ptr);
			const auto time_index = rh.Read<uint64_t>();
			const auto enc = rh.Read<EncodingType>();
			const auto num_byte = sizeof(double);
			if (first) {
				// Note: [0] is initial value, which is already dumped in DumpInitialBits()
				first = false;
			} else {
				CHECK(enc == EncodingType::eBinary);
				const uint64_t delta_time_index = time_index - prev_time_index;
				prev_time_index = time_index;
				wh
				.WriteLEB128(delta_time_index)
				.Write<double>(rh.Peek<double>());
			}
			rh.Skip(num_byte);
		}
	}
};

template<typename T>
class VariableInfoScalarInt {
	VariableInfo& info;

public:
	VariableInfoScalarInt(VariableInfo& info_) : info(info_) {}

private:
	inline void EmitValueChangeCommonPart(uint64_t current_time_index, EncodingType encoding) {
		if (current_time_index+1 == 0) {
			// This is the first value change, we need to remove everything
			// and then add the new value
			info.data.resize(0);
		}
        if (info.bitwidth != 1) {
            StreamVectorWriteHelper(info.data)
            .Write(current_time_index) // time index
            .Write(encoding) // encoding
            ;
        }
	}

	inline uint64_t ComputeEmitMemory(EncodingType encoding) {
		return sizeof(T) * BitPerEncodedBit(encoding);
	}

public:
	void Construct() {
        if (info.bitwidth == 1) {
            info.prev_time = 0;
            StreamVectorWriteHelper(info.data)
            .Write(T(1)) // initial 'x' value
            ;
        } else {
            StreamVectorWriteHelper(info.data)
            .Write(uint64_t(0)) // initial time index (don't care)
            .Write(EncodingType::eVerilog) // initial encoding
            .Write(T(0)).Write(T(-1)) // initial X value
            ;
        }
	}

	uint64_t EmitValueChange(uint64_t current_time_index, const uint64_t val) {
        StreamVectorWriteHelper h(info.data);
        if (info.bitwidth == 1) {
			uint64_t delta_time_index = current_time_index - info.prev_time;
			switch (val) {
			case 0: delta_time_index = (delta_time_index<<2) | (0<<1) | 0; break; // '0'
			case 1: delta_time_index = (delta_time_index<<2) | (1<<1) | 0; break; // '1'
			case 2: delta_time_index = (delta_time_index<<4) | (0<<1) | 1; break; // 'X'
			case 3: delta_time_index = (delta_time_index<<4) | (1<<1) | 1; break; // 'Z'
			// Not supporting VHDL now
			// LCOV_EXCL_START
			case 4: delta_time_index = (delta_time_index<<4) | (2<<1) | 1; break; // 'H'
			case 5: delta_time_index = (delta_time_index<<4) | (3<<1) | 1; break; // 'U'
			case 6: delta_time_index = (delta_time_index<<4) | (4<<1) | 1; break; // 'W'
			case 7: delta_time_index = (delta_time_index<<4) | (5<<1) | 1; break; // 'L'
			case 8: delta_time_index = (delta_time_index<<4) | (6<<1) | 1; break; // '-'
			case 9: delta_time_index = (delta_time_index<<4) | (7<<1) | 1; break; // '?'
			default: break;
			// LCOV_EXCL_STOP
			}
			h.WriteLEB128(delta_time_index);
            info.prev_time = current_time_index;
            return ComputeEmitMemory(EncodingType::eBinary);
        } else {
            EmitValueChangeCommonPart(current_time_index, EncodingType::eBinary);
            h.Write<T>(val);
            return sizeof(uint64_t) + sizeof(EncodingType) + ComputeEmitMemory(EncodingType::eBinary);
        }
	}

	uint64_t EmitValueChange(uint64_t current_time_index, const uint32_t* val, EncodingType encoding) {
		EmitValueChangeCommonPart(current_time_index, encoding);
		StreamVectorWriteHelper h(info.data);
		for (unsigned i = 0; i < BitPerEncodedBit(encoding); ++i) {
			// C++17: replace this with if constexpr
			if (sizeof(T) == 8) {
				uint64_t v = val[1]; // high bits
				v <<= 32;
				v |= val[0]; // low bits
				h.Write(v);
				val += 2;
			} else {
				h.Write<T>(val[0]);
				val += 1;
			}
		}
		return sizeof(uint64_t) + sizeof(EncodingType) + ComputeEmitMemory(encoding);
	}

	uint64_t EmitValueChange(uint64_t current_time_index, const uint64_t* val, EncodingType encoding) {
		EmitValueChangeCommonPart(current_time_index, encoding);
		StreamVectorWriteHelper h(info.data);
		for (unsigned i = 0; i < BitPerEncodedBit(encoding); ++i) {
			h.Write<T>(val[i]);
		}
		return sizeof(uint64_t) + sizeof(EncodingType) + ComputeEmitMemory(encoding);
	}

	void DumpInitialBits(std::vector<uint8_t> &buf) const {
        if (info.bitwidth == 1) {
            buf.push_back(info.data[0]);
            return;
        }
		// FST requires initial bits present
		DCHECK_GT(info.data.size(), sizeof(uint64_t) + sizeof(EncodingType));
		StreamVectorReaderHelper rh(info.data.data());
		const auto time_index = rh.Read<uint64_t>(); (void)time_index;
		const auto enc = rh.Read<EncodingType>();
		const auto bitwidth = info.bitwidth;

		switch (enc) {
		case EncodingType::eBinary: {
			auto v0 = rh.Read<T>();
			for (unsigned i = bitwidth; i-- > 0;) {
				const char c = ((v0 >> i) & T(1)) ? '1' : '0';
				buf.push_back(c);
			}
			break;
		}

		case EncodingType::eVerilog: {
			auto v0 = rh.Read<T>();
			auto v1 = rh.Read<T>();
			for (unsigned i = bitwidth; i-- > 0;) {
				const T b1 = ((v1 >> i) & T(1));
				const T b0 = ((v0 >> i) & T(1));
				const char c = kEncodedBitToCharTable[(b1 << 1) | b0];
				buf.push_back(c);
			}
			break;
		}
		// Not supporting VHDL now
		// LCOV_EXCL_START
		default:
		case EncodingType::eVhdl: {
			auto v0 = rh.Read<T>();
			auto v1 = rh.Read<T>();
			auto v2 = rh.Read<T>();
			for (unsigned i = bitwidth; i-- > 0;) {
				const T b2 = ((v2 >> i) & T(1));
				const T b1 = ((v1 >> i) & T(1));
				const T b0 = ((v0 >> i) & T(1));
				const char c = kEncodedBitToCharTable[(b2 << 2) | (b1 << 1) | b0];
				buf.push_back(c);
			}
			break;
		}}
		// LCOV_EXCL_STOP
	}

	void DumpValueChanges(std::vector<uint8_t> &buf) const {
		StreamVectorWriteHelper h(buf);
		StreamVectorReaderHelper rh(info.data.data());
		const uint8_t* tail = info.data.data() + info.data.size();
		const auto bitwidth = info.bitwidth;
		bool first = true;
		uint64_t prev_time_index = 0;
		if (bitwidth == 1) {
            // skip first byte of initial value
            std::copy(info.data.begin()+1, info.data.end(), std::back_inserter(buf));
			// while (true) {
			// 	if (rh.ptr == tail) {
			// 		break;
			// 	}
			// 	DCHECK_GT(tail, rh.ptr);
			// 	const auto time_index = rh.Read<uint64_t>();
			// 	const auto enc = rh.Read<EncodingType>();
			// 	const auto num_element = BitPerEncodedBit(enc);
			// 	const auto num_byte = num_element * sizeof(T);
			// 	if (first) {
			// 		// Note: [0] is initial value, which is already dumped in DumpInitialBits()
			// 		first = false;
			// 	} else {
			// 		unsigned val = 0;
			// 		for (unsigned i = 0; i < num_element; ++i) {
			// 			val |= rh.Peek<T>(i);
			// 		}
			// 		uint64_t delta_time_index = time_index - prev_time_index;
			// 		prev_time_index = time_index;
			// 		switch (val) {
			// 		case 0: delta_time_index = (delta_time_index<<2) | (0<<1) | 0; break; // '0'
			// 		case 1: delta_time_index = (delta_time_index<<2) | (1<<1) | 0; break; // '1'
			// 		case 2: delta_time_index = (delta_time_index<<4) | (0<<1) | 1; break; // 'X'
			// 		case 3: delta_time_index = (delta_time_index<<4) | (1<<1) | 1; break; // 'Z'
			// 		// Not supporting VHDL now
			// 		// LCOV_EXCL_START
			// 		case 4: delta_time_index = (delta_time_index<<4) | (2<<1) | 1; break; // 'H'
			// 		case 5: delta_time_index = (delta_time_index<<4) | (3<<1) | 1; break; // 'U'
			// 		case 6: delta_time_index = (delta_time_index<<4) | (4<<1) | 1; break; // 'W'
			// 		case 7: delta_time_index = (delta_time_index<<4) | (5<<1) | 1; break; // 'L'
			// 		case 8: delta_time_index = (delta_time_index<<4) | (6<<1) | 1; break; // '-'
			// 		case 9: delta_time_index = (delta_time_index<<4) | (7<<1) | 1; break; // '?'
			// 		default: break;
			// 		// LCOV_EXCL_STOP
			// 		}
			// 		h.WriteLEB128(delta_time_index);
			// 	}
			// 	rh.Skip(num_byte);
			// }
		} else {
			while (true) {
				if (rh.ptr == tail) {
					break;
				}
				CHECK_GT(tail, rh.ptr);
				const auto time_index = rh.Read<uint64_t>();
				const auto enc = rh.Read<EncodingType>();
				const auto num_element = BitPerEncodedBit(enc);
				const auto num_byte = num_element * sizeof(T);
				if (first) {
					first = false;
				} else {
					CHECK(enc == EncodingType::eBinary); // TODO
					const bool has_non_binary = enc != EncodingType::eBinary;
					const uint64_t delta_time_index = time_index - prev_time_index;
					prev_time_index = time_index;
					h
					.WriteLEB128((delta_time_index << 1) | has_non_binary)
					.WriteUIntPartialForValueChange(rh.Peek<T>(), bitwidth);
				}
				rh.Skip(num_byte);
			}
		}
	}
};

class VariableInfoLongInt {
	VariableInfo& info;
	unsigned num_words() const { return (info.bitwidth + 63) / 64; }

public:
	VariableInfoLongInt(VariableInfo& info_) : info(info_) {}

private:
	inline void EmitValueChangeCommonPart(uint64_t current_time_index, EncodingType encoding) {
		if (current_time_index+1 == 0) {
			info.data.resize(0);
		}
		StreamVectorWriteHelper(info.data)
		.Write(current_time_index) // time index
		.Write(encoding); // encoding
	}

public:
	void Construct() {
		StreamVectorWriteHelper h(info.data);
		const unsigned nw = num_words();
		h
		.Write(uint64_t(0)) // initial time index
		.Write(EncodingType::eVerilog) // initial encoding
		.Fill(uint64_t(0), nw).Fill(uint64_t(-1), nw); // initial X value
	}

	uint64_t EmitValueChange(uint64_t current_time_index, const uint64_t val) {
		EmitValueChangeCommonPart(current_time_index, EncodingType::eBinary);
		const unsigned nw = num_words();
		StreamVectorWriteHelper h(info.data);
		h.Write(val).Fill(uint64_t(0), nw - 1);
		return sizeof(uint64_t) + sizeof(EncodingType) + sizeof(uint64_t) * nw;
	}

	uint64_t EmitValueChange(uint64_t current_time_index, const uint32_t *val, EncodingType encoding) {
		EmitValueChangeCommonPart(current_time_index, encoding);
		StreamVectorWriteHelper h(info.data);
		// 32 bit is not the native encoding for LongInt
		// value_changes is vector<uint64_t>
		const unsigned nw32 = (info.bitwidth + 31) / 32;
		const unsigned nw64 = num_words();
		const unsigned bpb = BitPerEncodedBit(encoding);
		for (unsigned i = 0; i < bpb; ++i) {
			for (unsigned j = 0; j < nw32/2; ++j) {
				uint64_t v = val[1]; // high bits
				v <<= 32;
				v |= val[0]; // low bits
				h.Write(v);
				val += 2;
			}
			if (nw32 % 2 != 0) {
				h.Write(uint64_t(val[0]));
				val += 1;
			}
		}
		return sizeof(uint64_t) + sizeof(EncodingType) + sizeof(uint64_t) * nw64 * bpb;
	}

	uint64_t EmitValueChange(uint64_t current_time_index, const uint64_t *val, EncodingType encoding) {
		EmitValueChangeCommonPart(current_time_index, encoding);
		const unsigned nw64 = num_words() * BitPerEncodedBit(encoding);
		StreamVectorWriteHelper(info.data).Fill(val, nw64);
		return sizeof(uint64_t) + sizeof(EncodingType) + sizeof(uint64_t) * nw64;
	}

	void DumpInitialBits(std::vector<uint8_t> &buf) const {
		DCHECK_GT(info.data.size(), sizeof(uint64_t) + sizeof(EncodingType));
		StreamVectorReaderHelper rh(info.data.data());
		const auto time_index = rh.Read<uint64_t>(); (void)time_index;
		const auto enc = rh.Read<EncodingType>();
		const unsigned nw = num_words();
		switch (enc) {
		case EncodingType::eBinary: {
			for (unsigned word_index = nw; word_index-- > 0;) {
				const uint64_t v0 = rh.Peek<uint64_t>(word_index);
				const unsigned num_bit = (word_index * 64 + 64 > info.bitwidth) ? (info.bitwidth % 64) : 64;
				for (unsigned bit_index = num_bit; bit_index-- > 0;) {
					const char c = ((v0 >> bit_index) & uint64_t(1)) ? '1' : '0';
					buf.push_back(c);
				}
			}
			break;
		}
		case EncodingType::eVerilog: {
			for (unsigned word_index = nw; word_index-- > 0;) {
				const uint64_t v0 = rh.Peek<uint64_t>(nw*0 + word_index);
				const uint64_t v1 = rh.Peek<uint64_t>(nw*1 + word_index);
				const unsigned num_bit = (word_index * 64 + 64 > info.bitwidth) ? (info.bitwidth % 64) : 64;
				for (unsigned bit_index = num_bit; bit_index-- > 0;) {
					const bool b0 = ((v0 >> bit_index) & uint64_t(1));
					const bool b1 = ((v1 >> bit_index) & uint64_t(1));
					const char c = kEncodedBitToCharTable[(b1 << 1) | b0];
					buf.push_back(c);
				}
			}
			break;
		}
		default:
		case EncodingType::eVhdl: {
			// Not supporting VHDL now
			// LCOV_EXCL_START
			for (unsigned word_index = nw; word_index-- > 0;) {
				const uint64_t v0 = rh.Peek<uint64_t>(nw*0 + word_index);
				const uint64_t v1 = rh.Peek<uint64_t>(nw*1 + word_index);
				const uint64_t v2 = rh.Peek<uint64_t>(nw*2 + word_index);
				const unsigned num_bit = (word_index * 64 + 64 > info.bitwidth) ? (info.bitwidth % 64) : 64;
				for (unsigned bit_index = num_bit; bit_index-- > 0;) {
					const bool b0 = ((v0 >> bit_index) & uint64_t(1));
					const bool b1 = ((v1 >> bit_index) & uint64_t(1));
					const bool b2 = ((v2 >> bit_index) & uint64_t(1));
					const char c = kEncodedBitToCharTable[(b2 << 2) | (b1 << 1) | b0];
					buf.push_back(c);
				}
			}
			break;
			// LCOV_EXCL_STOP
		}
		rh.Skip(sizeof(uint64_t) * nw * BitPerEncodedBit(enc));
		}
	}

	void DumpValueChanges(std::vector<uint8_t> &buf) const {
		StreamVectorWriteHelper h(buf);
		StreamVectorReaderHelper rh(info.data.data());
		const uint8_t* tail = info.data.data() + info.data.size();
		const unsigned nw = num_words();
		const unsigned bitwidth = info.bitwidth; // Local copy for lambda capture/usage if needed

		bool first = true;
		uint64_t prev_time_index = 0;

		while (true) {
			if (rh.ptr == tail) break;
			DCHECK_GT(tail, rh.ptr);
			const auto time_index = rh.Read<uint64_t>();
			const auto enc = rh.Read<EncodingType>();
			const auto num_element = BitPerEncodedBit(enc);
			const auto num_byte = num_element * nw * sizeof(uint64_t);
			if (first) {
				// Note: [0] is initial value, which is already dumped in DumpInitialBits()
				first = false;
			} else {
				CHECK(enc == EncodingType::eBinary); // TODO
				const bool has_non_binary = enc != EncodingType::eBinary;
				const uint64_t delta_time_index = time_index - prev_time_index;
				prev_time_index = time_index;
				h.WriteLEB128((delta_time_index << 1) | has_non_binary);
				if (bitwidth % 64 != 0) {
					const unsigned remaining = bitwidth % 64;
					uint64_t hi64 = rh.Peek<uint64_t>(nw-1);
					// Write from nw-1 to 1
					for (unsigned j = nw - 1; j > 0; --j) {
						uint64_t lo64 = rh.Peek<uint64_t>(j-1);
						h.WriteUIntBE(
							(hi64 << (64 - remaining)) |
							(lo64 >> remaining)
						);
						hi64 = lo64;
					}
					// Write 0
					h.WriteUIntPartialForValueChange(hi64, remaining);
				} else {
					// Write from nw-1 to 0
					for (unsigned j = nw; j-- > 0;) {
						h.WriteUIntBE(rh.Peek<uint64_t>(j));
					}
				}
			}
			rh.Skip(num_byte);
		}
	}
};


} // namespace detail

template<typename Callable, typename... Args>
auto VariableInfo::DispatchHelper(Callable&& callable, Args&&... args) const {
	if (not is_real) {
		// Decision: the branch miss is too expensive for large design, so we only use 3 types of int
		if (bitwidth <= 8) {
			return callable(detail::VariableInfoScalarInt<uint8_t>(const_cast<VariableInfo&>(*this)), std::forward<Args>(args)...);
		// } else if (bitwidth <= 16) {
		// 	return callable(detail::VariableInfoScalarInt<uint16_t>(const_cast<VariableInfo&>(*this)), std::forward<Args>(args)...);
		// } else if (bitwidth <= 32) {
		// 	return callable(detail::VariableInfoScalarInt<uint32_t>(const_cast<VariableInfo&>(*this)), std::forward<Args>(args)...);
		} else if (bitwidth <= 64) {
			return callable(detail::VariableInfoScalarInt<uint64_t>(const_cast<VariableInfo&>(*this)), std::forward<Args>(args)...);
		} else {
			return callable(detail::VariableInfoLongInt(const_cast<VariableInfo&>(*this)), std::forward<Args>(args)...);
		}
	}
	return callable(detail::VariableInfoDouble(const_cast<VariableInfo&>(*this)), std::forward<Args>(args)...);
}

inline VariableInfo::VariableInfo(uint32_t bitwidth_, bool is_real_) : bitwidth(bitwidth_), is_real(is_real_) {
	DispatchHelper([this](auto obj) {
		obj.Construct();
		last_written_bytes = data.size();
	});
}

inline uint32_t VariableInfo::EmitValueChange(uint64_t current_time_index, const uint64_t val) {
	last_written_bytes = DispatchHelper([=](auto obj) {
		return obj.EmitValueChange(current_time_index, val);
	});
	return last_written_bytes;
}

inline uint32_t VariableInfo::EmitValueChange(uint64_t current_time_index, const uint32_t *val, EncodingType encoding) {
	last_written_bytes = DispatchHelper([=](auto obj) {
		return obj.EmitValueChange(current_time_index, val, encoding);
	});
	return last_written_bytes;
}

inline uint32_t VariableInfo::EmitValueChange(uint64_t current_time_index, const uint64_t *val, EncodingType encoding) {
	last_written_bytes = DispatchHelper([=](auto obj) {
		return obj.EmitValueChange(current_time_index, val, encoding);
	});
	return last_written_bytes;
}

inline void VariableInfo::DumpInitialBits(std::vector<uint8_t> &buf) const {
	DispatchHelper([&](auto obj) {
		obj.DumpInitialBits(buf);
	});
}

inline void VariableInfo::DumpValueChanges(std::vector<uint8_t> &buf) const {
	DispatchHelper([&](auto obj) {
		obj.DumpValueChanges(buf);
	});
}

} // namespace fst
