/*
 * Copyright (c) 2024 LK Trusty Authors. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <lib/device_tree/libfdt_helpers.h>
#include "err.h"
#include "lk/types.h"

/**
 * fdt_helper_read_cells32 - Read and convert cells.
 * @fdt_prop:       Pointer to property to read from.
 * @fdt_prop_len:   Length of @fdt_prop in bytes (not count of 32 bit values).
 * @row_index:      Row to read from.
 * @before_cells:   Number of cells to ignore in each row before the cells we
 *                  read and convert.
 * @cells:          Number of cells to read and convert to cpu endian.
 * @after_cells:    Number of cells to ignore in each row after the cell we read
 *                  and convert.
 * @valp:           Pointer to store converted value in.
 *
 * Return:
 * * %0:                Success.
 * * %ERR_OUT_OF_RANGE: @fdt_prop_len is too small for the requested read.
 * * %ERR_TOO_BIG:      value does not fit in uint64_t (only possible if
 *                      cells > 2).
 */
status_t fdt_helper_read_cells32(const fdt32_t* fdt_prop,
                                 int fdt_prop_len,
                                 int row_index,
                                 int before_cells,
                                 int cells,
                                 int after_cells,
                                 uint64_t* valp) {
    uint64_t ret = 0;
    int cell_index =
            row_index * (before_cells + cells + after_cells) + before_cells;
    if (cell_index + cells > (fdt_prop_len / (int)sizeof(fdt32_t))) {
        return ERR_OUT_OF_RANGE;
    }
    while (cells-- > 0) {
        if (ret > UINT32_MAX) {
            return ERR_TOO_BIG;
        }
        ret = ret << 32 | fdt32_to_cpu(fdt_prop[cell_index++]);
    }
    *valp = ret;
    return 0;
}

/**
 * fdt_helper_get_reg - Get address and size from reg property of a node.
 * @fdt:            Pointer to device tree.
 * @nodeoffset:     Node to use.
 * @reg_index:      Row to read from.
 *
 * Return:
 * * %0:                Success.
 * * %ERR_NOT_FOUND:    Node does not have a reg property.
 * * %ERR_OUT_OF_RANGE: reg property is too small for the requested read.
 * * %ERR_TOO_BIG:      reg value does not fit in paddr_t or size_t.
 */
status_t fdt_helper_get_reg(const void* fdt,
                            int nodeoffset,
                            int reg_index,
                            paddr_t* addrp,
                            size_t* sizep) {
    status_t ret = 0;
    int parent_offset = fdt_parent_offset(fdt, nodeoffset);
    int address_cells = fdt_address_cells(fdt, parent_offset);
    int size_cells = fdt_size_cells(fdt, parent_offset);
    int reg_prop_len;
    const fdt32_t* reg_prop =
            fdt_getprop(fdt, nodeoffset, "reg", &reg_prop_len);
    if (!reg_prop) {
        return ERR_NOT_FOUND;
    }
    if (addrp) {
        uint64_t addr64;
        ret = fdt_helper_read_cells32(reg_prop, reg_prop_len, reg_index, 0,
                                      address_cells, size_cells, &addr64);
        if (ret) {
            return ret;
        }
        if (addr64 > PADDR_MAX) {
            return ERR_TOO_BIG;
        }
        *addrp = addr64;
    }
    if (sizep) {
        uint64_t size64;
        ret = fdt_helper_read_cells32(reg_prop, reg_prop_len, reg_index,
                                      address_cells, size_cells, 0, &size64);
        if (ret) {
            return ret;
        }
        if (size64 > SIZE_MAX) {
            return ERR_TOO_BIG;
        }
        *sizep = size64;
    }
    return 0;
}
