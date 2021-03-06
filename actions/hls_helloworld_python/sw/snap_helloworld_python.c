/*
 * Copyright 2019 International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * SNAP HelloWorld Example
 *
 * Demonstration how to get data into the FPGA, process it using a SNAP
 * action and move the data out of the FPGA back to host-DRAM.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>

#include <osnap_tools.h>
#include <libosnap.h>
#include <action_changecase_python.h>
#include <osnap_hls_if.h>

int verbose_flag = 0;

static const char *mem_tab[] = { "HOST_DRAM", "CARD_DRAM", "TYPE_NVME" };

// Function that fills the MMIO registers / data structure
// these are all data exchanged between the application and the action
static void snap_prepare_helloworld(struct snap_job *cjob,
				 struct helloworldp_job *mjob,
				 void *addr_in,
				 uint32_t size_in,
				 uint8_t type_in,
				 void *addr_out,
				 uint32_t size_out,
				 uint8_t type_out)
{
	fprintf(stderr, "  prepare helloworldp job of %ld bytes size\n", sizeof(*mjob));

	assert(sizeof(*mjob) <= SNAP_JOBSIZE);
	memset(mjob, 0, sizeof(*mjob));

	// Setting input params : where text is located in host memory
	snap_addr_set(&mjob->in, addr_in, size_in, type_in,
		      SNAP_ADDRFLAG_ADDR | SNAP_ADDRFLAG_SRC);
	// Setting output params : where result will be written in host memory
	snap_addr_set(&mjob->out, addr_out, size_out, type_out,
		      SNAP_ADDRFLAG_ADDR | SNAP_ADDRFLAG_DST |
		      SNAP_ADDRFLAG_END);

	snap_job_set(cjob, mjob, sizeof(*mjob), NULL, 0);
}

/* main program of the application for the hls_helloworld example        */
/* This application will always be run on CPU and will call either       */
/* a software action (CPU executed) or a hardware action (FPGA executed) */
#ifdef PY_WRAP
int uppercase(char *input_str, char *output_str)
{
#else
int main(int argc, char *argv[])
{
	char *input_str  = argv[1];
	char *output_str = argv[2];
	assert(argc == 3);
#endif
	// Init of all the default values used
	int rc = 0;
	int card_no = 0;
	struct snap_card *card = NULL;
	struct snap_action *action = NULL;
	char device[128];
	struct snap_job cjob;
	struct helloworldp_job mjob;
	unsigned long timeout = 600;
	struct timeval etime, stime;
	ssize_t size = 1024 * 1024;
	uint8_t *ibuff = NULL, *obuff = NULL;
	uint8_t type_in = SNAP_ADDRTYPE_HOST_DRAM;
	uint64_t addr_in = 0x0ull;
	uint8_t type_out = SNAP_ADDRTYPE_HOST_DRAM;
	uint64_t addr_out = 0x0ull;
	int verify = 0;
	int exit_code = EXIT_SUCCESS;
	uint8_t trailing_zeros[1024] = { 0, };
	// default is interrupt mode enabled (vs polling)
	//snap_action_flag_t action_irq = (SNAP_ACTION_DONE_IRQ | SNAP_ATTACH_IRQ);
	snap_action_flag_t action_irq = SNAP_ACTION_DONE_IRQ;



	/* if input string is defined, use that as input */
	if (input_str != NULL) {

		int input_str_len;
		for (input_str_len = 0; input_str[input_str_len] != '\0'; ++input_str_len);

		printf("Length of the input string: %d\n", input_str_len);

		size = input_str_len * 2; //FIXME: its not that
		if (size < 0)
			goto out_error;

		/* Allocate in host memory the place to put the text to process */
		ibuff = snap_malloc(size); //64Bytes aligned malloc
		if (ibuff == NULL)
			goto out_error;
		memset(ibuff, 0, size);

		fprintf(stdout, "reading input data %d bytes from input string: %s\n",
			(int)size, input_str);

		// copy text from string to host memory FIXME: we don't need copy

		memcpy (ibuff, input_str, size);

		// prepare params to be written in MMIO registers for action
		type_in = SNAP_ADDRTYPE_HOST_DRAM;
		addr_in = (unsigned long)ibuff;
		//addr_in = (unsigned long)input_str;
	}

	/* if output string is defined, use that as output */

	size_t set_size = size + (verify ? sizeof(trailing_zeros) : 0);

	/* Allocate in host memory the place to put the text processed */
	obuff = snap_malloc(set_size); //64Bytes aligned malloc
	if (obuff == NULL)
		goto out_error;
	memset(obuff, 0x0, set_size);

	// prepare params to be written in MMIO registers for action
	type_out = SNAP_ADDRTYPE_HOST_DRAM;
	addr_out = (unsigned long)obuff;


	/* Display the parameters that will be used for the example */
	printf("PARAMETERS:\n"
	       "  input_str:   %s\n"
	       "  output_str:  %s\n"
	       "  type_in:     %x %s\n"
	       "  addr_in:     %016llx\n"
	       "  type_out:    %x %s\n"
	       "  addr_out:    %016llx\n"
	       "  size_in/out: %08lx\n",
	       input_str  ? input_str  : "unknown", output_str ? output_str : "unknown",
	       type_in,  mem_tab[type_in],  (long long)addr_in,
	       type_out, mem_tab[type_out], (long long)addr_out,
	       size);


	// Allocate the card that will be used
        if(card_no == 0)
                snprintf(device, sizeof(device)-1, "IBM,oc-snap");
        else
                snprintf(device, sizeof(device)-1, "/dev/ocxl/IBM,oc-snap.000%d:00:00.1.0", card_no);

	card = snap_card_alloc_dev(device, SNAP_VENDOR_ID_IBM,
				   SNAP_DEVICE_ID_SNAP);
	if (card == NULL) {
		fprintf(stderr, "err: failed to open card %u: %s\n",
			card_no, strerror(errno));
                fprintf(stderr, "Default mode is FPGA mode.\n");
                fprintf(stderr, "Did you want to run CPU mode ? => add SNAP_CONFIG=CPU before your command.\n");
                fprintf(stderr, "Otherwise make sure you ran snap_find_card and snap_maint for your selected card.\n");
		goto out_error;
	}

	// Attach the action that will be used on the allocated card
	action = snap_attach_action(card, ACTION_TYPE, action_irq, 60);
	if(action_irq)
		snap_action_assign_irq(action, ACTION_IRQ_SRC_LO);
	if (action == NULL) {
		fprintf(stderr, "err: failed to attach action %u: %s\n",
			card_no, strerror(errno));
		goto out_error1;
	}

	// Fill the stucture of data exchanged with the action
	snap_prepare_helloworld(&cjob, &mjob,
			     (void *)addr_in,  size, type_in,
			     (void *)addr_out, size, type_out);

	// uncomment to dump the job structure
	//__hexdump(stderr, &mjob, sizeof(mjob));


	// Collect the timestamp BEFORE the call of the action
	gettimeofday(&stime, NULL);

	// Call the action will:
	//    write all the registers to the action (MMIO)
	//  + start the action
	//  + wait for completion
	//  + read all the registers from the action (MMIO)
	rc = snap_action_sync_execute_job(action, &cjob, timeout);

	// Collect the timestamp AFTER the call of the action
	gettimeofday(&etime, NULL);
	if (rc != 0) {
		fprintf(stderr, "err: job execution %d: %s!\n", rc,
			strerror(errno));
		goto out_error2;
	}

	/* If the output buffer is in host DRAM we can write it to a file */

	fprintf(stdout, "writing output data %p %d bytes to output_str\n",
		obuff, (int)size);

	memcpy (output_str, obuff, size);

	fprintf(stdout, "output_str is %s\n", output_str);


	// test return code
	(cjob.retc == SNAP_RETC_SUCCESS) ? fprintf(stdout, "SUCCESS\n") : fprintf(stdout, "FAILED\n");
	if (cjob.retc != SNAP_RETC_SUCCESS) {
		fprintf(stderr, "err: Unexpected RETC=%x!\n", cjob.retc);
		goto out_error2;
	}

	// Compare the input and output if verify option -X is enabled
	if (verify) {
		if ((type_in  == SNAP_ADDRTYPE_HOST_DRAM) &&
		    (type_out == SNAP_ADDRTYPE_HOST_DRAM)) {
			rc = memcmp(ibuff, obuff, size);
			if (rc != 0)
				exit_code = EX_ERR_VERIFY;

			rc = memcmp(obuff + size, trailing_zeros, 1024);
			if (rc != 0) {
				fprintf(stderr, "err: trailing zero "
					"verification failed!\n");
				__hexdump(stderr, obuff + size, 1024);
				exit_code = EX_ERR_VERIFY;
			}

		} else
			fprintf(stderr, "warn: Verification works currently "
				"only with HOST_DRAM\n");
	}
	// Display the time of the action call (MMIO registers filled + execution)
	fprintf(stdout, "SNAP helloworld_python took %lld usec\n",
		(long long)timediff_usec(&etime, &stime));

	// Detach action + disallocate the card
	snap_detach_action(action);
	snap_card_free(card);

	__free(obuff);
	__free(ibuff);

#ifdef PY_WRAP
	return(exit_code);
#else
	exit(exit_code);
#endif

 out_error2:
	snap_detach_action(action);
 out_error1:
	snap_card_free(card);
 out_error:
	__free(obuff);
	__free(ibuff);
	exit(EXIT_FAILURE);
}
