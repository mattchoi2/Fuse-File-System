# Fuse-File-System
CS1550 Project 4
Matthew Choi

# Notes

cd example/

Only write code in "cs1550.c"

RUN Fuse
1. Compile the file system with "make"
2. Creates cs1550 & hello executables
3. Make a directory "mkdir testmount"
4. Run with "./hello testmount"
5. A program now runs in the background
  - It waits for file system operations
  - The program will only manipulate the "testmount" directory
  - Ex: the "ls" command will only show files created by hello.c
6. Unmount with "fusermount -u testmount"

7. Run natively with "./hello -d testmount"
  - Open another terminal and go to the examples folder
  - You can run normal file system commands (Ex: ls) in the new window and the native terminal will show which operations are done
  - If you use printf's, check the native THOTH to see them printed

Project 4

  - fuse_main() implements the file system in USER SPACE
    - Normally done thru kernel
    - takes in a struct for fuse_operations
  - fuse_operations will take a unix command and maps it to code to handle it

Structs Used

1. root_directory
  - Contains an array of entries for directories with name & startBlock
  - This is level 1
2. file_directory
  - An array of file entries
  - Files have a name, extension, size, startBlock
3.  disk_block
  - An array of bytes that store the DATA
  
