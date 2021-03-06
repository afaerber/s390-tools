/*
 * s390-tools/zipl/src/bootmap.c
 *   Functions to build the bootmap file.
 *
 * Copyright IBM Corp. 2001, 2009.
 *
 * Author(s): Carsten Otte <cotte@de.ibm.com>
 *            Peter Oberparleiter <Peter.Oberparleiter@de.ibm.com>
 */

#include "bootmap.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "boot.h"
#include "disk.h"
#include "error.h"
#include "misc.h"


/* Header text of the bootmap file */
static const char header_text[] = "zSeries bootmap file\n"
				  "created by zIPL\n";

/* Layout of SCSI disk block pointer */
struct scsi_blockptr {
	uint64_t blockno;
	uint16_t size;
	uint16_t blockct;
	uint8_t reserved[4];
} __attribute((packed));

/* Layout of FBA disk block pointer */
struct fba_blockptr {
	uint32_t blockno;
	uint16_t size;
	uint16_t blockct;
} __attribute((packed));

/* Layout of ECKD disk block pointer */
struct eckd_blockptr {
	uint16_t cyl;
	uint16_t head;
	uint8_t sec;
	uint16_t size;
	uint8_t blockct;
} __attribute((packed));

/* Pointer to dedicated empty block in bootmap. */
disk_blockptr_t empty_block;


/* Get size of a bootmap block pointer for disk with given INFO. */
static int
get_blockptr_size(struct disk_info* info)
{
	switch (info->type) {
	case disk_type_scsi:
	case disk_type_virtio:
		return sizeof(struct scsi_blockptr);
	case disk_type_fba:
		return sizeof(struct fba_blockptr);
	case disk_type_eckd_classic:
	case disk_type_eckd_compatible:
		return sizeof(struct eckd_blockptr);
	case disk_type_diag:
		break;
	}
	return 0;
}


void
bootmap_store_blockptr(void* buffer, disk_blockptr_t* ptr,
		       struct disk_info* info)
{
	struct scsi_blockptr* scsi;
	struct eckd_blockptr* eckd;
	struct fba_blockptr* fba;

	memset(buffer, 0, get_blockptr_size(info));
	if (ptr != NULL) {
		switch (info->type) {
		case disk_type_scsi:
		case disk_type_virtio:
			scsi = (struct scsi_blockptr *) buffer;
			scsi->blockno = ptr->linear.block;
			scsi->size = ptr->linear.size;
			scsi->blockct = ptr->linear.blockct;
			break;
		case disk_type_fba:
			fba = (struct fba_blockptr *) buffer;
			fba->blockno = ptr->linear.block;
			fba->size = ptr->linear.size;
			fba->blockct = ptr->linear.blockct;
			break;
		case disk_type_eckd_classic:
		case disk_type_eckd_compatible:
			eckd = (struct eckd_blockptr *) buffer;
			eckd->cyl = ptr->chs.cyl;
			eckd->head = ptr->chs.head |
				     ((ptr->chs.cyl >> 12) & 0xfff0);
			eckd->sec = ptr->chs.sec;
			eckd->size = ptr->chs.size;
			eckd->blockct = ptr->chs.blockct;
			break;
		case disk_type_diag:
			break;
		}
	}
}


#define PROGRAM_TABLE_BLOCK_SIZE	512

/* Calculate the maximum number of entries in the program table. INFO
 * specifies the type of disk. */
static int
get_program_table_size(struct disk_info* info)
{
	return PROGRAM_TABLE_BLOCK_SIZE / get_blockptr_size(info) - 1;
}



static int
check_menu_positions(struct job_menu_data* menu, char* name,
		     struct disk_info* info)
{
	int i;

	for (i=0; i < menu->num; i++) {
		if (menu->entry[i].pos >= get_program_table_size(info)) {
			error_reason("Position %d in menu '%s' exceeds "
				     "maximum for device (%d)",
				     menu->entry[i].pos, name,
				     get_program_table_size(info) - 1);
			return -1;
		}
	}
	return 0;
}


/* Write COUNT elements of the blocklist specified by LIST as a linked list
 * of segment table blocks to the file identified by file descriptor FD. Upon
 * success, return 0 and set SECTION_POINTER to point to the first block in
 * the resulting segment table. Return non-zero otherwise. */
int
add_segment_table(int fd, disk_blockptr_t* list, blocknum_t count,
		  disk_blockptr_t* segment_pointer,
		  struct disk_info* info)
{
	disk_blockptr_t next;
	void* buffer;
	blocknum_t max_offset;
	blocknum_t offset;
	int pointer_size;
	int rc;

	/* Allocate block memory */
	buffer = misc_malloc(info->phy_block_size);
	if (buffer == NULL)
		return -1;
	memset(&next, 0, sizeof(disk_blockptr_t));
	memset(buffer, 0, info->phy_block_size);
	pointer_size = get_blockptr_size(info);
	max_offset = info->phy_block_size / pointer_size - 1;
	/* Fill segment tables, starting from the last one */
	for (offset = (count - 1) % max_offset; count > 0; count--, offset--) {
		/* Replace holes with empty block if necessary*/
		if (disk_is_zero_block(&list[count-1], info))
			bootmap_store_blockptr(
					VOID_ADD(buffer, offset * pointer_size),
					&empty_block, info);
		else
			bootmap_store_blockptr(
					VOID_ADD(buffer, offset * pointer_size),
					&list[count-1], info);
		if (offset > 0)
			continue;
		/* Finalize segment table */
		offset = max_offset;
		bootmap_store_blockptr(VOID_ADD(buffer, offset * pointer_size),
				       &next, info);
		rc = disk_write_block_aligned(fd, buffer, info->phy_block_size,
					      &next, info);
		if (rc) {
			free(buffer);
			return rc;
		}
	}
	free(buffer);
	*segment_pointer = next;
	return 0;
}


static int
add_program_table(int fd, disk_blockptr_t* table, int entries,
		  disk_blockptr_t* pointer, struct disk_info* info)
{
	void* block;
	int i;
	int rc;
	int offset;

	block = misc_malloc(PROGRAM_TABLE_BLOCK_SIZE);
	if (block == NULL)
		return -1;
	memset(block, 0, PROGRAM_TABLE_BLOCK_SIZE);
	memcpy(block, ZIPL_MAGIC, ZIPL_MAGIC_SIZE);
	offset = get_blockptr_size(info);
	for (i=0; i < entries; i++) {
		bootmap_store_blockptr(VOID_ADD(block, offset), &table[i],
				       info);
		offset += get_blockptr_size(info);
	}
	/* Write program table */
	rc = disk_write_block_aligned(fd, block, PROGRAM_TABLE_BLOCK_SIZE,
				      pointer, info);
	free(block);
	return rc;
}


struct component_entry {
	uint8_t data[23];
	uint8_t type;
	union {
		uint64_t load_address;
		uint64_t load_psw;
	} address;
} __attribute((packed));

typedef enum {
	component_execute = 0x01,
	component_load = 0x02
} component_type;

static void
create_component_entry(void* buffer, disk_blockptr_t* pointer,
		       component_type type, uint64_t address,
		       struct disk_info* info)
{
	struct component_entry* entry;

	entry = (struct component_entry*) buffer;
	memset(entry, 0, sizeof(struct component_entry));
	entry->type = (uint8_t) type;
	switch (type) {
		case component_load:
			bootmap_store_blockptr(&entry->data, pointer,
					       info);
			entry->address.load_address = address;
			break;
		case component_execute:
			entry->address.load_psw = address;
			break;
	}
}


struct component_header {
	uint8_t magic[4];
	uint8_t type;
	uint8_t reserved[27];
}  __attribute((packed));

typedef enum {
	component_header_ipl = 0x00,
	component_header_dump = 0x01
} component_header_type;

static void
create_component_header(void* buffer, component_header_type type)
{
	struct component_header* header;

	header = (struct component_header*) buffer;
	memset(header, 0, sizeof(struct component_header));
	memcpy(&header->magic, ZIPL_MAGIC, ZIPL_MAGIC_SIZE);
	header->type = (uint8_t) type;
}


struct component_loc {
	address_t addr;
	size_t size;
};

static int
add_component_file(int fd, const char* filename, address_t load_address,
		   off_t offset, void* component, int add_files,
		   struct disk_info* info, struct job_target_data* target,
		   struct component_loc *location)
{
	struct disk_info* file_info;
	struct component_loc loc;
	disk_blockptr_t segment;
	disk_blockptr_t* list;
	char* buffer;
	size_t size;
	blocknum_t count;
	int rc;
	int from;
	unsigned int to;

	if (add_files) {
		/* Read file to buffer */
		rc = misc_read_file(filename, &buffer, &size, 0);
		if (rc) {
			error_text("Could not read file '%s'", filename);
			return rc;
		}
		/* Ensure minimum size */
		if (size <= (size_t) offset) {
			error_reason("File '%s' is too small (has to be "
				     "greater than %ld bytes)", filename,
				     (long) offset);
			free(buffer);
			return -1;
		}
		/* Write buffer */
		count = disk_write_block_buffer(fd, buffer + offset,
					size - offset, &list, info);
		free(buffer);
		if (count == 0) {
			error_text("Could not write to bootmap file");
			return -1;
		}
	} else {
		/* Make sure file is on correct device */
		rc = disk_get_info_from_file(filename, target, &file_info);
		if (rc)
			return -1;
		if (file_info->device != info->device) {
			disk_free_info(file_info);
			error_reason("File is not on target device");
			return -1;
		}
		/* Get block list from existing file */
		count = disk_get_blocklist_from_file(filename, &list,
						     file_info);
		disk_free_info(file_info);
		if (count == 0)
			return -1;
		if (count * info->phy_block_size <= (size_t) offset) {
			error_reason("File '%s' is too small (has to be "
				     "greater than %ld bytes)", filename,
				     (long) offset);
			free(list);
			return -1;
		}
		if (offset > 0) {
			/* Shorten list by offset */
			from = offset / info->phy_block_size;
			count -= from;
			for (to=0; to < count; to++, from++)
				list[to] = list[from];
		}
	}
	/* Fill in component location */
	loc.addr = load_address;
	loc.size = count * info->phy_block_size;
	/* Try to compact list */
	count = disk_compact_blocklist(list, count, info);
	/* Write segment table */
	rc = add_segment_table(fd, list, count, &segment, info);
	free(list);
	if (rc == 0) {
		create_component_entry(component, &segment, component_load,
				       load_address, info);
		/* Return location if requested */
		if (location != NULL)
			*location = loc;
	}
	return rc;
}


static int
add_component_buffer(int fd, void* buffer, size_t size, address_t load_address,
		     void* component, struct disk_info* info,
		     struct component_loc *location)
{
	struct component_loc loc;
	disk_blockptr_t segment;
	disk_blockptr_t* list;
	blocknum_t count;
	int rc;

	/* Write buffer */
	count = disk_write_block_buffer(fd, buffer, size, &list, info);
	if (count == 0) {
		error_text("Could not write to bootmap file");
		return -1;
	}
	/* Fill in component location */
	loc.addr = load_address;
	loc.size = count * info->phy_block_size;
	/* Try to compact list */
	count = disk_compact_blocklist(list, count, info);
	/* Write segment table */
	rc = add_segment_table(fd, list, count, &segment, info);
	free(list);
	if (rc == 0) {
		create_component_entry(component, &segment, component_load,
				       load_address, info);
		/* Return location if requested */
		if (location != NULL)
			*location = loc;
	}
	return rc;
}


static void
print_components(const char *name[], struct component_loc *loc, int num)
{
	const char *padding = "................";
	int i;

	printf("  component address:\n");
	/* Process all available components */
	for (i = 0; i < num; i++) {
		if (loc[i].size == 0)
			continue;
		printf("    %s%s: 0x%08llx-0x%08llx\n", name[i],
		       &padding[strlen(name[i])],
		       (unsigned long long) loc[i].addr,
		       (unsigned long long) (loc[i].addr + loc[i].size - 1));
	}
}


static int
add_ipl_program(int fd, struct job_ipl_data* ipl, disk_blockptr_t* program,
		int verbose, int add_files, component_header_type type,
		struct disk_info* info, struct job_target_data* target)
{
	struct stat stats;
	void* table;
	void* stage3;
	size_t stage3_size;
	const char *comp_name[4] = {"kernel image", "parmline",
				    "initial ramdisk", "internal loader"};
	struct component_loc comp_loc[4];
	int rc;
	int offset;

	memset(comp_loc, 0, sizeof(comp_loc));
	table = misc_malloc(info->phy_block_size);
	if (table == NULL)
		return -1;
	memset(table, 0, info->phy_block_size);
	/* Create component table */
	offset = 0;
	/* Fill in component table header */
	create_component_header(VOID_ADD(table, offset), type);
	offset += sizeof(struct component_header);
	/* Add kernel image */
	if (verbose) {
		printf("  kernel image......: %s\n", ipl->image);
	}
	rc = add_component_file(fd, ipl->image, ipl->image_addr,
				KERNEL_HEADER_SIZE, VOID_ADD(table, offset),
				add_files, info, target, &comp_loc[0]);
	if (rc) {
		error_text("Could not add image file '%s'", ipl->image);
		free(table);
		return rc;
	}
	offset += sizeof(struct component_entry);
	if (ipl->parmline != NULL) {
		/* Add kernel parmline */
		if (verbose) {
			printf("  kernel parmline...: '%s'\n", ipl->parmline);
		}
		rc = add_component_buffer(fd, ipl->parmline,
					  strlen(ipl->parmline) + 1,
					  ipl->parm_addr,
					  VOID_ADD(table, offset),
					  info, &comp_loc[1]);
		if (rc) {
			error_text("Could not add parmline '%s'",
				   ipl->parmline);
			free(table);
			return -1;
		}
		offset += sizeof(struct component_entry);
	}
	stats.st_size = 0;
	if (ipl->ramdisk != NULL) {
		/* Add ramdisk */
		if (verbose) {
			printf("  initial ramdisk...: %s\n", ipl->ramdisk);
		}
		/* Get ramdisk file size */
		if (stat(ipl->ramdisk, &stats)) {
			error_reason(strerror(errno));
			error_text("Could not get information for file '%s'",
				   ipl->ramdisk);
			free(table);
			return -1;
		}
		rc = add_component_file(fd, ipl->ramdisk,
					ipl->ramdisk_addr, 0,
					VOID_ADD(table, offset),
					add_files, info, target, &comp_loc[2]);
		if (rc) {
			error_text("Could not add ramdisk '%s'",
				   ipl->ramdisk);
			free(table);
			return -1;
		}
		offset += sizeof(struct component_entry);
	}
	/* Add stage 3 loader to bootmap */
	rc = boot_get_stage3(&stage3, &stage3_size, ipl->parm_addr,
			     ipl->ramdisk_addr, (size_t) stats.st_size,
			     ipl->image_addr,
			     (info->type == disk_type_scsi) ? 0 : 1);
	if (rc) {
		free(table);
		return rc;
	}
	rc = add_component_buffer(fd, stage3, stage3_size,
				  DEFAULT_STAGE3_ADDRESS,
				  VOID_ADD(table, offset), info, &comp_loc[3]);
	free(stage3);
	if (rc) {
		error_text("Could not add stage 3 boot loader");
		free(table);
		return -1;
	}
	offset += sizeof(struct component_entry);
	if (verbose)
		print_components(comp_name, comp_loc, 4);
	/* Terminate component table */
	create_component_entry(VOID_ADD(table, offset), NULL,
			       component_execute,
			       ZIPL_STAGE3_ENTRY_ADDRESS | PSW_LOAD,
			       info);
	/* Write component table */
	rc = disk_write_block_aligned(fd, table, info->phy_block_size,
				      program, info);
	free(table);
	return rc;
}


static int
add_segment_program(int fd, struct job_segment_data* segment,
		    disk_blockptr_t* program, int verbose, int add_files,
		    component_header_type type, struct disk_info* info,
		    struct job_target_data* target)
{
	const char *comp_name[1] = {"segment file"};
	struct component_loc comp_loc[1];
	void* table;
	int offset;
	int rc;

	memset(comp_loc, 0, sizeof(comp_loc));
	table = misc_malloc(info->phy_block_size);
	if (table == NULL)
		return -1;
	memset(table, 0, info->phy_block_size);
	/* Create component table */
	offset = 0;
	/* Fill in component table header */
	create_component_header(VOID_ADD(table, offset), type);
	offset += sizeof(struct component_header);
	/* Add segment file */
	if (verbose) {
		printf("  segment file......: %s\n", segment->segment);
	}
	rc = add_component_file(fd, segment->segment, segment->segment_addr, 0,
				VOID_ADD(table, offset), add_files, info,
				target, &comp_loc[0]);
	if (rc) {
		error_text("Could not add segment file '%s'",
			   segment->segment);
		free(table);
		return rc;
	}
	offset += sizeof(struct component_entry);
	/* Print component addresses */
	if (verbose)
		print_components(comp_name, comp_loc, 1);
	/* Terminate component table */
	create_component_entry(VOID_ADD(table, offset), NULL,
			       component_execute, PSW_DISABLED_WAIT, info);
	/* Write component table */
	rc = disk_write_block_aligned(fd, table, info->phy_block_size,
				      program, info);
	free(table);
	return rc;
}


#define DUMP_PARAM_MAX_LEN	896

static char *
create_dump_fs_parmline(const char* parmline, const char* root_dev,
			int part_num, uint64_t mem, int max_cpus)
{
	char* result;

	result = misc_malloc(DUMP_PARAM_MAX_LEN);
	if (!result)
		return NULL;
	snprintf(result, DUMP_PARAM_MAX_LEN, "%s%sroot=%s dump_part=%d "
		 "dump_mem=%lld maxcpus=%d", parmline ? parmline : "",
		 parmline ? " " : "", root_dev, part_num,
		 (unsigned long long) mem, max_cpus);
	result[DUMP_PARAM_MAX_LEN - 1] = 0;
	return result;
}


static int
get_dump_fs_parmline(char* partition, char* parameters, uint64_t mem,
		     struct disk_info* target_info,
		     struct job_target_data* target, char** result)
{
	char* buffer;
	struct disk_info* info;
	int rc;

	/* Get information about partition */
	rc = disk_get_info(partition, target, &info);
	if (rc) {
		error_text("Could not get information for dump partition '%s'",
			   partition);
		return rc;
	}
	if (((info->type != disk_type_scsi) && (info->type != disk_type_virtio))
	 || (info->partnum == 0)) {
		error_reason("Device '%s' is not a SCSI partition",
			     partition);
		disk_free_info(info);
		return -1;
	}
	if (info->device != target_info->device) {
		error_reason("Target directory is not on same device as "
			     "'%s'", partition);
		disk_free_info(info);
		return -1;
	}
	buffer = create_dump_fs_parmline(parameters, "/dev/ram0", info->partnum,
					 mem, 1);
	disk_free_info(info);
	if (buffer == NULL)
		return -1;
	*result = buffer;
	return 0;
}


static int
add_dump_fs_program(int fd, struct job_dump_fs_data* dump_fs,
		    disk_blockptr_t* program, int verbose,
		    int add_files, component_header_type type,
		    struct disk_info* info, struct job_target_data* target)
{
	struct job_ipl_data ipl;
	int rc;

	/* Convert fs dump job to IPL job */
	ipl.image = dump_fs->image;
	ipl.image_addr = dump_fs->image_addr;
	ipl.ramdisk = dump_fs->ramdisk;
	ipl.ramdisk_addr = dump_fs->ramdisk_addr;

	/* Get file system dump parmline */
	rc = get_dump_fs_parmline(dump_fs->partition, dump_fs->parmline,
				  dump_fs->mem, info, target, &ipl.parmline);
	if (rc)
		return rc;
	ipl.parm_addr = dump_fs->parm_addr;
	return add_ipl_program(fd, &ipl, program, verbose, 1,
			       type, info, target);
}


/* Build a program table from job data and set pointer to program table
 * block upon success. */
static int
build_program_table(int fd, struct job_data* job, disk_blockptr_t* pointer,
		    struct disk_info* info)
{
	disk_blockptr_t* table;
	int entries;
	int i;
	int rc;

	entries = get_program_table_size(info);
	/* Get some memory for the program table */
	table = (disk_blockptr_t *) misc_malloc(sizeof(disk_blockptr_t) *
						entries);
	if (table == NULL)
		return -1;
	memset((void *) table, 0, sizeof(disk_blockptr_t) * entries);
	/* Add programs */
	switch (job->id) {
	case job_ipl:
		if (job->command_line)
			printf("Adding IPL section\n");
		else
			printf("Adding IPL section '%s' (default)\n",
			       job->name);
		rc = add_ipl_program(fd, &job->data.ipl, &table[0],
				     verbose || job->command_line,
				     job->add_files, component_header_ipl,
				     info, &job->target);
		break;
	case job_segment:
		if (job->command_line)
			printf("Adding segment load section\n");
		else
			printf("Adding segment load section '%s' (default)\n",
			       job->name);
		rc = add_segment_program(fd, &job->data.segment, &table[0],
					 verbose || job->command_line,
					 job->add_files, component_header_ipl,
					 info, &job->target);
		break;
	case job_dump_fs:
		if (job->command_line)
			printf("Adding fs-dump section\n");
		else
			printf("Adding fs-dump section '%s' (default)\n",
			       job->name);
		rc = add_dump_fs_program(fd, &job->data.dump_fs, &table[0],
					 verbose || job->command_line,
					 job->add_files, component_header_dump,
					 info, &job->target);
		break;
	case job_menu:
		printf("Building menu '%s'\n", job->name);
		rc = 0;
		for (i=0; i < job->data.menu.num; i++) {
			switch (job->data.menu.entry[i].id) {
			case job_ipl:
				printf("Adding #%d: IPL section '%s'%s\n",
				       job->data.menu.entry[i].pos,
				       job->data.menu.entry[i].name,
				       (job->data.menu.entry[i].pos ==
				        job->data.menu.default_pos) ?
						" (default)": "");
				rc = add_ipl_program(fd,
					&job->data.menu.entry[i].data.ipl,
					&table[job->data.menu.entry[i].pos],
					verbose || job->command_line,
					job->add_files,	component_header_ipl,
					info, &job->target);
				break;
			case job_dump_fs:
				printf("Adding #%d: fs-dump section '%s'%s\n",
				       job->data.menu.entry[i].pos,
				       job->data.menu.entry[i].name,
				       (job->data.menu.entry[i].pos ==
				        job->data.menu.default_pos) ?
						" (default)": "");
				rc = add_dump_fs_program(fd,
					&job->data.menu.entry[i].data.dump_fs,
					&table[job->data.menu.entry[i].pos],
					verbose || job->command_line,
					job->add_files, component_header_dump,
					info, &job->target);
				break;
			case job_print_usage:
			case job_print_version:
			case job_segment:
			case job_dump_partition:
			case job_mvdump:
			case job_menu:
			case job_ipl_tape:
				rc = -1;
				/* Should not happen */
				break;
			}
			if (rc)
				break;
		}
		if (rc == 0) {
			/* Set default entry */
			table[0] = table[job->data.menu.default_pos];
		}
		break;
	case job_print_usage:
	case job_print_version:
	case job_dump_partition:
	default:
		/* Should not happen */
		rc = -1;
		break;
	}
	if (rc == 0) {
		/* Add program table block */
		rc = add_program_table(fd, table, entries, pointer, info);
	}
	free(table);
	return rc;
}


/* Write block of zeroes to the bootmap file FD and store the resulting
 * block pointer in BLOCK. Return zero on success, non-zero otherwise. */
static int
write_empty_block(int fd, disk_blockptr_t* block, struct disk_info* info)
{
	void* buffer;
	int rc;

	buffer = misc_malloc(info->phy_block_size);
	if (buffer == NULL)
		return -1;
	memset(buffer, 0, info->phy_block_size);
	rc = disk_write_block_aligned(fd, buffer, info->phy_block_size, block,
				      info);
	free(buffer);
	return rc;
}


int
bootmap_create(struct job_data* job, disk_blockptr_t* program_table,
	       disk_blockptr_t** stage2_list, blocknum_t* stage2_count,
	       char** new_device, struct disk_info** new_info)
{
	struct disk_info* info;
	char* device;
	char* filename;
	char *mapname;
	void* stage2_data;
	size_t stage2_size;
	int fd;
	int rc;

	/* Get full path of bootmap file */
	filename = misc_make_path(job->target.bootmap_dir,
			BOOTMAP_TEMPLATE_FILENAME);
	if (filename == NULL)
		return -1;
	/* Create temporary bootmap file */
	fd = mkstemp(filename);
	if (fd == -1) {
		error_reason(strerror(errno));
		error_text("Could not create file '%s':", filename);
		free(filename);
		return -1;
	}
	/* Retrieve target device information. Note that we have to
	 * call disk_get_info_from_file() to also get the file system
	 * block size. */
	rc = disk_get_info_from_file(filename, &job->target, &info);
	if (rc) {
		close(fd);
		free(filename);
		return -1;
	}
	/* Check for supported disk and driver types */
	if ((info->source == source_auto) && (info->type == disk_type_diag)) {
		error_reason("Unsupported disk type (%s)",
			     disk_get_type_name(info->type));
		disk_free_info(info);
		close(fd);
		free(filename);
		return -1;
	}
	if (verbose) {
		printf("Target device information\n");
		disk_print_info(info);
	}
	rc = misc_temp_dev(info->device, 1, &device);
	if (rc) {
		disk_free_info(info);
		close(fd);
		free(filename);
		return -1;
	}
	/* Check configuration number limits */
	if (job->id == job_menu) {
		rc = check_menu_positions(&job->data.menu, job->name, info);
		if (rc) {
			misc_free_temp_dev(device);
			disk_free_info(info);
			close(fd);
			free(filename);
			return rc;
		}
	}
	printf("Building bootmap in '%s'%s\n", job->target.bootmap_dir,
	       job->add_files ? " (files will be added to bootmap file)" :
	       "");
	/* Write bootmap header */
	rc = misc_write(fd, header_text, sizeof(header_text));
	if (rc) {
		error_text("Could not write to file '%s'", filename);
		misc_free_temp_dev(device);
		disk_free_info(info);
		close(fd);
		free(filename);
		return rc;
	}
	/* Write empty block to be read in place of holes in files */
	rc = write_empty_block(fd, &empty_block, info);
	if (rc) {
		error_text("Could not write to file '%s'", filename);
		misc_free_temp_dev(device);
		disk_free_info(info);
		close(fd);
		free(filename);
		return rc;
	}
	/* Build program table */
	rc = build_program_table(fd, job, program_table, info);
	if (rc)
		return rc;
	/* Add stage 2 loader to bootmap if necessary */
	switch (info->type) {
	case disk_type_fba:
		rc = boot_get_fba_stage2(&stage2_data, &stage2_size, job);
		if (rc) {
			misc_free_temp_dev(device);
			disk_free_info(info);
			close(fd);
			free(filename);
			return rc;
		}
		*stage2_count = disk_write_block_buffer(fd, stage2_data,
						stage2_size, stage2_list,
						info);
		free(stage2_data);
		if (*stage2_count == 0) {
			error_text("Could not write to file '%s'", filename);
			misc_free_temp_dev(device);
			disk_free_info(info);
			close(fd);
			free(filename);
			return -1;
		}
		break;
	case disk_type_eckd_classic:
		rc = boot_get_eckd_stage2(&stage2_data, &stage2_size, job);
		if (rc) {
			misc_free_temp_dev(device);
			disk_free_info(info);
			close(fd);
			free(filename);
			return rc;
		}
		*stage2_count = disk_write_block_buffer(fd, stage2_data,
						stage2_size, stage2_list,
						info);
		free(stage2_data);
		if (*stage2_count == 0) {
			error_text("Could not write to file '%s'", filename);
			misc_free_temp_dev(device);
			disk_free_info(info);
			close(fd);
			free(filename);
			return -1;
		}
		break;
	case disk_type_virtio:
		rc = boot_get_virtio_stage2(&stage2_data, &stage2_size, job);
		if (rc) {
			misc_free_temp_dev(device);
			disk_free_info(info);
			close(fd);
			free(filename);
			return rc;
		}
		*stage2_count = disk_write_block_buffer(fd, stage2_data,
						stage2_size, stage2_list,
						info);
		free(stage2_data);
		if (*stage2_count == 0) {
			error_text("Could not write to file '%s'", filename);
			misc_free_temp_dev(device);
			disk_free_info(info);
			close(fd);
			free(filename);
			return -1;
		}
		break;
	case disk_type_scsi:
	case disk_type_eckd_compatible:
	case disk_type_diag:
		*stage2_list = NULL;
		*stage2_count = 0;
		break;
	}
	close(fd);
	if (dry_run) {
		if (remove(filename) == -1)
			fprintf(stderr, "Warning: could not remove temporary "
				"file %s!\n", filename);
	} else {
		/* Rename to final bootmap name */
		mapname = misc_make_path(job->target.bootmap_dir,
				BOOTMAP_FILENAME);
		if (mapname == NULL) {
			misc_free_temp_dev(device);
			disk_free_info(info);
			free(filename);
			return -1;
		}
		rc = rename(filename, mapname);
		if (rc) {
			error_reason(strerror(errno));
			error_text("Could not overwrite file '%s':", mapname);
			misc_free_temp_dev(device);
			disk_free_info(info);
			free(mapname);
			free(filename);
			return rc;
		}
		free(mapname);
	}
	*new_device = device;
	*new_info = info;
	free(filename);
	return rc;
}
