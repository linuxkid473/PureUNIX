#ifndef EXT2_FILE_H
#define EXT2_FILE_H

#include <pureunix/types.h>

/* Read the entire regular file identified by inode ino into a kmalloc'd buffer.
   On success, *out_data points to the buffer (caller must kfree it),
   *out_size holds the file size, and the function returns 0.
   Returns -1 on error (bad inode type, I/O error, alloc failure, etc.). */
int ext2_read_file_ino(uint32_t ino, uint8_t **out_data, size_t *out_size);

#endif
