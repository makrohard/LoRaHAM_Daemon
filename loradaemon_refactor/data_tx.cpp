#include "data_tx.h"

/* --- DATA TX chunking --- */

size_t data_tx_chunk_size(size_t remaining)
{
    if (remaining > DATA_TX_MAX_CHUNK_SIZE)
        return DATA_TX_MAX_CHUNK_SIZE;

    return remaining;
}
