/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */
#pragma once

#include "ObjectMap.h"
#include "magma_logging.h"

namespace magma {

/**
 * RedisMap stores objects using the redis hash structure. This map requires a
 * serializer and deserializer to store objects as strings in the redis store
 */
template <typename ObjectType>
class RedisMap : public ObjectMap<ObjectType> {
public:
  RedisMap(
    std::shared_ptr<cpp_redis::client> client,
    const std::string& hash,
    std::function<bool(const ObjectType&, std::string&)> serializer,
    std::function<bool(const std::string&, ObjectType&)> deserializer)
    : client_(client),
      hash_(hash),
      serializer_(serializer),
      deserializer_(deserializer) {}

  /**
   * set serializes the object passed into a string and stores it at the key.
   * Returns false if the operation was unsuccessful
   */
  ObjectMapResult set(
      const std::string& key,
      const ObjectType& object) override {
    std::string value;
    if (!serializer_(object, value)) {
      MLOG(MERROR) << "Unable to serialize value for key " << key;
      return SERIALIZE_FAIL;
    }
    auto hset_future = client_->hset(hash_, key, value);
    client_->sync_commit();
    bool is_error = hset_future.get().is_error();
    if (is_error) {
      MLOG(MERROR) << "Error setting value in redis for key " << key;
      return CLIENT_ERROR;
    }
    return SUCCESS;
  }

  /**
   * get returns the object located at key. If the key was not found or the
   * operation was unsuccessful, this returns false
   */
  ObjectMapResult get(const std::string& key, ObjectType& object_out) override {
    auto hget_future = client_->hget(hash_, key);
    client_->sync_commit();
    auto reply = hget_future.get();
    if (reply.is_null()) {
      // value just doesn't exist
      return KEY_NOT_FOUND;
    } else if (reply.is_error()) {
      MLOG(MERROR) << "Unable to get value for key " << key;
      return CLIENT_ERROR;
    } else if (!reply.is_string()) {
      MLOG(MERROR) << "Value was not string for key " << key;
      return INCORRECT_VALUE_TYPE;
    }
    if (!deserializer_(reply.as_string(), object_out)) {
      MLOG(MERROR) << "Failed to deserialize key " << key
        << " with value " << reply.as_string();
      return DESERIALIZE_FAIL;
    }
    return SUCCESS;
  }

  /**
   * getall returns all values stored in the hash
   */
  ObjectMapResult getall(std::vector<ObjectType>& values_out) override {
      return getall(values_out, nullptr);
  }

  /**
   * getall is an extra overloaded function that also returns the keys of values
   * that failed to be deserialized.
   */
  ObjectMapResult getall(
    std::vector<ObjectType>& values_out,
    std::vector<std::string>* failed_keys) {
    auto hgetall_future = client_->hgetall(hash_);
    client_->sync_commit();
    auto reply = hgetall_future.get();
    if (reply.is_error()) {
      MLOG(MERROR) << "unable to perform hvals command";
      return CLIENT_ERROR;
    } else if (reply.is_null()) {
      // fine, just no values
      return SUCCESS;
    }
    auto array = reply.as_array();
    for (int i = 0; i < array.size(); i += 2) {
      auto key_reply = array[i];
      if (!key_reply.is_string()) {
        // this should essentially never happen
        MLOG(MERROR) << "Non string key found";
      }
      auto key = key_reply.as_string();
      auto value_reply = array[i+1];
      if (!value_reply.is_string()) {
        MLOG(MERROR) << "Non string value found";
        if (failed_keys != nullptr) failed_keys->push_back(key);
        continue;
      }
      ObjectType obj;
      if (!deserializer_(value_reply.as_string(), obj)) {
        MLOG(MERROR) << "Unable to deseralize value in map";
        if (failed_keys != nullptr) failed_keys->push_back(key);
        continue;
      }
      values_out.push_back(obj);
    }
    return SUCCESS;
  }

private:
  std::shared_ptr<cpp_redis::client> client_;
  std::string hash_;
  std::function<bool(const ObjectType&, std::string&)> serializer_;
  std::function<bool(const std::string&, ObjectType&)> deserializer_;
};

}
