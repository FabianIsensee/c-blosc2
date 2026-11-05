/*********************************************************************
  Blosc - Blocked Shuffling and Compression Library

  Copyright (c) 2021  Blosc Development Team <blosc@blosc.org>
  https://blosc.org
  License: BSD 3-Clause (see LICENSE.txt)

  See LICENSE.txt for details about copyright and rights to use.
**********************************************************************/

#include "b2nd.h"
#include "b2nd-private.h"

#include <stdint.h>

// Copies `nrows` rows of `row_nbytes` bytes each; consecutive rows are `src_stride` and `dst_stride` bytes apart.
// Rows of up to 64 bytes (e.g. 16 float32 or 32 int16 items: the rows of 16^3 float32 and 32x64x32 int16 blocks)
// are copied with fixed-size moves: a memcpy() call per row costs more than such a copy itself.  Two moves of n
// bytes, the second one ending at the end of the row, cover any length from n to 2 * n (they may overlap).
// Longer rows go to memcpy(), which is as fast or faster for them.
static inline void copy_rows(uint8_t *dst, int64_t dst_stride, const uint8_t *src, int64_t src_stride,
                             int64_t nrows, int64_t row_nbytes) {
#define COPY_ROWS(stmt)                     \
  for (int64_t r = 0; r < nrows; ++r) {     \
    stmt;                                   \
    dst += dst_stride;                      \
    src += src_stride;                      \
  }
#define MOVE_2X(n)                                                     \
  memcpy(dst, src, n);                                                 \
  memcpy(dst + row_nbytes - (n), src + row_nbytes - (n), n)

  if (row_nbytes > 64) {
    COPY_ROWS(memcpy(dst, src, (size_t) row_nbytes))
  } else if (row_nbytes > 32) {
    COPY_ROWS(MOVE_2X(32))
  } else if (row_nbytes == 32) {
    COPY_ROWS(memcpy(dst, src, 32))
  } else if (row_nbytes > 16) {
    COPY_ROWS(MOVE_2X(16))
  } else if (row_nbytes == 16) {
    COPY_ROWS(memcpy(dst, src, 16))
  } else if (row_nbytes > 8) {
    COPY_ROWS(MOVE_2X(8))
  } else if (row_nbytes == 8) {
    COPY_ROWS(memcpy(dst, src, 8))
  } else if (row_nbytes > 4) {
    COPY_ROWS(MOVE_2X(4))
  } else if (row_nbytes == 4) {
    COPY_ROWS(memcpy(dst, src, 4))
  } else if (row_nbytes > 2) {
    COPY_ROWS(MOVE_2X(2))
  } else if (row_nbytes == 2) {
    COPY_ROWS(memcpy(dst, src, 2))
  } else {
    COPY_ROWS(*dst = *src)
  }
#undef MOVE_2X
#undef COPY_ROWS
}


// Copies a box of `ndim` >= 2 dimensions given in bytes (all of `shape` > 0), or zeroes it if `bsrc` is NULL: the
// innermost dimension is a row of shape[ndim - 1] bytes, contiguous in both buffers; the strides of the other
// dimensions are in bytes.  The rows of dimension ndim - 2 go to copy_rows() (or memset()), the outer dimensions
// are walked once per plane of rows.
static void copy_box(int ndim, const int64_t *shape,
                     const uint8_t *bsrc, const int64_t *src_strides,
                     uint8_t *bdst, const int64_t *dst_strides) {
  const int64_t row_nbytes = shape[ndim - 1];
  const int64_t nrows = shape[ndim - 2];
  const int64_t src_row_stride = src_strides[ndim - 2];
  const int64_t dst_row_stride = dst_strides[ndim - 2];
  const int nouter = ndim - 2;
  int64_t index[B2ND_MAX_DIM + 1] = {0};
  for (;;) {
    int64_t src_offset = 0;
    int64_t dst_offset = 0;
    for (int i = 0; i < nouter; ++i) {
      src_offset += index[i] * src_strides[i];
      dst_offset += index[i] * dst_strides[i];
    }
    if (bsrc == NULL) {
      uint8_t *row = bdst + dst_offset;
      for (int64_t r = 0; r < nrows; ++r) {
        memset(row, 0, (size_t) row_nbytes);
        row += dst_row_stride;
      }
    } else {
      copy_rows(bdst + dst_offset, dst_row_stride, bsrc + src_offset, src_row_stride, nrows, row_nbytes);
    }
    int i = nouter - 1;
    while (i >= 0 && ++index[i] == shape[i]) {
      index[i] = 0;
      --i;
    }
    if (i < 0) {
      break;
    }
  }
}


// Simplifies a box of `ndim` dimensions with `box_shape` items (all > 0), at item strides `src_strides` and
// `dst_strides` in two buffers, into as few and as long rows as possible.  The box is described in bytes, with the
// bytes of an item as one more, innermost dimension.  Dimensions the box is one item wide in only shift its start:
// they are dropped.  A dimension is merged into the outer one before it wherever their items follow on
// contiguously in both buffers (the box covers the inner one whole in both): e.g. a block of (1, 128, 128, 3) uint8
// items copied into a (1, 512, 512, 3) image is 128 rows of 384 bytes, not 16384 rows of 3 bytes.  Writes the
// resulting `shape` and byte strides and returns their number of dimensions (>= 1); the innermost one always has a
// stride of 1.
static int simplify_box(int8_t ndim, int32_t itemsize, const int64_t *box_shape,
                        const int64_t *src_strides, const int64_t *dst_strides,
                        int64_t *shape, int64_t *src_bstrides, int64_t *dst_bstrides) {
  int n = 0;
  for (int i = 0; i <= ndim; ++i) {
    bool item = i == ndim;
    int64_t extent = item ? itemsize : box_shape[i];
    int64_t src_bstride = item ? 1 : src_strides[i] * itemsize;
    int64_t dst_bstride = item ? 1 : dst_strides[i] * itemsize;
    if (extent == 1 && !item) {
      continue;
    }
    if (n > 0 && src_bstrides[n - 1] == src_bstride * extent && dst_bstrides[n - 1] == dst_bstride * extent) {
      shape[n - 1] *= extent;
      src_bstrides[n - 1] = src_bstride;
      dst_bstrides[n - 1] = dst_bstride;
    } else {
      shape[n] = extent;
      src_bstrides[n] = src_bstride;
      dst_bstrides[n] = dst_bstride;
      ++n;
    }
  }
  return n;
}


// C-order strides (in items) of a buffer of shape `pad_shape`
static void c_strides(int8_t ndim, const int64_t *pad_shape, int64_t *strides) {
  strides[ndim - 1] = 1;
  for (int i = ndim - 2; i >= 0; --i) {
    strides[i] = strides[i + 1] * pad_shape[i + 1];
  }
}


int b2nd_copy_buffer2(int8_t ndim,
                      int32_t itemsize,
                      const void *src, const int64_t *src_pad_shape,
                      const int64_t *src_start, const int64_t *src_stop,
                      void *dst, const int64_t *dst_pad_shape,
                      const int64_t *dst_start) {
  // Compute the shape of the copy
  int64_t copy_shape[B2ND_MAX_DIM] = {0};
  for (int i = 0; i < ndim; ++i) {
    copy_shape[i] = src_stop[i] - src_start[i];
    if (copy_shape[i] == 0) {
      return BLOSC2_ERROR_SUCCESS;
    }
  }

  // Compute the strides
  int64_t src_strides[B2ND_MAX_DIM] = {0};
  c_strides(ndim, src_pad_shape, src_strides);
  int64_t dst_strides[B2ND_MAX_DIM] = {0};
  c_strides(ndim, dst_pad_shape, dst_strides);

  // Align the buffers removing unnecessary data
  int64_t src_start_n;
  blosc2_multidim_to_unidim(src_start, ndim, src_strides, &src_start_n);
  uint8_t *bsrc = (uint8_t *) src;
  bsrc = &bsrc[src_start_n * itemsize];

  int64_t dst_start_n;
  blosc2_multidim_to_unidim(dst_start, ndim, dst_strides, &dst_start_n);
  uint8_t *bdst = (uint8_t *) dst;
  bdst = &bdst[dst_start_n * itemsize];

  int64_t shape[B2ND_MAX_DIM + 1];
  int64_t src_bstrides[B2ND_MAX_DIM + 1];
  int64_t dst_bstrides[B2ND_MAX_DIM + 1];
  int n = simplify_box(ndim, itemsize, copy_shape, src_strides, dst_strides, shape, src_bstrides, dst_bstrides);
  if (n == 1) {
    memcpy(bdst, bsrc, (size_t) shape[0]);
  } else {
    copy_box(n, shape, bsrc, src_bstrides, bdst, dst_bstrides);
  }

  return BLOSC2_ERROR_SUCCESS;
}


/* See b2nd-private.h */
void b2nd_zero_box(int8_t ndim, int32_t itemsize, void *dst, const int64_t *dst_pad_shape,
                   const int64_t *box_start, const int64_t *box_stop) {
  int64_t box_shape[B2ND_MAX_DIM];
  for (int i = 0; i < ndim; ++i) {
    box_shape[i] = box_stop[i] - box_start[i];
    if (box_shape[i] <= 0) {
      return;
    }
  }
  int64_t strides[B2ND_MAX_DIM];
  c_strides(ndim, dst_pad_shape, strides);
  int64_t start_n;
  blosc2_multidim_to_unidim(box_start, ndim, strides, &start_n);
  uint8_t *bdst = (uint8_t *) dst + start_n * itemsize;

  int64_t shape[B2ND_MAX_DIM + 1];
  int64_t bstrides[B2ND_MAX_DIM + 1];
  int64_t unused[B2ND_MAX_DIM + 1];
  int n = simplify_box(ndim, itemsize, box_shape, strides, strides, shape, unused, bstrides);
  if (n == 1) {
    memset(bdst, 0, (size_t) shape[0]);
  } else {
    copy_box(n, shape, NULL, bstrides, bdst, bstrides);
  }
}


// Keep the old signature for API compatibility
int b2nd_copy_buffer(int8_t ndim,
                     uint8_t itemsize,
                     const void *src, const int64_t *src_pad_shape,
                     const int64_t *src_start, const int64_t *src_stop,
                     void *dst, const int64_t *dst_pad_shape,
                     const int64_t *dst_start) {
  // Simply cast itemsize to int32_t and delegate
  return b2nd_copy_buffer2(ndim, (int32_t)itemsize, src, src_pad_shape,
                          src_start, src_stop, dst, dst_pad_shape, dst_start);
}
