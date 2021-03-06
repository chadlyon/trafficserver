/**
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

/**
 * @file utils_internal.cc
 */
#include "utils_internal.h"
#include <cassert>
#include <ts/ts.h>
#include <pthread.h>
#include <cstdlib>
#include <cassert>
#include <cstddef>
#include "atscppapi/Plugin.h"
#include "atscppapi/GlobalPlugin.h"
#include "atscppapi/Transaction.h"
#include "atscppapi/TransactionPlugin.h"
#include "atscppapi/TransformationPlugin.h"
#include "atscppapi/utils.h"
#include "logging_internal.h"

using namespace atscppapi;

namespace
{
// This is the highest txn arg that can be used, we choose this
// value to minimize the likelihood of it causing any problems.
const int MAX_TXN_ARG = 15;
const int TRANSACTION_STORAGE_INDEX = MAX_TXN_ARG;

int
handleTransactionEvents(TSCont cont, TSEvent event, void *edata)
{
  // This function is only here to clean up Transaction objects
  TSHttpTxn ats_txn_handle = static_cast<TSHttpTxn>(edata);
  Transaction &transaction = utils::internal::getTransaction(ats_txn_handle);
  LOG_DEBUG("Got event %d on continuation %p for transaction (ats pointer %p, object %p)", event, cont, ats_txn_handle,
            &transaction);

  switch (event) {
  case TS_EVENT_HTTP_POST_REMAP:
    transaction.getClientRequest().getUrl().reset();
    // This is here to force a refresh of the cached client request url
    TSMBuffer hdr_buf;
    TSMLoc hdr_loc;
    (void)TSHttpTxnClientReqGet(static_cast<TSHttpTxn>(transaction.getAtsHandle()), &hdr_buf, &hdr_loc);
    break;
  case TS_EVENT_HTTP_SEND_REQUEST_HDR:
    utils::internal::initTransactionServerRequest(transaction);
    break;
  case TS_EVENT_HTTP_READ_RESPONSE_HDR:
    utils::internal::initTransactionServerResponse(transaction);
    break;
  case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
    utils::internal::initTransactionClientResponse(transaction);
    break;
  case TS_EVENT_HTTP_READ_CACHE_HDR:
    utils::internal::initTransactionCachedRequest(transaction);
    utils::internal::initTransactionCachedResponse(transaction);
    break;
  case TS_EVENT_HTTP_TXN_CLOSE: { // opening scope to declare plugins variable below
    const std::list<TransactionPlugin *> &plugins = utils::internal::getTransactionPlugins(transaction);
    for (std::list<TransactionPlugin *>::const_iterator iter = plugins.begin(), end = plugins.end(); iter != end; ++iter) {
      shared_ptr<Mutex> trans_mutex = utils::internal::getTransactionPluginMutex(**iter);
      LOG_DEBUG("Locking TransacitonPlugin mutex to delete transaction plugin at %p", *iter);
      trans_mutex->lock();
      LOG_DEBUG("Locked Mutex...Deleting transaction plugin at %p", *iter);
      delete *iter;
      trans_mutex->unlock();
    }
    delete &transaction;
  } break;
  default:
    assert(false); /* we should never get here */
    break;
  }
  TSHttpTxnReenable(ats_txn_handle, TS_EVENT_HTTP_CONTINUE);
  return 0;
}

void
setupTransactionManagement()
{
  // We must always have a cleanup handler available
  TSMutex mutex = NULL;
  TSCont cont = TSContCreate(handleTransactionEvents, mutex);
  TSHttpHookAdd(TS_HTTP_POST_REMAP_HOOK, cont);
  TSHttpHookAdd(TS_HTTP_SEND_REQUEST_HDR_HOOK, cont);
  TSHttpHookAdd(TS_HTTP_READ_RESPONSE_HDR_HOOK, cont);
  TSHttpHookAdd(TS_HTTP_SEND_RESPONSE_HDR_HOOK, cont);
  TSHttpHookAdd(TS_HTTP_READ_CACHE_HDR_HOOK, cont);
  TSHttpHookAdd(TS_HTTP_TXN_CLOSE_HOOK, cont);
}

void inline invokePluginForEvent(Plugin *plugin, TSHttpTxn ats_txn_handle, TSEvent event)
{
  Transaction &transaction = utils::internal::getTransaction(ats_txn_handle);
  switch (event) {
  case TS_EVENT_HTTP_PRE_REMAP:
    plugin->handleReadRequestHeadersPreRemap(transaction);
    break;
  case TS_EVENT_HTTP_POST_REMAP:
    plugin->handleReadRequestHeadersPostRemap(transaction);
    break;
  case TS_EVENT_HTTP_SEND_REQUEST_HDR:
    plugin->handleSendRequestHeaders(transaction);
    break;
  case TS_EVENT_HTTP_READ_RESPONSE_HDR:
    plugin->handleReadResponseHeaders(transaction);
    break;
  case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
    plugin->handleSendResponseHeaders(transaction);
    break;
  case TS_EVENT_HTTP_OS_DNS:
    plugin->handleOsDns(transaction);
    break;
  case TS_EVENT_HTTP_READ_REQUEST_HDR:
    plugin->handleReadRequestHeaders(transaction);
    break;
  case TS_EVENT_HTTP_READ_CACHE_HDR:
    plugin->handleReadCacheHeaders(transaction);
    break;
  case TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE:
    plugin->handleReadCacheLookupComplete(transaction);
    break;
  case TS_EVENT_HTTP_SELECT_ALT:
    plugin->handleSelectAlt(transaction);
    break;

  default:
    assert(false); /* we should never get here */
    break;
  }
}

} /* anonymous namespace */

Transaction &
utils::internal::getTransaction(TSHttpTxn ats_txn_handle)
{
  Transaction *transaction = static_cast<Transaction *>(TSHttpTxnArgGet(ats_txn_handle, TRANSACTION_STORAGE_INDEX));
  if (!transaction) {
    transaction = new Transaction(static_cast<void *>(ats_txn_handle));
    LOG_DEBUG("Created new transaction object at %p for ats pointer %p", transaction, ats_txn_handle);
    TSHttpTxnArgSet(ats_txn_handle, TRANSACTION_STORAGE_INDEX, transaction);
  }
  return *transaction;
}

shared_ptr<Mutex>
utils::internal::getTransactionPluginMutex(TransactionPlugin &transaction_plugin)
{
  return transaction_plugin.getMutex();
}

TSHttpHookID
utils::internal::convertInternalHookToTsHook(Plugin::HookType hooktype)
{
  switch (hooktype) {
  case Plugin::HOOK_READ_REQUEST_HEADERS_POST_REMAP:
    return TS_HTTP_POST_REMAP_HOOK;
  case Plugin::HOOK_READ_REQUEST_HEADERS_PRE_REMAP:
    return TS_HTTP_PRE_REMAP_HOOK;
  case Plugin::HOOK_READ_RESPONSE_HEADERS:
    return TS_HTTP_READ_RESPONSE_HDR_HOOK;
  case Plugin::HOOK_SEND_REQUEST_HEADERS:
    return TS_HTTP_SEND_REQUEST_HDR_HOOK;
  case Plugin::HOOK_SEND_RESPONSE_HEADERS:
    return TS_HTTP_SEND_RESPONSE_HDR_HOOK;
  case Plugin::HOOK_OS_DNS:
    return TS_HTTP_OS_DNS_HOOK;
  case Plugin::HOOK_READ_REQUEST_HEADERS:
    return TS_HTTP_READ_REQUEST_HDR_HOOK;
  case Plugin::HOOK_READ_CACHE_HEADERS:
    return TS_HTTP_READ_CACHE_HDR_HOOK;
  case Plugin::HOOK_CACHE_LOOKUP_COMPLETE:
    return TS_HTTP_CACHE_LOOKUP_COMPLETE_HOOK;
  case Plugin::HOOK_SELECT_ALT:
    return TS_HTTP_SELECT_ALT_HOOK;
  default:
    assert(false); // shouldn't happen, let's catch it early
    break;
  }
  return static_cast<TSHttpHookID>(-1);
}

TSHttpHookID
utils::internal::convertInternalTransformationTypeToTsHook(TransformationPlugin::Type type)
{
  switch (type) {
  case TransformationPlugin::RESPONSE_TRANSFORMATION:
    return TS_HTTP_RESPONSE_TRANSFORM_HOOK;
  case TransformationPlugin::REQUEST_TRANSFORMATION:
    return TS_HTTP_REQUEST_TRANSFORM_HOOK;
  default:
    assert(false); // shouldn't happen, let's catch it early
    break;
  }
  return static_cast<TSHttpHookID>(-1);
}

void
utils::internal::invokePluginForEvent(TransactionPlugin *plugin, TSHttpTxn ats_txn_handle, TSEvent event)
{
  ScopedSharedMutexLock scopedLock(plugin->getMutex());
  ::invokePluginForEvent(static_cast<Plugin *>(plugin), ats_txn_handle, event);
}

void
utils::internal::invokePluginForEvent(GlobalPlugin *plugin, TSHttpTxn ats_txn_handle, TSEvent event)
{
  ::invokePluginForEvent(static_cast<Plugin *>(plugin), ats_txn_handle, event);
}

std::string
utils::internal::consumeFromTSIOBufferReader(TSIOBufferReader reader)
{
  std::string str;
  int avail = TSIOBufferReaderAvail(reader);

  if (avail != TS_ERROR) {
    int consumed = 0;
    if (avail > 0) {
      str.reserve(avail + 1);

      int64_t data_len;
      const char *char_data;
      TSIOBufferBlock block = TSIOBufferReaderStart(reader);
      while (block != NULL) {
        char_data = TSIOBufferBlockReadStart(block, reader, &data_len);
        str.append(char_data, data_len);
        consumed += data_len;
        block = TSIOBufferBlockNext(block);
      }
    }
    TSIOBufferReaderConsume(reader, consumed);
  } else {
    LOG_ERROR("TSIOBufferReaderAvail returned error code %d for reader %p", avail, reader);
  }

  return str;
}


HttpVersion
utils::internal::getHttpVersion(TSMBuffer hdr_buf, TSMLoc hdr_loc)
{
  int version = TSHttpHdrVersionGet(hdr_buf, hdr_loc);
  if (version != TS_ERROR) {
    if ((TS_HTTP_MAJOR(version) == 0) && (TS_HTTP_MINOR(version) == 0)) {
      return HTTP_VERSION_0_9;
    }
    if ((TS_HTTP_MAJOR(version) == 1) && (TS_HTTP_MINOR(version) == 0)) {
      return HTTP_VERSION_1_0;
    }
    if ((TS_HTTP_MAJOR(version) == 1) && (TS_HTTP_MINOR(version) == 1)) {
      return HTTP_VERSION_1_1;
    } else {
      LOG_ERROR("Unrecognized version %d", version);
    }
  } else {
    LOG_ERROR("Could not get version; hdr_buf %p, hdr_loc %p", hdr_buf, hdr_loc);
  }
  return HTTP_VERSION_UNKNOWN;
}

void
utils::internal::initTransactionManagement()
{
  static pthread_once_t setup_pthread_once_control = PTHREAD_ONCE_INIT;
  pthread_once(&setup_pthread_once_control, setupTransactionManagement);
}
