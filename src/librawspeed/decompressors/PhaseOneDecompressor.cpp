/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2014-2015 Pedro Côrte-Real
    Copyright (C) 2017-2018 Roman Lebedev

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

#include "rawspeedconfig.h"
#include "decompressors/PhaseOneDecompressor.h"
#include "adt/Array1DRef.h"
#include "adt/Array2DRef.h"
#include "adt/Casts.h"
#include "adt/Invariant.h"
#include "adt/Point.h"
#include "bitstreams/BitStreamerMSB32.h"
#include "common/Common.h"
#include "common/RawImage.h"
#include "decoders/RawDecoderException.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rawspeed {

PhaseOneDecompressor::PhaseOneDecompressor(RawImage img,
                                           std::vector<PhaseOneStrip>&& strips_)
    : mRaw(std::move(img)), strips(std::move(strips_)) {
  if (mRaw->getDataType() != RawImageType::UINT16)
    ThrowRDE("Unexpected data type");

  if (mRaw->getCpp() != 1 || mRaw->getBpp() != sizeof(uint16_t))
    ThrowRDE("Unexpected cpp: %u", mRaw->getCpp());

  if (!mRaw->dim.hasPositiveArea() || mRaw->dim.x % 2 != 0 ||
      mRaw->dim.x > 11976 || mRaw->dim.y > 8854) {
    ThrowRDE("Unexpected image dimensions found: (%d; %d)", mRaw->dim.x,
             mRaw->dim.y);
  }

  prepareStrips();
}

void PhaseOneDecompressor::prepareStrips() {
  // The 'strips' vector should contain exactly one element per row of image.

  // If the length is different, then the 'strips' vector is clearly incorrect.
  if (strips.size() != static_cast<decltype(strips)::size_type>(mRaw->dim.y)) {
    ThrowRDE("Height (%d) vs strip count %zu mismatch", mRaw->dim.y,
             strips.size());
  }

  // Now, the strips in 'strips' vector aren't in order.
  // The 'decltype(strips)::value_type::n' is the row number of a strip.
  // We need to make sure that we have every row (0..mRaw->dim.y-1), once.
  // For that, first let's sort them to have monothonically increasting `n`.
  // This will also serialize the per-line outputting.
  std::sort(
      strips.begin(), strips.end(),
      [](const PhaseOneStrip& a, const PhaseOneStrip& b) { return a.n < b.n; });
  // And now ensure that slice number matches the slice's row.
  for (decltype(strips)::size_type i = 0; i < strips.size(); ++i)
    if (static_cast<decltype(strips)::size_type>(strips[i].n) != i)
      ThrowRDE("Strips validation issue.");
  // All good.
}

void PhaseOneDecompressor::decompressStrip(const PhaseOneStrip& strip) const {
  const Array2DRef<uint16_t> out(mRaw->getU16DataAsUncroppedArray2DRef());

  invariant(out.width() > 0);
  invariant(out.width() % 2 == 0);

  static constexpr std::array<const int, 10> length = {8,  7, 6,  9,  11,
                                                       10, 5, 12, 14, 13};

  BitStreamerMSB32 pump(strip.bs.peekRemainingBuffer().getAsArray1DRef());

  std::array<int32_t, 2> pred;
  pred.fill(0);
  std::array<int, 2> len;
  const int row = strip.n;
  for (int col = 0; col < out.width(); col++) {
    pump.fill(32);
    if (static_cast<unsigned>(col) >=
        (out.width() & ~7U)) // last 'width % 8' pixels.
      len[0] = len[1] = 14;
    else if ((col & 7) == 0) {
      for (int& i : len) {
        int j = 0;

        for (; j < 5; j++) {
          if (pump.getBitsNoFill(1) != 0) {
            if (col == 0)
              ThrowRDE("Can not initialize lengths. Data is corrupt.");

            // else, we have previously initialized lengths, so we are fine
            break;
          }
        }

        invariant((col == 0 && j > 0) || col != 0);
        if (j > 0)
          i = length[2 * (j - 1) + pump.getBitsNoFill(1)];
      }
    }

    int i = len[col & 1];
    if (i == 14) {
      pred[col & 1] = pump.getBitsNoFill(16);
      out(row, col) = implicit_cast<uint16_t>(pred[col & 1]);
    } else {
      pred[col & 1] +=
          static_cast<signed>(pump.getBitsNoFill(i)) + 1 - (1 << (i - 1));
      // FIXME: is the truncation the right solution here?
      out(row, col) = uint16_t(pred[col & 1]);
    }
  }
}

void PhaseOneDecompressor::decompressThread() const noexcept {
#ifdef HAVE_OPENMP
#pragma omp for schedule(static)
#endif
  for (const auto& strip :
       Array1DRef(strips.data(), implicit_cast<int>(strips.size()))) {
    try {
      decompressStrip(strip);
    } catch (const RawspeedException& err) {
      // Propagate the exception out of OpenMP magic.
      mRaw->setError(err.what());
    } catch (...) {
      // We should not get any other exception type here.
      __builtin_unreachable();
    }
  }
}

void PhaseOneDecompressor::decompress() const {
#ifdef HAVE_OPENMP
#pragma omp parallel default(none)                                             \
    num_threads(rawspeed_get_number_of_processor_cores())
#endif
  decompressThread();

  std::string firstErr;
  if (mRaw->isTooManyErrors(1, &firstErr)) {
    ThrowRDE("Too many errors encountered. Giving up. First Error:\n%s",
             firstErr.c_str());
  }
}

} // namespace rawspeed
