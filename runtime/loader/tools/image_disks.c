// SPDX-License-Identifier: MIT
// The image set's disks for the native kernel's loaders (image_disks.h).
#include "image_disks.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static void image_file(image_disk *d, const xh_imageset *set, const xh_imageset_file *f)
{
  snprintf(d->path, sizeof d->path, "%s/%s", set->dir, f->path);
  d->rel = f->path;
  d->size = f->size;
  struct stat st;
  if (stat(d->path, &st) || (uint64_t)st.st_size != f->size || (f->size & 511)) {
    fprintf(stderr, "%s: not the %llu bytes (whole sectors) manifest.json lists\n", d->path, (unsigned long long)f->size);
    exit(1);
  }
}

// In the order of their transports or PCI functions, as xnuhost describes its own (main.c image_set_disk).
unsigned image_disks(const char *dir, const char *data, bool check_data, xh_imageset *set, image_disk *disks, char seed_hex[65])
{
  const char *error = NULL;
  int rc = xh_imageset_load(dir, set, &error);
  if (rc) { fprintf(stderr, "%s: %s\n", dir, rc > 0 ? "no manifest.json" : error); exit(1); }
  unsigned n = 0;
  static const char *const images[] = { "os", "systemos", "app" };
  for (unsigned i = 0; i < 3; i++) {
    const xh_imageset_file *f = xh_imageset_role(set, images[i]);
    if (!f) { fprintf(stderr, "%s: no %s image in manifest.json\n", dir, images[i]); exit(1); }
    image_disk *d = &disks[n++];
    image_file(d, set, f);
    xh_verify *v = xh_verify_open(set, f, &error);   // the hash list, checked against the manifest
    if (!v) { fprintf(stderr, "%s: %s\n", d->path, error); exit(1); }
    d->list = xh_verify_list(v, &d->list_size);
    d->identity = (xh_disk_identity){ .role = f->role, .verified = xh_verify_digest(v), .volume_uuid = f->volume_uuid };
  }
  const xh_imageset_file *seed = xh_imageset_role(set, "data-seed");
  seed_hex[0] = 0;
  for (unsigned i = 0; seed && i < 32; i++) snprintf(seed_hex + i * 2, 3, "%02x", seed->sha256[i]);
  if (seed) {
    image_disk *d = &disks[n++];
    image_file(d, set, seed);
    d->identity = (xh_disk_identity){ .role = "seed", .bytes = seed->size, .sha256_hex = seed_hex };
  }
  image_disk *d = &disks[n++];
  struct stat st;
  if (check_data && (stat(data, &st) || st.st_size < (1ll << 30) || (st.st_size & 511))) {
    fprintf(stderr, "%s: the Data store must be a raw file of whole sectors, 1 GiB or more\n", data);
    exit(1);
  }
  snprintf(d->path, sizeof d->path, "%s", data);
  d->writable = true;
  d->identity = (xh_disk_identity){ .role = "data", .volume_uuid = set->data_uuid, .volume_name = set->data_name, .sha256_hex = seed_hex };
  return n;
}
