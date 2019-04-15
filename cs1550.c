/*
Create ".disk" file:
dd bs=1K count=5K if=/dev/zero of=.disk

COMPILE:
make

RUN:Inside THOTH
./cs1550 -d testmount

RUN:Outside THOTH
./cs1550 testmount

UNMOUNT:
fusermount -u testmount
*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

// Outlines the disk and blocks
#define DISK_SIZE 5242880 // 5242880 Bytes = 5 MB
#define	BLOCK_SIZE 512
static const char * DISK_FILE_NAME = ".disk";

// Number of blocks that can fit into the disk minus the tracking bitmap
#define BLOCK_COUNT (DISK_SIZE / BLOCK_SIZE - ((DISK_SIZE - 1) / (sizeof(long) * BLOCK_SIZE * BLOCK_SIZE) + 1))

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE)
#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

//The attribute packed means to not align these things
struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ;

struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
} ;

// typedef renames the directory to the second argument
typedef struct cs1550_directory_entry directory_entry;
typedef struct cs1550_root_directory root_directory;

struct cs1550_disk_block
{
	// Pointer to the next block in the file
	long nextBlock;
	// Each block has data
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block disk_block;
typedef struct cs1550_disk disk;
// Create a bitmap to keep track of open/free block

// There are a total of 512b maximum for the bit map
// The LAST BLOCKS should contain the bit map

// DATA BLOCK
	// Type 1: Root directory
		// - Stored at the first block in disk
		// - Bitmap should always be TAKEN (1) for block 0
	// Type 2: Subdirectory (directory_entry)
	// Type 3: File (file_directory)
		// - When type 2 or 3 created, check bitmap for a FREE (0) block
		// - Then add it to the disk and set the bitmap to 1 for that block


// =========== HELP FUNCTIONS ==============
static void * get_disk_block(int blockNum, FILE * disk) {
	printf("Return the disk block at %d\n", blockNum);
	void * block = malloc(BLOCK_SIZE);
	// From the beginning of the disk (SEEK_SET) seek to the correct block num on the disk
	fseek(disk, blockNum * BLOCK_SIZE, SEEK_SET);
	// Read into the block the entire block
	fread(block, BLOCK_SIZE, 1, disk);
	return block;
}

static root_directory * get_root_directory(FILE * disk) {
	return (root_directory *) get_disk_block(0, disk);
}

static int write_to_disk(void * block, int blockNum, FILE * disk) {
	fseek(disk, blockNum * BLOCK_SIZE, SEEK_SET);
	fwrite(block, BLOCK_SIZE, 1, disk);
}
// =========================================

/*
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	printf("=======================\n");
	printf("readdir() debug messages:\n");
	(void) offset;
	(void) fi;

	printf("Path %s\n", path);
	char directory[MAX_FILENAME + 1];
	char filename[MAX_FILENAME + 1];
	char extension[MAX_EXTENSION + 1];
	// Ensure the \0 ending character
	strcpy(directory, "");
	strcpy(filename, "");
	strcpy(extension, "");

	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
	printf("directory: %s\n", directory);
	printf("filename: %s\n", filename);
	printf("extension: %s\n", extension);

	if (strcmp(path, "/") == 0) { // If you are reading from the root...
		if (strcmp(directory, "") == 0) {
			printf("Directory given is %s\n", directory);
		}
	} else { // If you are reading from a subdirectory...

	}

	FILE * disk = fopen(DISK_FILE_NAME, "rb+");
	if (disk == NULL) {
		printf("ERROR: Could not open the file %s\n", MAX_FILENAME);
	}
	// Get the root from the disk
	root_directory * root = get_root_directory(disk);

	//the filler function allows us to add entries to the listing
	//read the fuse.h file for a description (in the ../include dir)
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	int i;
	for (i = 0; i < root->nDirectories; i++) {
		char * name = root->directories[i].dname;
		int start = root->directories[i].nStartBlock;
		filler(buf, name, NULL, 0);
		printf("Directory %d name is %s and starts at %d\n", i, name, start);
	}

	//printf("The number of directories is %d\n", root->nDirectories);

	free(root);
	fclose(disk);

	/*
	//add the user stuff (subdirs or files)
	//the +1 skips the leading '/' on the filenames
	filler(buf, newpath + 1, NULL, 0);
	*/
	printf("=======================\n");
	return 0;
}

/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not.
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int cs1550_getattr(const char *path, struct stat *stbuf)
{
	printf("=======================\n");
	printf("getattr() debug messages:\n");
	int res = 0;
	memset(stbuf, 0, sizeof(struct stat));
	// Info about the "struct stat": http://pubs.opengroup.org/onlinepubs/007908799/xsh/sysstat.h.html

	//is path the root dir?
	if (strcmp(path, "/") == 0) {
		// Set the "file mode"
		// F_IFDIR -> directory file type
		stbuf->st_mode = S_IFDIR | 0755;
		// Set the number of links to this file from other files
		// 2 because __?__
		stbuf->st_nlink = 2;
		res = 0;
	} else {
		printf("Path %s\n", path);
		char directory[MAX_FILENAME + 1];
		char filename[MAX_FILENAME + 1];
		char extension[MAX_FILENAME + 1];
		// Ensure the \0 ending character
		strcpy(directory, "");
		strcpy(filename, "");
		strcpy(extension, "");

		sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
		printf("directory: %s\n", directory);
		printf("filename: %s\n", filename);
		printf("extension: %s\n", extension);

		// Check for too large file names and extensions
		if (strlen(directory) > MAX_FILENAME || strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION) {
			res = -ENAMETOOLONG;
			return res;
		}

		// The system is a two-level system.  Directory required
		if (strcmp(directory, "") == 0) { // If no directory is given...
			return -ENOENT;
		} else { // Check to see if directory exists
			FILE * disk = fopen(DISK_FILE_NAME, "rb+");
			root_directory * root = get_root_directory(disk);
			int i;
			for (i = 0; i < root->nDirectories; i++) {
				printf("Looking for directory %s, comparing to %s\n", directory, root->directories[i].dname);
				if (strcmp(directory, root->directories[i].dname) == 0) { // Directory name already exists...
					i = -1;
					break;
				}
			}
			if (i != -1) { // Directory was not found
				res = -ENOENT; // DO NOT return here, disk still open
			} else {

				if (strcmp(filename, "") != 0) { // We look for a file
					printf("NOT YET, looking for filename %s\n", filename);
					//Check if name is a regular file
					/*
						//regular file, probably want to be read and write
						stbuf->st_mode = S_IFREG | 0666;
						stbuf->st_nlink = 1; //file links
						stbuf->st_size = 0; //file size - make sure you replace with real size!
						res = 0; // no error
					*/
				} else { // We aren't looking for a file, just a directory...
					stbuf->st_mode = S_IFDIR | 0755;
          stbuf->st_nlink = 2;
          res = 0; //no error
				}

			}
			free(root);
			fclose(disk);
		}
	}
	printf("=======================\n");
	return res;
}

/*
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode)
{
	(void) mode;
	(void) path;

	printf("=======================\n");
	printf("mkdir() debug messages:\n");

	FILE * disk = fopen(DISK_FILE_NAME, "rb+");
	char * new_directory = strtok(path, "/");
	root_directory * root = get_root_directory(disk);

	// Resave the root to the first block (0) in disk
	int currDirNum = root->nDirectories;

	// Give the new directory in the root a new name
	strcpy(root->directories[currDirNum].dname, new_directory);
	// Increase the number of directories
	root->nDirectories++;
	write_to_disk((void *) root, 0, disk);

	printf("Created the directory %s\n", new_directory);

	free(root);
	fclose(disk);

	// if(strlen(new_directory_name) > MAX_FILENAME){ // Check the length of the new directory name
	// 	return -ENAMETOOLONG;
	// } else if(sub_directory && sub_directory[0]){ //The user passed in a sub directory; this is illegal in our two-level file system
	// 					      //because the second level should only be files.  Return that permission was denied.
	// 	return -EPERM;
	// }
	//
	// printf("Directory is %s\n", directory);
	// printf("Subdirectory is %s\n", sub_directory);

	printf("=======================\n");
	return 0;
}

/*
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;
    return 0;
}

/*
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
{
	(void) path;
	(void) mode;
	(void) dev;
	return 0;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
    (void) path;

    return 0;
}

/*
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//read in data
	//set size and return, or error

	size = 0;

	return size;
}

/*
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char *path, const char *buf, size_t size,
			  off_t offset, struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//write data
	//set size (should be same as input) and return, or error

	return size;
}

/******************************************************************************
 *
 *  DO NOT MODIFY ANYTHING BELOW THIS LINE
 *
 *****************************************************************************/

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int cs1550_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

  return 0;
}


/*
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int cs1550_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
    .getattr	= cs1550_getattr,
    .readdir	= cs1550_readdir,
    .mkdir	= cs1550_mkdir,
	.rmdir = cs1550_rmdir,
    .read	= cs1550_read,
    .write	= cs1550_write,
	.mknod	= cs1550_mknod,
	.unlink = cs1550_unlink,
	.truncate = cs1550_truncate,
	.flush = cs1550_flush,
	.open	= cs1550_open,
};

//Don't change this.
int main(int argc, char *argv[])
{
	printf("test %d\n", 0);
	return fuse_main(argc, argv, &hello_oper, NULL);
}
