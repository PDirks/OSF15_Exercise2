#include "../include/block_store.h"

// Overriding these will probably break it since I'm not testing it that much
// it probably won't go crazy so long as the sizes are reasonable and powers of two
// So just don't touch it unless you want to debug it
#define BLOCK_COUNT 65536
#define BLOCK_SIZE 1024
#define FBM_SIZE ((BLOCK_COUNT >> 3) / BLOCK_SIZE)
#if (((FBM_SIZE * BLOCK_SIZE) << 3) != BLOCK_COUNT)
    #error "BLOCK MATH DIDN'T CHECK OUT"
#endif

// Handy macro, does what it says on the tin
#define BLOCKID_VALID(id) ((id > (FBM_SIZE - 1)) && (id < BLOCK_COUNT))

// TODO: Implement, comment, param check
// It needs to read count bytes from fd into buffer
// Probably a good idea to handle EINTR
// Maybe do a block-based read? It's more efficient, but it's more complex
// Return number of bytes read if it all worked (should always be count)
//  and 0 on error (can only really be a major disaster file error)
// There should be POSIX stuff for this somewhere
size_t utility_read_file(const int fd, uint8_t *buffer, const size_t count);

// TODO: implement, comment, param check
// Exactly the same as read, but we're writing count from buffer to fd
size_t utility_write_file(const int fd, const uint8_t *buffer, const size_t count);

// The magical glue that holds it all together
struct block_store {
    bitmap_t *dbm;
    // Why not. It'll track if we have unsaved changes.
    // It'll be handly in V2
    bitmap_t *fbm;
    uint8_t *data_blocks;
    // add an fd for V2 for better disk stuff
};

/*
 * PURPOSE: Create and allocate in memory a blockstore data type. Allocate all data members in the struct, as well as set all values to 0
 * INPUTS:
 *      void
 * RETURN:
 *      block_store pointer, with all data members fully allocated
 **/
block_store_t *block_store_create() {
    block_store_t *bs = malloc(sizeof(block_store_t));
    if (bs) {
    	// begin allocating block_store data members, starting with free block map bitmap
        bs->fbm = bitmap_create(BLOCK_COUNT);
        if (bs->fbm) {
            bs->dbm = bitmap_create(BLOCK_COUNT);
            if (bs->dbm) {
                // Eh, calloc, why not (technically a security risk if we don't)
                bs->data_blocks = calloc(BLOCK_SIZE, BLOCK_COUNT - FBM_SIZE);
                if (bs->data_blocks) {
                	// iterate through free and dirty blockmaps, setting everything to 0
                    for (size_t idx = 0; idx < FBM_SIZE; ++idx) {
                        bitmap_set(bs->fbm, idx);
                        bitmap_set(bs->dbm, idx);
                    }
                    block_store_errno = BS_OK;
                    return bs;
                }// end bs->data_blocks check

			    // malloc failure, clear out dbm
                bitmap_destroy(bs->dbm);
                
            }// end bs->dbm check

		    // malloc failure, clear out fbm
            bitmap_destroy(bs->fbm);

        }// end bs->fbm check

        free(bs);

    }// end bs check
    // malloc failure, throw error
    block_store_errno = BS_MEMORY;
    return NULL;
}// end block_store_create

/*
 * PURPOSE: free all members associated with a give block store
 * INPUTS:
 *      bs, the target block store to free
 * RETURN:
 *      void
 **/
void block_store_destroy(block_store_t *const bs) {
    if (bs) {
    	// call bitmap_destory on all members of inputted block store
        bitmap_destroy(bs->fbm);
        bs->fbm = NULL;
        bitmap_destroy(bs->dbm);
        bs->dbm = NULL;
        free(bs->data_blocks);
        bs->data_blocks = NULL;
        free(bs);
        block_store_errno = BS_OK;
        return;
    }
    block_store_errno = BS_PARAM;
}// end block_store_destroy

/*
 * PURPOSE: Grab a free block from the free block map and note that it is being used
 * INPUTS:
 *      bs, the block store we'll search through to get a free block
 * RETURN:
 *      if success, returns memory adress of our free block
 *		else, return 0
 **/
size_t block_store_allocate(block_store_t *const bs) {
    if (bs && bs->fbm) {
    	// grab a free block from the free block map
        size_t free_block = bitmap_ffz(bs->fbm);
        if (free_block != SIZE_MAX) {
        	// note in free block memory that our grabbed block is now used
            bitmap_set(bs->fbm, free_block);
            // not going to mark dbm because there's no change (yet)
            return free_block;
        }
        block_store_errno = BS_FULL;
        return 0;
    }
    block_store_errno = BS_PARAM;
    return 0;
}// end block_store_allocate

/*
    // V2
    size_t block_store_request(block_store_t *const bs, size_t block_id) {
    if (bs && bs->fbm && BLOCKID_VALID(block_id)) {
        if (!bitmap_test(bs->fbm, block_id)) {
            bitmap_set(bs->fbm, block_id);
            block_store_errno = BS_OK;
            return block_id;
        } else {
            block_store_errno = BS_IN_USE;
            return 0;
        }
    }
    block_store_errno = BS_PARAM;
    return 0;
    }
*/

/*
 * PURPOSE: Free up an inputted block from the block store's block map
 * INPUTS:
 *      bs, the block store that holds the block map
 *      block_id, our target block we're freeing
 * RETURN:
 *      our target block_id
 **/
size_t block_store_release(block_store_t *const bs, const size_t block_id) {
    if (bs && bs->fbm && BLOCKID_VALID(block_id)) {
        // we could clear the dirty bit, since the info is no longer in use but...
        // We'll keep it. Could be useful. Doesn't really hurt anything.
        // Keeps it more true to a standard block device.
        // You could also use this function to format the specified block for security reasons
        
        // note in free block memory that our target block_id is not being used anymore
        bitmap_reset(bs->fbm, block_id);
        block_store_errno = BS_OK;
        return block_id;
    }
    block_store_errno = BS_PARAM;
    return 0;
}// end block_store_release

/*
 * PURPOSE: Read from block memory by copying data across to buffer
 * INPUTS:
 *      bs, block store that holds all block information
 *      block_id, target block to read
 *		buffer, holds data copied from block map
 *		nbytes, number of bytes to read
 *		offset, how far from 0 do we start reading from?
 * RETURN:
 *      if success, return number of bytes to read
 *		else, return 0
 **/
size_t block_store_read(const block_store_t *const bs, const size_t block_id, void *buffer, const size_t nbytes, const size_t offset) {
    if (bs && bs->fbm && bs->data_blocks && BLOCKID_VALID(block_id)
            && buffer && nbytes && (nbytes + offset <= BLOCK_SIZE)) {
        // Not going to forbid reading of not-in-use blocks (but we'll log it via errno)
        
        // compute total offset using offset & size of datatype
        size_t total_offset = offset + (BLOCK_SIZE * (block_id - FBM_SIZE));
        
        // copy info from block into buffer
        memcpy(buffer, (void *)(bs->data_blocks + total_offset), nbytes);
        
        block_store_errno = bitmap_test(bs->fbm, block_id) ? BS_OK : BS_FBM_REQUEST_MISMATCH;
        return nbytes;
    }
    // technically we return BS_PARAM even if the internal structure of the BS object is busted
    // Which, in reality, would be more of a BS_INTERNAL or a BS_FATAL... but it'll add another branch to everything
    // And technically the bs is a parameter...
    block_store_errno = BS_PARAM;
    return 0;
}// end block_store_read

// Gotta take read in nbytes from the buffer and write it to the offset of the block
// Pretty easy, actually
// Gotta remember to mess with the DBM!
// Let's allow writing to blocks not marked as in use as well, but log it like with read
/*
 * PURPOSE: Write data by copying buffer data over to memory
 * INPUTS:
 *      bs, block store that holds addresses to all our data
 *		block_id, our target block to modify
 *		buffer, holds data to be copied over
 *		nbytes, number of bytes to copy
 *      offset, how far from 0 we are to start writing
 * RETURN:
 *      if success, return number of bytes to read
 *		else, return 0
 **/
size_t block_store_write(block_store_t *const bs, const size_t block_id, const void *buffer, const size_t nbytes, const size_t offset) {
	// param check (note the dbm check)
    if (bs && bs->fbm && bs->dbm && bs->data_blocks && BLOCKID_VALID(block_id)
            && buffer && nbytes && (nbytes + offset <= BLOCK_SIZE)) {
        // compute total offset using offset & size of datatype
        size_t total_offset = offset + (BLOCK_SIZE * (block_id - FBM_SIZE));
    
    	// note in the dbm that we're using this block now 
    	// follow-up:(do we need to set the block_id? or the total_offset?)
        bitmap_set(bs->dbm, block_id);
    
        // copy info from buffer into block
        memcpy((void *)(bs->data_blocks + total_offset), buffer, nbytes);
        
        // test out both the fbm and dbm since we're touching both in this fn...
        block_store_errno = bitmap_test(bs->fbm, block_id) ? BS_OK : BS_FBM_REQUEST_MISMATCH;
        block_store_errno = bitmap_test(bs->dbm, block_id) ? BS_OK : BS_FBM_REQUEST_MISMATCH;
        return nbytes;
    
    }
    // something wrong with parameters, note error in log and return 0
    block_store_errno = BS_FATAL;
    return 0;
}// end block_store_write

// TODO: Implement, comment, param check
// Gotta make a new BS object and read it from the file
// Need to remember to get the file format right, where's the FBM??
// Since it's just loaded from file, the DBM is easy
// Should probably make sure that the file is actually the right size
// There should be POSIX stuff for everything file-related
// Probably going to have a lot of resource management, better be careful
// Lots of different errors can happen
/*
 * PURPOSE: Create a new blockstore by reading it in from a saved file
 * INPUTS:
 *      filename, location to read our blockstore data
 * RETURN:
 *      new block store with all out data
 **/
block_store_t *block_store_import(const char *const filename) {
	if( filename ){
		const int fd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP);
        if (fd != -1) {
			if ( block_store_t * bs = block_store_create() != NULL ){
				// followup: check on count input, does it make sense? What is the buffer? bitmap_import()?
				if ( utility_read_file(fd, , BLOCK_SIZE * (BLOCK_COUNT - FBM_SIZE)) == (BLOCK_SIZE * (BLOCK_COUNT - FBM_SIZE)) ){
		    	
		    	}
			
			}
        }
		block_store_errno = BS_FILE_IO;
        close(fd);
        return 0;
	}
	else{
	    block_store_errno = BS_PARAM;
	    return NULL;
	}


    block_store_errno = BS_FATAL;
    return NULL;
}// end block_store_import

// TODO: Comment
/*
 * PURPOSE:
 * INPUTS:
 *      
 *      
 * RETURN:
 *      
 **/
size_t block_store_export(const block_store_t *const bs, const char *const filename) {
    // Thankfully, this is less of a mess than import...
    // we're going to ignore dbm, we'll treat export like it's making a new copy of the drive
    if (filename && bs && bs->fbm && bs->data_blocks) {
        const int fd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP);
        if (fd != -1) {
            if (utility_write_file(fd, bitmap_export(bs->fbm), FBM_SIZE * BLOCK_SIZE) == (FBM_SIZE * BLOCK_SIZE)) {
                if (utility_write_file(fd, bs->data_blocks, BLOCK_SIZE * (BLOCK_COUNT - FBM_SIZE)) == (BLOCK_SIZE * (BLOCK_COUNT - FBM_SIZE))) {
                    block_store_errno = BS_OK;
                    close(fd);
                    return BLOCK_SIZE * BLOCK_COUNT;
                }
            }
            block_store_errno = BS_FILE_IO;
            close(fd);
            return 0;
        }
        block_store_errno = BS_FILE_ACCESS;
        return 0;
    }
    block_store_errno = BS_PARAM;
    return 0;
}// end block_store_export

/*
 * PURPOSE: Returns an explanation for a given error code
 * INPUTS:
 *      bs_err, block store error to analyze
 * RETURN:
 *      string containing an explanaiton of the error
 **/
const char *block_store_strerror(block_store_status bs_err) {
    switch (bs_err) {
        case BS_OK:
            return "Ok";
        case BS_PARAM:
            return "Parameter error";
        case BS_INTERNAL:
            return "Generic internal error";
        case BS_FULL:
            return "Device full";
        case BS_IN_USE:
            return "Block in use";
        case BS_NOT_IN_USE:
            return "Block not in use";
        case BS_FILE_ACCESS:
            return "Could not access file";
        case BS_FATAL:
            return "Generic fatal error";
        case BS_FILE_IO:
            return "Error during disk I/O";
        case BS_MEMORY:
            return "Memory allocation failure";
        case BS_WARN:
            return "Generic warning";
        case BS_FBM_REQUEST_MISMATCH:
            return "Read/write request to a block not marked in use";
        default:
            return "???";
    }
}// end block_store_strerror


// V2 idea:
//  add an fd field to the struct (and have export(change name?) fill it out if it doesn't exist)
//  and use that for sync. When we sync, we only write the dirtied blocks
//  and once the FULL sync is complete, we format the dbm
//  So, if at any time, the connection to the file dies, we have the dbm saved so we can try again
//   but it's probably totally broken if the sync failed for whatever reason
//   I guess a new export will fix that?

/*
 * PURPOSE:
 * INPUTS:
 *      
 *      
 * RETURN:
 *      
 **/
size_t utility_read_file(const int fd, uint8_t *buffer, const size_t count) {
    return 0;
}// end utility_read_file

/*
 * PURPOSE:
 * INPUTS:
 *      
 *      
 * RETURN:
 *      
 **/
size_t utility_write_file(const int fd, const uint8_t *buffer, const size_t count) {
    return 0;
}// end utility_write_file
