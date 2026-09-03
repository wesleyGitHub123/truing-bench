/**
 * @file bringup_proto.h
 * Phase 1e bring-up: the SPEC §12 wire protocol encodes and decodes on the target
 * exactly as it does on the host.
 *
 * This is not a duplicate of the host suite. It exists because two things in the
 * codec depend on the TARGET C library rather than on our own code, and both fail
 * silently rather than loudly: the fixed-point float conversion behind
 * truing_json_f32() (newlib can be built without floating-point printf support, in
 * which case a conversion yields no digits at all) and strtod() on the way back in.
 * A frame that loses its numbers is still valid JSON, so nothing downstream would
 * notice. The checks below compare byte-exact frames instead.
 */
#ifndef TRUING_BRINGUP_PROTO_H
#define TRUING_BRINGUP_PROTO_H

#include <stdbool.h>

void truing_bringup_proto_section(int *pass, int *fail, bool *ok_out);

#endif /* TRUING_BRINGUP_PROTO_H */
