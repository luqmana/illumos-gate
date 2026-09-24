/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2019 Joyent, Inc.
 * Copyright 2026 Oxide Computer Company
 */

/*
 * Command utility to drive synthetic memory decoding.
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <strings.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libnvpair.h>
#include <sys/sysmacros.h>

#include <sys/mc.h>
#include "imc.h"
#include "zen_umc.h"

#define	MCDECODE_USAGE	2

/*
 * Write in 32k chunks.
 */
#define	MCDECODE_WRITE	(1024 * 32)

typedef struct mc_chan_id {
	uint32_t	mci_chip;
	uint32_t	mci_die;
	uint32_t	mci_mc;
	uint32_t	mci_chan;
} mc_chan_id_t;

typedef struct mc_backend {
	const char *mcb_name;
	void *(*mcb_init)(nvlist_t *, const char *);
	void (*mcb_decode_pa)(void *, uint64_t);
	void (*mcb_decode_chan_addr)(void *, uint64_t, const mc_chan_id_t *);
} mc_backend_t;

static const mc_backend_t *mc_cur_backend = NULL;

static void
mcdecode_usage(void)
{
	(void) fprintf(stderr,
	    "Usage: mcdecode -d address -f infile | device\n"
	    "       mcdecode -n address -c mc [-s chip] [-D die] "
	    "-f infile | device\n"
	    "       mcdecode -w outfile device\n"
	    "\n"
	    "\t-c  the memory controller a channel address belongs to\n"
	    "\t-d  decode physical address to the corresponding dimm\n"
	    "\t-D  the die a channel address belongs to (default 0)\n"
	    "\t-f  use decoder image from infile\n"
	    "\t-n  decode a channel address to the corresponding physical\n"
	    "\t    address and dimm\n"
	    "\t-s  the chip a channel address belongs to (default 0)\n"
	    "\t-w  write decoder snapshot state to the specified file\n");
	exit(MCDECODE_USAGE);
}

static void *
mcb_imc_init(nvlist_t *nvl, const char *file)
{
	imc_t *imc;

	imc = calloc(1, sizeof (*imc));
	if (imc == NULL) {
		err(EXIT_FAILURE, "failed to allocate memory for imc_t");
	}

	if (!imc_restore_decoder(nvl, imc)) {
		errx(EXIT_FAILURE, "failed to restore memory "
		    "controller snapshot in %s", file);
	}

	return (imc);
}

static void
mcb_imc_decode_pa(void *arg, uint64_t pa)
{
	const imc_t *imc = arg;
	imc_decode_state_t dec;

	bzero(&dec, sizeof (dec));
	if (!imc_decode_pa(imc, pa, &dec)) {
		errx(EXIT_FAILURE, "failed to decode address 0x%" PRIx64
		    " -- 0x%x, 0x%" PRIx64, pa, dec.ids_fail,
		    dec.ids_fail_data);
	}

	(void) printf("Decoded physical address 0x%" PRIx64 "\n"
	    "\tchip:\t\t\t%u\n"
	    "\tmemory controller:\t%u\n"
	    "\tchannel:\t\t%u\n"
	    "\tdimm:\t\t\t%u\n"
	    "\trank:\t\t\t%u\n",
	    pa, dec.ids_nodeid, dec.ids_tadid, dec.ids_channelid,
	    dec.ids_dimmid, dec.ids_rankid);
}

static void
mcb_imc_decode_chan_addr(void *arg, uint64_t addr, const mc_chan_id_t *id)
{
	errx(EXIT_FAILURE, "decoding channel addresses is not supported");
}

static void *
mcb_umc_init(nvlist_t *nvl, const char *file)
{
	zen_umc_t *umc;

	umc = calloc(1, sizeof (*umc));
	if (umc == NULL) {
		err(EXIT_FAILURE, "failed to allocate memory for zen_umc_t");
	}

	if (!zen_umc_restore_decoder(nvl, umc)) {
		errx(EXIT_FAILURE, "failed to restore memory "
		    "controller snapshot in %s", file);
	}

	return (umc);
}

static void
mcb_umc_print(const zen_umc_t *umc, const zen_umc_decoder_t *dec)
{
	uint32_t sock, die, comp;

	zen_fabric_id_decompose(&umc->umc_decomp, dec->dec_targ_fabid, &sock,
	    &die, &comp);
	(void) printf("\tsocket:\t\t\t%u\n"
	    "\tdie:\t\t\t%u\n"
	    "\tchannel:\t\t%u\n"
	    "\tchannel address\t\t0x%" PRIx64 "\n"
	    "\tdimm:\t\t\t%u\n"
	    "\trow:\t\t\t0x%x\n"
	    "\tcol:\t\t\t0x%x\n"
	    "\tbank:\t\t\t0x%x\n"
	    "\tbank group:\t\t0x%x\n"
	    "\trank mult:\t\t0x%x\n"
	    "\tchip-select:\t\t0x%x\n"
	    "\tsub-channel:\t\t0x%x\n",
	    sock, die, dec->dec_umc_chan->chan_logid, dec->dec_norm_addr,
	    dec->dec_dimm->ud_dimmno, dec->dec_dimm_row, dec->dec_dimm_col,
	    dec->dec_dimm_bank, dec->dec_dimm_bank_group, dec->dec_dimm_rm,
	    dec->dec_dimm_csno, dec->dec_dimm_subchan);
}

static void
mcb_umc_decode_pa(void *arg, uint64_t pa)
{
	zen_umc_t *umc = arg;
	zen_umc_decoder_t dec;

	bzero(&dec, sizeof (dec));
	if (!zen_umc_decode_pa(umc, pa, &dec)) {
		errx(EXIT_FAILURE, "failed to decode address 0x%" PRIx64
		    " -- '%s' (%s/0x%x), data 0x%" PRIx64, pa,
		    zen_umc_decode_strerror(dec.dec_fail),
		    zen_umc_decode_strenum(dec.dec_fail), dec.dec_fail,
		    dec.dec_fail_data);
	}

	(void) printf("Decoded physical address 0x%" PRIx64 "\n", pa);
	mcb_umc_print(umc, &dec);
}

static void
mcb_umc_decode_chan_addr(void *arg, uint64_t addr, const mc_chan_id_t *id)
{
	zen_umc_t *umc = arg;
	zen_umc_decoder_t dec;
	const zen_umc_chan_t *chan;

	chan = zen_umc_find_chan_by_id(umc, id->mci_chip, id->mci_die,
	    id->mci_mc);
	if (chan == NULL) {
		errx(EXIT_FAILURE, "failed to find UMC %u on socket %u, die %u",
		    id->mci_mc, id->mci_chip, id->mci_die);
	}

	bzero(&dec, sizeof (dec));
	if (!zen_umc_decode_norm_addr(umc, chan, addr, &dec)) {
		if (dec.dec_pa != UINT64_MAX) {
			(void) fprintf(stderr, "channel address 0x%" PRIx64
			    " mapped to physical address 0x%" PRIx64 ", "
			    "but decoding failed\n", addr, dec.dec_pa);
		}
		errx(EXIT_FAILURE, "failed to decode channel address 0x%" PRIx64
		    " -- '%s' (%s/0x%x), data 0x%" PRIx64, addr,
		    zen_umc_decode_strerror(dec.dec_fail),
		    zen_umc_decode_strenum(dec.dec_fail), dec.dec_fail,
		    dec.dec_fail_data);
	}

	(void) printf("Decoded channel address 0x%" PRIx64 "\n"
	    "\tphysical address:\t0x%" PRIx64 "\n", addr, dec.dec_pa);
	mcb_umc_print(umc, &dec);
}

static const mc_backend_t mc_backends[] = {
	{ "imc", mcb_imc_init, mcb_imc_decode_pa, mcb_imc_decode_chan_addr },
	{ "zen_umc", mcb_umc_init, mcb_umc_decode_pa, mcb_umc_decode_chan_addr }
};

static void *
mcdecode_from_file(const char *file)
{
	int fd, ret;
	struct stat st;
	void *addr;
	nvlist_t *nvl;
	char *driver;

	if ((fd = open(file, O_RDONLY)) < 0) {
		err(EXIT_FAILURE, "failed to open %s", file);
	}

	if (fstat(fd, &st) != 0) {
		err(EXIT_FAILURE, "failed to get file information for %s",
		    file);
	}

	addr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE,
	    fd, 0);
	if (addr == MAP_FAILED) {
		err(EXIT_FAILURE, "failed to map %s", file);
	}

	ret = nvlist_unpack(addr, st.st_size, &nvl, 0);
	if (ret != 0) {
		errc(EXIT_FAILURE, ret, "failed to unpack %s", file);
	}

	if (munmap(addr, st.st_size) != 0) {
		err(EXIT_FAILURE, "failed to unmap %s", file);
	}

	if (close(fd) != 0) {
		err(EXIT_FAILURE, "failed to close fd for %s", file);
	}

	if (nvlist_lookup_string(nvl, "mc_dump_driver", &driver) != 0) {
		errx(EXIT_FAILURE, "missing driver indication in dump %s",
		    file);
	}

	for (uint_t i = 0; i < ARRAY_SIZE(mc_backends); i++) {
		if (strcmp(driver, mc_backends[i].mcb_name) == 0) {
			void *data;

			mc_cur_backend = &mc_backends[i];
			data = mc_cur_backend->mcb_init(nvl, file);
			nvlist_free(nvl);
			return (data);
		}
	}

	errx(EXIT_FAILURE, "unknown driver dump source %s\n", driver);
}

static void
mcdecode_print_ioc(const mc_encode_ioc_t *ioc)
{
	(void) printf("\tchip:\t\t\t%u\n"
	    "\tdie:\t\t\t%u\n"
	    "\tmemory controller:\t%u\n"
	    "\tchannel:\t\t%u\n"
	    "\tchannel address\t\t0x%" PRIx64 "\n"
	    "\tdimm:\t\t\t%u\n",
	    ioc->mcei_chip, ioc->mcei_die, ioc->mcei_mc, ioc->mcei_chan,
	    ioc->mcei_chan_addr, ioc->mcei_dimm);
	if (ioc->mcei_rank != UINT8_MAX) {
		(void) printf("\trank:\t\t\t%u\n", ioc->mcei_rank);
	}

	if (ioc->mcei_row != UINT32_MAX) {
		(void) printf("\trow:\t\t\t0x%x\n", ioc->mcei_row);
	}

	if (ioc->mcei_column != UINT32_MAX) {
		(void) printf("\tcol:\t\t\t0x%x\n", ioc->mcei_column);
	}

	if (ioc->mcei_bank != UINT8_MAX) {
		(void) printf("\tbank:\t\t\t0x%x\n", ioc->mcei_bank);
	}

	if (ioc->mcei_bank_group != UINT8_MAX) {
		(void) printf("\tbank group:\t\t0x%x\n", ioc->mcei_bank_group);
	}

	if (ioc->mcei_rm != UINT8_MAX) {
		(void) printf("\trank mult:\t\t0x%x\n", ioc->mcei_rm);
	}

	if (ioc->mcei_cs != UINT8_MAX) {
		(void) printf("\tchip-select:\t\t0x%x\n", ioc->mcei_cs);
	}

	if (ioc->mcei_subchan != UINT8_MAX) {
		(void) printf("\tsub-channel:\t\t0x%x\n", ioc->mcei_subchan);
	}
}

static void
mcdecode_phys_addr(const char *device, uint64_t pa)
{
	int fd;
	mc_encode_ioc_t ioc;

	bzero(&ioc, sizeof (ioc));
	ioc.mcei_type = MET_PHYS_ADDR;
	ioc.mcei_pa = pa;

	if ((fd = open(device, O_RDONLY)) < 0) {
		err(EXIT_FAILURE, "failed to open %s", device);
	}

	if (ioctl(fd, MC_IOC_DECODE_ADDR, &ioc) != 0) {
		err(EXIT_FAILURE, "failed to issue decode ioctl");
	}

	if (ioc.mcei_err != 0) {
		(void) fprintf(stderr, "decoding of physical address 0x%" PRIx64
		    " failed with error 0x%x\n", pa, ioc.mcei_err);
		exit(EXIT_FAILURE);
	}

	(void) printf("Decoded physical address 0x%" PRIx64 "\n", pa);
	mcdecode_print_ioc(&ioc);

	(void) close(fd);
}

static void
mcdecode_chan_addr(const char *device, uint64_t addr, const mc_chan_id_t *id)
{
	int fd;
	mc_encode_ioc_t ioc;

	bzero(&ioc, sizeof (ioc));
	ioc.mcei_type = MET_CHAN_ADDR;
	ioc.mcei_chan_addr = addr;
	ioc.mcei_chip = id->mci_chip;
	ioc.mcei_die = id->mci_die;
	ioc.mcei_mc = id->mci_mc;
	ioc.mcei_chan = id->mci_chan;

	if ((fd = open(device, O_RDONLY)) < 0) {
		err(EXIT_FAILURE, "failed to open %s", device);
	}

	if (ioctl(fd, MC_IOC_DECODE_ADDR, &ioc) != 0) {
		if (errno == ENOTSUP) {
			errx(EXIT_FAILURE, "decoding channel addresses is not "
			    "supported by this memory controller driver");
		}
		err(EXIT_FAILURE, "failed to issue channel address decode "
		    "ioctl");
	}

	if (ioc.mcei_err != 0) {
		if (ioc.mcei_pa != UINT64_MAX) {
			(void) fprintf(stderr, "channel address 0x%" PRIx64
			    " corresponds to physical address 0x%" PRIx64
			    ", but decoding it failed\n", addr, ioc.mcei_pa);
		}
		(void) fprintf(stderr, "decoding of channel address 0x%" PRIx64
		    " failed with error 0x%x\n", addr, ioc.mcei_err);
		exit(EXIT_FAILURE);
	}

	(void) printf("Decoded channel address 0x%" PRIx64 "\n"
	    "\tphysical address:\t0x%" PRIx64 "\n", addr, ioc.mcei_pa);
	mcdecode_print_ioc(&ioc);

	(void) close(fd);
}

static void
mcdecode_dump(const char *device, const char *outfile)
{
	int fd;
	mc_snapshot_info_t mcs;
	char *buf;

	if ((fd = open(device, O_RDONLY)) < 0) {
		err(EXIT_FAILURE, "failed to open %s", device);
	}

	bzero(&mcs, sizeof (mcs));
	if (ioctl(fd, MC_IOC_DECODE_SNAPSHOT_INFO, &mcs) != 0) {
		err(EXIT_FAILURE, "failed to get decode snapshot information");
	}

	if ((buf = malloc(mcs.mcs_size)) == NULL) {
		err(EXIT_FAILURE, "failed to allocate %u bytes for the "
		    "dump snapshot", mcs.mcs_size);
	}

	if (ioctl(fd, MC_IOC_DECODE_SNAPSHOT, buf) != 0) {
		err(EXIT_FAILURE, "failed to retrieve decode snapshot");
	}
	(void) close(fd);

	if ((fd = open(outfile, O_RDWR | O_CREAT | O_TRUNC, 0644)) < 0) {
		err(EXIT_FAILURE, "failed to create output file %s", outfile);
	}

	while (mcs.mcs_size > 0) {
		ssize_t ret;
		size_t out = mcs.mcs_size > MCDECODE_WRITE ? MCDECODE_WRITE :
		    mcs.mcs_size;

		ret = write(fd, buf, out);
		if (ret < 0) {
			warn("failed to write to output file %s", outfile);
			(void) unlink(outfile);
			exit(EXIT_FAILURE);
		}

		buf += ret;
		mcs.mcs_size -= ret;
	}

	if (fsync(fd) != 0) {
		warn("failed to sync output file %s", outfile);
		(void) unlink(outfile);
		exit(EXIT_FAILURE);
	}

	(void) close(fd);
}

static uint64_t
mcdecode_parse_num(const char *arg, const char *desc, uint64_t max)
{
	const char *errstr = NULL;
	unsigned long long tmp;

	tmp = strtounumx(arg, 0, max, &errstr, 0);
	if (errstr != NULL) {
		err(EXIT_FAILURE, "failed to parse %s '%s': %s", desc, arg,
		    errstr);
	}

	return ((uint64_t)tmp);
}

int
main(int argc, char *argv[])
{
	int c;
	uint64_t pa = UINT64_MAX, chan_addr = UINT64_MAX;
	const char *outfile = NULL;
	const char *infile = NULL;
	void *backend;
	mc_chan_id_t chan_id = {
		.mci_chip = 0,
		.mci_die = 0,
		.mci_mc = UINT32_MAX,
		.mci_chan = 0,
	};

	while ((c = getopt(argc, argv, "c:d:D:f:n:s:w:")) != -1) {
		switch (c) {
		case 'c':
			chan_id.mci_mc = (uint32_t)mcdecode_parse_num(optarg,
			    "memory controller", UINT32_MAX - 1);
			break;
		case 'd':
			pa = mcdecode_parse_num(optarg, "physical address",
			    UINT64_MAX - 1);
			break;
		case 'D':
			chan_id.mci_die = (uint32_t)mcdecode_parse_num(optarg,
			    "die", UINT32_MAX);
			break;
		case 'f':
			infile = optarg;
			break;
		case 'n':
			chan_addr = mcdecode_parse_num(optarg,
			    "channel address", UINT64_MAX - 1);
			break;
		case 's':
			chan_id.mci_chip = (uint32_t)mcdecode_parse_num(optarg,
			    "chip", UINT32_MAX);
			break;
		case 'w':
			outfile = optarg;
			break;
		case ':':
			warnx("Option -%c requires an operand", optopt);
			mcdecode_usage();
			break;
		case '?':
			warnx("Unknown option: -%c", optopt);
			mcdecode_usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (outfile != NULL && infile != NULL) {
		errx(EXIT_FAILURE, "-f and -w cannot be used together");
	}

	if (pa != UINT64_MAX && outfile != NULL) {
		errx(EXIT_FAILURE, "-w and -d cannot be used together");
	}

	if (chan_addr != UINT64_MAX && outfile != NULL) {
		errx(EXIT_FAILURE, "-w and -n cannot be used together");
	}

	if (pa != UINT64_MAX && chan_addr != UINT64_MAX) {
		errx(EXIT_FAILURE, "-d and -n cannot be used together");
	}

	if (chan_addr != UINT64_MAX && chan_id.mci_mc == UINT32_MAX) {
		errx(EXIT_FAILURE, "-n requires a memory controller to be "
		    "specified with -c");
	}

	if (chan_addr == UINT64_MAX && chan_id.mci_mc != UINT32_MAX) {
		errx(EXIT_FAILURE, "-c can only be used with -n");
	}

	if (pa == UINT64_MAX && chan_addr == UINT64_MAX && outfile == NULL) {
		warnx("missing one of -d, -n, or -w\n");
		mcdecode_usage();

	}

	if (argc != 1 && infile == NULL) {
		errx(EXIT_FAILURE, "missing device argument");
	}

	if (infile == NULL) {
		if (pa != UINT64_MAX) {
			mcdecode_phys_addr(argv[0], pa);
		} else if (chan_addr != UINT64_MAX) {
			mcdecode_chan_addr(argv[0], chan_addr, &chan_id);
		} else {
			mcdecode_dump(argv[0], outfile);
		}

		return (0);
	}

	backend = mcdecode_from_file(infile);
	if (pa != UINT64_MAX) {
		mc_cur_backend->mcb_decode_pa(backend, pa);
	} else if (chan_addr != UINT64_MAX) {
		mc_cur_backend->mcb_decode_chan_addr(backend, chan_addr,
		    &chan_id);
	}
	return (0);
}
