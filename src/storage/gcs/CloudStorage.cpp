/*
 * Copyright 2020-present Philip Yu
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "CloudStorage.h"

#include <cstdio>
#include <fstream>
#include <glog/logging.h>
#include <iostream>
#include <unistd.h>

#include "common/Chars.h"
#include "google/cloud/storage/client.h"
#include "storage/local/File.h"

/**
 * A wrapper for interacting with AWS / S3
 */
namespace nebula {
namespace storage {
namespace gcs {

namespace {

using gcs = google::cloud::storage;
using google::cloud::StatusOr;

}
static constexpr auto S3_LIST_NO_LIMIT = std::numeric_limits<int>::max();

// Create 
const gcs::Client& s3client() {
  google::cloud::StatusOr<gcs::Client> client =
      gcs::Client::CreateDefaultClient();
  if (!client) {
    std::cerr << "Failed to create Storage Client, status=" << client.status()
              << "\n";
    return 1;
  }
  static Aws::SDKOptions options;
  static std::atomic<bool> initialized = false;
  if (!initialized) {
    Aws::InitAPI(options);
    initialized = true;
  }

  static Aws::Client::ClientConfiguration conf;
  conf.maxConnections = std::thread::hardware_concurrency();
  // conf.verifySSL = false;
  static const Aws::S3::S3Client S3C{ conf };
  return S3C;
}

std::vector<FileInfo> S3::list(const std::string& prefix) {
  // token for continuation
  Aws::String token;
  std::vector<FileInfo> objects;

  do {
    Aws::S3::Model::ListObjectsV2Request listReq;

    // It is important to specify "delimiter" to return folders only
    // Otherwise the call will return all objects at any level with maximum 1K keys
    listReq.SetBucket(this->bucket_);
    listReq.SetPrefix(prefix);
    listReq.SetMaxKeys(S3_LIST_NO_LIMIT);
    // if token has valid value, set it
    if (token.size() > 0) {
      listReq.SetContinuationToken(token);
    }

    // work on the outcome
    auto outcome = s3client().ListObjectsV2(listReq);
    if (!outcome.IsSuccess()) {
      LOG(ERROR) << fmt::format("Error listing prefix {0}: {1}", prefix, outcome.GetError().GetMessage());
      return {};
    }

    // translate into lists
    auto result = std::move(outcome.GetResultWithOwnership());

    // reset token for next fetch
    token = result.GetIsTruncated() ? result.GetNextContinuationToken() : "";

    // extraction into objects to return
#define EXTRACT_LIST(FETCH, KEY, SIZE, ISD)                     \
  {                                                             \
    const auto& list = result.FETCH();                          \
    for (auto itr = list.cbegin(); itr != list.cend(); ++itr) { \
      objects.emplace_back(ISD, 0, SIZE, itr->KEY(), bucket_);  \
    }                                                           \
  }

    // list all prefix first - folder operation
    EXTRACT_LIST(GetCommonPrefixes, GetPrefix, 0, true)

    // list all objects now - objects
    EXTRACT_LIST(GetContents, GetKey, itr->GetSize(), false)

#undef EXTRACT_LIST
  } while (token.size() > 0);

  return objects;
}

size_t S3::read(const std::string& key, char* buf, size_t size) {
  Aws::S3::Model::GetObjectRequest req;
  req.SetBucket(this->bucket_);
  req.SetKey(key);

  // Get the object
  req.SetResponseContentEncoding("utf-8");
  auto outcome = s3client().GetObject(req);
  if (!outcome.IsSuccess()) {
    LOG(ERROR) << "Error reading key: " << key << ". " << outcome.GetError().GetMessage();
    return 0;
  }

  // Get an Aws::IOStream reference to the retrieved file
  auto& stream = outcome.GetResultWithOwnership().GetBody();

  // read the whole thing into the buf which is big enough.
  // std::memcpy(buf, (void*)stream.rdbuf(), bytes);
  stream.seekg(0, std::ios::end);
  auto bytes = std::min<size_t>(stream.tellg(), size);
  stream.seekg(0, std::ios::beg);
  stream.read(buf, bytes);
  return bytes;
}

bool uploadFile(const Aws::S3::S3Client& client,
                const std::string& bucket,
                const std::string& key,
                const std::string& file) {
  Aws::S3::Model::PutObjectRequest req;
  req.SetBucket(bucket);
  req.SetKey(key);

  const std::shared_ptr<Aws::IOStream> bytes = Aws::MakeShared<Aws::FStream>(
    "nebula-upload", file, std::ios_base::in | std::ios_base::binary);
  req.SetBody(bytes);

  Aws::S3::Model::PutObjectOutcome outcome = client.PutObject(req);

  if (!outcome.IsSuccess()) {
    LOG(ERROR) << "Error: " << outcome.GetError().GetMessage();
    return false;
  }

  LOG(INFO) << "Success: upload " << file << " to key=" << key;
  return true;
}

bool downloadFile(const Aws::S3::S3Client& client,
                  const std::string& bucket,
                  const std::string& key,
                  const std::string& file) {
  // Set up the request
  Aws::S3::Model::GetObjectRequest req;
  req.SetBucket(bucket);
  req.SetKey(key);

  // set the response to stream into the file
  req.SetResponseStreamFactory([&file]() {
    return Aws::New<Aws::FStream>("s3", file, std::ios_base::out | std::ios_base::binary);
  });

  // Get the object
  auto result = client.GetObject(req);
  if (result.IsSuccess()) {
    // Get an Aws::IOStream reference to the retrieved file
    auto content = std::move(result.GetResultWithOwnership());

    // the object has no data
    if (content.GetContentLength() == 0) {
      LOG(WARNING) << "Seen an empty file: " << key;
      return false;
    }

    // return a copy of file
    return true;
  }

  auto error = result.GetError();
  LOG(ERROR) << "ERROR: " << error;
  return false;
}

inline bool S3::copy(const std::string& from, const std::string& to) {
  std::lock_guard<std::mutex> lock(s3s_);

  if (from.at(0) == '/') {
    return uploadFile(s3client(), this->bucket_, to, from);
  } else if (to.at(0) == '/') {
    return downloadFile(s3client(), this->bucket_, from, to);
  }

  // from s3 to s3
  LOG(WARNING) << "Not supporting s3 to s3 sync for now";
  return false;
}

void S3::download(const std::string& s3, const std::string& local) {
  std::lock_guard<std::mutex> lock(s3s_);
  LOG(INFO) << "Download: from " << s3 << " to " << local;

  auto files = list(s3);
  auto& client = s3client();
  for (auto& f : files) {
    // for each key, let's download it to current to folder
    if (!f.isDir) {
      // get file name
      auto nameOnly = nebula::common::Chars::last(f.name);
      downloadFile(client, this->bucket_, f.name, fmt::format("{0}/{1}", local, nameOnly));
    }
  }
}

void S3::upload(const std::string& local, const std::string& s3) {
  std::lock_guard<std::mutex> lock(s3s_);
  LOG(INFO) << "Upload: from " << local << " to " << s3;

  // need local file system to list files
  nebula::storage::local::File lfs;
  auto files = lfs.list(local);
  auto& client = s3client();
  for (auto& f : files) {
    if (!f.isDir) {
      uploadFile(client,
                 this->bucket_,
                 fmt::format("{0}/{1}", s3, f.name),
                 fmt::format("{0}/{1}", local, f.name));
    }
  }
}

bool S3::sync(const std::string& from, const std::string& to, bool recursive) {
  // need support local to S3 sync as well for writing/backup scenario
  N_ENSURE(!recursive, "support non-recursive sync only for now.");
  if (from.empty() || to.empty()) {
    LOG(WARNING) << "Invalid path: from=" << from << ", to=" << to;
    return false;
  }

  if (from.at(0) == '/') {
    upload(from, to);
  } else if (to.at(0) == '/') {
    download(from, to);
  } else {
    // from s3 to s3
    LOG(WARNING) << "Not supporting s3 to s3 sync for now";
    return false;
  }

  return true;
}

} // namespace aws
} // namespace storage
} // namespace nebula