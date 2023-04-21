/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2017 Axel Waggershauser
    Copyright (C) 2018 Roman Lebedev

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#pragma once

#include "decoders/RawDecoderException.h" // for ThrowException, Thro...
#include "decompressors/AbstractPrefixCodeDecoder.h" // for AbstractPrefixCodeDecoder...
#include "decompressors/BinaryPrefixTree.h" // for BinaryPrefixTree<>:...
#include "io/BitStream.h"                   // for BitStreamTraits
#include <algorithm>                        // for max, for_each, copy
#include <cassert>                          // for invariant
#include <initializer_list>                 // for initializer_list
#include <iterator>                         // for advance, next
#include <memory>                           // for unique_ptr, make_unique
#include <tuple>                            // for tie
#include <utility>                          // for pair
#include <vector>                           // for vector, vector<>::co...

namespace rawspeed {

template <typename CodeTag>
class PrefixCodeTreeDecoder final : public AbstractPrefixCodeDecoder<CodeTag> {
public:
  using Tag = CodeTag;
  using Base = AbstractPrefixCodeDecoder<CodeTag>;
  using Traits = typename Base::Traits;

  using Base::Base;

private:
  BinaryPrefixTree<CodeTag> tree;

  template <typename BIT_STREAM>
  inline std::pair<typename Base::CodeSymbol,
                   typename Traits::CodeValueTy /*codeValue*/>
  readSymbol(BIT_STREAM& bs) const {
    static_assert(
        BitStreamTraits<typename BIT_STREAM::tag>::canUseWithPrefixCodeDecoder,
        "This BitStream specialization is not marked as usable here");
    typename Base::CodeSymbol partial;

    const auto* top = &(tree.root->getAsBranch());

    // Read bits until either find the code or detect the incorrect code
    for (partial.code = 0, partial.code_len = 1;; ++partial.code_len) {
      invariant(partial.code_len <= Traits::MaxCodeLenghtBits);

      // Read one more bit
      const bool bit = bs.getBitsNoFill(1);

      // codechecker_false_positive [core.uninitialized.Assign]
      partial.code <<= 1;
      partial.code |= bit;

      // What is the last bit, which we have just read?

      // NOTE: The order *IS* important! Left to right, zero to one!
      const auto& newNode = top->buds[bit];

      if (!newNode) {
        // Got nothing in this direction.
        ThrowRDE("bad Huffman code: %u (len: %u)", partial.code,
                 partial.code_len);
      }

      if (static_cast<typename decltype(tree)::Node::Type>(*newNode) ==
          decltype(tree)::Node::Type::Leaf) {
        // Ok, great, hit a Leaf. This is it.
        return {partial, newNode->getAsLeaf().value};
      }

      // Else, this is a branch, continue looking.
      top = &(newNode->getAsBranch());
    }

    // We have either returned the found symbol, or thrown on incorrect symbol.
    __builtin_unreachable();
  }

public:
  void setup(bool fullDecode_, bool fixDNGBug16_) {
    AbstractPrefixCodeDecoder<CodeTag>::setup(fullDecode_, fixDNGBug16_);

    assert(Base::code.symbols.size() == Base::code.codeValues.size());
    for (unsigned codeIndex = 0; codeIndex != Base::code.symbols.size();
         ++codeIndex)
      tree.add(Base::code.symbols[codeIndex], Base::code.codeValues[codeIndex]);
  }

  template <typename BIT_STREAM>
  inline typename Traits::CodeValueTy decodeCodeValue(BIT_STREAM& bs) const {
    static_assert(
        BitStreamTraits<typename BIT_STREAM::tag>::canUseWithPrefixCodeDecoder,
        "This BitStream specialization is not marked as usable here");
    invariant(!Base::fullDecode);
    return decode<BIT_STREAM, false>(bs);
  }

  template <typename BIT_STREAM>
  inline int decodeDifference(BIT_STREAM& bs) const {
    static_assert(
        BitStreamTraits<typename BIT_STREAM::tag>::canUseWithPrefixCodeDecoder,
        "This BitStream specialization is not marked as usable here");
    invariant(Base::fullDecode);
    return decode<BIT_STREAM, true>(bs);
  }

  // The bool template paraeter is to enable two versions:
  // one returning only the length of the of diff bits (see Hasselblad),
  // one to return the fully decoded diff.
  // All ifs depending on this bool will be optimized out by the compiler
  template <typename BIT_STREAM, bool FULL_DECODE>
  inline int decode(BIT_STREAM& bs) const {
    static_assert(
        BitStreamTraits<typename BIT_STREAM::tag>::canUseWithPrefixCodeDecoder,
        "This BitStream specialization is not marked as usable here");
    invariant(FULL_DECODE == Base::fullDecode);

    bs.fill(32);

    typename Base::CodeSymbol symbol;
    typename Traits::CodeValueTy codeValue;
    std::tie(symbol, codeValue) = readSymbol(bs);

    return Base::template processSymbol<BIT_STREAM, FULL_DECODE>(bs, symbol,
                                                                 codeValue);
  }
};

} // namespace rawspeed
