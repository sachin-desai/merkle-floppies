#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include "floppies.h"
#include "utils.h"
#include "usr_args.h"
#include "recv.h"
#include "hash.h"
#include "mt_config.h"
#include "merkletree.h"
#include "mt_arr_list.h"

/* 
 * read_shard_blob
 *  	Read data floppy in a single read
 *
 * input: 
 * 	shard_file: floppy name to read, and verify
 * 	fs: shard data
 *
 * output:
 * 	ret: fail status if unable to read shard floppy
 */
static int
read_shard_blob(const char* shard_file, f_shard* fs)
{
	int ret = 0;
	FILE* fp = NULL;

	if (!(fp = fopen(shard_file, "r"))) {
		error_print("Unable to read %s\n", shard_file);
		ret = 1;
		goto error;
	}

	fread(fs->shard_blob, sizeof(fs->shard_blob), 1, fp);
	fclose(fp);

error:
	return ret;
}

/* 
 * verify_shard
 *  	Verify sha checksum
 *
 * input: 
 * 	fm: file metadata (read from floppy.meta)
 * 	fs: shard data
 * 	shard_file: floppy name to read, and verify
 * 	shard_bytes: total data bytes from the shard file 
 *
 * output:
 * 	ret: fail status if unable to read shard floppy, or floppy sha does not match
 */
static int
verify_shard(mt_t* mt, f_meta* fm, f_shard* fs,
	char* shard_file, unsigned long* shard_bytes)
{
	int ret = 0;
	SHA256_CTX ctx;
	unsigned long last_shard_sz = 0;
	unsigned char sha[SHA256_DIGEST_LENGTH];

	ret = read_shard_blob(shard_file, fs);
	if (ret)
		goto error;

	/* see if user gave us a meta floppy instead */
	if (fs->shard_info.idx == 0) {
		error_print("User error; metadata floppy '%s' included in list\n",
			shard_file); 
		ret = 1;
		goto error;
	}

	/* check if partial data floppy */ 
	*shard_bytes = sizeof(fs->shard_info.data);
	if (fs->shard_info.idx == fm->meta_info.total_shards) {
		last_shard_sz = fm->meta_info.file_sz % SHARD_DATA_SZ;
		if (last_shard_sz != 0) /* if exactly SHARD_DATA_SZ bytes*/
			*shard_bytes = last_shard_sz;
	}

	/* calculate sha */
	hash_init(&ctx);
	hash_update(&ctx, (unsigned char*) fs->shard_info.data, *shard_bytes);
	hash_final(sha, &ctx);

	/* check for corruption */
	if (memcmp(sha, fs->shard_info.sha, SHA256_DIGEST_LENGTH)) {
		error_print("Possible attack, or floppy '%s' is corrupted\n",
			shard_file);
		ret = 1;
		goto error;
	}

	/* check merkle tree to see if shard is part of file */
	hash_print(shard_file, fs->shard_info.sha);
	if (mt_verify(mt, fs->shard_info.sha, SHA256_DIGEST_LENGTH,
		fs->shard_info.idx - 1) == MT_ERR_ROOT_MISMATCH) {
		printf("Data from floppy '%s' not part of original file\n", shard_file);
		ret = 1;
		goto error;
	} else {
		// received good files++
		//add to valid list of floppies
	}

error:
	return ret;
}

/* 
 * process_shard_floppy
 *  	Write data to reconstructed file from non-corrupted floppies
 *
 * input: 
 * 	fm: file metadata (read from floppy.meta)
 * 	shard_file: floppy name to read, and verify
 * 	fo: output (aka final destination) file to write floppy data into 
 *
 * output:
 * 	ret: fail status if unable to read shard floppy, or floppy sha does not match
 */
static int
process_shard_floppy(mt_t* mt, f_meta* fm, char* shard_file, FILE* fo)
{
	int ret = 0;
	long offset = 0;
	struct floppy_shard fs;
	unsigned long shard_bytes = 0;

	ret = verify_shard(mt, fm, &fs, shard_file, &shard_bytes); 
	if (ret)
		goto error;

	debug_print("RD %s: shard bytes: %lu\n", shard_file, shard_bytes);

	/* add shard to the original file */
	offset = (fs.shard_info.idx-1) * SHARD_DATA_SZ;
	fseek(fo, offset, SEEK_SET);
	fwrite(fs.shard_info.data, shard_bytes, 1, fo);

error:
	return ret;
}

/* 
 * process_meta_floppy
 *  	Gather metadata information from guaranteed delivery file
 *
 * input: 
 * 	fm: file metadata information to populate
 *
 * output:
 * 	ret: fail status if unable to open meta data file 
 * 	      or floppy index is not zero (meta data index should be 0)
 */
static int
process_meta_floppy(f_meta* fm, const char* meta_file)
{
	int ret = 0;
	FILE* fp = NULL;
	//mt_hash_t mt_root;

	fp = fopen(meta_file, "r");
	if (!fp) {
		error_print("Unable to open metadata file '%s'\n", meta_file);
		ret = 1;
		goto error;
	}

	fread(fm->meta_blob, sizeof(fm->meta_blob), 1, fp);
	fclose(fp);

	/* not a metadata floppy */
	if (fm->meta_info.idx != 0) {
		ret = 1;
		goto error;
	}	

	/*mt_get_root(mt, mt_root);
	if(memcmp(mt_root, fm->meta_info.sha, SHA256_DIGEST_LENGTH))
		printf("DEBUG: root did not match!\n");*/


error:
	return ret;
}

/* 
 * mt_generate
 *  	Create merkle tree on the receiver side.
 *
 * input: 
 * 	mt: Merkle tree to generate
 * 	fm: file metadata (read from floppy.meta)
 *
 * output:
 * 	n/a
 */
static void
mt_generate(mt_t *mt, f_meta* fm)
{
	char buf[64];
	unsigned long i;
	struct floppy_shard fs;

	for (i = 1; i <= fm->meta_info.total_shards-2; i++) {
		sprintf(buf, "floppy.%lu", i);
		read_shard_blob(buf, &fs);
		mt_add(mt, fs.shard_info.sha, SHA256_DIGEST_LENGTH);
		debug_print("MT: Add floppy.%lu to hash tree", i);
	}
}

/* 
 * process_floppies
 *  	
 * input: 
 * 	floppy_list: comma separated list of received floppies to verify, and write to
 * 	             final output file
 *
 * output:
 * 	bool: fail status if unable to open destination file
 * 	      or unable to process any floppy in list
 */
int
process_floppies(struct usr_args* args)
{
	int i;
	int ret = 0;
	mt_t* mt = NULL;
	static FILE* fo = NULL;
	struct floppy_meta fm;
	mt_hash_t mt_root;

	// add sha value to meta floppy, and compared to mt above 
	ret = process_meta_floppy(&fm, args->meta_file);
	if (ret)
		goto error;

	/* hack to create local dedupe tree */
	mt = mt_create();
	mt_generate(mt, &fm);
	mt_get_root(mt, mt_root);
	mt_print(mt);
	/* -- end hack -- */

	/* see if file already exists */
	hash_print("calculated meta", mt_root);
	hash_print("received meta", fm.meta_info.sha);
	if(!memcmp(mt_root, fm.meta_info.sha, SHA256_DIGEST_LENGTH)) {
		printf("File exists in merkle tree, no floppy processing required\n");
		ret = 1;
		goto error;
	}

	fo = fopen(args->dst_file, "w+");
	if (!fo) {
		error_print("Unable to open destination file '%s'\n",
			args->dst_file);
		ret = 1;
		goto error;
	}

	for (i = 0; i < args->floppy_count; i++) {
		ret = process_shard_floppy(mt, &fm, args->floppy_list[i], fo);
		if (ret)
			goto error;
	}

	for (i = 0; i < args->floppy_count; i++) {
		// count all bitmap values set. 
		// if equal to floppy count, then success
		// else print floppies that require shipment
	}

error:
	if (mt)
		mt_delete(mt);

	if (fo) 
		fclose(fo);

	return ret;
}
