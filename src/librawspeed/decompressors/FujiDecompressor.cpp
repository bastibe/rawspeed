/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2016 Alexey Danilchenko
    Copyright (C) 2016 Alex Tutubalin
    Copyright (C) 2017 Uwe Müssel
    Copyright (C) 2017 Roman Lebedev

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "rawspeedconfig.h" // for HAVE_OPENMP
#include "decompressors/FujiDecompressor.h"
#include "MemorySanitizer.h"              // for MSan
#include "adt/Array2DRef.h"               // for Array2DRef
#include "adt/CroppedArray1DRef.h"        // for CroppedArray1DRef
#include "adt/CroppedArray2DRef.h"        // for CroppedArray2DRef
#include "adt/Point.h"                    // for iPoint2D
#include "common/BayerPhase.h"            // for getAsCFAColors, BayerPhase
#include "common/Common.h"                // for rawspeed_get_number_of_pro...
#include "common/RawImage.h"              // for RawImageData, RawImage
#include "common/XTransPhase.h"           // for XTransPhase, getAsCFAColors
#include "decoders/RawDecoderException.h" // for ThrowException, ThrowRDE
#include "io/Endianness.h"                // for Endianness, Endianness::big
#include "metadata/ColorFilterArray.h"    // for CFAColor, CFAColor::BLUE
#include <algorithm>                      // for max, fill, min, minmax
#include <cmath>                          // for abs
#include <cstdint>                        // for uint16_t, uint32_t, int8_t
#include <cstdlib>                        // for abs
#include <cstring>                        // for memcpy, memset
#include <initializer_list>               // for initializer_list
#include <optional>                       // for optional, operator!=
#include <string>                         // for string

namespace rawspeed {

namespace {

struct BayerTag;
struct XTransTag;

template <typename T> constexpr iPoint2D MCU;

template <> constexpr iPoint2D MCU<BayerTag> = {2, 2};

template <> constexpr iPoint2D MCU<XTransTag> = {6, 6};

} // namespace

FujiDecompressor::FujiDecompressor(const RawImage& img, ByteStream input_)
    : mRaw(img), input(input_) {
  if (mRaw->getCpp() != 1 || mRaw->getDataType() != RawImageType::UINT16 ||
      mRaw->getBpp() != sizeof(uint16_t))
    ThrowRDE("Unexpected component count / data type");

  input.setByteOrder(Endianness::big);

  header = FujiHeader(input);
  if (!header)
    ThrowRDE("compressed RAF header check");

  if (mRaw->dim != iPoint2D(header.raw_width, header.raw_height))
    ThrowRDE("RAF header specifies different dimensions!");

  if (12 == header.raw_bits) {
    ThrowRDE("Aha, finally, a 12-bit compressed RAF! Please consider providing "
             "samples on <https://raw.pixls.us/>, thanks!");
  }

  if (mRaw->cfa.getSize() == iPoint2D(6, 6)) {
    std::optional<XTransPhase> p = getAsXTransPhase(mRaw->cfa);
    if (!p)
      ThrowRDE("Invalid X-Trans CFA");
    if (p != iPoint2D(0, 0))
      ThrowRDE("Unexpected X-Trans phase: {%i,%i}. Please file a bug!", p->x,
               p->y);
  } else if (mRaw->cfa.getSize() == iPoint2D(2, 2)) {
    std::optional<BayerPhase> p = getAsBayerPhase(mRaw->cfa);
    if (!p)
      ThrowRDE("Invalid Bayer CFA");
    if (p != BayerPhase::RGGB)
      ThrowRDE("Unexpected Bayer phase: %i. Please file a bug!",
               static_cast<int>(*p));
  } else
    ThrowRDE("Unexpected CFA size");

  fuji_compressed_load_raw();
}

FujiDecompressor::fuji_compressed_params::fuji_compressed_params(
    const FujiDecompressor& d) {
  int cur_val;

  if ((d.header.block_size % 3 && d.header.raw_type == 16) ||
      (d.header.block_size & 1 && d.header.raw_type == 0)) {
    ThrowRDE("fuji_block_checks");
  }

  if (d.header.raw_type == 16) {
    line_width = (d.header.block_size * 2) / 3;
  } else {
    line_width = d.header.block_size >> 1;
  }

  q_point[0] = 0;
  q_point[1] = 0x12;
  q_point[2] = 0x43;
  q_point[3] = 0x114;
  q_point[4] = (1 << d.header.raw_bits) - 1;
  min_value = 0x40;

  cur_val = -q_point[4];
  q_table.resize(2 * (1 << d.header.raw_bits));

  for (int8_t* qt = &q_table[0]; cur_val <= q_point[4]; ++qt, ++cur_val) {
    if (cur_val <= -q_point[3]) {
      *qt = -4;
    } else if (cur_val <= -q_point[2]) {
      *qt = -3;
    } else if (cur_val <= -q_point[1]) {
      *qt = -2;
    } else if (cur_val < 0) {
      *qt = -1;
    } else if (cur_val == 0) {
      *qt = 0;
    } else if (cur_val < q_point[1]) {
      *qt = 1;
    } else if (cur_val < q_point[2]) {
      *qt = 2;
    } else if (cur_val < q_point[3]) {
      *qt = 3;
    } else {
      *qt = 4;
    }
  }

  // populting gradients
  if (q_point[4] == 0xFFFF) { // (1 << d.header.raw_bits) - 1
    total_values = 0x10000;   // 1 << d.header.raw_bits
    raw_bits = 16;            // d.header.raw_bits
    max_bits = 64;            // d.header.raw_bits * (64 / d.header.raw_bits)
    maxDiff = 1024;           // 1 << (d.header.raw_bits - 6)
  } else if (q_point[4] == 0x3FFF) {
    total_values = 0x4000;
    raw_bits = 14;
    max_bits = 56;
    maxDiff = 256;
  } else if (q_point[4] == 0xFFF) {
    total_values = 4096;
    raw_bits = 12;
    max_bits = 48; // out-of-family, there's greater pattern at play.
    maxDiff = 64;

    ThrowRDE("Aha, finally, a 12-bit compressed RAF! Please consider providing "
             "samples on <https://raw.pixls.us/>, thanks!");
  } else {
    ThrowRDE("FUJI q_point");
  }
}

void FujiDecompressor::fuji_compressed_block::reset(
    const fuji_compressed_params& params) {
  const unsigned line_size = sizeof(uint16_t) * (params.line_width + 2);

  linealloc.resize(ltotal * (params.line_width + 2), 0);
  lines = Array2DRef<uint16_t>(&linealloc[0], params.line_width + 2, ltotal);

  MSan::Allocated(CroppedArray2DRef(lines));

  // Zero-initialize first two (read-only, carry-in) lines of each color,
  // including first and last helper columns of the second row.
  // This is needed for correctness.
  for (xt_lines color : {R0, G0, B0}) {
    memset(&lines(color, 0), 0, 2 * line_size);

    // On the first row, we don't need to zero-init helper columns.
    MSan::Allocated(lines(color, 0));
    MSan::Allocated(lines(color, lines.width - 1));
  }

  // And the first (real, uninitialized) line of each color gets the content
  // of the last helper column from the last decoded sample of previous
  // line of that color.
  // Again, this is needed for correctness.
  for (xt_lines color : {R2, G2, B2})
    lines(color, lines.width - 1) = lines(color - 1, lines.width - 2);

  for (int j = 0; j < 3; j++) {
    for (int i = 0; i < 41; i++) {
      grad_even[j][i].value1 = params.maxDiff;
      grad_even[j][i].value2 = 1;
      grad_odd[j][i].value1 = params.maxDiff;
      grad_odd[j][i].value2 = 1;
    }
  }
}

template <typename Tag, typename T>
void FujiDecompressor::copy_line(const fuji_compressed_block& info,
                                 const FujiStrip& strip, int cur_line,
                                 T&& idx) const {
  const Array2DRef<uint16_t> img(mRaw->getU16DataAsUncroppedArray2DRef());

  std::array<CFAColor, MCU<Tag>.x * MCU<Tag>.y> CFAData;
  if constexpr (std::is_same_v<XTransTag, Tag>)
    CFAData = getAsCFAColors(XTransPhase(0, 0));
  else if constexpr (std::is_same_v<BayerTag, Tag>)
    CFAData = getAsCFAColors(BayerPhase::RGGB);
  else
    __builtin_unreachable();
  const Array2DRef<const CFAColor> CFA(CFAData.data(), MCU<Tag>.x, MCU<Tag>.y);

  iPoint2D MCUIdx;
  assert(MCU<Tag> == strip.h.MCU);
  const iPoint2D NumMCUs = strip.numMCUs(MCU<Tag>);
  for (MCUIdx.x = 0; MCUIdx.x != NumMCUs.x; ++MCUIdx.x) {
    for (MCUIdx.y = 0; MCUIdx.y != NumMCUs.y; ++MCUIdx.y) {
      const auto out =
          CroppedArray2DRef(img, strip.offsetX() + MCU<Tag>.x * MCUIdx.x,
                            strip.offsetY(cur_line) + MCU<Tag>.y * MCUIdx.y,
                            MCU<Tag>.x, MCU<Tag>.y);
      for (int MCURow = 0; MCURow != MCU<Tag>.y; ++MCURow) {
        for (int MCUCol = 0; MCUCol != MCU<Tag>.x; ++MCUCol) {
          int imgRow = MCU<Tag>.y * MCUIdx.y + MCURow;
          int imgCol = MCU<Tag>.x * MCUIdx.x + MCUCol;

          int row;

          switch (CFA(MCURow, MCUCol)) {
          case CFAColor::RED: // red
            row = R2 + (imgRow >> 1);
            break;

          case CFAColor::GREEN: // green
            row = G2 + imgRow;
            break;

          case CFAColor::BLUE: // blue
            row = B2 + (imgRow >> 1);
            break;

          default:
            __builtin_unreachable();
          }

          out(MCURow, MCUCol) = info.lines(row, 1 + idx(imgCol));
        }
      }
    }
  }
}

void FujiDecompressor::copy_line_to_xtrans(const fuji_compressed_block& info,
                                           const FujiStrip& strip,
                                           int cur_line) const {
  auto index = [](int imgCol) {
    return (((imgCol * 2 / 3) & 0x7FFFFFFE) | ((imgCol % 3) & 1)) +
           ((imgCol % 3) >> 1);
  };

  copy_line<XTransTag>(info, strip, cur_line, index);
}

void FujiDecompressor::copy_line_to_bayer(const fuji_compressed_block& info,
                                          const FujiStrip& strip,
                                          int cur_line) const {
  auto index = [](int imgCol) { return imgCol >> 1; };

  copy_line<BayerTag>(info, strip, cur_line, index);
}

inline int FujiDecompressor::fuji_zerobits(BitPumpMSB& pump) {
  int count = 0;

  // Count-and-skip all the leading `0`s.
  while (true) {
    constexpr int batchSize = 32;
    pump.fill(batchSize);
    uint32_t batch = pump.peekBitsNoFill(batchSize);
    int numZerosInThisBatch = countl_zero(batch);
    count += numZerosInThisBatch;
    bool allZeroes = numZerosInThisBatch == batchSize;
    int numBitsToSkip = numZerosInThisBatch;
    if (!allZeroes)
      numBitsToSkip += 1; // Also skip the first `1`.
    pump.skipBitsNoFill(numBitsToSkip);
    if (!allZeroes)
      break; // We're done!
  }

  return count;
}

// Given two non-negative numbers, how many times must the second number
// be multiplied by 2, for it to become not smaller than the first number?
// We are operating on arithmetical numbers here, without overflows.
int __attribute__((const)) FujiDecompressor::bitDiff(int value1, int value2) {
  assert(value1 >= 0);
  assert(value2 > 0);

  int lz1 = countl_zero((unsigned)value1);
  int lz2 = countl_zero((unsigned)value2);
  int decBits = std::max(lz2 - lz1, 0);
  if ((value2 << decBits) < value1)
    ++decBits;
  return std::min(decBits, 15);
}

__attribute__((always_inline)) int
FujiDecompressor::fuji_decode_sample(fuji_compressed_block& info, int grad,
                                     int interp_val,
                                     std::array<int_pair, 41>& grads) const {
  int gradient = std::abs(grad);

  int sampleBits = fuji_zerobits(info.pump);

  int codeBits;
  int codeDelta;
  if (sampleBits < common_info.max_bits - common_info.raw_bits - 1) {
    codeBits = bitDiff(grads[gradient].value1, grads[gradient].value2);
    codeDelta = sampleBits << codeBits;
  } else {
    codeBits = common_info.raw_bits;
    codeDelta = 1;
  }

  int code = 0;
  info.pump.fill(32);
  if (codeBits)
    code = info.pump.getBitsNoFill(codeBits);
  code += codeDelta;

  if (code < 0 || code >= common_info.total_values) {
    ThrowRDE("fuji_decode_sample");
  }

  if (code & 1) {
    code = -1 - code / 2;
  } else {
    code /= 2;
  }

  grads[gradient].value1 += std::abs(code);

  if (grads[gradient].value2 == common_info.min_value) {
    grads[gradient].value1 >>= 1;
    grads[gradient].value2 >>= 1;
  }

  grads[gradient].value2++;

  if (grad < 0) {
    interp_val -= code;
  } else {
    interp_val += code;
  }

  if (interp_val < 0) {
    interp_val += common_info.total_values;
  } else if (interp_val > common_info.q_point[4]) {
    interp_val -= common_info.total_values;
  }

  if (interp_val < 0)
    return 0;

  return std::min(interp_val, common_info.q_point[4]);
}

#define fuji_quant_gradient(v1, v2)                                            \
  (9 * ci.q_table[ci.q_point[4] + (v1)] + ci.q_table[ci.q_point[4] + (v2)])

__attribute__((always_inline)) std::pair<int, int>
FujiDecompressor::fuji_decode_interpolation_even_inner(
    const fuji_compressed_block& info, xt_lines c, int col) const {
  const auto& ci = common_info;

  int Rb = info.lines(c - 1, 1 + 2 * (col + 0) + 0);
  int Rc = info.lines(c - 1, 1 + 2 * (col - 1) + 1);
  int Rd = info.lines(c - 1, 1 + 2 * (col + 0) + 1);
  int Rf = info.lines(c - 2, 1 + 2 * (col + 0) + 0);

  int diffRcRb = std::abs(Rc - Rb);
  int diffRfRb = std::abs(Rf - Rb);
  int diffRdRb = std::abs(Rd - Rb);

  int Term0 = 2 * Rb;
  int Term1;
  int Term2;
  if (diffRcRb > std::max(diffRfRb, diffRdRb)) {
    Term1 = Rf;
    Term2 = Rd;
  } else {
    if (diffRdRb > std::max(diffRcRb, diffRfRb)) {
      Term1 = Rf;
    } else {
      Term1 = Rd;
    }
    Term2 = Rc;
  }

  int interp_val = Term0 + Term1 + Term2;
  interp_val >>= 2;

  int grad = fuji_quant_gradient(Rb - Rf, Rc - Rb);
  return {grad, interp_val};
}

__attribute__((always_inline)) std::pair<int, int>
FujiDecompressor::fuji_decode_interpolation_odd_inner(
    const fuji_compressed_block& info, xt_lines c, int col) const {
  const auto& ci = common_info;

  int Ra = info.lines(c + 0, 1 + 2 * (col + 0) + 0);
  int Rb = info.lines(c - 1, 1 + 2 * (col + 0) + 1);
  int Rc = info.lines(c - 1, 1 + 2 * (col + 0) + 0);
  int Rd = info.lines(c - 1, 1 + 2 * (col + 1) + 0);
  int Rg = info.lines(c + 0, 1 + 2 * (col + 1) + 0);

  int interp_val = (Ra + Rg);
  if (auto [min, max] = std::minmax(Rc, Rd); Rb < min || Rb > max) {
    interp_val += 2 * Rb;
    interp_val >>= 1;
  }
  interp_val >>= 1;

  int grad = fuji_quant_gradient(Rb - Rc, Rc - Ra);
  return {grad, interp_val};
}

#undef fuji_quant_gradient

__attribute__((always_inline)) int FujiDecompressor::fuji_decode_sample_even(
    fuji_compressed_block& info, xt_lines c, int col,
    std::array<int_pair, 41>& grads) const {
  auto [grad, interp_val] = fuji_decode_interpolation_even_inner(info, c, col);
  return fuji_decode_sample(info, grad, interp_val, grads);
}

__attribute__((always_inline)) int FujiDecompressor::fuji_decode_sample_odd(
    fuji_compressed_block& info, xt_lines c, int col,
    std::array<int_pair, 41>& grads) const {
  auto [grad, interp_val] = fuji_decode_interpolation_odd_inner(info, c, col);
  return fuji_decode_sample(info, grad, interp_val, grads);
}

__attribute__((always_inline)) int
FujiDecompressor::fuji_decode_interpolation_even(
    const fuji_compressed_block& info, xt_lines c, int col) const {
  auto [grad, interp_val] = fuji_decode_interpolation_even_inner(info, c, col);
  return interp_val;
}

void FujiDecompressor::fuji_extend_generic(const fuji_compressed_block& info,
                                           int start, int end) {
  for (int i = start; i <= end; i++) {
    info.lines(i, 0) = info.lines(i - 1, 1);
    info.lines(i, info.lines.width - 1) =
        info.lines(i - 1, info.lines.width - 2);
  }
}

void FujiDecompressor::fuji_extend_red(const fuji_compressed_block& info) {
  fuji_extend_generic(info, R2, R4);
}

void FujiDecompressor::fuji_extend_green(const fuji_compressed_block& info) {
  fuji_extend_generic(info, G2, G7);
}

void FujiDecompressor::fuji_extend_blue(const fuji_compressed_block& info) {
  fuji_extend_generic(info, B2, B4);
}

template <typename T>
__attribute__((always_inline)) void
FujiDecompressor::fuji_decode_block(T&& func_even, fuji_compressed_block& info,
                                    [[maybe_unused]] int cur_line) const {
  assert(common_info.line_width % 2 == 0);
  const int line_width = common_info.line_width / 2;

  auto pass = [this, &info, line_width, func_even](std::array<xt_lines, 2> c,
                                                   int row) {
    int grad = row % 3;

    struct ColorPos {
      int even = 0;
      int odd = 0;
    };

    std::array<ColorPos, 2> pos;
    for (int i = 0; i != line_width + 4; ++i) {
      if (i < line_width) {
        for (int comp = 0; comp != 2; comp++) {
          int& col = pos[comp].even;
          int sample =
              func_even(c[comp], col, info.grad_even[grad], row, i, comp);
          info.lines(c[comp], 1 + 2 * col + 0) = sample;
          ++col;
        }
      }

      if (i >= 4) {
        for (int comp = 0; comp != 2; comp++) {
          int& col = pos[comp].odd;
          int sample =
              fuji_decode_sample_odd(info, c[comp], col, info.grad_odd[grad]);
          info.lines(c[comp], 1 + 2 * col + 1) = sample;
          ++col;
        }
      }
    }
  };

  using Tag = BayerTag;
  const std::array<CFAColor, MCU<Tag>.x * MCU<Tag>.y> CFAData =
      getAsCFAColors(BayerPhase::RGGB);
  const Array2DRef<const CFAColor> CFA(CFAData.data(), MCU<Tag>.x, MCU<Tag>.y);

  std::array<int, 3> PerColorCounter;
  std::fill(PerColorCounter.begin(), PerColorCounter.end(), 0);
  auto ColorCounter = [&PerColorCounter](CFAColor c) -> int& {
    switch (c) {
    case CFAColor::RED:
    case CFAColor::GREEN:
    case CFAColor::BLUE:
      return PerColorCounter[static_cast<uint8_t>(c)];
    default:
      __builtin_unreachable();
    }
  };

  auto CurLineForColor = [&ColorCounter](CFAColor c) {
    xt_lines res;
    switch (c) {
    case CFAColor::RED:
      res = R2;
      break;
    case CFAColor::GREEN:
      res = G2;
      break;
    case CFAColor::BLUE:
      res = B2;
      break;
    default:
      __builtin_unreachable();
    }
    int& off = ColorCounter(c);
    res = static_cast<xt_lines>(res + off);
    ++off;
    return res;
  };

  for (int row = 0; row != 6; ++row) {
    CFAColor c0 = CFA(row % CFA.height, /*col=*/0);
    CFAColor c1 = CFA(row % CFA.height, /*col=*/1);
    pass({CurLineForColor(c0), CurLineForColor(c1)}, row);
    for (CFAColor c : {c0, c1}) {
      switch (c) {
      case CFAColor::RED:
        fuji_extend_red(info);
        break;
      case CFAColor::GREEN:
        fuji_extend_green(info);
        break;
      case CFAColor::BLUE:
        fuji_extend_blue(info);
        break;
      default:
        __builtin_unreachable();
      }
    }
  }
}

void FujiDecompressor::xtrans_decode_block(fuji_compressed_block& info,
                                           int cur_line) const {
  fuji_decode_block(
      [this, &info](xt_lines c, int col, std::array<int_pair, 41>& grads,
                    int row, int i, int comp) {
        if ((comp == 0 && (row == 0 || (row == 2 && i % 2 == 0) ||
                           (row == 4 && i % 2 != 0) || row == 5)) ||
            (comp == 1 && (row == 1 || row == 2 || (row == 3 && i % 2 != 0) ||
                           (row == 5 && i % 2 == 0))))
          return fuji_decode_interpolation_even(info, c, col);
        assert((comp == 0 && (row == 1 || (row == 2 && i % 2 != 0) ||
                              row == 3 || (row == 4 && i % 2 == 0))) ||
               (comp == 1 && (row == 0 || (row == 3 && i % 2 == 0) ||
                              row == 4 || (row == 5 && i % 2 != 0))));
        return fuji_decode_sample_even(info, c, col, grads);
      },
      info, cur_line);
}

void FujiDecompressor::fuji_bayer_decode_block(fuji_compressed_block& info,
                                               int cur_line) const {
  fuji_decode_block(
      [this, &info](xt_lines c, int col, std::array<int_pair, 41>& grads,
                    [[maybe_unused]] int row, [[maybe_unused]] int i,
                    [[maybe_unused]] int comp) {
        return fuji_decode_sample_even(info, c, col, grads);
      },
      info, cur_line);
}

void FujiDecompressor::fuji_decode_strip(fuji_compressed_block& info_block,
                                         const FujiStrip& strip) const {
  BitPumpMSB pump(strip.bs);

  const unsigned line_size = sizeof(uint16_t) * (common_info.line_width + 2);

  struct i_pair {
    int a;
    int b;
  };

  const std::array<i_pair, 3> colors = {{{R0, 5}, {G0, 8}, {B0, 5}}};

  for (int cur_line = 0; cur_line < strip.height(); cur_line++) {
    if (header.raw_type == 16) {
      xtrans_decode_block(info_block, cur_line);
    } else {
      fuji_bayer_decode_block(info_block, cur_line);
    }

    if (header.raw_type == 16) {
      copy_line_to_xtrans(info_block, strip, cur_line);
    } else {
      copy_line_to_bayer(info_block, strip, cur_line);
    }

    if (cur_line + 1 == strip.height())
      break;

    // Last two lines of each color become the first two lines.
    for (auto i : colors) {
      memcpy(&info_block.lines(i.a, 0), &info_block.lines(i.a + i.b - 2, 0),
             2 * line_size);
    }

    for (auto i : colors) {
      const auto out = CroppedArray2DRef(
          info_block.lines, /*offsetCols=*/0, /*offsetRows=*/i.a + 2,
          /*croppedWidth=*/info_block.lines.width, /*croppedHeight=*/i.b - 2);

      // All other lines of each color become uninitialized.
      MSan::Allocated(out);

      // And the first (real, uninitialized) line of each color gets the content
      // of the last helper column from the last decoded sample of previous
      // line of that color.
      info_block.lines(i.a + 2, info_block.lines.width - 1) =
          info_block.lines(i.a + 2 - 1, info_block.lines.width - 2);
    }
  }
}

void FujiDecompressor::fuji_compressed_load_raw() {
  common_info = fuji_compressed_params(*this);

  // read block sizes
  std::vector<uint32_t> block_sizes;
  block_sizes.resize(header.blocks_in_row);
  for (auto& block_size : block_sizes)
    block_size = input.getU32();

  // some padding?
  if (const uint64_t raw_offset = sizeof(uint32_t) * header.blocks_in_row;
      raw_offset & 0xC) {
    const int padding = 0x10 - (raw_offset & 0xC);
    input.skipBytes(padding);
  }

  // calculating raw block offsets
  strips.reserve(header.blocks_in_row);

  for (const auto& block_size : block_sizes)
    strips.emplace_back(input.getStream(block_size));
}

void FujiDecompressor::decompressThread() const noexcept {
  fuji_compressed_block block_info;

#ifdef HAVE_OPENMP
#pragma omp for schedule(static)
#endif
  for (int block = 0; block < header.blocks_in_row; ++block) {
    FujiStrip strip(header, block, strips[block]);
    block_info.reset(common_info);
    try {
      block_info.pump = BitPumpMSB(strip.bs);
      fuji_decode_strip(block_info, strip);
    } catch (const RawspeedException& err) {
      // Propagate the exception out of OpenMP magic.
      mRaw->setError(err.what());
    }
  }
}

void FujiDecompressor::decompress() const {
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

FujiDecompressor::FujiHeader::FujiHeader(ByteStream& bs)
    : signature(bs.getU16()), version(bs.getByte()), raw_type(bs.getByte()),
      raw_bits(bs.getByte()), raw_height(bs.getU16()),
      raw_rounded_width(bs.getU16()), raw_width(bs.getU16()),
      block_size(bs.getU16()), blocks_in_row(bs.getByte()),
      total_lines(bs.getU16()),
      MCU(raw_type == 16 ? ::rawspeed::MCU<XTransTag>
                         : ::rawspeed::MCU<BayerTag>) {}

FujiDecompressor::FujiHeader::operator bool() const {
  // general validation
  const bool invalid =
      (signature != 0x4953 || version != 1 || raw_height > 0x3000 ||
       raw_height < FujiStrip::lineHeight() ||
       raw_height % FujiStrip::lineHeight() || raw_width > 0x3000 ||
       raw_width < 0x300 || raw_width % 24 || raw_rounded_width > 0x3000 ||
       block_size != 0x300 || raw_rounded_width < block_size ||
       raw_rounded_width % block_size ||
       raw_rounded_width - raw_width >= block_size || blocks_in_row > 0x10 ||
       blocks_in_row == 0 || blocks_in_row != raw_rounded_width / block_size ||
       blocks_in_row != roundUpDivision(raw_width, block_size) ||
       total_lines > 0x800 || total_lines == 0 ||
       total_lines != raw_height / FujiStrip::lineHeight() ||
       (raw_bits != 12 && raw_bits != 14 && raw_bits != 16) ||
       (raw_type != 16 && raw_type != 0));

  return !invalid;
}

} // namespace rawspeed
