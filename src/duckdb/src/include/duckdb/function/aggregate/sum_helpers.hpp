//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/aggregate/sum_helpers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

struct RegularAdd {
	template <class STATE, class T>
	static void AddNumber(STATE &state, T input) {
		state.value += input;
	}

	template <class STATE, class T>
	static void AddConstant(STATE &state, T input, idx_t count) {
		state.value += input * count;
	}
};

struct HugeintAdd {
	static void AddValue(hugeint_t &result, uint64_t value, int positive) {
		// integer summation taken from Tim Gubner et al. - Efficient Query Processing
		// with Optimistically Compressed Hash Tables & Strings in the USSR

		// add the value to the lower part of the hugeint
		result.lower += value;
		// now handle overflows
		int overflow = result.lower < value;
		// we consider two situations:
		// (1) input[idx] is positive, and current value is lower than value: overflow
		// (2) input[idx] is negative, and current value is higher than value: underflow
		if (!(overflow ^ positive)) {
			// in the case of an overflow or underflow we either increment or decrement the upper base
			// positive: +1, negative: -1
			result.upper += -1 + 2 * positive;
		}
	}

	template <class STATE, class T>
	static void AddNumber(STATE &state, T input) {
		AddValue(state.value, uint64_t(input), input >= 0);
	}

	template <class STATE, class T>
	static void AddConstant(STATE &state, T input, idx_t count) {
		// add a constant X number of times
		// fast path: check if value * count fits into a uint64_t
		// note that we check if value * VECTOR_SIZE fits in a uint64_t to avoid having to actually do a division
		// this is still a pretty high number (18014398509481984) so most positive numbers will fit
		if (input >= 0 && uint64_t(input) < (NumericLimits<uint64_t>::Maximum() / STANDARD_VECTOR_SIZE)) {
			// if it does just multiply it and add the value
			uint64_t value = uint64_t(input) * count;
			AddValue(state.value, value, 1);
		} else {
			// if it doesn't fit we have two choices
			// either we loop over count and add the values individually
			// or we convert to a hugeint and multiply the hugeint
			// the problem is that hugeint multiplication is expensive
			// hence we switch here: with a low count we do the loop
			// with a high count we do the hugeint multiplication
			if (count < 8) {
				for (idx_t i = 0; i < count; i++) {
					AddValue(state.value, uint64_t(input), input >= 0);
				}
			} else {
				hugeint_t addition = hugeint_t(input) * count;
				state.value += addition;
			}
		}
	}
};

template <class STATEOP, class ADDOP>
struct BaseSumOperation {
	template <class STATE>
	static void Initialize(STATE *state) {
		state->value = 0;
		STATEOP::template Initialize<STATE>(state);
	}

	template <class STATE, class OP>
	static void Combine(STATE source, STATE *target) {
		STATEOP::template Combine<STATE>(source, target);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask, idx_t idx) {
		STATEOP::template AddValues<STATE>(state, 1);
		ADDOP::template AddNumber<STATE, INPUT_TYPE>(*state, input[idx]);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask,
	                              idx_t count) {
		STATEOP::template AddValues<STATE>(state, count);
		ADDOP::template AddConstant<STATE, INPUT_TYPE>(*state, *input, count);
	}

	static bool IgnoreNull() {
		return true;
	}
};

} // namespace duckdb
