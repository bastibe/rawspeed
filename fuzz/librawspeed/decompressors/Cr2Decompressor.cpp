/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2022 Roman Lebedev

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

#include "decompressors/Cr2Decompressor.h"
#include "HuffmanTable/Common.h"        // for createHuffmanTable
#include "common/RawImage.h"            // for RawImage, RawImageData
#include "common/RawspeedException.h"   // for RawspeedException
#include "decompressors/HuffmanTable.h" // for HuffmanTable
#include "fuzz/Common.h"                // for CreateRawImage
#include "io/Buffer.h"                  // for Buffer, DataBuffer
#include "io/ByteStream.h"              // for ByteStream
#include "io/Endianness.h"              // for Endianness, Endianness::little
#include <cassert>                      // for assert
#include <cstdint>                      // for uint8_t
#include <cstdio>                       // for size_t

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size) {
  assert(Data);

  try {
    const rawspeed::Buffer b(Data, Size);
    const rawspeed::DataBuffer db(b, rawspeed::Endianness::little);
    rawspeed::ByteStream bs(db);

    rawspeed::RawImage mRaw(CreateRawImage(bs));

    const int N_COMP = bs.getI32();
    const int X_S_F = bs.getI32();
    const int Y_S_F = bs.getI32();
    const std::tuple<int /*N_COMP*/, int /*X_S_F*/, int /*Y_S_F*/> format = {
        N_COMP, X_S_F, Y_S_F};

    const int frame_w = bs.getI32();
    const int frame_h = bs.getI32();
    const rawspeed::iPoint2D frame(frame_w, frame_h);

    using slice_type = uint16_t;
    const auto numSlices = bs.get<slice_type>();
    const auto sliceWidth = bs.get<slice_type>();
    const auto lastSliceWidth = bs.get<slice_type>();

    const rawspeed::Cr2Slicing slicing(numSlices, sliceWidth, lastSliceWidth);

    const unsigned num_unique_hts = bs.getU32();
    std::vector<rawspeed::HuffmanTable> uniqueHts;
    std::generate_n(std::back_inserter(uniqueHts), num_unique_hts, [&bs]() {
      return createHuffmanTable<rawspeed::HuffmanTable>(bs);
    });

    const unsigned num_hts = bs.getU32();
    std::vector<const rawspeed::HuffmanTable*> hts;
    std::generate_n(std::back_inserter(hts), num_hts, [&bs, &uniqueHts]() {
      if (unsigned uniq_ht_idx = bs.getU32(); uniq_ht_idx < uniqueHts.size())
        return &uniqueHts[uniq_ht_idx];
      ThrowRSE("Unknown unique huffman table");
    });

    const unsigned num_pred = bs.getU32();
    (void)bs.check(num_pred, sizeof(uint16_t));
    std::vector<uint16_t> initPred;
    initPred.reserve(num_pred);
    std::generate_n(std::back_inserter(initPred), num_pred,
                    [&bs]() { return bs.get<uint16_t>(); });

    rawspeed::Cr2Decompressor d(mRaw, format, frame, slicing, hts, initPred,
                                bs.getSubStream(/*offset=*/0));
    mRaw->createData();
    d.decompress();

    mRaw->checkMemIsInitialized();
  } catch (const rawspeed::RawspeedException&) {
    // Exceptions are good, crashes are bad.
  }

  return 0;
}
