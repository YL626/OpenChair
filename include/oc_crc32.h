#ifndef OC_CRC32_H
#define OC_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32/IEEE 802.3 (reflected polynomial 0xEDB88320, init and final XOR
 * 0xFFFFFFFF), as used by zlib. Detects random damage; not a security check.
 * To checksum data in pieces, pass the previous result as crc; start with 0. */
uint32_t oc_crc32(uint32_t crc, const void *data, size_t len);

#endif
