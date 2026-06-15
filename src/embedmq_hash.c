#include <stdint.h>
#include "../include/embedmq.h"

/* FNV-1a 32-bit hash
 * https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
 *
 * Properties relevant to embedmq:
 *   - Single pass over the name bytes — O(n)
 *   - No collisions observed for typical event name sets (< 256 names)
 *   - Output is deterministic and platform-independent
 *   - 0x00000000 is not a valid output for any non-empty string
 *     (FNV offset basis 0x811C9DC5 guarantees a non-zero start)
 */

#define FNV1A_OFFSET_BASIS  UINT32_C(0x811C9DC5)
#define FNV1A_PRIME         UINT32_C(0x01000193)

uint32_t embedmq_uuid(const char *name)
{
    uint32_t hash = FNV1A_OFFSET_BASIS;
    if (!name) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        hash ^= (uint32_t)*p;
        hash *= FNV1A_PRIME;
    }
    return hash ? hash : 1; /* guarantee non-zero; maps 0→1 (astronomically rare) */
}
