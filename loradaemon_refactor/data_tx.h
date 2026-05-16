#ifndef LORAHAM_DATA_TX_H
#define LORAHAM_DATA_TX_H

#include <stddef.h>

/* --- DATA TX chunking --- */

#define DATA_TX_MAX_CHUNK_SIZE 255

size_t data_tx_chunk_size(size_t remaining);

#endif
