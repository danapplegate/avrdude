/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2000-2004  Brian S. Dean <bsd@bsdhome.com>
 * Copyright (C) 2011 Darell Tan <darell.tan@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* $Id$ */

#include "ac_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "avrdude.h"
#include "libavrdude.h"

#include "tpi.h"

FP_UpdateProgress update_progress;

#define DEBUG 0

/* TPI: returns 1 if NVM controller busy, 0 if free */
int avr_tpi_poll_nvmbsy(const PROGRAMMER *pgm) {
  unsigned char cmd;
  unsigned char res;

  cmd = TPI_CMD_SIN | TPI_SIO_ADDR(TPI_IOREG_NVMCSR);
  (void)pgm->cmd_tpi(pgm, &cmd, 1, &res, 1);
  return (res & TPI_IOREG_NVMCSR_NVMBSY);
}

/* TPI chip erase sequence */
int avr_tpi_chip_erase(const PROGRAMMER *pgm, const AVRPART *p) {
	int err;
  AVRMEM *mem;

  if (p->prog_modes & PM_TPI) {
    pgm->pgm_led(pgm, ON);

    /* Set Pointer Register */
    mem = avr_locate_mem(p, "flash");
    if (mem == NULL) {
      avrdude_message(MSG_INFO, "No flash memory to erase for part %s\n",
          p->desc);
      return -1;
    }

		unsigned char cmd[] = {
			/* write pointer register high byte */
			(TPI_CMD_SSTPR | 0),
			((mem->offset & 0xFF) | 1),
			/* and low byte */
			(TPI_CMD_SSTPR | 1),
			((mem->offset >> 8) & 0xFF),
	    /* write CHIP_ERASE command to NVMCMD register */
			(TPI_CMD_SOUT | TPI_SIO_ADDR(TPI_IOREG_NVMCMD)),
			TPI_NVMCMD_CHIP_ERASE,
			/* write dummy value to start erase */
			TPI_CMD_SST,
			0xFF
		};

    while (avr_tpi_poll_nvmbsy(pgm))
        ;

    err = pgm->cmd_tpi(pgm, cmd, sizeof(cmd), NULL, 0);
    if(err)
	return err;

    while (avr_tpi_poll_nvmbsy(pgm));

    pgm->pgm_led(pgm, OFF);

    return 0;
  } else {
		avrdude_message(MSG_INFO, "%s called for a part that has no TPI\n", __func__);
		return -1;
	}
}

/* TPI program enable sequence */
int avr_tpi_program_enable(const PROGRAMMER *pgm, const AVRPART *p, unsigned char guard_time) {
	int err, retry;
	unsigned char cmd[2];
	unsigned char response;

	if(p->prog_modes & PM_TPI) {
		/* set guard time */
		cmd[0] = (TPI_CMD_SSTCS | TPI_REG_TPIPCR);
		cmd[1] = guard_time;

		err = pgm->cmd_tpi(pgm, cmd, sizeof(cmd), NULL, 0);
    if(err)
			return err;

		/* read TPI ident reg */
    cmd[0] = (TPI_CMD_SLDCS | TPI_REG_TPIIR);
		err = pgm->cmd_tpi(pgm, cmd, 1, &response, sizeof(response));
    if (err || response != TPI_IDENT_CODE) {
      avrdude_message(MSG_INFO, "TPIIR not correct\n");
      return -1;
    }

		/* send SKEY command + SKEY */
		err = pgm->cmd_tpi(pgm, tpi_skey_cmd, sizeof(tpi_skey_cmd), NULL, 0);
		if(err)
			return err;

		/* check if device is ready */
		for(retry = 0; retry < 10; retry++)
		{
			cmd[0] =  (TPI_CMD_SLDCS | TPI_REG_TPISR);
			err = pgm->cmd_tpi(pgm, cmd, 1, &response, sizeof(response));
			if(err || !(response & TPI_REG_TPISR_NVMEN))
				continue;

			return 0;
		}

		avrdude_message(MSG_INFO, "Error enabling TPI external programming mode:");
		avrdude_message(MSG_INFO, "Target does not reply\n");
		return -1;

	} else {
		avrdude_message(MSG_INFO, "%s called for a part that has no TPI\n", __func__);
		return -1;
	}
}

/* TPI: setup NVMCMD register and pointer register (PR) for read/write/erase */
static int avr_tpi_setup_rw(const PROGRAMMER *pgm, const AVRMEM *mem,
			    unsigned long addr, unsigned char nvmcmd)
{
  unsigned char cmd[4];
  int rc;

  /* set NVMCMD register */
  cmd[0] = TPI_CMD_SOUT | TPI_SIO_ADDR(TPI_IOREG_NVMCMD);
  cmd[1] = nvmcmd;
  rc = pgm->cmd_tpi(pgm, cmd, 2, NULL, 0);
  if (rc == -1)
    return -1;

  /* set Pointer Register (PR) */
  cmd[0] = TPI_CMD_SSTPR | 0;
  cmd[1] = (mem->offset + addr) & 0xFF;
  rc = pgm->cmd_tpi(pgm, cmd, 2, NULL, 0);
  if (rc == -1)
    return -1;

  cmd[0] = TPI_CMD_SSTPR | 1;
  cmd[1] = ((mem->offset + addr) >> 8) & 0xFF;
  rc = pgm->cmd_tpi(pgm, cmd, 2, NULL, 0);
  if (rc == -1)
    return -1;

  return 0;
}

int avr_read_byte_default(const PROGRAMMER *pgm, const AVRPART *p, const AVRMEM *mem,
                          unsigned long addr, unsigned char * value)
{
  unsigned char cmd[4];
  unsigned char res[4];
  unsigned char data;
  int r;
  OPCODE * readop, * lext;

  if (pgm->cmd == NULL) {
    avrdude_message(MSG_INFO, "%s: Error: %s programmer uses avr_read_byte_default() but does not\n"
                    "provide a cmd() method.\n",
                    progname, pgm->type);
    return -1;
  }

  pgm->pgm_led(pgm, ON);
  pgm->err_led(pgm, OFF);

  if (p->prog_modes & PM_TPI) {
    if (pgm->cmd_tpi == NULL) {
      avrdude_message(MSG_INFO, "%s: Error: %s programmer does not support TPI\n",
          progname, pgm->type);
      return -1;
    }

    while (avr_tpi_poll_nvmbsy(pgm));

    /* setup for read */
    avr_tpi_setup_rw(pgm, mem, addr, TPI_NVMCMD_NO_OPERATION);

    /* load byte */
    cmd[0] = TPI_CMD_SLD;
    r = pgm->cmd_tpi(pgm, cmd, 1, value, 1);
    if (r == -1) 
      return -1;

    return 0;
  }

  /*
   * figure out what opcode to use
   */
  if (mem->op[AVR_OP_READ_LO]) {
    if (addr & 0x00000001)
      readop = mem->op[AVR_OP_READ_HI];
    else
      readop = mem->op[AVR_OP_READ_LO];
    addr = addr / 2;
  }
  else {
    readop = mem->op[AVR_OP_READ];
  }

  if (readop == NULL) {
#if DEBUG
    avrdude_message(MSG_INFO, "avr_read_byte_default(): operation not supported on memory type \"%s\"\n",
                    mem->desc);
#endif
    return -1;
  }

  /*
   * If this device has a "load extended address" command, issue it.
   */
  lext = mem->op[AVR_OP_LOAD_EXT_ADDR];
  if (lext != NULL) {
    memset(cmd, 0, sizeof(cmd));

    avr_set_bits(lext, cmd);
    avr_set_addr(lext, cmd, addr);
    r = pgm->cmd(pgm, cmd, res);
    if (r < 0)
      return r;
  }

  memset(cmd, 0, sizeof(cmd));

  avr_set_bits(readop, cmd);
  avr_set_addr(readop, cmd, addr);
  r = pgm->cmd(pgm, cmd, res);
  if (r < 0)
    return r;
  data = 0;
  avr_get_output(readop, res, &data);

  pgm->pgm_led(pgm, OFF);

  *value = data;

  return 0;
}


/*
 * Return the number of "interesting" bytes in a memory buffer,
 * "interesting" being defined as up to the last non-0xff data
 * value. This is useful for determining where to stop when dealing
 * with "flash" memory, since writing 0xff to flash is typically a
 * no-op. Always return an even number since flash is word addressed.
 * Only apply this optimisation on flash-type memory.
 */
int avr_mem_hiaddr(const AVRMEM * mem)
{
  int i, n;
  static int disableffopt;

  /* calling once with NULL disables any future trailing-0xff optimisation */
  if(!mem) {
    disableffopt = 1;
    return 0;
  }

  if(disableffopt)
    return mem->size;

  /* if the memory is not a flash-type memory do not remove trailing 0xff */
  if(!avr_mem_is_flash_type(mem))
    return mem->size;

  /* return the highest non-0xff address regardless of how much
     memory was read */
  for (i=mem->size-1; i>0; i--) {
    if (mem->buf[i] != 0xff) {
      n = i+1;
      if (n & 0x01)
        return n+1;
      else
        return n;
    }
  }

  return 0;
}


/*
 * Read the entirety of the specified memory type into the corresponding
 * buffer of the avrpart pointed to by p. If v is non-NULL, verify against
 * v's memory area, only those cells that are tagged TAG_ALLOCATED are
 * verified.
 *
 * Return the number of bytes read, or < 0 if an error occurs.
 */
int avr_read(const PROGRAMMER *pgm, const AVRPART *p, const char *memtype, const AVRPART *v) {
  AVRMEM *mem = avr_locate_mem(p, memtype);
  if (mem == NULL) {
    avrdude_message(MSG_INFO, "No %s memory for part %s\n", memtype, p->desc);
    return LIBAVRDUDE_GENERAL_FAILURE;
  }

  return avr_read_mem(pgm, p, mem, v);
}


/*
 * Read the entirety of the specified memory into the corresponding buffer of
 * the avrpart pointed to by p. If v is non-NULL, verify against v's memory
 * area, only those cells that are tagged TAG_ALLOCATED are verified.
 *
 * Return the number of bytes read, or < 0 if an error occurs.
 */
int avr_read_mem(const PROGRAMMER *pgm, const AVRPART *p, const AVRMEM *mem, const AVRPART *v) {
  unsigned long i, lastaddr;
  unsigned char cmd[4];
  AVRMEM *vmem = NULL;
  int rc;

  if (v != NULL)
      vmem = avr_locate_mem(v, mem->desc);
  /*
   * start with all 0xff
   */
  memset(mem->buf, 0xff, mem->size);

  /* supports "paged load" thru post-increment */
  if ((p->prog_modes & PM_TPI) && mem->page_size > 1 &&
      mem->size % mem->page_size == 0 && pgm->cmd_tpi != NULL) {

    while (avr_tpi_poll_nvmbsy(pgm));

    /* setup for read (NOOP) */
    avr_tpi_setup_rw(pgm, mem, 0, TPI_NVMCMD_NO_OPERATION);

    /* load bytes */
    for (lastaddr = i = 0; i < mem->size; i++) {
      if (vmem == NULL ||
          (vmem->tags[i] & TAG_ALLOCATED) != 0)
      {
        if (lastaddr != i) {
          /* need to setup new address */
          avr_tpi_setup_rw(pgm, mem, i, TPI_NVMCMD_NO_OPERATION);
          lastaddr = i;
        }
        cmd[0] = TPI_CMD_SLD_PI;
        rc = pgm->cmd_tpi(pgm, cmd, 1, mem->buf + i, 1);
        lastaddr++;
        if (rc == -1) {
          avrdude_message(MSG_INFO, "avr_read_mem(): error reading address 0x%04lx\n", i);
          return -1;
        }
      }
      report_progress(i, mem->size, NULL);
    }
    return avr_mem_hiaddr(mem);
  }

  if (pgm->paged_load != NULL && mem->page_size > 1 &&
      mem->size % mem->page_size == 0) {
    /*
     * the programmer supports a paged mode read
     */
    int need_read, failure;
    unsigned int pageaddr;
    unsigned int npages, nread;

    /* quickly scan number of pages to be written to first */
    for (pageaddr = 0, npages = 0;
         pageaddr < mem->size;
         pageaddr += mem->page_size) {
      /* check whether this page must be read */
      for (i = pageaddr;
           i < pageaddr + mem->page_size;
           i++)
        if (vmem == NULL /* no verify, read everything */ ||
            (mem->tags[i] & TAG_ALLOCATED) != 0 /* verify, do only
                                                    read pages that
                                                    are needed in
                                                    input file */) {
          npages++;
          break;
        }
    }

    for (pageaddr = 0, failure = 0, nread = 0;
         !failure && pageaddr < mem->size;
         pageaddr += mem->page_size) {
      /* check whether this page must be read */
      for (i = pageaddr, need_read = 0;
           i < pageaddr + mem->page_size;
           i++)
        if (vmem == NULL /* no verify, read everything */ ||
            (vmem->tags[i] & TAG_ALLOCATED) != 0 /* verify, do only
                                                    read pages that
                                                    are needed in
                                                    input file */) {
          need_read = 1;
          break;
        }
      if (need_read) {
        rc = pgm->paged_load(pgm, p, mem, mem->page_size,
                            pageaddr, mem->page_size);
        if (rc < 0)
          /* paged load failed, fall back to byte-at-a-time read below */
          failure = 1;
      } else {
        avrdude_message(MSG_DEBUG, "%s: avr_read_mem(): skipping page %u: no interesting data\n",
                        progname, pageaddr / mem->page_size);
      }
      nread++;
      report_progress(nread, npages, NULL);
    }
    if (!failure)
      return avr_mem_hiaddr(mem);
    /* else: fall back to byte-at-a-time write, for historical reasons */
  }

  if (strcmp(mem->desc, "signature") == 0) {
    if (pgm->read_sig_bytes) {
      return pgm->read_sig_bytes(pgm, p, mem);
    }
  }

  for (i=0; i < mem->size; i++) {
    if (vmem == NULL ||
	(vmem->tags[i] & TAG_ALLOCATED) != 0)
    {
      rc = pgm->read_byte(pgm, p, mem, i, mem->buf + i);
      if (rc != LIBAVRDUDE_SUCCESS) {
        avrdude_message(MSG_INFO, "avr_read_mem(): error reading address 0x%04lx\n", i);
        if (rc == LIBAVRDUDE_GENERAL_FAILURE) {
          avrdude_message(MSG_INFO, "    read operation not supported for memory %s\n",
                          mem->desc);
          return LIBAVRDUDE_NOTSUPPORTED;
        }
        avrdude_message(MSG_INFO, "    read operation failed for memory %s\n", mem->desc);
        return LIBAVRDUDE_SOFTFAIL;
      }
    }
    report_progress(i, mem->size, NULL);
  }

  return avr_mem_hiaddr(mem);
}



/*
 * write a page data at the specified address
 */
int avr_write_page(const PROGRAMMER *pgm, const AVRPART *p, const AVRMEM *mem,
                   unsigned long addr)
{
  unsigned char cmd[4];
  unsigned char res[4];
  OPCODE * wp, * lext;

  if (pgm->cmd == NULL) {
    avrdude_message(MSG_INFO, "%s: Error: %s programmer uses avr_write_page() but does not\n"
                    "provide a cmd() method.\n",
                    progname, pgm->type);
    return -1;
  }

  wp = mem->op[AVR_OP_WRITEPAGE];
  if (wp == NULL) {
    avrdude_message(MSG_INFO, "avr_write_page(): memory \"%s\" not configured for page writes\n",
                    mem->desc);
    return -1;
  }

  /*
   * if this memory is word-addressable, adjust the address
   * accordingly
   */
  if ((mem->op[AVR_OP_LOADPAGE_LO]) || (mem->op[AVR_OP_READ_LO]))
    addr = addr / 2;

  pgm->pgm_led(pgm, ON);
  pgm->err_led(pgm, OFF);

  /*
   * If this device has a "load extended address" command, issue it.
   */
  lext = mem->op[AVR_OP_LOAD_EXT_ADDR];
  if (lext != NULL) {
    memset(cmd, 0, sizeof(cmd));

    avr_set_bits(lext, cmd);
    avr_set_addr(lext, cmd, addr);
    pgm->cmd(pgm, cmd, res);
  }

  memset(cmd, 0, sizeof(cmd));

  avr_set_bits(wp, cmd);
  avr_set_addr(wp, cmd, addr);
  pgm->cmd(pgm, cmd, res);

  /*
   * since we don't know what voltage the target AVR is powered by, be
   * conservative and delay the max amount the spec says to wait
   */
  usleep(mem->max_write_delay);

  pgm->pgm_led(pgm, OFF);
  return 0;
}


int avr_write_byte_default(const PROGRAMMER *pgm, const AVRPART *p, const AVRMEM *mem,
                   unsigned long addr, unsigned char data)
{
  unsigned char cmd[4];
  unsigned char res[4];
  unsigned char r;
  int ready;
  int tries;
  unsigned long start_time;
  unsigned long prog_time;
  unsigned char b;
  unsigned short caddr;
  OPCODE * writeop;
  int rc;
  int readok=0;
  struct timeval tv;

  if (pgm->cmd == NULL) {
    avrdude_message(MSG_INFO, "%s: Error: %s programmer uses avr_write_byte_default() but does not\n"
                    "provide a cmd() method.\n",
                    progname, pgm->type);
    return -1;
  }

  if (p->prog_modes & PM_TPI) {
    if (pgm->cmd_tpi == NULL) {
      avrdude_message(MSG_INFO, "%s: Error: %s programmer does not support TPI\n",
          progname, pgm->type);
      return -1;
    }

    if (strcmp(mem->desc, "flash") == 0) {
      avrdude_message(MSG_INFO, "Writing a byte to flash is not supported for %s\n", p->desc);
      return -1;
    } else if ((mem->offset + addr) & 1) {
      avrdude_message(MSG_INFO, "Writing a byte to an odd location is not supported for %s\n", p->desc);
      return -1;
    }

    while (avr_tpi_poll_nvmbsy(pgm));

    /* must erase fuse first */
    if (strcmp(mem->desc, "fuse") == 0) {
      /* setup for SECTION_ERASE (high byte) */
      avr_tpi_setup_rw(pgm, mem, addr | 1, TPI_NVMCMD_SECTION_ERASE);

      /* write dummy byte */
      cmd[0] = TPI_CMD_SST;
      cmd[1] = 0xFF;
      rc = pgm->cmd_tpi(pgm, cmd, 2, NULL, 0);

      while (avr_tpi_poll_nvmbsy(pgm));
    }

    /* setup for WORD_WRITE */
    avr_tpi_setup_rw(pgm, mem, addr, TPI_NVMCMD_WORD_WRITE);

    cmd[0] = TPI_CMD_SST_PI;
    cmd[1] = data;
    rc = pgm->cmd_tpi(pgm, cmd, 2, NULL, 0);
    /* dummy high byte to start WORD_WRITE */
    cmd[0] = TPI_CMD_SST_PI;
    cmd[1] = data;
    rc = pgm->cmd_tpi(pgm, cmd, 2, NULL, 0);

    while (avr_tpi_poll_nvmbsy(pgm));

    return 0;
  }

  if (!mem->paged && (p->flags & AVRPART_IS_AT90S1200) == 0) {
    /* 
     * check to see if the write is necessary by reading the existing
     * value and only write if we are changing the value; we can't
     * use this optimization for paged addressing.
     *
     * For mysterious reasons, on the AT90S1200, this read operation
     * sometimes causes the high byte of the same word to be
     * programmed to the value of the low byte that has just been
     * programmed before.  Avoid that optimization on this device.
     */
    rc = pgm->read_byte(pgm, p, mem, addr, &b);
    if (rc != 0) {
      if (rc != -1) {
        return -2;
      }
      /*
       * the read operation is not support on this memory type
       */
    }
    else {
      readok = 1;
      if (b == data) {
        return 0;
      }
    }
  }

  /*
   * determine which memory opcode to use
   */
  if (mem->op[AVR_OP_WRITE_LO]) {
    if (addr & 0x01)
      writeop = mem->op[AVR_OP_WRITE_HI];
    else
      writeop = mem->op[AVR_OP_WRITE_LO];
    caddr = addr / 2;
  }
  else if (mem->paged && mem->op[AVR_OP_LOADPAGE_LO]) {
    if (addr & 0x01)
      writeop = mem->op[AVR_OP_LOADPAGE_HI];
    else
      writeop = mem->op[AVR_OP_LOADPAGE_LO];
    caddr = addr / 2;
  }
  else {
    writeop = mem->op[AVR_OP_WRITE];
    caddr = addr;
  }

  if (writeop == NULL) {
#if DEBUG
    avrdude_message(MSG_INFO, "avr_write_byte_default(): write not supported for memory type \"%s\"\n",
                    mem->desc);
#endif
    return -1;
  }


  pgm->pgm_led(pgm, ON);
  pgm->err_led(pgm, OFF);

  memset(cmd, 0, sizeof(cmd));

  avr_set_bits(writeop, cmd);
  avr_set_addr(writeop, cmd, caddr);
  avr_set_input(writeop, cmd, data);
  pgm->cmd(pgm, cmd, res);

  if (mem->paged) {
    /*
     * in paged addressing, single bytes to be written to the memory
     * page complete immediately, we only need to delay when we commit
     * the whole page via the avr_write_page() routine.
     */
    pgm->pgm_led(pgm, OFF);
    return 0;
  }

  if (readok == 0) {
    /*
     * read operation not supported for this memory type, just wait
     * the max programming time and then return 
     */
    usleep(mem->max_write_delay); /* maximum write delay */
    pgm->pgm_led(pgm, OFF);
    return 0;
  }

  tries = 0;
  ready = 0;
  while (!ready) {

    if ((data == mem->readback[0]) ||
        (data == mem->readback[1])) {
      /* 
       * use an extra long delay when we happen to be writing values
       * used for polled data read-back.  In this case, polling
       * doesn't work, and we need to delay the worst case write time
       * specified for the chip.
       */
      usleep(mem->max_write_delay);
      rc = pgm->read_byte(pgm, p, mem, addr, &r);
      if (rc != 0) {
        pgm->pgm_led(pgm, OFF);
        pgm->err_led(pgm, OFF);
        return -5;
      }
    }
    else {
      gettimeofday (&tv, NULL);
      start_time = (tv.tv_sec * 1000000) + tv.tv_usec;
      do {
        /*
         * Do polling, but timeout after max_write_delay.
	 */
        rc = pgm->read_byte(pgm, p, mem, addr, &r);
        if (rc != 0) {
          pgm->pgm_led(pgm, OFF);
          pgm->err_led(pgm, ON);
          return -4;
        }
        gettimeofday (&tv, NULL);
        prog_time = (tv.tv_sec * 1000000) + tv.tv_usec;
      } while ((r != data) &&
               ((prog_time-start_time) < mem->max_write_delay));
    }

    /*
     * At this point we either have a valid readback or the
     * max_write_delay is expired.
     */
    
    if (r == data) {
      ready = 1;
    }
    else if (mem->pwroff_after_write) {
      /*
       * The device has been flagged as power-off after write to this
       * memory type.  The reason we don't just blindly follow the
       * flag is that the power-off advice may only apply to some
       * memory bits but not all.  We only actually power-off the
       * device if the data read back does not match what we wrote.
       */
      pgm->pgm_led(pgm, OFF);
      avrdude_message(MSG_INFO, "%s: this device must be powered off and back on to continue\n",
                      progname);
      if (pgm->pinno[PPI_AVR_VCC]) {
        avrdude_message(MSG_INFO, "%s: attempting to do this now ...\n", progname);
        pgm->powerdown(pgm);
        usleep(250000);
        rc = pgm->initialize(pgm, p);
        if (rc < 0) {
          avrdude_message(MSG_INFO, "%s: initialization failed, rc=%d\n", progname, rc);
          avrdude_message(MSG_INFO, "%s: can't re-initialize device after programming the "
                          "%s bits\n", progname, mem->desc);
          avrdude_message(MSG_INFO, "%s: you must manually power-down the device and restart\n"
                          "%s:   %s to continue.\n",
                          progname, progname, progname);
          return -3;
        }
        
        avrdude_message(MSG_INFO, "%s: device was successfully re-initialized\n",
                progname);
        return 0;
      }
    }

    tries++;
    if (!ready && tries > 5) {
      /*
       * we wrote the data, but after waiting for what should have
       * been plenty of time, the memory cell still doesn't match what
       * we wrote.  Indicate a write error.
       */
      pgm->pgm_led(pgm, OFF);
      pgm->err_led(pgm, ON);
      
      return -6;
    }
  }

  pgm->pgm_led(pgm, OFF);
  return 0;
}


/*
 * write a byte of data at the specified address
 */
int avr_write_byte(const PROGRAMMER *pgm, const AVRPART *p, const AVRMEM *mem,
                   unsigned long addr, unsigned char data)
{
  return pgm->write_byte(pgm, p, mem, addr, data);
}


/*
 * Write the whole memory region of the specified memory from its buffer of
 * the avrpart pointed to by p to the device.  Write up to size bytes from
 * the buffer.  Data is only written if the corresponding tags byte is set.
 * Data beyond size bytes are not affected.
 *
 * Return the number of bytes written, or LIBAVRDUDE_GENERAL_FAILURE on error.
 */
int avr_write(const PROGRAMMER *pgm, const AVRPART *p, const char *memtype, int size, int auto_erase) {
  AVRMEM *m = avr_locate_mem(p, memtype);
  if (m == NULL) {
    avrdude_message(MSG_INFO, "No \"%s\" memory for part %s\n",
            memtype, p->desc);
    return LIBAVRDUDE_GENERAL_FAILURE;
  }

  return avr_write_mem(pgm, p, m, size, auto_erase);
}

/*
 * Write the whole memory region of the specified memory from its buffer of
 * the avrpart pointed to by p to the device.  Write up to size bytes from
 * the buffer.  Data is only written if the corresponding tags byte is set.
 * Data beyond size bytes are not affected.
 *
 * Return the number of bytes written, or LIBAVRDUDE_GENERAL_FAILURE on error.
 */
int avr_write_mem(const PROGRAMMER *pgm, const AVRPART *p, const AVRMEM *m, int size, int auto_erase) {
  int              rc;
  int              newpage, page_tainted, flush_page, do_write;
  int              wsize;
  unsigned int     i, lastaddr;
  unsigned char    data;
  int              werror;
  unsigned char    cmd[4];

  pgm->err_led(pgm, OFF);

  werror  = 0;

  wsize = m->size;
  if (size < wsize) {
    wsize = size;
  }
  else if (size > wsize) {
    avrdude_message(MSG_INFO, "%s: WARNING: %d bytes requested, but memory region is only %d"
                    "bytes\n"
                    "%sOnly %d bytes will actually be written\n",
                    progname, size, wsize,
                    progbuf, wsize);
  }


  if ((p->prog_modes & PM_TPI) && m->page_size > 1 && pgm->cmd_tpi) {
    if (wsize == 1) {
      /* fuse (configuration) memory: only single byte to write */
      return avr_write_byte(pgm, p, m, 0, m->buf[0]) == 0? 1: LIBAVRDUDE_GENERAL_FAILURE;
    }

    while (avr_tpi_poll_nvmbsy(pgm));

    /* setup for WORD_WRITE */
    avr_tpi_setup_rw(pgm, m, 0, TPI_NVMCMD_WORD_WRITE);

    /* make sure it's aligned to a word boundary */
    if (wsize & 0x1) {
      wsize++;
    }

    /* write words, low byte first */
    for (lastaddr = i = 0; i < wsize; i += 2) {
      if ((m->tags[i] & TAG_ALLOCATED) != 0 ||
          (m->tags[i + 1] & TAG_ALLOCATED) != 0) {

        if (lastaddr != i) {
          /* need to setup new address */
          avr_tpi_setup_rw(pgm, m, i, TPI_NVMCMD_WORD_WRITE);
          lastaddr = i;
        }

        cmd[0] = TPI_CMD_SST_PI;
        cmd[1] = m->buf[i];
        rc = pgm->cmd_tpi(pgm, cmd, 2, NULL, 0);

        cmd[1] = m->buf[i + 1];
        rc = pgm->cmd_tpi(pgm, cmd, 2, NULL, 0);

        lastaddr += 2;

        while (avr_tpi_poll_nvmbsy(pgm));
      }
      report_progress(i, wsize, NULL);
    }
    return i;
  }

  if (pgm->paged_write != NULL && m->page_size > 1) {
    /*
     * the programmer supports a paged mode write
     */
    int need_write, failure;
    unsigned int pageaddr;
    unsigned int npages, nwritten;

    /* quickly scan number of pages to be written to first */
    for (pageaddr = 0, npages = 0;
         pageaddr < wsize;
         pageaddr += m->page_size) {
      /* check whether this page must be written to */
      for (i = pageaddr;
           i < pageaddr + m->page_size;
           i++)
        if ((m->tags[i] & TAG_ALLOCATED) != 0) {
          npages++;
          break;
        }
    }

    for (pageaddr = 0, failure = 0, nwritten = 0;
         !failure && pageaddr < wsize;
         pageaddr += m->page_size) {
      /* check whether this page must be written to */
      for (i = pageaddr, need_write = 0;
           i < pageaddr + m->page_size;
           i++)
        if ((m->tags[i] & TAG_ALLOCATED) != 0) {
          need_write = 1;
          break;
        }
      if (need_write) {
        rc = 0;
        if (auto_erase)
          rc = pgm->page_erase(pgm, p, m, pageaddr);
        if (rc >= 0)
          rc = pgm->paged_write(pgm, p, m, m->page_size, pageaddr, m->page_size);
        if (rc < 0)
          /* paged write failed, fall back to byte-at-a-time write below */
          failure = 1;
      } else {
        avrdude_message(MSG_DEBUG, "%s: avr_write_mem(): skipping page %u: no interesting data\n",
                        progname, pageaddr / m->page_size);
      }
      nwritten++;
      report_progress(nwritten, npages, NULL);
    }
    if (!failure)
      return wsize;
    /* else: fall back to byte-at-a-time write, for historical reasons */
  }

  if (pgm->write_setup) {
      pgm->write_setup(pgm, p, m);
  }

  newpage = 1;
  page_tainted = 0;
  flush_page = 0;

  for (i=0; i<wsize; i++) {
    data = m->buf[i];
    report_progress(i, wsize, NULL);

    /*
     * Find out whether the write action must be invoked for this
     * byte.
     *
     * For non-paged memory, this only happens if TAG_ALLOCATED is
     * set for the byte.
     *
     * For paged memory, TAG_ALLOCATED also invokes the write
     * operation, which is actually a page buffer fill only.  This
     * "taints" the page, and upon encountering the last byte of each
     * tainted page, the write operation must also be invoked in order
     * to actually write the page buffer to memory.
     */
    do_write = (m->tags[i] & TAG_ALLOCATED) != 0;
    if (m->paged) {
      if (newpage) {
        page_tainted = do_write;
      } else {
        page_tainted |= do_write;
      }
      if (i % m->page_size == m->page_size - 1 ||
          i == wsize - 1) {
        /* last byte this page */
        flush_page = page_tainted;
        newpage = 1;
      } else {
        flush_page = newpage = 0;
      }
    }

    if (!do_write && !flush_page) {
      continue;
    }

    if (do_write) {
      rc = avr_write_byte(pgm, p, m, i, data);
      if (rc) {
        avrdude_message(MSG_INFO, " ***failed;  ");
        avrdude_message(MSG_INFO, "\n");
        pgm->err_led(pgm, ON);
        werror = 1;
      }
    }

    /*
     * check to see if it is time to flush the page with a page
     * write
     */
    if (flush_page) {
      rc = avr_write_page(pgm, p, m, i);
      if (rc) {
        avrdude_message(MSG_INFO, " *** page %d (addresses 0x%04x - 0x%04x) failed "
                        "to write\n",
                        i % m->page_size,
                        i - m->page_size + 1, i);
        avrdude_message(MSG_INFO, "\n");
        pgm->err_led(pgm, ON);
          werror = 1;
      }
    }

    if (werror) {
      /*
       * make sure the error led stay on if there was a previous write
       * error, otherwise it gets cleared in avr_write_byte()
       */
      pgm->err_led(pgm, ON);
    }
  }

  return i;
}



/*
 * read the AVR device's signature bytes
 */
int avr_signature(const PROGRAMMER *pgm, const AVRPART *p) {
  int rc;

  report_progress (0,1,"Reading");
  rc = avr_read(pgm, p, "signature", 0);
  if (rc < LIBAVRDUDE_SUCCESS) {
    avrdude_message(MSG_INFO, "%s: error reading signature data for part \"%s\", rc=%d\n",
                    progname, p->desc, rc);
    return rc;
  }
  report_progress (1,1,NULL);

  return LIBAVRDUDE_SUCCESS;
}

static uint8_t get_fuse_bitmask(AVRMEM * m) {
  uint8_t bitmask_r = 0;
  uint8_t bitmask_w = 0;
  int i;

  if (!m || m->size > 1) {
    // not a fuse, compare bytes directly
    return 0xFF;
  }

  if (m->op[AVR_OP_WRITE] == NULL ||
      m->op[AVR_OP_READ] == NULL)
    // no memory operations provided by configuration, compare directly
    return 0xFF;

  // For fuses, only compare bytes that are actually written *and* read.
  for (i = 0; i < 32; i++) {
    if (m->op[AVR_OP_WRITE]->bit[i].type == AVR_CMDBIT_INPUT)
      bitmask_w |= (1 << m->op[AVR_OP_WRITE]->bit[i].bitno);
    if (m->op[AVR_OP_READ]->bit[i].type == AVR_CMDBIT_OUTPUT)
      bitmask_r |= (1 << m->op[AVR_OP_READ]->bit[i].bitno);
  }
  return bitmask_r & bitmask_w;
}

int compare_memory_masked(AVRMEM * m, uint8_t b1, uint8_t b2) {
  uint8_t bitmask = get_fuse_bitmask(m);
  return (b1 & bitmask) != (b2 & bitmask);
}

/*
 * Verify the memory buffer of p with that of v.  The byte range of v,
 * may be a subset of p.  The byte range of p should cover the whole
 * chip's memory size.
 *
 * Return the number of bytes verified, or -1 if they don't match.
 */
int avr_verify(const AVRPART * p, const AVRPART * v, const char * memtype, int size)
{
  int i;
  unsigned char * buf1, * buf2;
  int vsize;
  AVRMEM * a, * b;

  a = avr_locate_mem(p, memtype);
  if (a == NULL) {
    avrdude_message(MSG_INFO, "avr_verify(): memory type \"%s\" not defined for part %s\n",
                    memtype, p->desc);
    return -1;
  }

  b = avr_locate_mem(v, memtype);
  if (b == NULL) {
    avrdude_message(MSG_INFO, "avr_verify(): memory type \"%s\" not defined for part %s\n",
                    memtype, v->desc);
    return -1;
  }

  buf1  = a->buf;
  buf2  = b->buf;
  vsize = a->size;

  if (vsize < size) {
    avrdude_message(MSG_INFO, "%s: WARNING: requested verification for %d bytes\n"
                    "%s%s memory region only contains %d bytes\n"
                    "%sOnly %d bytes will be verified.\n",
                    progname, size,
                    progbuf, memtype, vsize,
                    progbuf, vsize);
    size = vsize;
  }

  for (i=0; i<size; i++) {
    if ((b->tags[i] & TAG_ALLOCATED) != 0 &&
        buf1[i] != buf2[i]) {
      uint8_t bitmask = get_fuse_bitmask(a);
      if((buf1[i] & bitmask) != (buf2[i] & bitmask)) {
        // Mismatch is not just in unused bits
        avrdude_message(MSG_INFO, "%s: verification error, first mismatch at byte 0x%04x\n"
                        "%s0x%02x != 0x%02x\n",
                        progname, i,
                        progbuf, buf1[i], buf2[i]);
        return -1;
      } else {
        // Mismatch is only in unused bits
        if ((buf1[i] | bitmask) != 0xff) {
          // Programmer returned unused bits as 0, must be the part/programmer
          avrdude_message(MSG_INFO, "%s: WARNING: ignoring mismatch in unused bits of \"%s\"\n"
                          "%s(0x%02x != 0x%02x). To prevent this warning fix the part\n"
                          "%sor programmer definition in the config file.\n",
                          progname, memtype, progbuf, buf1[i], buf2[i], progbuf);
        } else {
          // Programmer returned unused bits as 1, must be the user
          avrdude_message(MSG_INFO, "%s: WARNING: ignoring mismatch in unused bits of \"%s\"\n"
                          "%s(0x%02x != 0x%02x). To prevent this warning set unused bits\n"
                          "%sto 1 when writing (double check with your datasheet first).\n",
                          progname, memtype, progbuf, buf1[i], buf2[i], progbuf);
        }
      }
    }
  }

  return size;
}


int avr_get_cycle_count(const PROGRAMMER *pgm, const AVRPART *p, int *cycles) {
  AVRMEM * a;
  unsigned int cycle_count = 0;
  unsigned char v1;
  int rc;
  int i;

  a = avr_locate_mem(p, "eeprom");
  if (a == NULL) {
    return -1;
  }

  for (i=4; i>0; i--) {
    rc = pgm->read_byte(pgm, p, a, a->size-i, &v1);
  if (rc < 0) {
    avrdude_message(MSG_INFO, "%s: WARNING: can't read memory for cycle count, rc=%d\n",
            progname, rc);
    return -1;
  }
    cycle_count = (cycle_count << 8) | v1;
  }

   /*
   * If the EEPROM is erased, the cycle count reads 0xffffffff.
   * In this case we return a cycle_count of zero.
   * So, the calling function don't have to care about whether or not
   * the cycle count was initialized.
   */
  if (cycle_count == 0xffffffff) {
    cycle_count = 0;
  }

  *cycles = (int) cycle_count;

  return 0;
}


int avr_put_cycle_count(const PROGRAMMER *pgm, const AVRPART *p, int cycles) {
  AVRMEM * a;
  unsigned char v1;
  int rc;
  int i;

  a = avr_locate_mem(p, "eeprom");
  if (a == NULL) {
    return -1;
  }

  for (i=1; i<=4; i++) {
    v1 = cycles & 0xff;
    cycles = cycles >> 8;

    rc = avr_write_byte(pgm, p, a, a->size-i, v1);
    if (rc < 0) {
      avrdude_message(MSG_INFO, "%s: WARNING: can't write memory for cycle count, rc=%d\n",
              progname, rc);
      return -1;
    }
  }

  return 0;
}


// Typical order in which memories show in avrdude.conf, runtime adds unknown ones (if any)
const char *avr_mem_order[100] = {
  "eeprom",       "flash",        "application",  "apptable",
  "boot",         "lfuse",        "hfuse",        "efuse",
  "fuse",         "fuse0",        "wdtcfg",       "fuse1",
  "bodcfg",       "fuse2",        "osccfg",       "fuse3",
  "fuse4",        "tcd0cfg",      "fuse5",        "syscfg0",
  "fuse6",        "syscfg1",      "fuse7",        "append",
  "codesize",     "fuse8",        "fuse9",        "bootend",
  "bootsize",     "fuses",        "lock",         "lockbits",
  "tempsense",    "signature",    "prodsig",      "sernum",
  "calibration",  "osccal16",     "osccal20",     "osc16err",
  "osc20err",     "usersig",      "userrow",      "data",
};

void avr_add_mem_order(const char *str) {
  for(size_t i=0; i < sizeof avr_mem_order/sizeof *avr_mem_order; i++) {
    if(avr_mem_order[i] && !strcmp(avr_mem_order[i], str))
      return;
    if(!avr_mem_order[i]) {
      avr_mem_order[i] = cfg_strdup("avr_mem_order()", str);
      return;
    }
  }
  avrdude_message(MSG_INFO,
    "%s: avr_mem_order[] under-dimensioned in avr.c; increase and recompile\n",
    progname);
  exit(1);
}

int avr_mem_is_flash_type(const AVRMEM *mem) {
  return
     strcmp(mem->desc, "flash") == 0 ||
     strcmp(mem->desc, "application") == 0 ||
     strcmp(mem->desc, "apptable") == 0 ||
     strcmp(mem->desc, "boot") == 0;
}

int avr_mem_is_eeprom_type(const AVRMEM *mem) {
  return strcmp(mem->desc, "eeprom") == 0;
}

int avr_mem_is_known(const char *str) {
  if(str && *str)
    for(size_t i=0; i < sizeof avr_mem_order/sizeof *avr_mem_order; i++)
      if(avr_mem_order[i] && !strcmp(avr_mem_order[i], str))
        return 1;
  return 0;
}

int avr_mem_might_be_known(const char *str) {
  if(str && *str)
    for(size_t i=0; i < sizeof avr_mem_order/sizeof *avr_mem_order; i++)
      if(avr_mem_order[i] && !strncmp(avr_mem_order[i], str, strlen(str)))
        return 1;
  return 0;
}


int avr_chip_erase(const PROGRAMMER *pgm, const AVRPART *p) {
  return pgm->chip_erase(pgm, p);
}

int avr_unlock(const PROGRAMMER *pgm, const AVRPART *p) {
  int rc = -1;

  if (pgm->unlock)
    rc = pgm->unlock(pgm, p);

  return rc;
}

/*
 * Report the progress of a read or write operation from/to the device
 *
 * The first call of report_progress() should look like this (for a write):
 *
 * report_progress(0, 1, "Writing");
 *
 * Then hdr should be passed NULL on subsequent calls *
 * report_progress(k, n, NULL); // k/n signifies proportion of work done
 *
 * with 0 <= k < n, while the operation is progressing. Once the operation is
 * complete, a final call should be made as such to ensure proper termination
 * of the progress report; choose one of the following three forms:
 *
 * report_progress(n, n, NULL); // finished OK, terminate with double \n
 * report_progress(1, 0, NULL); // finished OK, do not print terminating \n
 * report_progress(1, -1, NULL); // finished not OK, print double \n
 *
 * It is OK to call report_progress(1, -1, NULL) in a subroutine when
 * encountering a fatal error to terminate the reporting here and there even
 * though no report may have been started.
 */

void report_progress(int completed, int total, const char *hdr) {
  static int last;
  static double start_time;
  int percent;
  struct timeval tv;
  double t;

  if (update_progress == NULL)
    return;

  percent =
    completed >= total || total <= 0? 100:
    completed < 0? 0:
    completed < INT_MAX/100? 100*completed/total: completed/(total/100);

  gettimeofday(&tv, NULL);
  t = tv.tv_sec + ((double)tv.tv_usec)/1000000;

  if(hdr || !start_time)
    start_time = t;

  if(hdr || percent > last) {
    last = percent;
    update_progress(percent, t - start_time, hdr, total < 0? -1: !!total);
  }
}
