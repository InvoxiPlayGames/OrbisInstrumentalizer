#pragma once

#include <stdint.h>

// Finds the game's PLT stubs for imports from a library (names sorted alphabetically, all the
// game imports from it). Returns how many were found; missing ones are left at 0.
// known_targets (optional) holds the library's address for each name, 0 where not known.
int FindImportStubs(const char *library_name, const char *const *names, int count, uint64_t base_address,
                    const uint64_t *known_targets, uint64_t *stubs_out, uint64_t *gots_out);

// where one of our own imports really goes, 0 if it isn't bound
uint64_t ResolveOwnImport(const void *function);
