/*
 * SPI flash interface
 *
 * Copyright (C) 2008 Atmel Corporation
 * Copyright (C) 2010 Reinhard Meyer, EMK Elektronik
 *
 * Licensed under the GPL-2 or later.
 */

#include <common.h>
#include <fdtdec.h>
#include <malloc.h>
#include <spi.h>
#include <spi_flash.h>
#include <watchdog.h>

#include "spi_flash_internal.h"

DECLARE_GLOBAL_DATA_PTR;

static void spi_flash_addr(u32 addr, u8 *cmd)
{
	/* cmd[0] is actual command */
	cmd[1] = addr >> 16;
	cmd[2] = addr >> 8;
	cmd[3] = addr >> 0;
}

static int spi_flash_read_write(struct spi_slave *spi,
				const u8 *cmd, size_t cmd_len,
				const u8 *data_out, u8 *data_in,
				size_t data_len)
{
	unsigned long flags = SPI_XFER_BEGIN;
	int ret;

	if ((spi->is_dual == MODE_DUAL_STACKED) && (spi->u_page == 1))
		flags |= SPI_FLASH_U_PAGE;

	if (data_len == 0)
		flags |= SPI_XFER_END;

	ret = spi_xfer(spi, cmd_len * 8, cmd, NULL, flags);
	if (ret) {
		debug("SF: Failed to send command (%zu bytes): %d\n",
				cmd_len, ret);
	} else if (data_len != 0) {
		ret = spi_xfer(spi, data_len * 8, data_out, data_in, SPI_XFER_END);
		if (ret)
			debug("SF: Failed to transfer %zu bytes of data: %d\n",
					data_len, ret);
	}

	return ret;
}

int spi_flash_cmd(struct spi_slave *spi, u8 cmd, void *response, size_t len)
{
	return spi_flash_cmd_read(spi, &cmd, 1, response, len);
}

int spi_flash_cmd_read(struct spi_slave *spi, const u8 *cmd,
		size_t cmd_len, void *data, size_t data_len)
{
	return spi_flash_read_write(spi, cmd, cmd_len, NULL, data, data_len);
}

int spi_flash_cmd_write(struct spi_slave *spi, const u8 *cmd, size_t cmd_len,
		const void *data, size_t data_len)
{
	return spi_flash_read_write(spi, cmd, cmd_len, data, NULL, data_len);
}

int spi_flash_cmd_write_multi(struct spi_flash *flash, u32 offset,
		size_t len, const void *buf)
{
	unsigned long write_addr, byte_addr, page_size;
	size_t chunk_len, actual;
	int ret;
	u8 cmd[4];
	u32 start;
	u8 bank_sel;
	int is_dual = flash->spi->is_dual;

	start = offset;
	page_size = flash->page_size;

	ret = spi_claim_bus(flash->spi);
	if (ret) {
		debug("SF: unable to claim SPI bus\n");
		return ret;
	}

	cmd[0] = CMD_PAGE_PROGRAM;
	for (actual = 0; actual < len; actual += chunk_len) {
		write_addr = offset;
		if (is_dual == MODE_DUAL_PARALLEL)
			write_addr /= 2;

		if (is_dual == MODE_DUAL_STACKED) {
			if (write_addr >= (flash->size / 2))
				flash->spi->u_page = 1;
			else
				flash->spi->u_page = 0;
		}

		bank_sel = write_addr / SPI_FLASH_16MB_BOUN;
		if ((is_dual == MODE_DUAL_STACKED) && (flash->spi->u_page == 1))
			bank_sel -= ((flash->size / 2) / SPI_FLASH_16MB_BOUN);

		ret = spi_flash_cmd_bankaddr_write(flash, bank_sel);
		if (ret) {
			debug("SF: fail to set bank%d\n", bank_sel);
			return ret;
		}

		byte_addr = offset % page_size;

		chunk_len = min(len - actual, page_size - byte_addr);

		if (flash->spi->max_write_size)
			chunk_len = min(chunk_len, flash->spi->max_write_size);

		spi_flash_addr(write_addr, cmd);

		debug("PP: 0x%p => cmd = { 0x%02x 0x%02x%02x%02x } chunk_len = %zu\n",
		      buf + actual, cmd[0], cmd[1], cmd[2], cmd[3], chunk_len);

		ret = spi_flash_cmd_write_enable(flash);
		if (ret < 0) {
			debug("SF: enabling write failed\n");
			break;
		}

		ret = spi_flash_cmd_write(flash->spi, cmd, 4,
					  buf + actual, chunk_len);
		if (ret < 0) {
			debug("SF: write failed\n");
			break;
		}

		ret = spi_flash_cmd_wait_ready(flash, SPI_FLASH_PROG_TIMEOUT);
		if (ret)
			break;

		offset += chunk_len;
	}

	printf("SF: program %s %zu bytes @ %#x\n",
	      ret ? "failure" : "success", len, start);

	spi_release_bus(flash->spi);
	return ret;
}

int spi_flash_read_common(struct spi_flash *flash, const u8 *cmd,
		size_t cmd_len, void *data, size_t data_len)
{
	struct spi_slave *spi = flash->spi;
	int ret;

	spi_claim_bus(spi);
	ret = spi_flash_cmd_read(spi, cmd, cmd_len, data, data_len);
	spi_release_bus(spi);

	return ret;
}

int spi_flash_cmd_read_fast(struct spi_flash *flash, u32 offset,
		size_t len, void *data)
{
	u8 cmd[5];
	u8 bank_sel;
	u32 remain_len, read_len, read_addr;
	u32 bank_boun;
	int ret = -1;
	int is_dual = flash->spi->is_dual;

	/* Handle memory-mapped SPI */
	if (flash->memory_map)
		memcpy(data, flash->memory_map + offset, len);

	bank_boun = SPI_FLASH_16MB_BOUN;
	if (is_dual == MODE_DUAL_PARALLEL)
		bank_boun = SPI_FLASH_16MB_BOUN << 1;

	cmd[0] = CMD_READ_ARRAY_FAST;
	cmd[sizeof(cmd)-1] = 0x00;

	while (len) {
		read_addr = offset;
		if (is_dual == MODE_DUAL_PARALLEL)
			read_addr /= 2;

		if (is_dual == MODE_DUAL_STACKED) {
			if (read_addr >= (flash->size / 2))
				flash->spi->u_page = 1;
			else
				flash->spi->u_page = 0;
		}

		bank_sel = read_addr / SPI_FLASH_16MB_BOUN;
		if ((is_dual == MODE_DUAL_STACKED) && (flash->spi->u_page == 1))
			bank_sel -= ((flash->size / 2) / bank_boun);

		ret = spi_flash_cmd_bankaddr_write(flash, bank_sel);
		if (ret) {
			debug("SF: fail to set bank%d\n", bank_sel);
			return ret;
		}

		if ((is_dual == MODE_DUAL_STACKED) && (flash->spi->u_page == 1))
			bank_sel += ((flash->size / 2) / bank_boun);

		remain_len = (bank_boun * (bank_sel + 1) - read_addr);
		if (len < remain_len)
			read_len = len;
		else
			read_len = remain_len;

		spi_flash_addr(read_addr, cmd);

		ret = spi_flash_read_common(flash, cmd, sizeof(cmd),
							data, read_len);
		if (ret < 0) {
			debug("SF: read failed\n");
			break;
		}

		offset += read_len;
		len -= read_len;
		data += read_len;
	}

	return ret;
}

int spi_flash_cmd_wait_ready(struct spi_flash *flash, unsigned long timeout)
{
	struct spi_slave *spi = flash->spi;
	unsigned long timebase;
	int ret;
	u8 status;
	u8 check_status = 0x0;
	u8 poll_bit = STATUS_WIP;
	u8 cmd = CMD_READ_STATUS;

	if ((flash->idcode0 == 0x20) &&
			(flash->size >= SPI_FLASH_512MB_MIC)) {
		poll_bit = STATUS_PEC;
		check_status = poll_bit;
		cmd = CMD_FLAG_STATUS;
	}

	ret = spi_xfer(spi, 8, &cmd, NULL, SPI_XFER_BEGIN);
	if (ret) {
		debug("SF: fail to read %s status register\n",
			cmd == CMD_READ_STATUS ? "read" : "flag");
		return ret;
	}

	timebase = get_timer(0);
	do {
		WATCHDOG_RESET();

		ret = spi_xfer(spi, 8, NULL, &status, 0);
		if (ret)
			return -1;

		if ((status & poll_bit) == check_status)
			break;

	} while (get_timer(timebase) < timeout);

	spi_xfer(spi, 0, NULL, NULL, SPI_XFER_END);

	if ((status & poll_bit) == check_status)
		return 0;

	/* Timed out */
	debug("SF: time out!\n");
	return -1;
}

int spi_flash_cmd_erase(struct spi_flash *flash, u32 offset, size_t len)
{
	u32 start, end, erase_size, erase_addr;
	int ret;
	u8 cmd[4];
	u8 bank_sel;
	int is_dual = flash->spi->is_dual;

	erase_size = flash->sector_size;
	if (offset % erase_size || len % erase_size) {
		debug("SF: Erase offset/length not multiple of erase size\n");
		return -1;
	}

	ret = spi_claim_bus(flash->spi);
	if (ret) {
		debug("SF: Unable to claim SPI bus\n");
		return ret;
	}

	if (erase_size == 4096)
		cmd[0] = CMD_ERASE_4K;
	else
		cmd[0] = CMD_ERASE_64K;
	start = offset;
	end = start + len;

	while (len) {
		erase_addr = offset;
		if (is_dual == MODE_DUAL_PARALLEL)
			erase_addr /= 2;

		if (is_dual == MODE_DUAL_STACKED) {
			if (erase_addr >= (flash->size / 2))
				flash->spi->u_page = 1;
			else
				flash->spi->u_page = 0;
		}

		bank_sel = erase_addr / SPI_FLASH_16MB_BOUN;
		if ((is_dual == MODE_DUAL_STACKED) && (flash->spi->u_page == 1))
			bank_sel -= ((flash->size / 2) / SPI_FLASH_16MB_BOUN);

		ret = spi_flash_cmd_bankaddr_write(flash, bank_sel);
		if (ret) {
			debug("SF: fail to set bank%d\n", bank_sel);
			return ret;
		}

		spi_flash_addr(erase_addr, cmd);

		debug("SF: erase %2x %2x %2x %2x (%x)\n", cmd[0], cmd[1],
		      cmd[2], cmd[3], erase_addr);

		ret = spi_flash_cmd_write_enable(flash);
		if (ret)
			goto out;

		ret = spi_flash_cmd_write(flash->spi, cmd, sizeof(cmd), NULL, 0);
		if (ret)
			goto out;

		ret = spi_flash_cmd_wait_ready(flash, SPI_FLASH_PAGE_ERASE_TIMEOUT);
		if (ret)
			goto out;

		offset += erase_size;
		len -= erase_size;
	}

	printf("SF: Successfully erased %zu bytes @ %#x\n", end, start);

 out:
	spi_release_bus(flash->spi);
	return ret;
}

int spi_flash_cmd_write_status(struct spi_flash *flash, u8 sr)
{
	u8 cmd;
	int ret;

	ret = spi_flash_cmd_write_enable(flash);
	if (ret < 0) {
		debug("SF: enabling write failed\n");
		return ret;
	}

	cmd = CMD_WRITE_STATUS;
	ret = spi_flash_cmd_write(flash->spi, &cmd, 1, &sr, 1);
	if (ret) {
		debug("SF: fail to write status register\n");
		return ret;
	}

	ret = spi_flash_cmd_wait_ready(flash, SPI_FLASH_PROG_TIMEOUT);
	if (ret < 0) {
		debug("SF: write status register timed out\n");
		return ret;
	}

	return 0;
}

int spi_flash_cmd_bankaddr_write(struct spi_flash *flash, u8 bank_sel)
{
	u8 cmd, idcode0;
	int ret;

	if (flash->bank_curr == bank_sel) {
		debug("SF: not require to enable bank%d\n", bank_sel);
		return 0;
	}

	idcode0 = flash->idcode0;
	if (idcode0 == 0x01) {
		cmd = CMD_BANKADDR_BRWR;
	} else if ((idcode0 == 0xef) || (idcode0 == 0x20)) {
		cmd = CMD_EXTNADDR_WREAR;
	} else {
		printf("SF: Unsupported bank addr write %02x\n", idcode0);
		return -1;
	}

	ret = spi_flash_cmd_write_enable(flash);
	if (ret < 0) {
		debug("SF: enabling write failed\n");
		return ret;
	}

	ret = spi_flash_cmd_write(flash->spi, &cmd, 1, &bank_sel, 1);
	if (ret) {
		debug("SF: fail to write bank addr register\n");
		return ret;
	}
	flash->bank_curr = bank_sel;

	ret = spi_flash_cmd_wait_ready(flash, SPI_FLASH_PROG_TIMEOUT);
	if (ret < 0) {
		debug("SF: write config register timed out\n");
		return ret;
	}

	return 0;
}

int spi_flash_cmd_bankaddr_read(struct spi_flash *flash, void *data)
{
	u8 cmd;
	u8 idcode0 = flash->idcode0;

	if (idcode0 == 0x01) {
		cmd = CMD_BANKADDR_BRRD;
	} else if ((idcode0 == 0xef) || (idcode0 == 0x20)) {
		cmd = CMD_EXTNADDR_RDEAR;
	} else {
		printf("SF: Unsupported bank addr read %02x\n", idcode0);
		return -1;
	}

	return spi_flash_read_common(flash, &cmd, 1, data, 1);
}

#ifdef CONFIG_OF_CONTROL
int spi_flash_decode_fdt(const void *blob, struct spi_flash *flash)
{
	fdt_addr_t addr;
	fdt_size_t size;
	int node;

	/* If there is no node, do nothing */
	node = fdtdec_next_compatible(blob, 0, COMPAT_GENERIC_SPI_FLASH);
	if (node < 0)
		return 0;

	addr = fdtdec_get_addr_size(blob, node, "memory-map", &size);
	if (addr == FDT_ADDR_T_NONE) {
		debug("%s: Cannot decode address\n", __func__);
		return 0;
	}

	if (flash->size != size) {
		debug("%s: Memory map must cover entire device\n", __func__);
		return -1;
	}
	flash->memory_map = (void *)addr;

	return 0;
}
#endif /* CONFIG_OF_CONTROL */

/*
 * The following table holds all device probe functions
 *
 * shift:  number of continuation bytes before the ID
 * idcode: the expected IDCODE or 0xff for non JEDEC devices
 * probe:  the function to call
 *
 * Non JEDEC devices should be ordered in the table such that
 * the probe functions with best detection algorithms come first.
 *
 * Several matching entries are permitted, they will be tried
 * in sequence until a probe function returns non NULL.
 *
 * IDCODE_CONT_LEN may be redefined if a device needs to declare a
 * larger "shift" value.  IDCODE_PART_LEN generally shouldn't be
 * changed.  This is the max number of bytes probe functions may
 * examine when looking up part-specific identification info.
 *
 * Probe functions will be given the idcode buffer starting at their
 * manu id byte (the "idcode" in the table below).  In other words,
 * all of the continuation bytes will be skipped (the "shift" below).
 */
#define IDCODE_CONT_LEN 0
#define IDCODE_PART_LEN 5
static const struct {
	const u8 shift;
	const u8 idcode;
	struct spi_flash *(*probe) (struct spi_slave *spi, u8 *idcode);
} flashes[] = {
	/* Keep it sorted by define name */
#ifdef CONFIG_SPI_FLASH_ATMEL
	{ 0, 0x1f, spi_flash_probe_atmel, },
#endif
#ifdef CONFIG_SPI_FLASH_EON
	{ 0, 0x1c, spi_flash_probe_eon, },
#endif
#ifdef CONFIG_SPI_FLASH_MACRONIX
	{ 0, 0xc2, spi_flash_probe_macronix, },
#endif
#ifdef CONFIG_SPI_FLASH_SPANSION
	{ 0, 0x01, spi_flash_probe_spansion, },
#endif
#ifdef CONFIG_SPI_FLASH_SST
	{ 0, 0xbf, spi_flash_probe_sst, },
#endif
#ifdef CONFIG_SPI_FLASH_STMICRO
	{ 0, 0x20, spi_flash_probe_stmicro, },
#endif
#ifdef CONFIG_SPI_FLASH_WINBOND
	{ 0, 0xef, spi_flash_probe_winbond, },
#endif
#ifdef CONFIG_SPI_FRAM_RAMTRON
	{ 6, 0xc2, spi_fram_probe_ramtron, },
# undef IDCODE_CONT_LEN
# define IDCODE_CONT_LEN 6
#endif
	/* Keep it sorted by best detection */
#ifdef CONFIG_SPI_FLASH_STMICRO
	{ 0, 0xff, spi_flash_probe_stmicro, },
#endif
#ifdef CONFIG_SPI_FRAM_RAMTRON_NON_JEDEC
	{ 0, 0xff, spi_fram_probe_ramtron, },
#endif
};
#define IDCODE_LEN (IDCODE_CONT_LEN + IDCODE_PART_LEN)

struct spi_flash *spi_flash_probe(unsigned int bus, unsigned int cs,
		unsigned int max_hz, unsigned int spi_mode)
{
	struct spi_slave *spi;
	struct spi_flash *flash = NULL;
	int ret, i, shift;
	u8 idcode[IDCODE_LEN], *idp;
	u8 curr_bank = 0;

	spi = spi_setup_slave(bus, cs, max_hz, spi_mode);
	if (!spi) {
		printf("SF: Failed to set up slave\n");
		return NULL;
	}

	ret = spi_claim_bus(spi);
	if (ret) {
		debug("SF: Failed to claim SPI bus: %d\n", ret);
		goto err_claim_bus;
	}

	/* Read the ID codes */
	ret = spi_flash_cmd(spi, CMD_READ_ID, idcode, sizeof(idcode));
	if (ret)
		goto err_read_id;

#ifdef DEBUG
	printf("SF: Got idcodes\n");
	print_buffer(0, idcode, 1, sizeof(idcode), 0);
#endif

	/* count the number of continuation bytes */
	for (shift = 0, idp = idcode;
	     shift < IDCODE_CONT_LEN && *idp == 0x7f;
	     ++shift, ++idp)
		continue;

	/* search the table for matches in shift and id */
	for (i = 0; i < ARRAY_SIZE(flashes); ++i)
		if (flashes[i].shift == shift && flashes[i].idcode == *idp) {
			/* we have a match, call probe */
			flash = flashes[i].probe(spi, idp);
			if (flash)
				break;
		}

	if (!flash) {
		printf("SF: Unsupported manufacturer %02x\n", *idp);
		goto err_manufacturer_probe;
	}

#ifdef CONFIG_OF_CONTROL
	if (spi_flash_decode_fdt(gd->fdt_blob, flash)) {
		debug("SF: FDT decode error\n");
		goto err_manufacturer_probe;
	}
#endif
	printf("SF: Detected %s with page size ", flash->name);
	print_size(flash->sector_size, ", total ");
	print_size(flash->size, "");
	if (flash->memory_map)
		printf(", mapped at %p", flash->memory_map);
	puts("\n");

	flash->idcode0 = *idp;
	if (((flash->spi->is_dual == MODE_SINGLE) &&
			(flash->size > SPI_FLASH_16MB_BOUN)) ||
			(((flash->spi->is_dual == MODE_DUAL_STACKED) ||
			(flash->spi->is_dual == MODE_DUAL_PARALLEL)) &&
			((flash->size / 2) > SPI_FLASH_16MB_BOUN))) {
		if (spi_flash_cmd_bankaddr_read(flash, &curr_bank)) {
			debug("SF: fail to read bank addr register\n");
			goto err_manufacturer_probe;
		}
		flash->bank_curr = curr_bank;
	} else {
		flash->bank_curr = curr_bank;
	}

	spi_release_bus(spi);

	return flash;

err_manufacturer_probe:
err_read_id:
	spi_release_bus(spi);
err_claim_bus:
	spi_free_slave(spi);
	return NULL;
}

void *spi_flash_do_alloc(int offset, int size, struct spi_slave *spi,
			 const char *name)
{
	struct spi_flash *flash;
	void *ptr;

	ptr = malloc(size);
	if (!ptr) {
		debug("SF: Failed to allocate memory\n");
		return NULL;
	}
	memset(ptr, '\0', size);
	flash = (struct spi_flash *)(ptr + offset);

	/* Set up some basic fields - caller will sort out sizes */
	flash->spi = spi;
	flash->name = name;

	flash->read = spi_flash_cmd_read_fast;
	flash->write = spi_flash_cmd_write_multi;
	flash->erase = spi_flash_cmd_erase;

	return flash;
}

void spi_flash_free(struct spi_flash *flash)
{
	spi_free_slave(flash->spi);
	free(flash);
}
