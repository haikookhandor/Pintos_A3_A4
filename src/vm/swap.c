#include "vm/swap.h"

void swap_initialize (void) {
    swap_block = block_get_role(BLOCK_SWAP);
    if (!swap_block) return;
    swap_avail = bitmap_create(block_size(swap_block)/SECTORS_PER_PAGE);
    if (!swap_avail) return;
    bitmap_set_all(swap_avail, SWAP_RELEASED);
    lock_init(&swap_mutex);
}

size_t swap_out (void *frame) {
    if (!swap_block || !swap_avail) PANIC("NO SWAP PARTITION!!!!");
    lock_acquire(&swap_mutex);
    size_t free_index = bitmap_scan_and_flip(swap_avail, 0, 1, SWAP_RELEASED);
    if (free_index == BITMAP_ERROR) PANIC("SWAP PARTITION FULL!!!!");
    for (size_t i = 0; i < SECTORS_PER_PAGE; i++) {
        block_write(swap_block, free_index * SECTORS_PER_PAGE + i, (uint8_t *) frame + i * BLOCK_SECTOR_SIZE);
    }
    lock_release(&swap_mutex);
    return free_index;
}

void swap_in (size_t used_index, void *frame) {
    if (!swap_block || !swap_avail) return;
    lock_acquire(&swap_mutex);
    if (bitmap_test(swap_avail, used_index) == SWAP_RELEASED) PANIC("SWAP IN A FREE BLOCK!!!!");
    bitmap_flip(swap_avail, used_index);
    for (size_t i = 0; i < SECTORS_PER_PAGE;  i++) {
        block_read(swap_block, used_index * SECTORS_PER_PAGE + i, (uint8_t *) frame + i * BLOCK_SECTOR_SIZE);
    }
    lock_release(&swap_mutex);
}