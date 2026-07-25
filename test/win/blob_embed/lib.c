/* Lives in the STATIC library. mesh_loader.cpp references these symbols the same
 * way, which is exactly why this approach survives static linking where an RCDATA
 * resource does not: the linker pulls a member in because a symbol is referenced. */
#include <stdint.h>
extern const unsigned char probe_blob[];
extern const uint64_t probe_blob_len;

uint64_t blob_size(void) { return probe_blob_len; }
unsigned char blob_first_byte(void) { return probe_blob[0]; }
