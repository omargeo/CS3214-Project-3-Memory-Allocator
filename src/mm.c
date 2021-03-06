/*
 * Simple, 64-bit allocator based on implicit free lists,
 * first fit placement, and boundary tag coalescing, as described
 * in the CS:APP2e text. Blocks must be aligned to 16 byte
 * boundaries. Minimum block size is 16 bytes.
 *
 * This version is loosely based on
 * http://csapp.cs.cmu.edu/3e/ics3/code/vm/malloc/mm.c
 * but unlike the book's version, it does not use C preprocessor
 * macros or explicit bit operations.
 *
 * It follows the book in counting in units of 4-byte words,
 * but note that this is a choice (my actual solution chooses
 * to count everything in bytes instead.)
 *
 * You may use this code as a starting point for your implementation
 * if you want.
 *
 * Adapted for CS3214 Summer 2020 by gback
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>


#include "mm.h"
#include "memlib.h"
#include "config.h"

#define LOG2(X) ((unsigned) (8*sizeof (unsigned int) - __builtin_clz((X)) - 1))

struct boundary_tag {
    int inuse:1;        // inuse bit
    int size:31;        // size of block, in words
                        // block size
};

/* FENCE is used for heap prologue/epilogue. */
const struct boundary_tag FENCE = {
    .inuse = 1,
    .size = 0};

/* FreeList struct used to store the free blocks */
struct freelist
{
    struct list list;
    size_t size;
};

/* A C struct describing the beginning of each block.
 * For implicit lists, used and free blocks have the same
 * structure, so one struct will suffice for this example.
 *
 * If each block is aligned at 12 mod 16, each payload will
 * be aligned at 0 mod 16.
 */
struct block
{
    struct boundary_tag header; /* offset 0, at address 12 mod 16 */
    char payload[0];            /* offset 4, at address 0 mod 16 */
};

struct free_block
{
    struct boundary_tag header; /* offset 0, at address 12 mod 16 */
    struct list_elem elem;
};

/* Basic constants and macros */
#define WSIZE sizeof(struct boundary_tag) /* Word and header/footer size (bytes) */
#define MIN_BLOCK_SIZE_WORDS 8            /* Minimum block size in words */
#define CHUNKSIZE (1 << 10)               /* Extend heap by this amount (words) */
#define NUM_LISTS 32                      /* will start of as 1 just to make sure the free list works and will be changed later to 7 */
                                          /* a helper function will be used to decide where the free block will be added */

static inline size_t max(size_t x, size_t y)
{
    return x > y ? x : y;
}

static size_t align(size_t size)
{
    return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

static bool is_aligned(size_t size) __attribute__((__unused__));
static bool is_aligned(size_t size)
{
    return size % ALIGNMENT == 0;
}

/* Global variables */
static struct block *heap_listp = 0;       /* Pointer to first block */
struct freelist freeblock_list[NUM_LISTS]; /* Free block list */

#if (LIST_POLICY == SEG_LIST)

/**
 * @brief takes a freeblock and decides where it should be added
 * in the list according to its size
 *
 * @param freeblock the block that is free to be added in the list
 *
 * @return an int that represents the corresponding list
 */
static int get_freelist(size_t bsize)
{
    for (int i = 0; i < NUM_LISTS; i++)
    {
        if (bsize <= freeblock_list[i].size) {
            

            return i;
            //break;
        }
    }

    return NUM_LISTS -1;
   
    // int index = LOG2(bsize) ;
    
    // if(index >= NUM_LISTS) index = NUM_LISTS-1;
    // //printf("math:%d\n", index);

    // return index;

}
#endif

/* Function prototypes for internal helper routines */
static struct block *extend_heap(size_t words);
static struct block *place(struct block *bp, size_t asize);
static struct block *find_fit(size_t asize);
static struct block *coalesce(struct block *bp);
struct block *realloc_place(struct block *blk, size_t word_num);
// static void mm_checkheap(int verbose);

/* Given a block, obtain previous's block footer.
   Works for left-most block also. */
static struct boundary_tag *prev_blk_footer(struct block *blk)
{
    return &blk->header - 1;
}

/* Return if block is free */
static bool blk_free(struct block *blk)
{
    return !blk->header.inuse;
}

/* Return size of block is free */
static size_t blk_size(struct block *blk)
{
    return blk->header.size;
}

/* Given a block, obtain pointer to previous block.
   Not meaningful for left-most block. */
static struct block *prev_blk(struct block *blk)
{
    struct boundary_tag *prevfooter = prev_blk_footer(blk);
    assert(prevfooter->size != 0);
    return (struct block *)((void *)blk - WSIZE * prevfooter->size);
}

/* Given a block, obtain pointer to next block.
   Not meaningful for right-most block. */
static struct block *next_blk(struct block *blk)
{
    assert(blk_size(blk) != 0);
    return (struct block *)((void *)blk + WSIZE * blk->header.size);
}

/* Given a block, obtain its footer boundary tag */
static struct boundary_tag *get_footer(struct block *blk)
{
    return ((void *)blk + WSIZE * blk->header.size) - sizeof(struct boundary_tag);
}

/* Set a block's size and inuse bit in header and footer */
static void set_header_and_footer(struct block *blk, int size, int inuse)
{
    blk->header.inuse = inuse;
    blk->header.size = size;
    *get_footer(blk) = blk->header; /* Copy header to footer */
}

/* Mark a block as used and set its size. */
static void mark_block_used(struct block *blk, int size)
{
    set_header_and_footer(blk, size, 1);
}

/* Mark a block as free and set its size. */
static void mark_block_free(struct block *blk, int size)
{
    set_header_and_footer(blk, size, 0);
}

/*
 * mm_init - Initialize the memory manager
 */
int mm_init(void)
{
    assert(offsetof(struct block, payload) == 4);
    assert(sizeof(struct boundary_tag) == 4);
    /*initilize free_mem list*/
    list_init(&free_mem);
    /* Create the initial empty heap */
    struct boundary_tag *initial = mem_sbrk(4 * sizeof(struct boundary_tag));
    if (initial == (void *)-1)
        return -1;

    /* We use a slightly different strategy than suggested in the book.
     * Rather than placing a min-sized prologue block at the beginning
     * of the heap, we simply place two fences.
     * The consequence is that coalesce() must call prev_blk_footer()
     * and not prev_blk() because prev_blk() cannot be called on the
     * left-most block.
     */
    initial[2] = FENCE; /* Prologue footer */
    heap_listp = (struct block *)&initial[3];
    initial[3] = FENCE; /* Epilogue header */
    list_init(&freeblock_list[0].list);
    freeblock_list[0].size = 1;
    for (int i = 1; i < NUM_LISTS; i++)
    {
        list_init(&freeblock_list[i].list);
        freeblock_list[i].size = freeblock_list[i - 1].size * 2;
    }
    // freeblock_list[NUM_LISTS-1].size = 999999;

    /* Extend the empty heap with a free block of CHUNKSIZE bytes */
    if (extend_heap(CHUNKSIZE) == NULL)
        return -1;
    return 0;
}

/*
 * mm_malloc - Allocate a block with at least size bytes of payload
 */
void *mm_malloc(size_t size)
{
    struct block *bp;

    /* Ignore spurious requests */
    if (size == 0)
        return NULL;

    /* Adjust block size to include overhead and alignment reqs. */
    #if(LIST_POLICY == EXPLICIT_LIST | SEG_LIST)
    size += 2 * sizeof(struct boundary_tag);    /* account for tags  */ //no need list_elem??
    #endif
    /* Adjusted block size in words */
    size_t awords = max(MIN_BLOCK_SIZE_WORDS, align(size) / WSIZE); /* respect minimum size */

    /* Search the free list for a fit */
    if ((bp = find_fit(awords)) != NULL)
    {
        bp = place(bp, awords);
        // mm_checkheap(0);
        return bp->payload;
    }

    /* No fit found. Get more memory and place the block */
    size_t extendwords = max(awords, CHUNKSIZE); /* Amount to extend heap if no fit */
    if ((bp = extend_heap(extendwords)) == NULL)
        return NULL;

    bp = place(bp, awords);
    // mm_checkheap(0);
    return bp->payload;
}

/*
 * mm_free - Free a block
 */
void mm_free(void *bp)
{
    assert(heap_listp != 0); // assert that mm_init was called
    if (bp == 0)
        return;

    /* Find block from user pointer */
    struct block *blk = bp - offsetof(struct block, payload);

    mark_block_free(blk, blk_size(blk));

    #if(LIST_POLICY == SEG_LIST)
    // struct free_block * newblk = ((struct free_block*)blk);
    // int list_index = get_freelist(newblk->header.size);
    // list_push_back(&freeblock_list[list_index].list, &newblk->elem); //get_freelist(newblk)].list
    #endif
    coalesce(blk);
}

/*
 * coalesce - Boundary tag coalescing. Return ptr to coalesced block
 */
static struct block *coalesce(struct block *bp)
{
    bool prev_alloc = prev_blk_footer(bp)->inuse; /* is previous block allocated? */
    bool next_alloc = !blk_free(next_blk(bp));    /* is next block allocated? */
    size_t size = blk_size(bp);

    if (prev_alloc && next_alloc)
    { /* Case 1 */
        // both are allocated, nothing to coalesce
        struct free_block * newblk = ((struct free_block*)bp);
        int list_index = get_freelist(newblk->header.size);
        list_push_back(&freeblock_list[list_index].list, &newblk->elem);
        return bp;
    }

    else if (prev_alloc && !next_alloc)
    { /* Case 2 */
        // combine this block and next block by extending it


#if (LIST_POLICY == SEG_LIST)
        int coal_size = size + blk_size(next_blk(bp));
        list_remove(&((struct free_block*)next_blk(bp))->elem); // remove block that will be coalesced
        mark_block_free(bp,  coal_size);
        
        int index = get_freelist(coal_size);
        list_push_back(&freeblock_list[index].list, &((struct free_block*)bp)->elem);
       
        
        #endif
        
        
    }

    else if (!prev_alloc && next_alloc) {      /* Case 3 */
        // combine previous and this block by extending previous
        

#if (LIST_POLICY == SEG_LIST)
        int coal_size = size + blk_size(prev_blk(bp));
        bp = prev_blk(bp);
        int index = get_freelist(bp->header.size);
        if (freeblock_list[index].size < coal_size)
        {
            list_remove(&((struct free_block *)bp)->elem);
            index = get_freelist(coal_size);
            list_push_back(&freeblock_list[index].list, &((struct free_block *)bp)->elem);
        }
        mark_block_free(bp, size + blk_size(bp));

#endif
    }

    else {                                     /* Case 4 */
        // combine all previous, this, and next block into one
        
        

        
        #if(LIST_POLICY == SEG_LIST)
        int index = get_freelist(prev_blk(bp)->header.size);
        mark_block_free(prev_blk(bp),
                        size + blk_size(next_blk(bp)) + blk_size(prev_blk(bp)));
        list_remove(&((struct free_block*)next_blk(bp))->elem);
        bp = prev_blk(bp);
        int coal_size = bp->header.size;

        if (freeblock_list[index].size < coal_size)
        {
            list_remove(&((struct free_block *)bp)->elem);
            index = get_freelist(coal_size);
            list_push_back(&freeblock_list[index].list, &((struct free_block *)bp)->elem);
        }
#endif
    }
    return bp;
}

/**
 * @brief reallocates the given block size to the new one
 * 
 * @param ptr the block to be reallocated
 * @param size the new size
 * @return void* 
 */
void *mm_realloc(void *ptr, size_t size)
{
    /* If size == 0 then this is just free, and we return NULL. */
    if (size == 0)
    {
        mm_free(ptr);
        return 0;
    }

    /* If oldptr is NULL, then this is just malloc. */
    if (ptr == NULL)
    {
        return mm_malloc(size);
    }

#if (LIST_POLICY == SEG_LIST)
    size += 2 * sizeof(struct boundary_tag);                        /* account for tags  */
                                                                    /* Adjusted block size in words */
    size_t awords = max(MIN_BLOCK_SIZE_WORDS, align(size) / WSIZE); /* respect minimum size */

    struct block *blk = ptr - offsetof(struct block, payload);
    blk = realloc_place(blk, awords);
    if (blk != NULL)
        return blk->payload;
#endif

    void *newptr = mm_malloc(size);

    /* If realloc() fails the original block is left untouched  */
    if (!newptr)
    {
        return 0;
    }

    /* Copy the old data. */
    struct block *oldblock = ptr - offsetof(struct block, payload);
    size_t oldsize = blk_size(oldblock) * WSIZE;
    if (size < oldsize)
        oldsize = size;
    memcpy(newptr, ptr, oldsize);

    /* Free the old block. */
    mm_free(ptr);

    return newptr;
}

/**
 * @brief a helper funciton that helps to reallocates a block
 * 
 * @param blk to be reallocated
 * @param word_num the new size
 * @return struct block* 
 */
struct block *realloc_place(struct block *blk, size_t word_num)
{
    
    bool next_alloc = !blk_free(next_blk(blk)); 
    bool prev_alloc = prev_blk_footer(blk)->inuse;   /* is previous block allocated? */
    size_t add_size = word_num - blk_size(blk);
    if(add_size <= 0) return blk;
    if(!next_alloc) //CASE 2 next is free
    {
        struct block *ne_blk = next_blk(blk);
        size_t ne_size = blk_size(ne_blk);
        if(ne_size >= add_size) //next block size is big enough to take the addition
        {
            if ((ne_size - add_size) >= MIN_BLOCK_SIZE_WORDS)
            {
                list_remove(&((struct free_block *)ne_blk)->elem);
                mark_block_used(blk, word_num);
                ne_blk = next_blk(blk);

                mark_block_free(ne_blk, ne_size - add_size);

                int index = get_freelist(ne_size - add_size);
                list_push_back(&freeblock_list[index].list, &((struct free_block *)ne_blk)->elem);
            }
            else
            {
                
                list_remove(&((struct free_block*)ne_blk)->elem);
                mark_block_used(blk,blk_size(ne_blk)+blk_size(blk));
            }
            return blk;
        }
        else 
        {
            if(next_blk(ne_blk)->header.size ==0)
            {
 
                extend_heap(add_size-ne_size);
                list_remove(&((struct free_block*)ne_blk)->elem);
                mark_block_used(blk,blk_size(ne_blk)+blk_size(blk));
                return blk;

            }

        }
        
    }
    else if(!prev_alloc && next_alloc) //prev is free 
    {
        struct block *prev = prev_blk(blk);
        size_t prev_size = blk_size(prev);
        if(prev_size > add_size) //next block size is big enough to take the addition
        {
            // if ((prev_size - add_size) >= MIN_BLOCK_SIZE_WORDS) 
            // { 
            //     printf("curr:%ld\n", blk_size(blk));
            //     printf("prev:%ld\n", prev_size);
            //     printf("add:%ld\n", add_size);
            //     list_remove(&((struct free_block*)prev)->elem);
            //     struct block* new_blk = (struct block*)(((void*)blk) -  WSIZE * add_size);
            //     mark_block_used(new_blk, word_num);

            
            //     memcpy(new_blk->payload, blk->payload, blk->header.size * WSIZE);
                
                
            //     mark_block_free(prev, prev_size-add_size);
                
            //     int index = get_freelist(prev_size-add_size);
            //     list_push_back(&freeblock_list[index].list, &((struct free_block*)prev)->elem);
            //     blk = new_blk;
            
            
            // }
            // else 
            // {
            //     // list_remove(&((struct free_block*)prev_blk)->elem);
            //     // mark_block_used(blk,blk_size(prev_blk)+blk_size(blk));
            //     return NULL;
            // }
            // return blk;
        }

    }
    return NULL;
}


/**
 * @brief this checks that all blocks in the free list are free and not used
 * 
 * @param verbose 
 */
void mm_checkheap(int verbose)
{
    for (int list_index = 0; list_index < NUM_LISTS; list_index++)
    {
        struct list_elem *e = list_begin(&freeblock_list[list_index].list);
        for (; e != list_end(&freeblock_list[list_index].list); e = list_next(e))
        {
            
            assert(blk_free((struct block *)list_entry(e, struct free_block, elem)));
        }
    }
}

/*
 * The remaining routines are internal helper routines
 */

/*
 * extend_heap - Extend heap with free block and return its block pointer
 */
static struct block *extend_heap(size_t words)
{

    void *bp = mem_sbrk(words * WSIZE);

    if ((intptr_t)bp == -1)
        return NULL;

    /* Initialize free block header/footer and the epilogue header.
     * Note that we overwrite the previous epilogue here. */
    struct block *blk = bp - sizeof(FENCE);
    mark_block_free(blk, words);

    next_blk(blk)->header = FENCE;

    /* Coalesce if the previous block was free */
    return coalesce(blk);
}

/*
 * place - Place block of asize words at start of free block bp
 *         and split if remainder would be at least minimum block size
 */
static struct block *place(struct block *bp, size_t asize)
{
    size_t csize = blk_size(bp);

#if (LIST_POLICY == SEG_LIST)

    if ((csize - asize) >= MIN_BLOCK_SIZE_WORDS)
    {
        list_remove(&((struct free_block *)bp)->elem);
        mark_block_free(bp, csize - asize);
        int index = get_freelist(bp->header.size);
        list_push_back(&freeblock_list[index].list, &((struct free_block *)bp)->elem);

        bp = next_blk(bp);
        mark_block_used(bp, asize);
        // list_push_back(&free_mem, &((struct free_block*)bp_next)->elem);
    }
    else
    {
        list_remove(&((struct free_block *)bp)->elem);
        mark_block_used(bp, csize);
    }
#endif

    return bp;
}

/*
 * find_fit - Find a fit for a block with asize words
 */
static struct block *find_fit(size_t asize)
{
    /* First fit search */

#if (LIST_POLICY == SEG_LIST)

    int list_index = get_freelist(asize);

    for (; list_index < NUM_LISTS; list_index++)
    {
        int count = 0;
        struct list_elem *e = list_begin(&freeblock_list[list_index].list);
        for (; e != list_end(&freeblock_list[list_index].list); e = list_next(e))
        {
            if(count == 5) break;
            struct free_block *bp = list_entry(e,struct free_block,elem);
            if (blk_free((struct block*)bp) && asize <= blk_size((struct block*)bp))  return (struct block*)bp;
            count++;
            
        }
    }
#endif
    /* No fit */
    return NULL;
}

team_t team = {
    /* Team name */
    "A7A",
    /* First member's full name */
    "Omar Elgeoushy",
    "omarelgeoushy@vt.edu",
    /* Second member's full name (leave blank if none) */
    "Walid Zeineldin",
    "wmoz@vt.edu",
};
