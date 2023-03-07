/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2017-2023 Roman Lebedev

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

#ifndef IMPL
#error IMPL must be defined to one of rawspeeds huffman table implementations
#endif

#include "common/RawspeedException.h"          // for RawspeedException
#include "decompressors/BinaryHuffmanTree.h"   // for BinaryHuffmanTree<>::...
#include "decompressors/HuffmanTable.h"        // IWYU pragma: keep
#include "decompressors/HuffmanTable/Common.h" // for createHuffmanTable
#include "decompressors/HuffmanTableLUT.h"     // IWYU pragma: keep
#include "decompressors/HuffmanTableLookup.h"  // IWYU pragma: keep
#include "decompressors/HuffmanTableTree.h"    // for HuffmanTableTree
#include "decompressors/HuffmanTableVector.h"  // for HuffmanTableVector
#include "io/BitPumpJPEG.h"                    // for BitStream<>::fillCache
#include "io/BitPumpMSB.h"                     // IWYU pragma: keep
#include "io/BitPumpMSB32.h"                   // IWYU pragma: keep
#include "io/BitStream.h"                      // for BitStream
#include "io/Buffer.h"                         // for Buffer, DataBuffer
#include "io/ByteStream.h"                     // for ByteStream
#include "io/Endianness.h"                     // for Endianness, Endiannes...
#include <cassert>                             // for assert
#include <cstdint>                             // for uint8_t
#include <cstdio>                              // for size_t
#include <initializer_list>                    // for initializer_list
#include <vector>                              // for vector

namespace rawspeed {
struct BaselineHuffmanTableTag;
struct VC5HuffmanTableTag;
} // namespace rawspeed

template <typename Pump, bool IsFullDecode, typename HT>
static void workloop(rawspeed::ByteStream bs, const HT& ht) {
  Pump bits(bs);
  while (true)
    ht.template decode<Pump, IsFullDecode>(bits);
  // FIXME: do we need to escape the result to avoid dead code elimination?
}

template <typename Pump, typename HT>
static void checkPump(rawspeed::ByteStream bs, const HT& ht) {
  if (ht.isFullDecode())
    workloop<Pump, /*IsFullDecode=*/true>(bs, ht);
  else
    workloop<Pump, /*IsFullDecode=*/false>(bs, ht);
}

template <typename Tag> static void checkFlavour(rawspeed::ByteStream bs) {
  const auto ht = createHuffmanTable<rawspeed::IMPL<Tag>>(bs);

  // should have consumed 16 bytes for n-codes-per-length, at *least* 1 byte
  // as code value, and a byte per 'fixDNGBug16'/'fullDecode' booleans
  assert(bs.getPosition() >= 19);

  // Which bit pump should we use?
  switch (bs.getByte()) {
  case 0:
    checkPump<rawspeed::BitPumpMSB>(bs, ht);
    break;
  case 1:
    checkPump<rawspeed::BitPumpMSB32>(bs, ht);
    break;
  case 2:
    checkPump<rawspeed::BitPumpJPEG>(bs, ht);
    break;
  default:
    ThrowRSE("Unknown bit pump");
  }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size) {
  assert(Data);

  try {
    const rawspeed::Buffer b(Data, Size);
    const rawspeed::DataBuffer db(b, rawspeed::Endianness::little);
    rawspeed::ByteStream bs(db);

    // Which flavor?
    switch (bs.getByte()) {
    case 0:
      checkFlavour<rawspeed::BaselineHuffmanTableTag>(bs);
      break;
    case 1:
      checkFlavour<rawspeed::VC5HuffmanTableTag>(bs);
      break;
    default:
      ThrowRSE("Unknown flavor");
    }
  } catch (const rawspeed::RawspeedException&) {
    return 0;
  }

  __builtin_unreachable();
}
