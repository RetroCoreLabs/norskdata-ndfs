/**
 * NDFS boot loader detection implementation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/boot_loader.h>
#include "endian_util.h"
#include <string.h>
#include <stdlib.h>

/* ---- Internal: access page 0 data from filesystem -------------------- */

/* We need access to the raw data pointer.  The filesystem struct is opaque,
   but ndfs_to_buffer() gives us a copy.  For efficiency we read page 0
   via ndfs_to_buffer and only copy 1 page.  Alternatively, since the
   master block is at a known offset, we use ndfs_to_buffer. */

static ndfs_error_t get_page0(ndfs_filesystem_t *fs, uint8_t *buf)
{
    uint8_t *full;
    size_t full_size;
    ndfs_error_t err;

    err = ndfs_to_buffer(fs, &full, &full_size);
    if (err != NDFS_OK) return err;

    if (full_size < NDFS_PAGE_SIZE) {
        free(full);
        return NDFS_ERR_TOO_SMALL;
    }
    memcpy(buf, full, NDFS_PAGE_SIZE);
    free(full);
    return NDFS_OK;
}

/* ---- BPUN/FLOMON detection ------------------------------------------- */

/**
 * Try to parse BPUN binary section starting after a '!' delimiter.
 * Returns true if valid BPUN or FLOMON was found.
 */
static bool try_parse_bpun(const uint8_t *data, size_t excl_pos,
                           ndfs_boot_code_t *code)
{
    size_t pos = excl_pos + 1;
    uint16_t address, count, file_checksum, calc_checksum;
    size_t data_bytes;
    size_t total_needed;
    size_t i;
    uint8_t *code_data;

    if (pos + 4 > NDFS_PAGE_SIZE) return false;

    address = ndfs_read_u16be(data, pos);
    pos += 2;
    count = ndfs_read_u16be(data, pos);
    pos += 2;

    data_bytes = (size_t)count * 2;
    total_needed = pos + data_bytes + 4; /* +4 for checksum and action */
    if (total_needed > NDFS_PAGE_SIZE) return false;

    /* Read code data and calculate checksum */
    code_data = NULL;
    calc_checksum = 0;
    if (data_bytes > 0) {
        code_data = (uint8_t *)malloc(data_bytes);
        if (!code_data) return false;

        for (i = 0; i < data_bytes; i += 2) {
            uint16_t word;
            code_data[i]     = data[pos + i];
            code_data[i + 1] = data[pos + i + 1];
            word = ndfs_read_u16be(data, pos + i);
            calc_checksum = (uint16_t)(calc_checksum + word);
        }
    }
    pos += data_bytes;

    /* Read file checksum */
    file_checksum = ndfs_read_u16be(data, pos);
    pos += 2;

    /* Check for FLOMON marker: address=0, count=0, checksum=0 */
    if (address == 0 && count == 0 && file_checksum == 0) {
        uint8_t word_count;
        uint8_t *flomon_data;
        size_t flomon_idx;

        free(code_data);

        if (pos >= NDFS_PAGE_SIZE) {
            code->format = NDFS_BOOT_FLOMON;
            code->data = NULL;
            code->data_size = 0;
            code->word_count = 0;
            code->checksum_valid = true;
            return true;
        }

        word_count = data[pos];
        pos++;

        code->format = NDFS_BOOT_FLOMON;
        code->word_count = word_count;
        code->start_address = 0;
        code->load_address = 0;
        code->checksum_valid = true;

        if (word_count == 0) {
            code->data = NULL;
            code->data_size = 0;
            return true;
        }

        /* FLOMON: each word stored as 00 HI 00 LO (4 bytes per word) */
        flomon_data = (uint8_t *)malloc((size_t)word_count * 2);
        if (!flomon_data) {
            code->data = NULL;
            code->data_size = 0;
            return true;
        }

        flomon_idx = 0;
        for (i = 0; i < (size_t)word_count; i++) {
            if (pos + 4 > NDFS_PAGE_SIZE) break;
            if (data[pos] != 0) break;     /* pad1 */
            flomon_data[flomon_idx++] = data[pos + 1]; /* HI */
            if (data[pos + 2] != 0) break; /* pad2 */
            flomon_data[flomon_idx++] = data[pos + 3]; /* LO */
            pos += 4;
        }

        code->data = flomon_data;
        code->data_size = flomon_idx;
        return true;
    }

    /* Standard BPUN format */
    code->format = NDFS_BOOT_BPUN;
    code->load_address = address;
    code->word_count = count;
    code->data = code_data;
    code->data_size = data_bytes;
    code->checksum_valid = (calc_checksum == file_checksum);

    /* Read action field (optional) */
    if (pos + 2 <= NDFS_PAGE_SIZE) {
        code->start_address = ndfs_read_u16be(data, pos);
    }

    return true;
}

/*
 * ND-100 CPU opcodes (16-bit, big-endian on disk). A real bootstrap always begins
 * by disabling interrupts (and usually paging) before touching hardware - these
 * are the ONLY two words a genuine boot sector can start with. Verified against
 * the assembler source (norsk_data/nd100-as/instr.h: PIOF=0150405, IOF=0150401)
 * and cross-checked against nd100x/asm/DISK-IMAGE-INVENTORY.md, which uses the
 * same two values to classify real disk images.
 */
#define NDFS_OPCODE_PIOF 0xD105u /* octal 150405 - disable interrupts + paging */
#define NDFS_OPCODE_IOF  0xD101u /* octal 150401 - disable interrupts only */

/*
 * Literal IOX instruction word = base opcode 0164000 (octal) OR'd with an 11-bit
 * device address in bits 10-0 (norsk_data/nd100-as/instr.h:
 * OPC("iox",A_IOX,0164000)). IOXT (device address taken from the T register at
 * runtime - used by SCSI/NCR-5386) is the single fixed word 0150415 octal; no
 * device address is visible in the instruction stream for it.
 */
#define NDFS_IOX_OPCODE_BASE 0xE800u /* octal 164000 */
#define NDFS_IOX_OPCODE_MASK 0xF800u /* top 5 bits select the IOX instruction class */
#define NDFS_IOX_DEVICE_MASK 0x07FFu /* low 11 bits carry the literal device address */
#define NDFS_IOXT_OPCODE     0xD10Du /* octal 150415 (SCSI/NCR-5386, indirect) */

/*
 * Hard-disk controller IOX device bases (octal, converted to decimal), each
 * covering an 8-word register window. Source: nd100-dis/DEVICE-BASES.md
 * (thumbwheel-selectable base addresses extracted from the RetroCore NDBus
 * controller drivers).
 */
static const uint16_t NDFS_SMD_ECC_BASES[]    = { 0x360, 0x368, 0x160, 0x168 }; /* 1540,1550,540,550 octal */
static const uint16_t NDFS_WINCHESTER_BASES[] = { 0x140, 0x148 };               /* 500,510 octal */
static const uint16_t NDFS_FLOPPY_BASES[]     = { 0x370, 0x378 };               /* 1560,1570 octal */

/** Checks whether a 16-bit word is the genuine ND-100 bootstrap prologue. */
static bool is_prologue_opcode(uint16_t word)
{
    return word == NDFS_OPCODE_PIOF || word == NDFS_OPCODE_IOF;
}

/** Checks whether a device address falls within any of the given controllers'
 *  8-word register windows (base .. base+7). */
static bool is_in_device_window(uint16_t address, const uint16_t *bases, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (address >= bases[i] && address <= (uint16_t)(bases[i] + 7)) return true;
    }
    return false;
}

/**
 * Scans page 0 for a literal IOX (SMD/ECC, Winchester, Floppy) or indirect IOXT
 * (SCSI/NCR-5386) instruction to classify which hard-disk controller the
 * bootstrap targets. Only meaningful once is_prologue_opcode() has already
 * confirmed the page is a genuine bootstrap.
 */
static ndfs_boot_controller_type_t detect_controller_type(const uint8_t *data)
{
    size_t word_count = NDFS_PAGE_SIZE / 2;
    size_t i;

    for (i = 0; i < word_count; i++) {
        uint16_t word = ndfs_read_u16be(data, i * 2);

        if (word == NDFS_IOXT_OPCODE) {
            return NDFS_CONTROLLER_SCSI;
        }

        if ((word & NDFS_IOX_OPCODE_MASK) == NDFS_IOX_OPCODE_BASE) {
            uint16_t device_address = (uint16_t)(word & NDFS_IOX_DEVICE_MASK);

            if (is_in_device_window(device_address, NDFS_SMD_ECC_BASES,
                                     sizeof(NDFS_SMD_ECC_BASES) / sizeof(NDFS_SMD_ECC_BASES[0])))
                return NDFS_CONTROLLER_SMD_ECC;
            if (is_in_device_window(device_address, NDFS_WINCHESTER_BASES,
                                     sizeof(NDFS_WINCHESTER_BASES) / sizeof(NDFS_WINCHESTER_BASES[0])))
                return NDFS_CONTROLLER_WINCHESTER;
            if (is_in_device_window(device_address, NDFS_FLOPPY_BASES,
                                     sizeof(NDFS_FLOPPY_BASES) / sizeof(NDFS_FLOPPY_BASES[0])))
                return NDFS_CONTROLLER_FLOPPY;
        }
    }

    return NDFS_CONTROLLER_UNKNOWN;
}

/**
 * Check if page 0 contains a genuine raw hard-disk bootstrap (SMD/ECC,
 * Winchester, SCSI). Unlike BPUN/FLOMON, raw bootstraps carry no ASCII
 * preamble or delimiter, so the only reliable signature is the CPU opcode
 * itself: real boot code always starts with PIOF or IOF. Anything else -
 * including data that merely "looks non-uniform" - is not bootable.
 */
static bool is_valid_binary(const uint8_t *data)
{
    return is_prologue_opcode(ndfs_read_u16be(data, 0));
}

/* ---- Internal load from raw page 0 ---------------------------------- */

static ndfs_error_t load_from_page0(const uint8_t *page0, ndfs_boot_code_t *code)
{
    int i;
    int limit = 512;

    memset(code, 0, sizeof(*code));
    code->format = NDFS_BOOT_NONE;

    /* Try BPUN/FLOMON: find '!' delimiter */
    for (i = 0; i < limit && i < (int)NDFS_PAGE_SIZE; i++) {
        if (page0[i] == 0x21) {
            if (try_parse_bpun(page0, (size_t)i, code)) {
                return NDFS_OK;
            }
        }
    }

    /* Try raw binary */
    if (is_valid_binary(page0)) {
        code->format = NDFS_BOOT_BINARY;
        code->controller_type = detect_controller_type(page0);
        code->load_address = 0;
        code->start_address = 0;
        /* Copy entire page 0 as code data (up to master block area) */
        code->data_size = NDFS_MASTER_BLOCK_OFFSET;
        code->data = (uint8_t *)malloc(code->data_size);
        if (code->data) {
            memcpy(code->data, page0, code->data_size);
        } else {
            code->data_size = 0;
        }
        code->checksum_valid = false;
        return NDFS_OK;
    }

    return NDFS_OK; /* NDFS_BOOT_NONE */
}

/* ---- Public API ------------------------------------------------------ */

ndfs_error_t ndfs_detect_boot_format(ndfs_filesystem_t *fs,
                                     ndfs_boot_format_t *format)
{
    uint8_t page0[NDFS_PAGE_SIZE];
    ndfs_boot_code_t code;
    ndfs_error_t err;

    if (!fs || !format) return NDFS_ERR_NULL_PTR;

    err = get_page0(fs, page0);
    if (err != NDFS_OK) return err;

    err = load_from_page0(page0, &code);
    *format = code.format;
    ndfs_boot_code_destroy(&code);
    return err;
}

ndfs_error_t ndfs_load_boot_code(ndfs_filesystem_t *fs,
                                 ndfs_boot_code_t *code)
{
    uint8_t page0[NDFS_PAGE_SIZE];
    ndfs_error_t err;

    if (!fs || !code) return NDFS_ERR_NULL_PTR;

    err = get_page0(fs, page0);
    if (err != NDFS_OK) return err;

    return load_from_page0(page0, code);
}

ndfs_error_t ndfs_is_bootable(ndfs_filesystem_t *fs, bool *bootable)
{
    ndfs_boot_format_t fmt;
    ndfs_error_t err;

    if (!fs || !bootable) return NDFS_ERR_NULL_PTR;

    err = ndfs_detect_boot_format(fs, &fmt);
    if (err != NDFS_OK) return err;

    *bootable = (fmt != NDFS_BOOT_NONE);
    return NDFS_OK;
}

ndfs_error_t ndfs_detect_boot_controller_type(ndfs_filesystem_t *fs,
                                              ndfs_boot_controller_type_t *type)
{
    ndfs_boot_code_t code;
    ndfs_error_t err;

    if (!fs || !type) return NDFS_ERR_NULL_PTR;

    err = ndfs_load_boot_code(fs, &code);
    if (err != NDFS_OK) return err;

    *type = code.controller_type;
    ndfs_boot_code_destroy(&code);
    return NDFS_OK;
}

void ndfs_boot_code_destroy(ndfs_boot_code_t *bc)
{
    if (!bc) return;
    free(bc->data);
    bc->data = NULL;
    bc->data_size = 0;
}
