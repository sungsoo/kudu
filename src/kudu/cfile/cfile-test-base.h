// Copyright (c) 2013, Cloudera, inc

#ifndef KUDU_CFILE_TEST_BASE_H
#define KUDU_CFILE_TEST_BASE_H

#include <glog/logging.h>
#include <algorithm>
#include <string>

#include "kudu/cfile/cfile-test-base.h"
#include "kudu/cfile/cfile.h"
#include "kudu/cfile/cfile_reader.h"
#include "kudu/cfile/cfile.pb.h"
#include "kudu/common/columnblock.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/util/env.h"
#include "kudu/util/env_util.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/status.h"

DEFINE_int32(cfile_test_block_size, 1024,
             "Block size to use for testing cfiles. "
             "Default is low to stress code, but can be set higher for "
             "performance testing");

namespace kudu {
namespace cfile {

class CFileTestBase : public KuduTest {
 protected:
  void WriteTestFileStrings(const string &path,
                            EncodingType encoding,
                            CompressionType compression,
                            int num_entries,
                            const char *format) {
    shared_ptr<WritableFile> sink;
    ASSERT_STATUS_OK(env_util::OpenFileForWrite(env_.get(), path, &sink));
    WriterOptions opts;
    opts.write_posidx = true;
    opts.write_validx = true;
    // Use a smaller block size to exercise multi-level
    // indexing.
    opts.block_size = FLAGS_cfile_test_block_size;
    opts.storage_attributes = ColumnStorageAttributes(encoding, compression);
    Writer w(opts, STRING, false, sink);

    ASSERT_STATUS_OK(w.Start());

    // Append given number of values to the test tree
    char data[20];
    for (int i = 0; i < num_entries; i++) {
      int len = snprintf(data, sizeof(data), format, i);
      Slice slice(data, len);

      Status s = w.AppendEntries(&slice, 1);
      // Dont use ASSERT because it accumulates all the logs
      // even for successes
      if (!s.ok()) {
        FAIL() << "Failed Append(" << i << ")";
      }
    }

    ASSERT_STATUS_OK(w.Finish());
  }

  void WriteTestFileUInt32(const string &path,
                         EncodingType encoding,
                         CompressionType compression,
                         int num_entries) {
    shared_ptr<WritableFile> sink;
    ASSERT_STATUS_OK(env_util::OpenFileForWrite(env_.get(), path, &sink));
    WriterOptions opts;
    opts.write_posidx = true;
    // Use a smaller block size to exercise multi-level
    // indexing.
    opts.block_size = FLAGS_cfile_test_block_size;
    opts.storage_attributes = ColumnStorageAttributes(encoding, compression);
    Writer w(opts, UINT32, false, sink);

    ASSERT_STATUS_OK(w.Start());

    uint32_t block[8096];

    // Append given number of values to the test tree
    int i = 0;
    while (i < num_entries) {
      int towrite = std::min(num_entries - i, 8096);
      for (int j = 0; j < towrite; j++) {
        block[j] = i++ * 10;
      }

      Status s = w.AppendEntries(block, towrite);
      // Dont use ASSERT because it accumulates all the logs
      // even for successes
      if (!s.ok()) {
        FAIL() << "Failed Append(" << (i - towrite) << ")";
      }
    }

    ASSERT_STATUS_OK(w.Finish());
  }
};

// Fast unrolled summing of a vector.
// GCC's auto-vectorization doesn't work here, because there isn't
// enough guarantees on alignment and it can't seem to decode the
// constant stride.
template<class Indexable>
uint64_t FastSum(const Indexable &data, size_t n) {
  uint64_t sums[4] = {0, 0, 0, 0};
  int rem = n;
  int i = 0;
  while (rem >= 4) {
    sums[0] += data[i];
    sums[1] += data[i+1];
    sums[2] += data[i+2];
    sums[3] += data[i+3];
    i += 4;
    rem -= 4;
  }
  while (rem > 0) {
    sums[3] += data[i++];
    rem--;
  }
  return sums[0] + sums[1] + sums[2] + sums[3];
}

template<DataType Type>
static void TimeReadFileForDataType(gscoped_ptr<CFileIterator> &iter, int &count) {
  ScopedColumnBlock<Type> cb(8192);

  uint64_t sum = 0;
  while (iter->HasNext()) {
    size_t n = cb.nrows();
    ASSERT_STATUS_OK_FAST(iter->CopyNextValues(&n, &cb));
    sum += FastSum(cb, n);
    count += n;
    cb.arena()->Reset();
  }
  LOG(INFO)<< "Sum: " << sum;
  LOG(INFO)<< "Count: " << count;
}

static void TimeReadFile(const string &path, size_t *count_ret) {
  Env *env = Env::Default();
  Status s;

  gscoped_ptr<CFileReader> reader;
  ASSERT_STATUS_OK(CFileReader::Open(env, path, ReaderOptions(), &reader));

  gscoped_ptr<CFileIterator> iter;
  ASSERT_STATUS_OK(reader->NewIterator(&iter));
  ASSERT_STATUS_OK(iter->SeekToOrdinal(0));

  Arena arena(8192, 8*1024*1024);
  int count = 0;
  switch (reader->data_type()) {
    case UINT32:
    {
      TimeReadFileForDataType<UINT32>(iter, count);
      break;
    }
    case INT32:
    {
      TimeReadFileForDataType<INT32>(iter, count);
      ScopedColumnBlock<UINT32> cb(8192);

      uint64_t sum = 0;
      while (iter->HasNext()) {
        size_t n = cb.nrows();
        ASSERT_STATUS_OK_FAST(iter->CopyNextValues(&n, &cb));
        sum += FastSum(cb, n);
        count += n;
        cb.arena()->Reset();
      }
      LOG(INFO) << "Sum: " << sum;
      LOG(INFO) << "Count: " << count;
      break;
    }
    case STRING:
    {
      ScopedColumnBlock<STRING> cb(100);
      uint64_t sum_lens = 0;
      while (iter->HasNext()) {
        size_t n = cb.nrows();
        ASSERT_STATUS_OK_FAST(iter->CopyNextValues(&n, &cb));
        for (int i = 0; i < n; i++) {
          sum_lens += cb[i].size();
        }
        count += n;
        cb.arena()->Reset();
      }
      LOG(INFO) << "Sum of value lengths: " << sum_lens;
      LOG(INFO) << "Count: " << count;
      break;
    }
    default:
      FAIL() << "Unknown type: " << reader->data_type();
  }
  *count_ret = count;
}

} // namespace cfile
} // namespace kudu

#endif