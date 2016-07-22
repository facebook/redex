/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "DexLoader.h"
#include "DexDefs.h"
#include "DexAccess.h"
#include "Trace.h"
#include "WorkQueue.h"

#define DL_FAIL (1)
#define DL_SUCCESS (0)

class DexLoader {
  DexIdx* m_idx;
  dex_class_def* m_class_defs;
  DexClasses* m_classes;
  uint8_t* m_dexmmap;
  ssize_t m_dex_size;

 public:
  DexLoader() {}
  ~DexLoader() {
    if (m_idx) delete m_idx;
    if (m_dexmmap) munmap(m_dexmmap, m_dex_size);
  }
  DexClasses load_dex(const char* location);
  void load_dex_class(int num);
};

static int open_dex_file(const char* location,
                         uint8_t*& dmapping,
                         ssize_t& dsize) {
  int fd = open(location, O_RDONLY);
  struct stat buf;
  if (fd < 0) {
    fprintf(stderr, "Cannot open dump file %s\n", location);
    return DL_FAIL;
  }
  if (fstat(fd, &buf)) {
    fprintf(stderr, "Cannot fstat file %s\n", location);
    return DL_FAIL;
  }
  dsize = buf.st_size;
  dmapping =
      (uint8_t*)mmap(nullptr, dsize, PROT_READ, MAP_FILE | MAP_SHARED, fd, 0);
  close(fd);
  if (dmapping == MAP_FAILED) {
    dmapping = nullptr;
    perror("Address space allocation failed for mmap\n");
    return DL_FAIL;
  }
  return DL_SUCCESS;
}

static int validate_dex_header(dex_header* dh) {
  if (memcmp(dh->magic, DEX_HEADER_DEXMAGIC, sizeof(dh->magic))) {
    fprintf(stderr, "Bad dex magic %s\n", dh->magic);
    return DL_FAIL;
  }
  return DL_SUCCESS;
}

struct class_load_work {
  DexLoader* dl;
  int num;
};

static void class_work(void* arg) {
  auto clw = reinterpret_cast<class_load_work*>(arg);
  clw->dl->load_dex_class(clw->num);
}

void DexLoader::load_dex_class(int num) {
  dex_class_def* cdef = m_class_defs + num;
  DexClass* dc = new DexClass(m_idx, cdef);
  m_classes->insert_at(dc, num);
}

DexClasses DexLoader::load_dex(const char* location) {
  dex_header* dh;
  if (open_dex_file(location, m_dexmmap, m_dex_size) != DL_SUCCESS) {
    exit(1); // FIXME(snay)
  }
  dh = (dex_header*)m_dexmmap;
  if (validate_dex_header(dh) != DL_SUCCESS) {
    exit(1); // FIXME(snay)
  }

  m_idx = new DexIdx(dh);
  m_class_defs = (dex_class_def*)(m_dexmmap + dh->class_defs_off);
  if (dh->class_defs_size <= 0) return DexClasses(0);
  DexClasses classes(dh->class_defs_size);
  m_classes = &classes;
  auto workptrs = new work_item[dh->class_defs_size];
  auto lwork = new class_load_work[dh->class_defs_size];
  for (uint32_t i = 0; i < dh->class_defs_size; i++) {
    lwork[i].dl = this;
    lwork[i].num = i;
    workptrs[i] = work_item{class_work, &lwork[i]};
  }
  WorkQueue wq;
  wq.run_work_items(workptrs, dh->class_defs_size);
  delete[] lwork;
  delete[] workptrs;
  return classes;
}

DexClasses load_classes_from_dex(const char* location) {
  DexLoader dl;
  return dl.load_dex(location);
}
