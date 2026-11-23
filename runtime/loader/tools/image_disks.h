// SPDX-License-Identifier: MIT
// The image set's disks as the native kernel's loaders give them (xnuloader-qemu, xnuloader-crosvm): os, systemos and app
// read-only with their hash lists, the Data seed read-only, the Data store writable, each with the identity xnuhost gives
// its own disks (runtime/xnuhost/src/main.c image_set_disk), read through xnuhost's imageset.c.
#ifndef XNULOADER_IMAGE_DISKS_H
#define XNULOADER_IMAGE_DISKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "imageset.h"

#define IMAGE_DISKS_MAX 5

typedef struct {
  char             path[1024];       // the file on this machine (the Data store: as given, maybe another machine's)
  const char      *rel;              // its path in the image set (manifest.json), NULL for the Data store
  uint64_t         size;             // the manifest's bytes, 0 for the Data store
  bool             writable;
  xh_disk_identity identity;
  const uint8_t   *list;             // the hash list of a read-only image, or NULL
  size_t           list_size;
} image_disk;

// Reads the set in DIR (exits with a message on any fault: a file of another size than the manifest's, a hash list that
// does not match it). DATA is the Data store; check_data: it is a file here, checked (a raw file of whole sectors, 1
// GiB or more), else only named (a store on the phone). Returns the number of disks, the store last.
unsigned image_disks(const char *dir, const char *data, bool check_data, xh_imageset *set, image_disk *disks, char seed_hex[65]);

#endif
