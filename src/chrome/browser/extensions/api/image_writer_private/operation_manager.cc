// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/lazy_instance.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/extensions/api/image_writer_private/destroy_partitions_operation.h"
#include "chrome/browser/extensions/api/image_writer_private/error_messages.h"
#include "chrome/browser/extensions/api/image_writer_private/operation.h"
#include "chrome/browser/extensions/api/image_writer_private/operation_manager.h"
#include "chrome/browser/extensions/api/image_writer_private/write_from_file_operation.h"
#include "chrome/browser/extensions/api/image_writer_private/write_from_url_operation.h"
#include "chrome/browser/extensions/event_router_forwarder.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/notification_service.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/extension_host.h"

namespace image_writer_api = extensions::api::image_writer_private;

namespace extensions {
namespace image_writer {

using content::BrowserThread;

OperationManager::OperationManager(content::BrowserContext* context)
    : profile_(Profile::FromBrowserContext(context)), weak_factory_(this) {
  registrar_.Add(this, chrome::NOTIFICATION_EXTENSION_UNINSTALLED,
                 content::Source<Profile>(profile_));
  registrar_.Add(this, chrome::NOTIFICATION_EXTENSION_UNLOADED_DEPRECATED,
                 content::Source<Profile>(profile_));
  registrar_.Add(this, chrome::NOTIFICATION_EXTENSION_PROCESS_TERMINATED,
                 content::Source<Profile>(profile_));
  registrar_.Add(this, chrome::NOTIFICATION_EXTENSION_HOST_VIEW_SHOULD_CLOSE,
                 content::Source<Profile>(profile_));
  registrar_.Add(this, chrome::NOTIFICATION_EXTENSION_HOST_DESTROYED,
                 content::Source<Profile>(profile_));
}

OperationManager::~OperationManager() {
}

void OperationManager::Shutdown() {
  for (OperationMap::iterator iter = operations_.begin();
       iter != operations_.end();
       iter++) {
    BrowserThread::PostTask(BrowserThread::FILE,
                            FROM_HERE,
                            base::Bind(&Operation::Abort,
                                       iter->second));
  }
}

void OperationManager::StartWriteFromUrl(
    const ExtensionId& extension_id,
    GURL url,
    const std::string& hash,
    const std::string& device_path,
    const Operation::StartWriteCallback& callback) {
#if defined(OS_CHROMEOS)
  // Chrome OS can only support a single operation at a time.
  if (operations_.size() > 0) {
#else
  OperationMap::iterator existing_operation = operations_.find(extension_id);

  if (existing_operation != operations_.end()) {
#endif
    return callback.Run(false, error::kOperationAlreadyInProgress);
  }

  scoped_refptr<Operation> operation(
      new WriteFromUrlOperation(weak_factory_.GetWeakPtr(),
                                extension_id,
                                profile_->GetRequestContext(),
                                url,
                                hash,
                                device_path));
  operations_[extension_id] = operation;
  BrowserThread::PostTask(BrowserThread::FILE,
                          FROM_HERE,
                          base::Bind(&Operation::Start, operation));
  callback.Run(true, "");
}

void OperationManager::StartWriteFromFile(
    const ExtensionId& extension_id,
    const base::FilePath& path,
    const std::string& device_path,
    const Operation::StartWriteCallback& callback) {
#if defined(OS_CHROMEOS)
  // Chrome OS can only support a single operation at a time.
  if (operations_.size() > 0) {
#else
  OperationMap::iterator existing_operation = operations_.find(extension_id);

  if (existing_operation != operations_.end()) {
#endif
    return callback.Run(false, error::kOperationAlreadyInProgress);
  }

  scoped_refptr<Operation> operation(new WriteFromFileOperation(
      weak_factory_.GetWeakPtr(), extension_id, path, device_path));
  operations_[extension_id] = operation;
  BrowserThread::PostTask(BrowserThread::FILE,
                          FROM_HERE,
                          base::Bind(&Operation::Start, operation));
  callback.Run(true, "");
}

void OperationManager::CancelWrite(
    const ExtensionId& extension_id,
    const Operation::CancelWriteCallback& callback) {
  Operation* existing_operation = GetOperation(extension_id);

  if (existing_operation == NULL) {
    callback.Run(false, error::kNoOperationInProgress);
  } else {
    BrowserThread::PostTask(BrowserThread::FILE,
                            FROM_HERE,
                            base::Bind(&Operation::Cancel, existing_operation));
    DeleteOperation(extension_id);
    callback.Run(true, "");
  }
}

void OperationManager::DestroyPartitions(
    const ExtensionId& extension_id,
    const std::string& device_path,
    const Operation::StartWriteCallback& callback) {
  OperationMap::iterator existing_operation = operations_.find(extension_id);

  if (existing_operation != operations_.end()) {
    return callback.Run(false, error::kOperationAlreadyInProgress);
  }

  scoped_refptr<Operation> operation(new DestroyPartitionsOperation(
      weak_factory_.GetWeakPtr(), extension_id, device_path));
  operations_[extension_id] = operation;
  BrowserThread::PostTask(BrowserThread::FILE,
                          FROM_HERE,
                          base::Bind(&Operation::Start, operation));
  callback.Run(true, "");
}

void OperationManager::OnProgress(const ExtensionId& extension_id,
                                  image_writer_api::Stage stage,
                                  int progress) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  image_writer_api::ProgressInfo info;
  info.stage = stage;
  info.percent_complete = progress;

  scoped_ptr<base::ListValue> args(
      image_writer_api::OnWriteProgress::Create(info));
  scoped_ptr<Event> event(new Event(
      image_writer_api::OnWriteProgress::kEventName, args.Pass()));

  EventRouter::Get(profile_)
      ->DispatchEventToExtension(extension_id, event.Pass());
}

void OperationManager::OnComplete(const ExtensionId& extension_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  scoped_ptr<base::ListValue> args(image_writer_api::OnWriteComplete::Create());
  scoped_ptr<Event> event(new Event(
      image_writer_api::OnWriteComplete::kEventName, args.Pass()));

  EventRouter::Get(profile_)
      ->DispatchEventToExtension(extension_id, event.Pass());

  DeleteOperation(extension_id);
}

void OperationManager::OnError(const ExtensionId& extension_id,
                               image_writer_api::Stage stage,
                               int progress,
                               const std::string& error_message) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  image_writer_api::ProgressInfo info;

  DLOG(ERROR) << "ImageWriter error: " << error_message;

  info.stage = stage;
  info.percent_complete = progress;

  scoped_ptr<base::ListValue> args(
      image_writer_api::OnWriteError::Create(info, error_message));
  scoped_ptr<Event> event(new Event(
      image_writer_api::OnWriteError::kEventName, args.Pass()));

  EventRouter::Get(profile_)
      ->DispatchEventToExtension(extension_id, event.Pass());

  DeleteOperation(extension_id);
}

Operation* OperationManager::GetOperation(const ExtensionId& extension_id) {
  OperationMap::iterator existing_operation = operations_.find(extension_id);

  if (existing_operation == operations_.end())
    return NULL;
  return existing_operation->second.get();
}

void OperationManager::DeleteOperation(const ExtensionId& extension_id) {
  OperationMap::iterator existing_operation = operations_.find(extension_id);
  if (existing_operation != operations_.end()) {
    operations_.erase(existing_operation);
  }
}

void OperationManager::Observe(int type,
                               const content::NotificationSource& source,
                               const content::NotificationDetails& details) {
  switch (type) {
    case chrome::NOTIFICATION_EXTENSION_UNINSTALLED: {
      DeleteOperation(content::Details<const Extension>(details).ptr()->id());
      break;
    }
    case chrome::NOTIFICATION_EXTENSION_UNLOADED_DEPRECATED: {
      DeleteOperation(content::Details<const Extension>(details).ptr()->id());
      break;
    }
    case chrome::NOTIFICATION_EXTENSION_PROCESS_TERMINATED: {
      DeleteOperation(content::Details<const Extension>(details).ptr()->id());
      break;
    }
    case chrome::NOTIFICATION_EXTENSION_HOST_VIEW_SHOULD_CLOSE: {
      DeleteOperation(
        content::Details<ExtensionHost>(details)->extension()->id());
      break;
    }
    case chrome::NOTIFICATION_EXTENSION_HOST_DESTROYED: {
      DeleteOperation(
        content::Details<ExtensionHost>(details)->extension()->id());
      break;
    }
    default: {
      NOTREACHED();
      break;
    }
  }
}

OperationManager* OperationManager::Get(content::BrowserContext* context) {
  return BrowserContextKeyedAPIFactory<OperationManager>::Get(context);
}

static base::LazyInstance<BrowserContextKeyedAPIFactory<OperationManager> >
    g_factory = LAZY_INSTANCE_INITIALIZER;

BrowserContextKeyedAPIFactory<OperationManager>*
OperationManager::GetFactoryInstance() {
  return g_factory.Pointer();
}


}  // namespace image_writer
}  // namespace extensions
