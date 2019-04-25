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
#define BIT_MAP_SIZE 1280 // Thx Piazza
static const char * DISK_FILE_NAME = ".disk";

// Number of blocks that can fit into the disk minus the tracking bitmap
#define BLOCK_COUNT (DISK_SIZE / BLOCK_SIZE - ((DISK_SIZE - 1) / (sizeof(long) * BLOCK_SIZE * BLOCK_SIZE) + 1))

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE)
#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

// Used in bitmap management
#define ROOT_BIT_OFFSET 1
#define BITMAP_BIT_OFFSET 3 // The blocks where the bitmaps are stored

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
	// Each block has data
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block disk_block;
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
static void * get_disk_block(long blockNum, FILE * disk) {
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

static directory_entry * get_directory(long blockNum, FILE * disk) {
	return (directory_entry *) get_disk_block(blockNum, disk);
}

static void write_to_disk(void * block, long blockNum, FILE * disk) {
	fseek(disk, blockNum * BLOCK_SIZE, SEEK_SET);
	fwrite(block, BLOCK_SIZE, 1, disk);
}

static directory_entry * get_directory_from_root(char * directoryName, long * dirBlock) {
	*dirBlock = 0;
	FILE * disk = fopen(DISK_FILE_NAME, "rb+");
	root_directory * root = get_root_directory(disk);
	directory_entry * dir = NULL;
	int i;
	for (i = 0; i < root->nDirectories; i++) {
		if (strcmp(root->directories[i].dname, directoryName) == 0) {
			printf("The directory %s is at block %ld\n", root->directories[i].dname, root->directories[i].nStartBlock);
			*dirBlock = root->directories[i].nStartBlock;
		}
	}
	if (*dirBlock != 0) { dir = get_directory(*dirBlock, disk); }
	free(root);
	fclose(disk);
	return dir;
}

static int write_file_to_disk(long blockNum, const char * buf, int size, int offset, FILE * disk) {
	int res = 0;
	// Go to the block to write, plus offset
	fseek(disk, (blockNum * BLOCK_SIZE) + offset, SEEK_SET);
	printf("Writing buffer starting at block %d + %d of size %d\n", blockNum, offset, size);
	// Write the buffer to the disk
	fwrite(buf, size, 1, disk);
	return res;
}

// Modify and return number n at position p with bit value b
static int modifyBit(char n, int p, int b) {
    char mask = 1 << p;
    return (n & ~mask) | ((b << p) & mask);
}

// Allows the mkdir() new directory to be assigned a block to hold all file links
static long find_open_block(FILE * disk) {
	// Accounts for the root block, which will never be open
	fseek(disk, -(BIT_MAP_SIZE), SEEK_END);
	char byte;
	long byteCount = 0;
	long bitCount = 0;
	while(fread(&byte, 1, 1, disk) == 1) { // Read byte by byte
		int i;
		for (i = 0; i < 8; i++) { // Get the size in BITS
			//printf("At byte %d and bit %d\n", byteCount, bitCount);
			// The first 1 bit is for the root
			if (byteCount == 0 && i < ROOT_BIT_OFFSET) { printf("Avoid at root\n"); bitCount ++; continue; }
			// The last 3 bits are for the bit map!
			if (byteCount == BIT_MAP_SIZE && 7 - i <= BITMAP_BIT_OFFSET) {  printf("Avoid at bitmap blocks\n"); bitCount ++; continue; }
			int bit = (byte >> i) & 0x1; // Get the ith bit in the byte
			if (bit == 0) {
				printf("Open block found at block number %d\n", bitCount);
				byte = modifyBit(byte, i, 1); // Write a 1 into the byte at position i
				//printf("New byte is %d\n", byte);
				fseek(disk, -1, SEEK_CUR); // Go back to the current byte
				fwrite(&byte, 1, 1, disk); // Now write the new byte bitmap there!
				return bitCount;
			}
			bitCount++;
		}
		byteCount++;
	}
	return -1; // If code makes it here, the disk is FULL
}

// =========================================

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
	printf("=======================\n");
	printf("read() debug messages:\n");
	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//read in data
	//set size and return, or error
	int res = 0;
	char * directory;
	char * filename;
	char * extension;
	char pathCopy[strlen(path)];
	strcpy(pathCopy, path);
	printf("Path %s\n", path);
	directory = strtok(pathCopy, "/");
	filename = strtok(NULL, "."); //NULL indicates to continue where strtok left off at
	extension = strtok(NULL, ".");
	FILE * disk = fopen(DISK_FILE_NAME, "rb+");
	root_directory * root = get_root_directory(disk);
	directory_entry * dir = NULL;
	long dirBlock = 0;
	size_t filesize = 0;
	int i;
	// Check to see if file requested is a directory
	dir = get_directory_from_root(filename, &dirBlock);
	if (dir != NULL) {
		// You cannot request a file as a directory
		return -EISDIR;
	} else {
		// Check for the directory existing
		dir = get_directory_from_root(directory, &dirBlock);
		printf("Good! Beginning to search thru directory %s with %d files\n", directory, dir->nFiles);
		for (i = 0; i < dir->nFiles; i++) {
			printf("Looking for file. Comparing %s to %s\n", filename, dir->files[i].fname);
			if (strcmp(dir->files[i].fname, filename) == 0) {
				filesize = dir->files[i].fsize;
				printf("The filesize is %d and offset is %d\n", filesize, offset);
				if (filesize > offset) {
					// Seek to the beginning of the file
					fseek(disk, dir->files[i].nStartBlock * BLOCK_SIZE, SEEK_SET);
					int j;
					int blocks2read = (filesize / BLOCK_SIZE) + 1; // Ensures a round-up
					for (j = 0; j < blocks2read; j++) {
						disk_block * curr_block = malloc(BLOCK_SIZE);
						fread(curr_block, BLOCK_SIZE, 1, disk);
						int bytes = 0;
						while (bytes < MAX_DATA_IN_BLOCK) {
							// Read the file into buf[] to read
							buf[bytes] = curr_block->data[bytes];
							bytes ++;
						}
						printf("Read %d bytes\n", bytes);
						printf("Reading block %d of %d starting at block %ld\n", j, blocks2read, dir->files[i].nStartBlock);
						free(curr_block);
					}
				} else {
					res = -1;
				}
			}
			break;
		}
	}
	printf("=======================\n");
	free(dir);
	free(root);
	fclose(disk);

	if (res != 0) { return res; }
	return filesize - offset;
}

/*
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char *path, const char *buf, size_t size,
			  off_t offset, struct fuse_file_info *fi)
{
	printf("=======================\n");
	printf("write() debug messages:\n");
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;
	int res = 0;
	char * directory;
	char * filename;
	char * extension;
	char pathCopy[strlen(path)];
	strcpy(pathCopy, path);
	printf("Path %s\n", path);
	directory = strtok(pathCopy, "/");
	filename = strtok(NULL, "."); //NULL indicates to continue where strtok left off at
	extension = strtok(NULL, ".");
	FILE * disk = fopen(DISK_FILE_NAME, "rb+");
	root_directory * root = get_root_directory(disk);
	directory_entry * dir = NULL;
	long dirBlock = 0;
	int i;
	// Check for the directory existing
	dir = get_directory_from_root(directory, &dirBlock);
	// Check for the file existing
	for (i = 0; i < dir->nFiles; i++) {
		if (strcmp(dir->files[i].fname, filename) == 0) {
			if (offset > size) { // Make sure that we don't go over the limit
				res = -EFBIG;
			} else {
				// Assign the size of the new block
				dir->files[i].fsize = size;
				dir->files[i].nStartBlock = find_open_block(disk);
				printf("The size of the new file %s is %d (%d as a strlen)\n", dir->files[i].fname, dir->files[i].fsize, strlen(buf));
				// Once we update the directory to have the new cs1550_file_directory, we can update the directory
				write_to_disk((void *) dir, dirBlock, disk);
				// We also have to write the file's data to disk at the recorded block!
				res = write_file_to_disk(dir->files[i].nStartBlock, buf, size, offset, disk);
			}
			break;
		}
	}
	fclose(disk);
	free(root);
	free(dir);
	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//write data
	//set size (should be same as input) and return, or error
	printf("=======================\n");
	if (res == 0) { return size; }
	return res;
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
	printf("=======================\n");
	printf("mknod() debug messages:\n");
	int res = 0;
	char * directory;
	char * filename;
	char * extension;
	char pathCopy[strlen(path)];
	strcpy(pathCopy, path);
	printf("Path %s\n", path);
	directory = strtok(pathCopy, "/");
	filename = strtok(NULL, "."); //NULL indicates to continue where strtok left off at
	extension = strtok(NULL, ".");

	if (filename == NULL) { // You cannot have a file in root!
		return -EPERM; // No permissions to make a file in root >:(
	}
	FILE * disk = fopen(DISK_FILE_NAME, "rb+");

	// Check for too large file names and extensions
	if (strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION) {
		return -ENAMETOOLONG;
	}
	root_directory * root = get_root_directory(disk);
	directory_entry * dir = NULL;
	long dirBlock = 0;
	int i;
	// Get the directory
	dir = get_directory_from_root(directory, &dirBlock);
	// Check for duplicate files!
	for (i = 0; i < dir->nFiles; i++) {
		// printf("Comparing existing file %s to new file %s\n", dir->files[i].fname, filename);
		if (strcmp(dir->files[i].fname, filename) == 0) {
			printf("MATCH!\n");
			i = -1;
			break;
		}
	}
	// Create the file once we know its NOT in root and NOT a dupe
	if (i != -1) {
		strcpy(dir->files[dir->nFiles].fname, filename);
		strcpy(dir->files[dir->nFiles].fext, extension);
		dir->files[dir->nFiles].nStartBlock = find_open_block(disk);
		printf("FILE %s with extension %s created at open block %d\n", dir->files[dir->nFiles].fname, dir->files[dir->nFiles].fext, dir->files[dir->nFiles].nStartBlock);
		dir->nFiles = dir->nFiles + 1;
		// Write the updated directory back to disk
		write_to_disk((void *) dir, dirBlock, disk);
	} else {
		res = -EEXIST;
	}
	printf("=======================\n");
	fclose(disk);
	free(root);
	free(dir);
	return res;
}

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

	FILE * disk = fopen(DISK_FILE_NAME, "rb+");
	if (disk == NULL) {
		printf("ERROR: Could not open the disk %s\n", MAX_FILENAME);
	}
	// Get the root from the disk
	root_directory * root = get_root_directory(disk);

	//the filler function allows us to add entries to the listing
	//read the fuse.h file for a description (in the ../include dir)
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	if (strcmp(path, "/") == 0) { // If you are reading from the root ONLY...
		int i;
		for (i = 0; i < root->nDirectories; i++) {
			char * name = root->directories[i].dname;
			int start = root->directories[i].nStartBlock;
			filler(buf, name, NULL, 0);
			printf("Directory %d name is %s and starts at %d\n", i, name, start);
		}
	} else if (strcmp(directory, "") != 0) { // If you are reading from a subdirectory...
		long dirBlock = 0;
		directory_entry * dir = get_directory_from_root(directory, &dirBlock);
		int i;
		for (i = 0; i < dir->nFiles; i++) {
			filler(buf, strcat(strcat(dir->files[i].fname, "."), dir->files[i].fext), NULL, 0);
		}
	}

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
			long dirBlock = 0;
			directory_entry * dir = get_directory_from_root(directory, &dirBlock);
			if (dir == NULL) { // Directory was not found
				res = -ENOENT; // DO NOT return here, disk still open
			} else {
				if (strcmp(filename, "") != 0) { // We look for a file
					int i;
					for (i = 0; i < dir->nFiles; i++) {
						printf("Comparing existing file %s to the file %s out of %d total files\n", dir->files[i].fname, filename, dir->nFiles);
						if (strcmp(dir->files[i].fname, filename) == 0) {
							printf("FILE %s FOUND!\n", filename);
							i = -1;
							stbuf->st_mode = S_IFREG | 0666;
							stbuf->st_nlink = 1; // file links
							stbuf->st_size = dir->files[i].fsize; // file size
							break;
						}
					}
					if (i != -1) { res = -ENOENT; }
				} else { // We aren't looking for a file, just a directory...
					stbuf->st_mode = S_IFDIR | 0755;
          stbuf->st_nlink = 2;
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
	printf("=======================\n");
	printf("mkdir() debug messages:\n");
	(void) mode;
	(void) path;
	int res = 0;

	char * new_directory = strtok(path, "/");
	if (strtok(NULL, "/") != NULL) { // Means the user planned to make a second-level directory (big NO NO!)
		return -EPERM; // Message sent by first-level directory gang
	}
	if (strlen(new_directory) > MAX_FILENAME) { // If the file name was too long...
		return -ENAMETOOLONG;
	}

	FILE * disk = fopen(DISK_FILE_NAME, "rb+");
	root_directory * root = get_root_directory(disk);
	if (root->nDirectories >= MAX_DIRS_IN_ROOT) { // When the directories in the root are full
    res = -ENOSPC;
  } else { // Otherwise go ahead and make a new directory in root
		// Resave the root to the first block (0) in disk
		int currDirNum = root->nDirectories;
		long startBlock = find_open_block(disk);
		if (startBlock == -1) { // Means the disk is FULL!
			res = -ENOSPC;
		} else {
			// Give the new directory in the root a new name
			strcpy(root->directories[currDirNum].dname, new_directory);
			root->directories[currDirNum].nStartBlock = startBlock;
			// Increase the number of directories
			root->nDirectories++;
			printf("WRITING ROOT TO DISK WITH NEW DIRECTORY\n");
			printf("New dir: name %s at block %d\n", root->directories[currDirNum].dname, root->directories[currDirNum].nStartBlock);
			write_to_disk((void *) root, 0, disk);
		}
	}
	free(root);
	fclose(disk);
	printf("=======================\n");
	return res;
}

/*
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;
	int res = 0;
  return res;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
    (void) path;

    return 0;
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
